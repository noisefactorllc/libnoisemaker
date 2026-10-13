#include "png.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace nm_render {

namespace {

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 24));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

void chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    put32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

}  // namespace

// A zlib stream of stored (uncompressed) deflate blocks: no compressor, and
// the PNG decodes to exactly these rows.
std::vector<uint8_t> encode_png_rgba8(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        const uint8_t* row = rgba.data() + size_t(y) * width * 4;
        raw.insert(raw.end(), row, row + size_t(width) * 4);
    }
    std::vector<uint8_t> zlib{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    size_t offset = 0;
    do {
        const size_t length = std::min<size_t>(65535, raw.size() - offset);
        const bool last = offset + length == raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(uint8_t(length));
        zlib.push_back(uint8_t(length >> 8));
        zlib.push_back(uint8_t(~length));
        zlib.push_back(uint8_t(~length >> 8));
        zlib.insert(zlib.end(), raw.begin() + long(offset), raw.begin() + long(offset + length));
        offset += length;
    } while (offset < raw.size());
    put32(zlib, (b << 16) | a);

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> header;
    put32(header, width);
    put32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, no filter set, no interlace
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", {});
    return png;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path.c_str(), "wb"), &std::fclose);
    if (!file) return false;
    return std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size();
}

}  // namespace nm_render

namespace nm_render {

namespace {

[[noreturn]] void bad(const std::string& what) { throw std::runtime_error("PNG: " + what); }

// RFC 1951 inflate: stored, fixed and dynamic Huffman blocks, refusing to
// produce more than limit bytes.
class Inflater {
public:
    Inflater(const uint8_t* data, size_t size, size_t limit) : data_(data), size_(size), limit_(limit) {}

    // The input offset just past the last block, byte aligned.
    size_t end() const { return pos_ + (bitPos_ != 0 ? 1 : 0); }

    std::vector<uint8_t> run() {
        out_.reserve(limit_);
        bool last = false;
        while (!last) {
            last = bits(1) != 0;
            const uint32_t type = bits(2);
            if (type == 0) {
                stored();
            } else if (type == 1) {
                Huffman lit, dist;
                fixedTables(lit, dist);
                block(lit, dist);
            } else if (type == 2) {
                Huffman lit, dist;
                dynamicTables(lit, dist);
                block(lit, dist);
            } else {
                bad("invalid deflate block type");
            }
        }
        return std::move(out_);
    }

private:
    struct Huffman {
        std::array<uint16_t, 16> counts{};
        std::vector<uint16_t> symbols;
    };

    uint32_t bit() {
        if (pos_ >= size_) bad("deflate stream ends early");
        const uint32_t b = (data_[pos_] >> bitPos_) & 1u;
        if (++bitPos_ == 8) {
            bitPos_ = 0;
            ++pos_;
        }
        return b;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= bit() << i;
        return v;
    }

    // The code's unused space: negative when oversubscribed, zero when
    // complete.
    static int build(Huffman& h, const uint8_t* lengths, size_t n) {
        h.counts.fill(0);
        for (size_t i = 0; i < n; ++i) ++h.counts[lengths[i]];
        h.counts[0] = 0;
        int left = 1;
        for (size_t len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.counts[len];
            if (left < 0) return left;
        }
        std::array<uint16_t, 16> offsets{};
        for (int len = 1; len < 16; ++len) offsets[size_t(len)] = uint16_t(offsets[size_t(len - 1)] + h.counts[size_t(len - 1)]);
        h.symbols.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            if (lengths[i] != 0) h.symbols[offsets[lengths[i]]++] = uint16_t(i);
        }
        return left;
    }

    static size_t used(const Huffman& h) {
        size_t total = 0;
        for (size_t len = 1; len < 16; ++len) total += h.counts[len];
        return total;
    }

    void emit(uint8_t byte) {
        if (out_.size() >= limit_) bad("more image data than the header declares");
        out_.push_back(byte);
    }

