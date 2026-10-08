#include "check.h"
#include "core/catalog/catalog.h"
#include "core/value/json.h"
#include <noisemaker/core.h>

#include "catalog_version.h"

#include <string_view>

int main() {
    const nm::CatalogFile* file = nm::catalog_find("effects/synth/noise.json");
    NM_CHECK(file != nullptr);
    nm::json::parse(file->bytes);
    NM_CHECK(nm::catalog_files().size() == NM_CATALOG_FILE_COUNT);
    NM_CHECK(nm::catalog_files().size() == 798);
    NM_CHECK(std::string_view(nm_engine_version()) == "1.0.271");
    NM_CHECK(nm::catalog_find("effects/unknown/nope.json") == nullptr);
    return 0;
}
