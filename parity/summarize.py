#!/usr/bin/env python3
"""The PARITY-SUMMARY line of a sweep ledger, for scripts/parity-summary.

  python3 parity/summarize.py LEDGER SWEEP_RC [case-id ...]

LEDGER is parity/sweep.py's ledger (LEDGER.effects.json beside it holds the
per-effect evidence of a full run) and SWEEP_RC the sweep's exit status. With
case ids, only those cases count. Prints the PARITY-SUMMARY line on stdout and
the cases outside the published contract on stderr.

Exit 0 iff every expected case executed, no case fails, is deferred, skipped
or missing, every near case lies within its reviewed per-case policy (the
policy and mechanism the sweep records with it), every catalog effect is
evidenced (full runs) and the sweep exited 0; 1 otherwise. Under the
operator's parity rule (2026-10-15) a NEAR case with a reviewed policy
passes; it is still counted apart from exact and strict.
"""

import json
import os
import sys


def has_policy(row):
    """A near case's reviewed per-case policy: its bound and its mechanism."""
    return bool(row.get("policy")) and bool(row.get("mechanism"))


def summarize(rows, only=(), evidence=None):
    """(counts, outside, uninformative, missing_effects, unreviewed_near)."""
    only = set(only)
    rows = [r for r in rows if not only or r["program"] in only]
    c = dict(expected=len(rows), executed=0, exact=0, strict=0, near=0, defer=0, skip=0, fail=0, missing=0,
             uninformative=0)
    outside, uninformative, unreviewed = [], [], []
    for r in rows:
        b = r.get("bucket", r["verdict"])
        if b == "MISSING":
            c["missing"] += 1
            outside.append(r["program"])
            continue
        c["executed"] += 1
        if b == "EXACT":
            c["exact"] += 1
        elif b == "STRICT":
            c["strict"] += 1
        elif b == "UNINFORMATIVE":
            c["uninformative"] += 1
            uninformative.append(r["program"])
        elif b == "NEAR":
            c["near"] += 1
            outside.append(r["program"])
            if not has_policy(r):
                unreviewed.append(r["program"])
        else:
            c["fail"] += 1
            outside.append(r["program"])
    missing_effects = []
    if not only and evidence is not None:
        missing_effects = sorted(k for k, v in evidence.items() if not v)
        c["effects"] = len(evidence)
        c["effects_evidenced"] = len(evidence) - len(missing_effects)
    return c, outside, uninformative, missing_effects, unreviewed


def passes(counts, missing_effects, unreviewed, sweep_rc):
    """The exit rule of the module docstring."""
    return (counts["executed"] == counts["expected"]
            and counts["fail"] + counts["missing"] + counts["skip"] + counts["defer"] == 0
            and not unreviewed and not missing_effects and sweep_rc == 0)


def main(argv):
    ledger, sweep_rc, only = argv[1], int(argv[2]), argv[3:]
    with open(ledger) as f:
        rows = json.load(f)
    evidence = None
    effects_file = ledger + ".effects.json"
    if not only and os.path.exists(effects_file):
        with open(effects_file) as f:
            evidence = json.load(f)
    c, outside, uninformative, missing_effects, unreviewed = summarize(rows, only, evidence)
    print("PARITY-SUMMARY " + json.dumps(c, separators=(",", ":")))
    if outside:
        print("parity-summary: cases outside the published contract: " + " ".join(outside), file=sys.stderr)
    if uninformative:
        print("parity-summary: passes on uninformative goldens (not evidence): " + " ".join(uninformative),
              file=sys.stderr)
    if missing_effects:
        print("parity-summary: effects without informative exact or strict evidence: " + " ".join(missing_effects),
              file=sys.stderr)
    if sweep_rc != 0:
        print(f"parity-summary: the sweep exited {sweep_rc} (the minter or nm-render failed, or a case is outside the contract)",
              file=sys.stderr)
    return 0 if passes(c, missing_effects, unreviewed, sweep_rc) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
