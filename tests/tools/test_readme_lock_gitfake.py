#!/usr/bin/env python3
"""Fake git for the readme-lock history_state regression tests.

tests/tools/test_readme_lock.py puts a wrapper named "git" on PATH and
selects a scenario with NM_FAKE_SCENARIO before calling
check.history_state. State lives inside the -C root, so scenarios never
leak between roots or runs.

Scenarios (see SCENARIOS):
- not-shallow: rev-parse prints "false"; no fetch runs.
- fetch-success-unshallows: rev-parse prints "true" before the fetch, the
  fetch exits 0, and rev-parse prints "false" after it.
- fetch-success-still-shallow: the fetch exits 0 but the flag stays "true".
- fetch-fail: the fetch exits 1 and the flag stays "true".
"""
import os
import sys

SCENARIOS = {
    "not-shallow": {"before": "false", "fetch": 0, "after": "false"},
    "fetch-success-unshallows": {"before": "true", "fetch": 0,
                                 "after": "false"},
    "fetch-success-still-shallow": {"before": "true", "fetch": 0,
                                    "after": "true"},
    "fetch-fail": {"before": "true", "fetch": 1, "after": "true"},
}


def state_path(root):
    return os.path.join(root, ".fake-git-state")


def main():
    scenario = SCENARIOS.get(os.environ.get("NM_FAKE_SCENARIO", ""))
    if scenario is None:
        print("fake-git: NM_FAKE_SCENARIO is unset or unknown", file=sys.stderr)
        return 64
    args = sys.argv[1:]
    root = args[args.index("-C") + 1] if "-C" in args else "default"
    path = state_path(root)
    if "rev-parse" in args and "--is-shallow-repository" in args:
        unshallowed = os.path.exists(path)
        print(scenario["after"] if unshallowed else scenario["before"])
        return 0
    if "fetch" in args:
        if scenario["fetch"] == 0:
            with open(path, "w") as handle:
                handle.write("1")
        return scenario["fetch"]
    return 1


if __name__ == "__main__":
    sys.exit(main())
