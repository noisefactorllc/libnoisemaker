#!/usr/bin/env python3
"""Check the README tracking statement against the parity lock.

usage: check-readme-lock.py [README.md] [parity/reference.json]

The tracking sentence is the one sentence that begins "It tracks". It must
either name exactly the release parity/reference.json locks, or name the
lock file with no literal release number.

The sentence is anchored to Git history. When the sentence names a literal
release number, that number must match the version the lock held when the
sentence was written. Every move of the lock version after this check was
added must be followed by a sentence update before the tree is checked
again, unless the sentence names no literal release number. This
grandfathers the sentence as it was published before the check landed;
that known stale sentence stays tracked in the open issue.

The history rules need the history that anchors the sentence. When that
history is not readable, the check fetches the repository's own history
once when the clone is shallow, and otherwise states what it skipped and
passes. The local gate run has the history.
Exits 1 and lists each failure.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

LOCK = "parity/reference.json"
SENTENCE = re.compile(r"It tracks.*?\.(?=\s|$)", re.DOTALL)
NUMBER = re.compile(r"\b\d+\.\d+\.\d+\b")


def sentence_of(text):
    match = SENTENCE.search(text)
    return None if match is None else " ".join(match.group(0).split())


def numbers_in(sentence):
    return [] if sentence is None else NUMBER.findall(sentence)


def evaluate(sentence, written_version, stale_move):
    """Check one tracking sentence against its history.

    written_version is the version the lock held when the sentence was
    written, or None when history cannot anchor it. stale_move is True
    when a lock move after this check was added is newer than the
    sentence's last write.
    """
    if sentence is None:
        return ["README.md holds no 'It tracks ...' sentence"]
    errors = []
    numbers = numbers_in(sentence)
    if numbers:
        if written_version is not None and any(
                number != written_version for number in numbers):
            errors.append("README.md tracks "
                          f"{', '.join(numbers)} but the lock held "
                          f"{written_version} when the sentence was written")
        elif stale_move:
            errors.append("a lock move after this check landed left the "
                          "tracking sentence unchanged")
    elif LOCK not in sentence:
        errors.append("README.md names neither the locked release nor " + LOCK)
    return errors


def git(root, *args, seconds=30):
    result = subprocess.run(["git", "-C", str(root), *args],
                            capture_output=True, text=True, timeout=seconds)
    return result.returncode, result.stdout.strip()


def blob_text(root, spec):
    code, text = git(root, "show", spec)
    return text if code == 0 else None


def blob_sentence(root, commit):
    return sentence_of(blob_text(root, f"{commit}:README.md"))


def blob_version(root, commit):
    text = blob_text(root, f"{commit}:{LOCK}")
    if text is None:
        return None
    try:
        return json.loads(text)["version"]
    except (ValueError, KeyError):
        return None


def anchored_state(root, check_name):
    """Anchor the sentence and the lock moves in readable Git history.

    Returns (version the lock held when the sentence was written, the
    sentence's last-write commit, the newest lock move after the check was
    added), or None when history cannot anchor the statement.
    """
    code, _ = git(root, "rev-parse", "--is-shallow-repository")
    if code != 0:
        return None
    code, log = git(root, "log", "--format=%H %P", "--", "README.md")
    if code != 0 or not log:
        return None
    written_version, written_at = None, None
    for fields in (line.split() for line in log.splitlines()):
        commit, parents = fields[0], fields[1:]
        text = blob_text(root, f"{commit}:README.md")
        if text is None:
            return None
        current = sentence_of(text)
        parent = None
        if parents:
            parent_text = blob_text(root, f"{parents[0]}:README.md")
            if parent_text is None:
                return None
            parent = sentence_of(parent_text)
        if current != parent:
            written_version, written_at = blob_version(root, commit), commit
            break
    if not check_name:
        return written_version, written_at, None
    code, log = git(root, "log", "--format=%H %P", "--diff-filter=A", "--",
                    check_name)
    if code != 0 or not log:
        return written_version, written_at, None
    introduction = log.splitlines()[-1].split()[0]
    code, log = git(root, "log", "--format=%H %P",
                    f"{introduction}..HEAD", "--", LOCK)
    if code != 0:
        return written_version, written_at, None
    for fields in (line.split() for line in log.splitlines()):
        commit, parents = fields[0], fields[1:]
        version = blob_version(root, commit)
        previous = blob_version(root, parents[0]) if parents else None
        if version is not None and version != previous:
            return written_version, written_at, commit
    return written_version, written_at, None


def history_state(root, check_name):
    """Anchor the statement, fetching history when the clone is shallow.

    A shallow clone hides the commits the anchor needs. When the readable
    history cannot anchor the statement and the clone is shallow, one
    unshallow fetch of the repository's own remote repairs that; the check
    degrades to the stated skip when even the fetch cannot.
    """
    state = anchored_state(root, check_name)
    if state is not None:
        return state
    code, shallow = git(root, "rev-parse", "--is-shallow-repository")
    if code != 0 or shallow != "true":
        return None
    code, _ = git(root, "fetch", "--unshallow", "origin", seconds=120)
    if code != 0:
        return None
    return anchored_state(root, check_name)


def descendant_of(root, commit, ancestor):
    code, _ = git(root, "merge-base", "--is-ancestor", ancestor, commit)
    return code == 0


def main(argv):
    if len(argv) > 3:
        print("usage: check-readme-lock.py [README.md] [parity/reference.json]",
              file=sys.stderr)
        return 2
    root = Path.cwd()
    readme = Path(argv[1]) if len(argv) > 1 else root / "README.md"
    lock = Path(argv[2]) if len(argv) > 2 else root / LOCK
    sentence = sentence_of(readme.read_text(encoding="utf-8"))
    if len(argv) == 1:
        try:
            name = str(Path(__file__).resolve().relative_to(root.resolve()))
        except ValueError:
            name = None
        state = history_state(root, name) if name else None
    else:
        state = None
    written_version, written_at, newest_move = (
        state if state is not None else (None, None, None))
    if state is None:
        print("check-readme-lock: full Git history unavailable; the write-time "
              "version and the lock moves after this check were not verified",
              file=sys.stderr)
    stale_move = newest_move is not None and (
            written_at is None or not descendant_of(root, written_at,
                                                    newest_move))
    errors = evaluate(sentence, written_version, stale_move)
    if errors:
        for error in errors:
            print(f"check-readme-lock: {error}", file=sys.stderr)
        return 1
    content = json.loads(lock.read_text(encoding="utf-8"))
    print(f"check-readme-lock: the tracking sentence is anchored; the lock "
          f"holds {content['version']} ({content['commit']})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
