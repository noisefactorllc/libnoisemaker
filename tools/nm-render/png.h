#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nm_render {

// An 8-bit RGBA PNG of the rows in order, row 0 first.
std::vector<uint8_t> encode_png_rgba8(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
bool write_file(const std::string& path, const std::vector<uint8_t>& bytes);

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // rows in file order, row 0 first
};
// A non-interlaced 8-bit PNG (gray, gray-alpha, RGB or RGBA) as RGBA rows;
// throws std::runtime_error naming what it cannot read.
Image decode_png(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> read_file(const std::string& path);

}  // namespace nm_render
