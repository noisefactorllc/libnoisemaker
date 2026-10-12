#!/usr/bin/env python3
"""Build the pinned Dawn once and install it for -DNM_DAWN_PREBUILT_DIR.

Dawn is configured on its own, with the options in cmake/DawnOptions.cmake,
from the tree tools/fetch_dawn.py prepares. The install prefix gets a
.nm-commit stamp with the lock's commit, which cmake/Dawn.cmake checks.
"""

import argparse
import json
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCK = ROOT / "third_party" / "dawn.lock.json"
OPTIONS = pathlib.Path("cmake") / "DawnOptions.cmake"
STAMP = ".nm-commit"


def lock_commit():
    return json.loads(LOCK.read_text())["dawn"]["commit"]


def commands(args):
    configure = [args.cmake, "-S", str(args.source), "-B", str(args.build)]
    if args.generator:
        configure += ["-G", args.generator]
    if args.arch:
        configure += ["-A", args.arch]
    configure += ["-C", str(ROOT / OPTIONS), "-DDAWN_ENABLE_INSTALL=ON", "-DCMAKE_BUILD_TYPE=Release",
                  f"-DCMAKE_INSTALL_PREFIX={args.prefix}"]
    if args.osx_arch:
        configure.append(f"-DCMAKE_OSX_ARCHITECTURES={args.osx_arch}")
    if args.osx_deployment_target:
        configure.append(f"-DCMAKE_OSX_DEPLOYMENT_TARGET={args.osx_deployment_target}")
    build = [args.cmake, "--build", str(args.build), "--config", "Release", "--target", "webgpu_dawn", "--parallel"]
    install = [args.cmake, "--install", str(args.build), "--config", "Release"]
    return [configure, build, install]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--prefix", required=True, type=pathlib.Path, help="install prefix")
    parser.add_argument("--source", type=pathlib.Path, default=ROOT / ".cache" / "dawn" / "src")
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / ".cache" / "dawn-build")
    parser.add_argument("--cmake", default="cmake", help="CMake executable")
    parser.add_argument("--generator", help="CMake generator")
    parser.add_argument("--arch", help="CMake -A platform (Visual Studio)")
    parser.add_argument("--osx-arch", help="CMAKE_OSX_ARCHITECTURES")
    parser.add_argument("--osx-deployment-target", help="CMAKE_OSX_DEPLOYMENT_TARGET")
    parser.add_argument("--dry-run", action="store_true", help="print the steps without running them")
    args = parser.parse_args()

    commit = lock_commit()
    steps = commands(args)
    if args.dry_run:
        for step in steps:
            print(shlex.join(step))
        print(f"stamp {args.prefix.as_posix()}/{STAMP} {commit}")
        return 0

    args.prefix = args.prefix.resolve()
    steps = commands(args)
    have = (args.source / STAMP).read_text().strip() if (args.source / STAMP).exists() else "missing"
    if have != commit:
        raise SystemExit(f"Dawn at {args.source} is {have}; the lock wants {commit}. Run: python3 tools/fetch_dawn.py")
    for step in steps:
        print("+ " + shlex.join(step), flush=True)
        subprocess.run(step, check=True)
    (args.prefix / STAMP).write_text(commit + "\n")
    print(f"dawn {commit} installed at {args.prefix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
