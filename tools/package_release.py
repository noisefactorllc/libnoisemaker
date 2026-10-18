#!/usr/bin/env python3
"""Install a shared build into a release archive and check its contents.

The archive is libnoisemaker-<version>-<target>.tar.gz (a .zip on Windows)
holding one directory of that name: include/noisemaker, the shared library
and, on Windows, its import library, lib/cmake/noisemaker, share/noisemaker
(ENGINE, TOOLCHAIN) and share/doc/noisemaker (LICENSE, acorn-LICENSE.txt and
catalog-NOTICES.txt, the notices of the works in the embedded catalog).
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tarfile
import zipfile

REQUIRED = [
    "include/noisemaker/core.h",
    "include/noisemaker/core.hpp",
    "include/noisemaker/value.hpp",
    "lib/cmake/noisemaker/noisemakerConfig.cmake",
    "lib/cmake/noisemaker/noisemakerConfigVersion.cmake",
    "lib/cmake/noisemaker/noisemakerTargets.cmake",
    "share/noisemaker/ENGINE",
    "share/noisemaker/TOOLCHAIN",
    "share/doc/noisemaker/LICENSE",
    "share/doc/noisemaker/acorn-LICENSE.txt",
    "share/doc/noisemaker/catalog-NOTICES.txt",
]
SHARED = {
    "linux": ["lib/libnoisemaker_core.so"],
    "darwin": ["lib/libnoisemaker_core.dylib"],
    "win32": ["bin/noisemaker_core.dll", "lib/noisemaker_core.lib"],
}


def cache_value(build, name):
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(name + ":"):
            return line.split("=", 1)[1]
    raise SystemExit(f"{name} is not in {build / 'CMakeCache.txt'}")


def check_macos(library, archs, minos):
    have = subprocess.run(["lipo", "-archs", str(library)], capture_output=True, text=True, check=True)
    if sorted(have.stdout.split()) != sorted(archs.split()):
        raise SystemExit(f"{library}: architectures {have.stdout.strip()!r}, expected {archs!r}")
    build = subprocess.run(["vtool", "-show-build", str(library)], capture_output=True, text=True, check=True)
    found = re.findall(r"^\s*minos\s+(\S+)", build.stdout, re.MULTILINE)
    if len(found) != len(archs.split()) or any(value != minos for value in found):
        raise SystemExit(f"{library}: minimum macOS {found}, expected {minos} for each of {archs}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", required=True, type=pathlib.Path, help="configured and built build directory")
    parser.add_argument("--target", required=True, help="release target name, e.g. linux-x64")
    parser.add_argument("--out", required=True, type=pathlib.Path, help="directory for the archive")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--cmake", default="cmake", help="CMake executable")
    parser.add_argument("--expect-archs", help="macOS: the architectures every library must contain")
    parser.add_argument("--expect-minos", help="macOS: the minimum OS every slice must declare")
    args = parser.parse_args()

    build = args.build.resolve()
    if cache_value(build, "NM_BUILD_SHARED") not in ("ON", "TRUE", "1"):
        raise SystemExit("release archives need a build configured with NM_BUILD_SHARED=ON")
    version = cache_value(build, "CMAKE_PROJECT_VERSION")
    name = f"libnoisemaker-{version}-{args.target}"
    out = args.out.resolve()
    stage = out / "stage" / name
    if stage.exists():
        raise SystemExit(f"{stage} exists; use an empty --out directory")
    subprocess.run([args.cmake, "--install", str(build), "--config", args.config, "--prefix", str(stage)], check=True)

    platform = "win32" if sys.platform == "win32" else ("darwin" if sys.platform == "darwin" else "linux")
    missing = [path for path in REQUIRED + SHARED[platform] if not (stage / path).is_file()]
    if missing:
        raise SystemExit(f"{name} lacks {', '.join(missing)}")
    if platform == "darwin" and args.expect_archs:
        check_macos(stage / SHARED["darwin"][0], args.expect_archs, args.expect_minos)

    files = sorted(path for path in stage.rglob("*") if path.is_file())
    if platform == "win32":
        archive = out / f"{name}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
            for path in files:
                zf.write(path, path.relative_to(stage.parent).as_posix())
    else:
        archive = out / f"{name}.tar.gz"
        with tarfile.open(archive, "w:gz") as tf:
            for path in files:
                tf.add(path, path.relative_to(stage.parent).as_posix())
    print(archive)
    return 0


if __name__ == "__main__":
    sys.exit(main())
