#include "core/catalog/catalog.h"

#include <noisemaker/core.h>

#include "catalog_version.h"

#include <algorithm>

namespace nm {
const CatalogFile* catalog_find(std::string_view path) {
    const auto files = catalog_files();
    const auto it = std::lower_bound(files.begin(), files.end(), path,
                                     [](const CatalogFile& file, std::string_view key) {
                                         return file.path < key;
                                     });
    return it != files.end() && it->path == path ? &*it : nullptr;
}
}  // namespace nm

extern "C" const char* nm_engine_version(void) { return NM_CATALOG_VERSION; }
