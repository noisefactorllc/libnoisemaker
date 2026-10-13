// nm-render's PNG codec: decoding images zlib compressed with stored,
// fixed and dynamic Huffman blocks, rows using all five filter types, in
// gray, gray-alpha, RGB and RGBA (Python's zlib made them; the digest is
// FNV-1a 64 of the expected RGBA rows), the corruptions below refused, and
// the encoder's output decoding to its input.
#include "check.h"
#include "png.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Vector {
    const char* name;
    const char* hex;
    uint32_t width;
    uint32_t height;
    uint64_t digest;
};

const Vector kVectors[] = {
    {"rgba stored", "89504e470d0a1a0a0000000d4948445200000007000000050806000000899af6d80000009c4944415478010191006eff000b30557a9fc4e90e33587da2c7ec11365b80a5caef14395e83a8cdf201173c6186949494949494949494949494949494949494949494949494020c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c031e3043555050d0d0d0d0505050d0d0d0d0505050d0d0d0d0505050d0040c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0cd63d2e81e8c557b60000000049454e44ae426082", 7, 5, 0x20222b0c44981651ull},  // BTYPE 0
    {"rgba fixed", "89504e470d0a1a0a0000000d494844520000000600000005080600000066589de600000035494441547801636080021b28a88002c62d400092f00102980210cd74040ab640018ccf0c927d0404301a644c1410b0806419b00000418b29e375a341370000000049454e44ae426082", 6, 5, 0x01932e575a8b6b65ull},  // BTYPE 1
    {"rgba dynamic", "89504e470d0a1a0a0000000d49484452000000400000002808060000004ecefce9000003d04944415478daeddaf95f0c711cc7f19ddd49ba28e9be741f2825154952940e1d3add476eb972e5b68988c8514a9452a48844ae92905ce5889012a5dc2521b7f7fa033c4acbce36f3f97de687efee3ce7f57d7c87c5320b8ce6f49912dbc16a7abc98755082e48039499ded8253ba382c3c20e71892aee8bcecb08adbcaa3ea1edc6ccde161393abee1a7f50322728d4645e6f71c1b75a1d784ed85bd27c55cb1981a77bdef8cdd25fd6725deb69d9b7c77d0fcd4fb8317a5953b2dc9a874599ef964d8aaac1acfd0e375de6b4fbef45b7fe6cd888d790da3379f7f3f6eebc58f13775cfe3c79e7d56fd376ddf83973cf4df6ecbd7744e6edbbd771c1fe07128b0f3eeab4f4d0639915479e765d7dec99c29a13cf95d79d7aa5b6e1ecdb6e9bcebdd3de52d0a4b7edd227c3e8a22f3d62af7d37892f669925dce2f4492aed60955226667de0a1e480f48ace7687abba381cad9673ccae5574ce79a1e276fab5ba476ebde670e2f702d078d82c9a0f87a564e6c2a6d670db743527143f6bcb8724a8379c365ddddabf0001c14808260ac1c421981404938660b2104c1e822941305508a601c1b420982e04338060dd21983104338560e610cc1282f5836036106c2004b387604320d85008e60ac1dc21981704f38160fe106c24041b03c1c643b040083605824d876041106c0e040b86600b215808045b06c15642302e040b8360e1102c028245e637ea445df8a0bfbdb0d928e6cad79e71d77ff4da5d42f44ebc4d5a24df15ed9b7a5fbc7f5ab9946d46a5f4a0cc27b283b36ae49d8ed729b99c7ca93aeccc1b0dcfbc062d6f82b7000c82949e7ffcbcb155ccddda0d807f710b92eeaf41161b828940b08e104c02827582603210ac2b04538060ca104c0d82758360da104c0f821942b01e10cc0482a1a38a1092d71092c508c95b08c95284641942f22142b20221598590ac4648d622245f20245f2324eb11928d08c90f08c96684e45784e40f84248190241192a208497184a41442521a21298b909447482a21245511921a08492d84a42e42d20021d91d21698c903445489a23242d1192fd10923608c98108497b84e41084e45084a42b42d21d21e98590f421780bf09fdd2131d4415000ef796a9560fb42b0f5973308f2632b2cac150843fd085e06332548e7278042087205710b5200f94da961d1b9020b9af4fc09de023008b6fbf9c3769854b3706f3f08b299126cf5d0f5400421e98b900c20780b208476f16d4bcd16d6d737df4a9022087205750b0641aa1c8808a20261e8488297c14c09d2793bcc20289c275a7c3b5466d1b9020b9b8d4611bc056010a4338222ea961e2dfcf684d3aa6f4f84c45052484fb4f836bf00eed9595c4452d7c00000000049454e44ae426082", 64, 40, 0xfefe45f814bd3baaull},  // BTYPE 2
    {"rgb dynamic", "89504e470d0a1a0a0000000d49484452000000210000000a0802000000b5eda0cd000003664944415478da45d26d4c12011cc771e0bc14518f87c8d003453d9240cd7ca88659ab399badb94a7b9a664fba9c96994b57f6b05aadb6569a6665b35af36133d334b69acdd67a50b3274b534141d2138a211c2789a2a8dd0593ffdbdfbeaffe1f0a05f48496f204c1626954ecfa8d89493b76ef49cfcac9cd2f3e7ff1724959c5bd9ac74d0dcdaf5f757efd38d0a31c434d568bdd46833cb8017e48a02436326ecbb64d8969db77e51c3c527cfed4b5924bf71f95563455d7b4ca5f747de91c547f1fd7eba8e8f424e4cbf4617259de2c2e9be96381381087c966b2c863c32208c3b08855e166231e16193661363122a2220ce68955e2481cd7af96c6ba13fb1a62d7127b0cb103c46e217621b1e3abe3440b184643013b4da5d7da40eaf0d86027b00068543a3b08aab5ea790a0dedfdfcdedf8ba6fad1cee3d2877f7ed2702060e04b07ec0daafbda058141b3c02c80aa46db5adb798870de6dc617d5a06fda3a602e3207ce0dff1a95cb895e0a5c397386d1dbab397a2837bbe56d2be8e3d5df3df41b35ac8d8f9ea4837465bf42673144c9d67878ba51954aa556371ebb41e6ce0014fdaa6d07d3f7d63fbc270af64751cd9f51559750c84304b04e43f443a1e1d108d1dbacd31defdd6a6de03141881f03b3608faaea4e64e7f311fec4b8555ede80159d380e0b11233ef9f0f61320af20cf572a328fe00f2a6a4f9ecd9204fb6398d954555f7b2c9fec67318b95ec4f3bfbeaf2eb64cfa082741ae5cecdeb972fce4c998dbf47d58ade6f5d1f36262425a7ecdd7f383bb7a0f01cc5dd93edbb9c2f92482365f16f3f74f7f429c6464c06f3dcccd51bd7ee9655d65437bd90b7b56e4ade929ab12ff3404e61fe590f6f9a3bcc5a261186c8e2633e76bf1bd450bff799381083c92eadbcc562d1d95ecba62008ba5b7ad385a0a4bc64114159559d0b41634ba30b41737d8b0bc1cbe62617826734e289e33afb1c989e9a493cd9800e9108d2b2335d0832d2b216111cdeb975f32282fd2949198b08d253135c08e2d625b8106c009e3ebf9057ec78225f1826b6cd4f2f014804a14152314eb53910ac08964ccfff7522a8933714141739112022a97561d6d17b0bf93cfdb0c281203408a68f8c2849046e46edfa18a447a593399e08f388271b9d08026091598f3b9e18c5916053c4fe1f8160e94a80d81d08f85c64c2ae732280c5ce9e4440f6369cff0f910be3f66868773f0000000049454e44ae426082", 33, 10, 0x6aa3daa05f855c4eull},  // BTYPE 2
    {"gray fixed", "89504e470d0a1a0a0000000d4948445200000009000000050800000000b23899b700000022494441547801636090b50a2f99b8eef40b4656592860628501662e412860818b0100df7105f8eabfb3090000000049454e44ae426082", 9, 5, 0x501eb31e52ed649cull},  // BTYPE 1
    {"gray-alpha stored", "89504e470d0a1a0a0000000d4948445200000005000000060804000000a1f29cc00000004d494441547801014200bdff00fff2e5d8cbbeb1a4978a017d70e6e6e6e6e6e6e6e6027e7e7e7e7e7e7e7e7e7e03fcf5b2b2b2b2b2b2b2b2047e7ee6e6e6e6e6e6e6e60075685b4e4134271a0d0086f926b72bf19e980000000049454e44ae426082", 5, 6, 0x14e1939985f2630dull},  // BTYPE 0
    {"rgba noise", "89504e470d0a1a0a0000000d49484452000000100000000a0806000000bdbede9c000001154944415478da636038d6132c797f598ed18ffdadde4237e6a56a7fdc51e7c275717aaccaab0de5b6cc272784c93c5e996ff6e750a7bfe8ed45197a5f77377af05d999da8fe764b9503fb59c6c9510acfa424c1400a86101c1cc2481ca62993a74c990cc253a02c14ce14821ce615dc6f2f84875fbb1e1e76fd7a18105f4370c2611c2003c201094339d7a11c16906130574d4176229c3305e6dec930c74f41f206c3a31579a6bf0f76f889dc5a98aefb6557833befe559096a6f3657dab39d991429ff744d91c5bfa3dd8112f796641b7edfd7e225707d6eb2d6876db5ce9ce7a7c528bd5ccf586ac37482a8d0c211bc4c4487168ee0659ef9e7de415c0104e46086e875588842821716885803486a0aee108571006854fac412099ba90000000049454e44ae426082", 16, 10, 0xe0a2d684662b5c41ull},  // BTYPE 1
};

