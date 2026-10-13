#include "png.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

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
