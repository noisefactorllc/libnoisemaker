#include "check.h"
#include "core/catalog/catalog.h"
#include "core/value/json.h"
#include <noisemaker/core.h>

#include "catalog_version.h"

#include <string_view>

int main() {
    const libnoisemaker::CatalogFile* file = libnoisemaker::catalog_find("effects/synth/noise.json");
    NM_CHECK(file != nullptr);
    libnoisemaker::json::parse(file->bytes);
    NM_CHECK(libnoisemaker::catalog_files().size() == NM_CATALOG_FILE_COUNT);
    NM_CHECK(std::string_view(nm_engine_version()) == "1.0.320");
    NM_CHECK(libnoisemaker::catalog_find("effects/unknown/nope.json") == nullptr);
    return 0;
}