    uint32_t decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= int(bit());
            const int count = h.counts[size_t(len)];
            if (code - count < first) return h.symbols[size_t(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        bad("invalid Huffman code");
    }

    void stored() {
        // The rest of the current byte is padding.
        if (bitPos_ != 0) {
            bitPos_ = 0;
            ++pos_;
        }
        if (pos_ + 4 > size_) bad("stored block ends early");
        const uint32_t len = data_[pos_] | (uint32_t(data_[pos_ + 1]) << 8);
        const uint32_t nlen = data_[pos_ + 2] | (uint32_t(data_[pos_ + 3]) << 8);
        if ((len ^ 0xFFFFu) != nlen) bad("stored block length check fails");
        pos_ += 4;
        if (pos_ + len > size_) bad("stored block ends early");
        if (out_.size() + len > limit_) bad("more image data than the header declares");
        out_.insert(out_.end(), data_ + pos_, data_ + pos_ + len);
        pos_ += len;
    }

    static void fixedTables(Huffman& lit, Huffman& dist) {
        std::array<uint8_t, 288> lengths{};
        for (size_t i = 0; i < 144; ++i) lengths[i] = 8;
        for (size_t i = 144; i < 256; ++i) lengths[i] = 9;
        for (size_t i = 256; i < 280; ++i) lengths[i] = 7;
        for (size_t i = 280; i < 288; ++i) lengths[i] = 8;
        build(lit, lengths.data(), lengths.size());
        std::array<uint8_t, 30> d{};
        d.fill(5);
        build(dist, d.data(), d.size());
    }

    void dynamicTables(Huffman& lit, Huffman& dist) {
        const uint32_t hlit = bits(5) + 257, hdist = bits(5) + 1, hclen = bits(4) + 4;
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        std::array<uint8_t, 19> codeLengths{};
        for (uint32_t i = 0; i < hclen; ++i) codeLengths[order[i]] = uint8_t(bits(3));
        Huffman lengthCodes;
        if (build(lengthCodes, codeLengths.data(), codeLengths.size()) != 0) bad("invalid code length code");
        std::vector<uint8_t> lengths(hlit + hdist, 0);
        for (uint32_t i = 0; i < hlit + hdist;) {
            const uint32_t sym = decode(lengthCodes);
            if (sym < 16) {
                lengths[i++] = uint8_t(sym);
                continue;
            }
            uint32_t repeat = 0;
            uint8_t value = 0;
            if (sym == 16) {
                if (i == 0) bad("repeat with no previous length");
                value = lengths[i - 1];
                repeat = 3 + bits(2);
            } else if (sym == 17) {
                repeat = 3 + bits(3);
            } else {
                repeat = 11 + bits(7);
            }
            if (i + repeat > hlit + hdist) bad("code lengths overflow");
            for (uint32_t k = 0; k < repeat; ++k) lengths[i++] = value;
        }
        if (lengths[256] == 0) bad("no end-of-block code");
        // Incomplete codes only for a single symbol, as zlib allows.
        const int litLeft = build(lit, lengths.data(), hlit);
        if (litLeft < 0 || (litLeft > 0 && used(lit) != 1)) bad("invalid literal/length code");
        const int distLeft = build(dist, lengths.data() + hlit, hdist);
        if (distLeft < 0 || (distLeft > 0 && used(dist) > 1)) bad("invalid distance code");
    }

    void block(const Huffman& lit, const Huffman& dist) {
        static const uint16_t lengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                                67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                                4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t distBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                              193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t distExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            const uint32_t sym = decode(lit);
            if (sym < 256) {
                emit(uint8_t(sym));
            } else if (sym == 256) {
                return;
            } else {
                const uint32_t li = sym - 257;
                if (li >= 29) bad("invalid length symbol");
                const uint32_t length = lengthBase[li] + bits(lengthExtra[li]);
                const uint32_t di = decode(dist);
                if (di >= 30) bad("invalid distance symbol");
                const uint32_t distance = distBase[di] + bits(distExtra[di]);
                if (distance > out_.size()) bad("distance before the start");
                const size_t from = out_.size() - distance;
                for (uint32_t k = 0; k < length; ++k) emit(out_[from + k]);
            }
        }
    }

    const uint8_t* data_;
    size_t size_;
    size_t limit_;
    size_t pos_ = 0;
    int bitPos_ = 0;
    std::vector<uint8_t> out_;
};

uint32_t get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return uint8_t(a);
    if (pb <= pc) return uint8_t(b);
    return uint8_t(c);
}

}  // namespace

std::vector<uint8_t> read_file(const std::string& path) {
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) throw std::runtime_error("cannot read " + path);
    std::vector<uint8_t> bytes;
    uint8_t buffer[65536];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, file.get())) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    return bytes;
}

