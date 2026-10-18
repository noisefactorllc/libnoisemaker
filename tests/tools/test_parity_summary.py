"""parity/summarize.py: the PARITY-SUMMARY line and scripts/parity-summary's
exit status, on ledgers built here. A near case within its reviewed per-case
policy passes; a near case without one, a failure, a missing case and an
unevidenced effect do not."""

import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("summarize", ROOT / "parity" / "summarize.py")
summarize = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(summarize)

POLICY = {"tolerance": 42.001, "ssim_min": 0.99933}


def exact(name):
    return {"program": name, "verdict": "EXACT", "bucket": "EXACT", "policy": {"tolerance": 2.001, "ssim_min": 0.98}}


def near(name, mechanism="reviewed side by side: the same"):
    row = {"program": name, "verdict": "NEAR", "bucket": "NEAR", "policy": POLICY}
    if mechanism is not None:
        row["mechanism"] = mechanism
    return row


def run(rows, sweep_rc=0, only=(), evidence=None):
    """(exit status, PARITY-SUMMARY counts) of summarize.main on a ledger file."""
    with tempfile.TemporaryDirectory() as tmp:
        ledger = pathlib.Path(tmp) / "ledger.json"
        ledger.write_text(json.dumps(rows))
        if evidence is not None:
            (pathlib.Path(tmp) / "ledger.json.effects.json").write_text(json.dumps(evidence))
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = summarize.main(["summarize.py", str(ledger), str(sweep_rc), *only])
    line = out.getvalue().strip()
    assert line.startswith("PARITY-SUMMARY "), line
    return status, json.loads(line[len("PARITY-SUMMARY "):])


class NearPolicyTests(unittest.TestCase):
    def test_reviewed_near_cases_pass(self):
        status, counts = run([exact("a"), near("b"), near("c")])
        self.assertEqual(status, 0)
        self.assertEqual((counts["exact"], counts["near"], counts["fail"]), (1, 2, 0))

    def test_reviewed_near_cases_pass_a_full_run(self):
        status, counts = run([exact("a"), near("b")], evidence={"filter.a": ["a"], "filter.b": ["b"]})
        self.assertEqual(status, 0)
        self.assertEqual((counts["effects"], counts["effects_evidenced"]), (2, 2))

    def test_a_near_case_without_a_policy_fails(self):
        status, counts = run([exact("a"), near("b", mechanism=None)])
        self.assertEqual(status, 1)
        self.assertEqual(counts["near"], 1)

    def test_a_case_outside_its_bound_fails(self):
        # The sweep grades a case beyond its policy's bound as FAIL.
        status, counts = run([exact("a"), {"program": "b", "verdict": "FAIL", "bucket": "FAIL", "policy": POLICY}])
        self.assertEqual(status, 1)
        self.assertEqual(counts["fail"], 1)

    def test_a_missing_case_fails(self):
        status, counts = run([near("a"), {"program": "b", "verdict": "MISSING", "bucket": "MISSING"}])
        self.assertEqual(status, 1)
        self.assertEqual((counts["executed"], counts["missing"]), (1, 1))

    def test_a_failed_sweep_fails(self):
        status, _ = run([exact("a"), near("b")], sweep_rc=1)
        self.assertEqual(status, 1)

    def test_an_unevidenced_effect_fails(self):
        status, counts = run([exact("a"), near("b")], evidence={"filter.a": ["a"], "filter.b": []})
        self.assertEqual(status, 1)
        self.assertEqual(counts["effects_evidenced"], 1)

    def test_case_ids_limit_the_count(self):
        status, counts = run([exact("a"), near("b"), {"program": "c", "verdict": "FAIL", "bucket": "FAIL"}], only=("a", "b"))
        self.assertEqual(status, 0)
        self.assertEqual(counts["expected"], 2)

    def test_uninformative_passes_count_apart(self):
        status, counts = run([{"program": "a", "verdict": "EXACT", "bucket": "UNINFORMATIVE"}, near("b")])
        self.assertEqual(status, 0)
        self.assertEqual((counts["exact"], counts["uninformative"]), (0, 1))


if __name__ == "__main__":
    unittest.main()
