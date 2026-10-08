#!/usr/bin/env python3
"""Embed a verified catalog as byte arrays for C++ compilers, including MSVC."""

import json
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: embed.py CATALOG_DIR DATA_CPP VERSION_HEADER")
    catalog = Path(sys.argv[1])
    output = Path(sys.argv[2])
    header = Path(sys.argv[3])
    root = Path(__file__).resolve().parents[2]
    lock = json.loads((root / "parity/reference.json").read_text())
    paths = sorted(p for p in catalog.rglob("*") if p.is_file())
    output.parent.mkdir(parents=True, exist_ok=True)
    header.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w") as cpp:
        cpp.write('#include "core/catalog/catalog.h"\n\nnamespace nm {\nnamespace {\n')
        for index, path in enumerate(paths):
            data = path.read_bytes()
            cpp.write(f"static const unsigned char file_{index}[] = {{\n")
            for offset in range(0, len(data), 20):
                cpp.write("  " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 20]) + ",\n")
            if not data:
                cpp.write("  0x00,\n")
            cpp.write("};\n")
        cpp.write("static const CatalogFile catalog[] = {\n")
        for index, path in enumerate(paths):
            rel = path.relative_to(catalog).as_posix()
            size = path.stat().st_size
            cpp.write(f'  {{"{rel}", std::string_view(reinterpret_cast<const char*>(file_{index}), {size})}},\n')
        cpp.write("};\n}  // namespace\n")
        cpp.write("std::span<const CatalogFile> catalog_files() { return catalog; }\n}  // namespace nm\n")
    header.write_text(
        "#pragma once\n"
        f'#define NM_CATALOG_VERSION "{lock["version"]}"\n'
        f"#define NM_CATALOG_FILE_COUNT {len(paths)}\n"
    )
    print(f"CATALOG EMBED: {len(paths)} files")


if __name__ == "__main__":
    main()