Image decode_png(const std::vector<uint8_t>& bytes) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8) != 0) bad("not a PNG file");
    Image image;
    uint8_t depth = 0, colorType = 0, compression = 0, filterMethod = 0, interlace = 0;
    std::vector<uint8_t> idat;
    size_t pos = 8;
    bool header = false;
    bool ended = false;
    while (pos + 12 <= bytes.size() && !ended) {
        const uint32_t length = get32(&bytes[pos]);
        if (pos + 12 + size_t(length) > bytes.size()) bad("chunk ends early");
        const uint8_t* type = &bytes[pos + 4];
        const uint8_t* data = &bytes[pos + 8];
        if (crc32(type, size_t(length) + 4) != get32(data + length)) bad("chunk CRC mismatch");
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (header || pos != 8 || length != 13) bad("IHDR must come first, once, with 13 bytes");
            header = true;
            image.width = get32(data);
            image.height = get32(data + 4);
            depth = data[8];
            colorType = data[9];
            compression = data[10];
            filterMethod = data[11];
            interlace = data[12];
            // Checked before any image data is collected. Host textures are
            // at most 16384 pixels on a side and 4096 x 4096 in all, so the
            // sizes below stay far from overflow.
            if (image.width == 0 || image.height == 0 || image.width > 16384 || image.height > 16384 ||
                uint64_t(image.width) * image.height > uint64_t(4096) * 4096) {
                bad("image size must be 1 to 16384 pixels on a side and at most 4096 x 4096 in all");
            }
            if (compression != 0 || filterMethod != 0) bad("unknown compression or filter method");
            if (depth != 8 || interlace != 0) bad("only non-interlaced 8-bit images are supported");
        } else if (!header) {
            bad("IHDR must come first");
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data, data + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            ended = true;
        }
        pos += 12 + size_t(length);
    }
    if (!header || !ended) bad("missing IHDR or IEND");
    // Beside the file, decoding holds its IDAT data (no larger than the
    // file), the inflated rows (at most 67,112,960 bytes, reserved once and
    // unfiltered in place; the IDAT data is freed before the next copy) and
    // the RGBA copy (at most 67,108,864 bytes).
    size_t channels = 0;
    switch (colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: bad("unsupported color type " + std::to_string(colorType));
    }
    // zlib: deflate with a window of at most 32 KiB, a valid header check and
    // no preset dictionary, then the Adler-32 of the data.
    if (idat.size() < 6) bad("image data too short");
    const uint8_t cmf = idat[0], flg = idat[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((uint32_t(cmf) << 8) | flg) % 31 != 0 || (flg & 0x20) != 0) {
        bad("not a zlib deflate stream");
    }
    const size_t stride = size_t(image.width) * channels;
    const size_t expected = (stride + 1) * size_t(image.height);
    Inflater inflater(idat.data() + 2, idat.size() - 2, expected);
    std::vector<uint8_t> raw = inflater.run();
    if (raw.size() != expected) bad("image data size differs from the header");
    if (2 + inflater.end() + 4 > idat.size()) bad("zlib checksum missing");
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    if (get32(&idat[2 + inflater.end()]) != ((b << 16) | a)) bad("zlib checksum mismatch");
    idat = std::vector<uint8_t>();
    // Unfilter in place: each row's previous and upper bytes are already
    // decoded when a byte reads them.
    for (size_t y = 0; y < image.height; ++y) {
        uint8_t* row = &raw[y * (stride + 1) + 1];
        const uint8_t filter = row[-1];
        const uint8_t* up = y > 0 ? row - (stride + 1) : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= channels ? row[x - channels] : 0;
            const int b = up ? up[x] : 0;
            const int c = up && x >= channels ? up[x - channels] : 0;
            int value = row[x];
            switch (filter) {
                case 0: break;
                case 1: value += a; break;
                case 2: value += b; break;
                case 3: value += (a + b) / 2; break;
                case 4: value += paeth(a, b, c); break;
                default: bad("invalid filter type");
            }
            row[x] = uint8_t(value);
        }
    }
    image.rgba.resize(size_t(image.width) * image.height * 4);
    for (size_t y = 0; y < image.height; ++y) {
        const uint8_t* row = &raw[y * (stride + 1) + 1];
        for (size_t x = 0; x < image.width; ++x) {
            const uint8_t* p = row + x * channels;
            uint8_t* q = &image.rgba[(y * image.width + x) * 4];
            if (channels >= 3) {
                q[0] = p[0];
                q[1] = p[1];
                q[2] = p[2];
            } else {
                q[0] = q[1] = q[2] = p[0];
            }
            q[3] = channels == 4 ? p[3] : channels == 2 ? p[1] : 255;
        }
    }
    return image;
}

}  // namespace nm_render
