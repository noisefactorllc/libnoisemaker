#pragma once

#include <span>
#include <string_view>

namespace libnoisemaker {
struct CatalogFile {
    std::string_view path;
    std::string_view bytes;
};

const CatalogFile* catalog_find(std::string_view path);
std::span<const CatalogFile> catalog_files();
}  // namespace libnoisemaker
