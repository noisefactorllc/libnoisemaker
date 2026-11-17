#!/usr/bin/env python3
"""Check that a Windows noisemaker_core.dll exports only the documented API.

usage: windows_exports.py noisemaker_core.dll include/noisemaker

The documented API is what the installed headers declare: the C functions
core.h declares with NM_API, and the classes and functions core.hpp and
value.hpp declare with NM_CXX_API. MSVC exports only what is marked
dllexport, so the export table must hold exactly these names:

- each C export is an NM_API function of core.h, and each of those is
  exported;
- each C++ export lies in namespace libnoisemaker (or libnoisemaker::json),
  and is either a member of an NM_CXX_API class, including the members the
  compiler generates for it, or an NM_CXX_API free function, and each of
  those classes and functions is exported.

The check then removes each NM_API and NM_CXX_API marking in turn from a
copy of the headers and requires the same export table to fail against it,
so the check cannot pass without reading the markings.
Exits 1 and lists each failure.
"""

import re
import struct
import sys
from pathlib import Path

C_FUNCTION = re.compile(r"\bNM_API\s[^;(]*?\b(nm_\w+)\s*\(")
CXX_CLASS = re.compile(r"\bclass\s+NM_CXX_API\s+(\w+)")
CXX_FUNCTION = re.compile(r"\bNM_CXX_API\s+[^;{]*?\b(\w+)\s*\(")
# The leading qualified name of an MSVC decorated symbol: ?name@scope@...@@
# or ??<code>Class@scope@...@@ for a special member (constructor ?0,
# destructor ?1, operator ?<letter or digit>, vftable ?_7 and the like).
DECORATED = re.compile(r"^\?(\?_?[0-9A-Z])?([A-Za-z_]\w*)@((?:[A-Za-z_]\w*@)+)@")


def export_names(dll):
    data = Path(dll).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{dll} is not a PE image")
    sections, optional_size = struct.unpack_from("<H12xH", data, pe + 6)
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    export_rva = struct.unpack_from("<I", data, directories)[0]
    table = optional + optional_size
    spans = [struct.unpack_from("<8xIIII", data, table + 40 * index) for index in range(sections)]

    def offset(rva):
        for size, address, raw_size, raw in spans:
            if address <= rva < address + max(size, raw_size):
                return raw + rva - address
        raise SystemExit(f"{dll}: RVA {rva:#x} is outside every section")

    if export_rva == 0:
        return []
    # IMAGE_EXPORT_DIRECTORY: NumberOfNames at 24, AddressOfNames at 32.
    count, names = struct.unpack_from("<24xI4xI", data, offset(export_rva))
    result = []
    for index in range(count):
        name = offset(struct.unpack_from("<I", data, offset(names) + 4 * index)[0])
        result.append(data[name:data.index(b"\0", name)].decode("ascii"))
    return result


def namespace_at(text, position):
    """The nested namespace, "json" or "", that encloses a header position."""
    opened = text.rfind("namespace json {", 0, position)
    return "json" if opened != -1 and text.rfind("// namespace json", 0, position) < opened else ""


def declarations(headers):
    """C functions, and C++ classes and functions keyed by (namespace, name),
    with each class's body."""
    c_api = set(C_FUNCTION.findall(headers["core.h"]))
    classes, functions = {}, set()
    for file in ("core.hpp", "value.hpp"):
        text = headers[file]
        for match in CXX_CLASS.finditer(text):
            end = text.find("\n};", match.end())
            classes[(namespace_at(text, match.start()), match.group(1))] = text[match.end():end]
        for match in CXX_FUNCTION.finditer(text):
            if not text[match.start():match.end()].startswith("NM_CXX_API class"):
                functions.add((namespace_at(text, match.start()), match.group(1)))
    return c_api, classes, functions


def problems(exports, headers):
    c_api, classes, functions = declarations(headers)
    found, seen_classes, seen_functions = [], set(), set()
    for name in exports:
        if not name.startswith("?"):
            if name not in c_api:
                found.append(f"{name}: not an NM_API function of core.h")
            continue
        match = DECORATED.match(name)
        if not match:
            found.append(f"{name}: not a plain libnoisemaker name")
            continue
        special, identifier, scope_text = match.groups()
        scopes = scope_text.rstrip("@").split("@")  # innermost first
        if scopes[-1] != "libnoisemaker":
            found.append(f"{name}: outside namespace libnoisemaker")
            continue
        namespace = "json" if "json" in scopes[:-1] else ""
        owners = [scope for scope in scopes[:-1] if scope != "json"]
        if special:
            owner, member = identifier, None
            if owners:
                found.append(f"{name}: special member of a nested class")
                continue
        elif owners:
            owner, member = owners[0], identifier
        else:
            owner, member = None, identifier
        if owner is None:
            seen_functions.add((namespace, member))
            if (namespace, member) not in functions:
                found.append(f"{name}: {member} is not an NM_CXX_API function")
            continue
        seen_classes.add((namespace, owner))
        if (namespace, owner) not in classes:
            found.append(f"{name}: class {owner} is not NM_CXX_API")
        elif member is not None and not re.search(rf"\b{re.escape(member)}\s*\(", classes[(namespace, owner)]):
            found.append(f"{name}: {owner}::{member} is not declared in its class")
    for missing in sorted(c_api - set(exports)):
        found.append(f"{missing}: declared NM_API but not exported")
    for namespace, missing in sorted(set(classes) - seen_classes):
        found.append(f"class {missing}: declared NM_CXX_API but not exported")
    for namespace, missing in sorted(functions - seen_functions):
        found.append(f"{missing}: declared NM_CXX_API but not exported")
    return found


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[2])
    exports = export_names(sys.argv[1])
    include = Path(sys.argv[2])
    headers = {name: (include / name).read_text(encoding="utf-8") for name in ("core.h", "core.hpp", "value.hpp")}
    failures = problems(exports, headers)
    print(f"{len(exports)} exports, {sum(not name.startswith('?') for name in exports)} C")

    # Each marking removed must make the same exports fail.
    markings = 0
    for file, text in headers.items():
        for match in re.finditer(r"\bNM_API\b(?!\s*__)|\bNM_CXX_API\b", text):
            line = text[text.rfind("\n", 0, match.start()) + 1:text.find("\n", match.end())]
            if line.lstrip().startswith("#"):
                continue
            markings += 1
            stripped = dict(headers)
            stripped[file] = text[:match.start()] + text[match.end():]
            if not problems(exports, stripped):
                failures.append(f"{file}: removing the marking in '{line.strip()}' does not fail the check")
    if markings == 0:
        failures.append("no NM_API or NM_CXX_API marking found in the headers")
    print(f"{markings} markings, each removal fails the check")

    for failure in failures:
        print(f"FAIL {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