std::vector<uint8_t> unhex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

uint64_t fnv(const std::vector<uint8_t>& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : bytes) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

uint32_t crc32(const std::vector<uint8_t>& bytes, size_t from, size_t to) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = from; i < to; ++i) {
        crc ^= bytes[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(uint8_t(v >> (24 - 8 * k)));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out, start, out.size()));
}

// An RGBA PNG of this size whose zlib stream holds this deflate data.
std::vector<uint8_t> png_with_deflate(const std::vector<uint8_t>& deflate, uint32_t width = 1, uint32_t height = 1) {
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> header;
    put32(header, width);
    put32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", header);
    std::vector<uint8_t> zlib{0x78, 0x01};
    zlib.insert(zlib.end(), deflate.begin(), deflate.end());
    put32(zlib, 1);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", {});
    return png;
}

// Deflate bits, least significant first.
struct Bits {
    std::vector<uint8_t> bytes;
    int used = 8;
    void put(uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            if (used == 8) {
                bytes.push_back(0);
                used = 0;
            }
            bytes.back() = uint8_t(bytes.back() | (((value >> i) & 1u) << used));
            ++used;
        }
    }
};

// A dynamic block header whose code length code gives symbols 0 and 1 one
// bit each, then 258 lengths of 1: 257 one-bit literal codes, oversubscribed.
// With allOnes false, the code length code itself is oversubscribed.
std::vector<uint8_t> dynamic_header(bool allOnes) {
    static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    Bits bits;
    bits.put(1, 1);
    bits.put(2, 2);
    bits.put(0, 5);
    bits.put(0, 5);
    bits.put(allOnes ? 14 : 15, 4);
    for (int i = 0; i < (allOnes ? 18 : 19); ++i) bits.put(allOnes ? (order[i] <= 1 ? 1 : 0) : 1, 3);
    if (allOnes) {
        for (int i = 0; i < 258; ++i) bits.put(1, 1);
    }
    return bits.bytes;
}

