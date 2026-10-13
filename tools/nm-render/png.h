#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nm_render {

// An 8-bit RGBA PNG of the rows in order, row 0 first.
std::vector<uint8_t> encode_png_rgba8(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
bool write_file(const std::string& path, const std::vector<uint8_t>& bytes);

}  // namespace nm_render
