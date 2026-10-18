#!/usr/bin/env python3
"""Check that each license notice in a built catalog has an entry in catalog-NOTICES.txt.

usage: check_notices.py CATALOG_DIR NOTICES_FILE

A license notice is a line of a catalog file that names a license, a
copyright or the public domain (MARKER). Each one must belong to a work in
WORKS: one of the work's patterns must match a line of the comment block that
holds the notice, or the line next to it. Each work in WORKS must have an
entry in NOTICES_FILE, and each entry a work in WORKS. An entry gives the
work's permission notice, or says that the work is in the public domain. Each
Noisemaker source line that an entry cites (shaders/effects/...:N) must hold a
license notice in the built catalog. Exits 1 and lists each failure.
"""

import re
import sys
from pathlib import Path

MARKER = re.compile(r"(?i:\b(?:licen[cs]e[sd]?|copyright|public domain|all rights reserved)\b)|©"
                    r"|\b(?:MIT|BSD|Apache|L?GPL|MPL|CC0|CC[- ]BY|WTFPL|Unlicense)\b")
# Entry name -> patterns that identify the work where the catalog credits it.
WORKS = {
    "Noisemaker": r"Noisemaker value noise|py-noisemaker|noisefactorllc/noisemaker",
    "webgl-noise": r"Ashima|Gustavson|webgl-noise|Simplex 2D - MIT License",
    "glsl-pcg-prng": r"PCG PRNG|glsl-pcg-prng",
    "glsl-voronoi-noise": r"glsl-voronoi-noise",
    "Wagner": r"spite/Wagner",
    "glfx.js": r"glfx\.js",
    "Shaders (rupak10987)": r"rupak10987/Shaders",
    "fractals (darkeclipz)": r"darkeclipz/fractals",
    "Oklab": r"(?i:oklab transform)|bottosson\.github\.io/posts/oklab",
    "Inigo Quilez": r"iquilezles\.org|Inigo Quilez",
    "Julia fractal": r"nuclear\.mutantstargoat\.com",
}
SEPARATOR = "-" * 80
CITATION = re.compile(r"^\s+shaders/effects/(\S+)/(glsl|wgsl)/(\S+):(\d+)$")


def is_comment(line):
    text = line.strip()
    return text.startswith(("//", "/*", "*")) or text.endswith("*/")


def block(lines, index):
    """The comment block that holds lines[index], with one line on each side."""
    start = end = index
    if is_comment(lines[index]):
        while start > 0 and is_comment(lines[start - 1]):
            start -= 1
        while end + 1 < len(lines) and is_comment(lines[end + 1]):
            end += 1
    return lines[max(start - 1, 0):end + 2]


def read_entries(text):
    """{entry name: entry text} of a notices file; the text before the first separator is its preamble."""
    sections = text.split("\n" + SEPARATOR + "\n")
    return {section.split("\n", 1)[0]: section for section in sections[1:]}


def check(catalog, notices):
    errors = []
    entries = read_entries(notices.read_text(encoding="utf-8"))
    for name in sorted(set(WORKS) - set(entries)):
        errors.append(f"{notices.name}: no entry for {name}")
    for name, entry in sorted(entries.items()):
        if name not in WORKS:
            errors.append(f"{notices.name}: entry {name!r} is not a work that check_notices.py knows")
        if "Permission is hereby granted" not in entry and "public domain" not in entry:
            errors.append(f"{notices.name}: entry {name!r} has no permission notice or public-domain statement")
        for line in entry.splitlines():
            cited = CITATION.match(line)
            if not cited:
                continue
            effect, language, filename, number = cited.groups()
            path = catalog / language / effect / filename
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                errors.append(f"{notices.name}: entry {name!r} cites {line.strip()}, which is not in the catalog")
                continue
            number = int(number)
            if not 1 <= number <= len(lines) or not MARKER.search(lines[number - 1]):
                errors.append(f"{notices.name}: entry {name!r} cites {line.strip()}, which holds no license notice")
    patterns = {name: re.compile(pattern) for name, pattern in WORKS.items()}
    for path in sorted(p for p in catalog.rglob("*") if p.is_file()):
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for index, line in enumerate(lines):
            if not MARKER.search(line):
                continue
            context = block(lines, index)
            if not any(pattern.search(text) for pattern in patterns.values() for text in context):
                rel = path.relative_to(catalog).as_posix()
                errors.append(f"{rel}:{index + 1}: no entry in {notices.name} covers {line.strip()!r}")
    return errors


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_notices.py CATALOG_DIR NOTICES_FILE")
    catalog, notices = Path(sys.argv[1]), Path(sys.argv[2])
    if not catalog.is_dir():
        raise SystemExit(f"{catalog} is not a built catalog")
    errors = check(catalog, notices)
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print(f"CATALOG NOTICES: {len(read_entries(notices.read_text(encoding='utf-8')))} entries cover every license notice")
    return 0


if __name__ == "__main__":
    sys.exit(main())