bool throws(const std::vector<uint8_t>& bytes, const char* reason = nullptr) {
    try {
        nm_render::decode_png(bytes);
    } catch (const std::runtime_error& e) {
        return reason == nullptr || std::string(e.what()).find(reason) != std::string::npos;
    }
    return false;
}

}  // namespace

int main() {
    for (const Vector& v : kVectors) {
        const nm_render::Image image = nm_render::decode_png(unhex(v.hex));
        NM_CHECK(image.width == v.width);
        NM_CHECK(image.height == v.height);
        NM_CHECK(image.rgba.size() == size_t(v.width) * v.height * 4);
        if (fnv(image.rgba) != v.digest) {
            std::fprintf(stderr, "%s: wrong pixels\n", v.name);
            return 1;
        }
    }
    // A flipped CRC bit, a truncated stream, a bad signature, a missing
    // IEND, an IHDR filter method, a zlib header check and an Adler-32 byte
    // are refused.
    std::vector<uint8_t> bytes = unhex(kVectors[2].hex);
    std::vector<uint8_t> corrupt = bytes;
    corrupt[40] ^= 1;
    NM_CHECK(throws(corrupt));
    NM_CHECK(throws(std::vector<uint8_t>(bytes.begin(), bytes.begin() + long(bytes.size() / 2))));
    corrupt = bytes;
    corrupt[1] = 'Q';
    NM_CHECK(throws(corrupt));
    NM_CHECK(throws(std::vector<uint8_t>(bytes.begin(), bytes.end() - 12)));
    // Rewrites a chunk's byte and its CRC: the IHDR starts at 8, the IDAT
    // after it at 33.
    const auto rewrite = [&](size_t chunk, size_t offset, uint8_t value) {
        std::vector<uint8_t> out = bytes;
        const uint32_t length = (uint32_t(out[chunk]) << 24) | (uint32_t(out[chunk + 1]) << 16) |
                                (uint32_t(out[chunk + 2]) << 8) | out[chunk + 3];
        out[chunk + 8 + offset] = value;
        const std::vector<uint8_t> body(out.begin() + long(chunk + 4), out.begin() + long(chunk + 8 + length));
        uint32_t crc = 0xFFFFFFFFu;
        for (uint8_t byte : body) {
            crc ^= byte;
            for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
        }
        crc = ~crc;
        for (int k = 0; k < 4; ++k) out[chunk + 8 + length + size_t(k)] = uint8_t(crc >> (24 - 8 * k));
        return out;
    };
    NM_CHECK(!throws(rewrite(8, 11, 0)));  // the unchanged filter method decodes
    NM_CHECK(throws(rewrite(8, 11, 1)));
    NM_CHECK(throws(rewrite(33, 1, uint8_t(bytes[33 + 9] ^ 1))));
    const uint32_t idatLength = (uint32_t(bytes[33]) << 24) | (uint32_t(bytes[34]) << 16) | (uint32_t(bytes[35]) << 8) | bytes[36];
    NM_CHECK(throws(rewrite(33, idatLength - 1, uint8_t(bytes[33 + 8 + idatLength - 1] ^ 1))));
    // Oversubscribed Huffman codes: the code length code, and the literal
    // and length code.
    NM_CHECK(throws(png_with_deflate(dynamic_header(false))));
    NM_CHECK(throws(png_with_deflate(dynamic_header(true))));
    // Larger than 4096 x 4096 in all, though each side is allowed.
    NM_CHECK(throws(png_with_deflate({}, 4097, 4096), "4096 x 4096"));
    NM_CHECK(throws(png_with_deflate({}, 16385, 1), "16384"));
    NM_CHECK(!throws(png_with_deflate({}, 16384, 1), "pixels"));
    // The encoder's stored blocks, over 65535 bytes so they split.
    std::vector<uint8_t> rgba(300 * 70 * 4);
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = uint8_t(i * 131 + (i >> 9));
    const nm_render::Image round = nm_render::decode_png(nm_render::encode_png_rgba8(300, 70, rgba));
    NM_CHECK(round.width == 300 && round.height == 70 && round.rgba == rgba);
    return 0;
}
