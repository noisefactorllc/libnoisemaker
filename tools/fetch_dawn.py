#!/usr/bin/env python3
"""Fetch pinned Dawn and its dependencies into a local cache."""

import argparse
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCK = ROOT / "third_party" / "dawn.lock.json"
STAMP = ".nm-commit"


def load_lock(path):
    return json.loads(pathlib.Path(path).read_text())


def git(*args):
    result = subprocess.run(["git", *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def parse_gitlink(ls_tree_output, path):
    parts = ls_tree_output.split()
    if len(parts) < 4 or parts[1] != "commit" or parts[3] != path:
        raise SystemExit(f"{path} is not a gitlink: {ls_tree_output.strip()!r}")
    return parts[2]


def gitlink(git_dir, commit, path):
    return parse_gitlink(git("--git-dir", str(git_dir), "ls-tree", commit, path), path)


def fetch_bare(git_dir, repository, commit):
    if not (git_dir / "HEAD").exists():
        git_dir.mkdir(parents=True, exist_ok=True)
        git("init", "--bare", "-q", str(git_dir))
    present = subprocess.run(
        ["git", "--git-dir", str(git_dir), "cat-file", "-e", f"{commit}^{{commit}}"],
        capture_output=True,
    ).returncode == 0
    if not present:
        git("--git-dir", str(git_dir), "fetch", "--depth", "1", "--no-tags", "-q", repository, commit)


def extract(git_dir, commit, dest):
    stamp = dest / STAMP
    if stamp.exists() and stamp.read_text().strip() == commit:
        return
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    archive = subprocess.run(
        ["git", "--git-dir", str(git_dir), "archive", "--format=tar", commit],
        capture_output=True, check=True,
    ).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(dest, filter="data")
    stamp.write_text(commit + "\n")


def check(lock, src):
    problems = []
    want = {".": lock["dawn"]["commit"], **{d["path"]: d["commit"] for d in lock["dependencies"]}}
    for path, commit in want.items():
        stamp = src / path / STAMP
        have = stamp.read_text().strip() if stamp.exists() else "missing"
        if have != commit:
            problems.append(f"{path}: have {have}, want {commit}")
    for line in problems:
        print(line, file=sys.stderr)
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", default=os.environ.get("NM_DAWN_CACHE", str(ROOT / ".cache" / "dawn")))
    parser.add_argument("--check", action="store_true", help="verify the cache against the lock; fetch nothing")
    args = parser.parse_args()
    lock = load_lock(LOCK)
    cache = pathlib.Path(args.cache).resolve()
    src = cache / "src"
    if args.check:
        return check(lock, src)
    dawn = lock["dawn"]
    dawn_git = cache / "git" / "dawn.git"
    fetch_bare(dawn_git, dawn["repository"], dawn["commit"])
    for dep in lock["dependencies"]:
        recorded = gitlink(dawn_git, dawn["commit"], dep["path"])
        if recorded != dep["commit"]:
            raise SystemExit(f"{dep['path']}: lock has {dep['commit']}, Dawn records {recorded}")
    extract(dawn_git, dawn["commit"], src)
    for dep in lock["dependencies"]:
        dep_git = cache / "git" / (dep["path"].replace("/", "_") + ".git")
        fetch_bare(dep_git, dep["repository"], dep["commit"])
        extract(dep_git, dep["commit"], src / dep["path"])
    print(f"dawn {dawn['commit']} ready at {src}")
    return check(lock, src)


if __name__ == "__main__":
    sys.exit(main())
