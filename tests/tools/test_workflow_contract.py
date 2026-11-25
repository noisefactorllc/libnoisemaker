"""The 2.x branch's workflow contract.

The test jobs run on every push to ``2.x``, and every release, notice or
dispatch job runs only for ``main``. A job runs only for ``main`` when its
condition names ``refs/heads/main`` or a ``workflow_run`` head branch of
``main``, or when every trigger of its workflow is a ``schedule`` (which runs
only from the default branch) or a ``push`` or ``workflow_run`` filtered to
``main`` alone.
"""

import pathlib
import re
import unittest

WORKFLOWS = pathlib.Path(__file__).resolve().parents[2] / ".github" / "workflows"

# Workflows whose jobs are test jobs, and must run on every 2.x push.
TEST_WORKFLOWS = ["ci.yml"]
# Workflows whose jobs release, notify or dispatch, and must run only for main.
RELEASE_WORKFLOWS = ["export-kit.yml", "release.yml"]

MAIN_ONLY_CONDITIONS = (
    re.compile(r"github\.ref\s*==\s*'refs/heads/main'"),
    re.compile(r"github\.event\.workflow_run\.head_branch\s*==\s*'main'"),
)


def _lines(name):
    lines = []
    for raw in (WORKFLOWS / name).read_text(encoding="utf-8").splitlines():
        raw = re.sub(r"\s+#[^'\"]*$", "", raw)
        if raw.strip() and not raw.lstrip().startswith("#"):
            lines.append((len(raw) - len(raw.lstrip(" ")), raw.strip()))
    return lines


def _children(lines, start, indent):
    """Return (key, value, index) for each key one level below lines[start]."""
    found = []
    child = None
    for index in range(start + 1, len(lines)):
        depth, text = lines[index]
        if depth <= indent:
            break
        if child is None:
            child = depth
        if depth == child and not text.startswith("- "):
            key, _, value = text.partition(":")
            found.append((key.strip().strip("'\""), value.strip(), index))
    return found


def _block(lines, index):
    """The scalar at lines[index]: inline, or a folded or literal block."""
    depth, text = lines[index]
    value = text.partition(":")[2].strip()
    if value[:1] not in (">", "|"):
        return value
    parts = []
    for later in range(index + 1, len(lines)):
        if lines[later][0] <= depth:
            break
        parts.append(lines[later][1])
    return " ".join(parts)


def _top(lines, key):
    for index, (depth, text) in enumerate(lines):
        if depth == 0 and text.partition(":")[0].strip().strip("'\"") == key:
            return index
    raise AssertionError(f"no top-level {key!r}")


def triggers(name):
    """Map each trigger to its branch filter (None when it has none)."""
    lines = _lines(name)
    start = _top(lines, "on")
    result = {}
    for event, value, index in _children(lines, start, 0):
        branches = None
        for key, inline, child in _children(lines, index, lines[index][0]):
            if key != "branches":
                continue
            if inline.startswith("["):
                branches = [b.strip().strip("'\"") for b in inline.strip("[]").split(",") if b.strip()]
            else:
                branches = []
                for later in range(child + 1, len(lines)):
                    depth, text = lines[later]
                    if depth <= lines[child][0] and not text.startswith("- "):
                        break
                    if text.startswith("- "):
                        branches.append(text[2:].strip().strip("'\""))
        result[event] = branches
    return result


def jobs(name):
    """Map each job to its condition ('' when it has none)."""
    lines = _lines(name)
    start = _top(lines, "jobs")
    result = {}
    for job, _, index in _children(lines, start, 0):
        condition = ""
        for key, _, child in _children(lines, index, lines[index][0]):
            if key == "if":
                condition = _block(lines, child)
        result[job] = condition
    return result


def runs_only_for_main(name, condition):
    # A condition with an alternative can hold outside main, so only the
    # triggers can make such a job main-only.
    if "||" not in condition and any(pattern.search(condition) for pattern in MAIN_ONLY_CONDITIONS):
        return True
    for event, branches in triggers(name).items():
        if event == "schedule":
            continue
        if event in ("push", "workflow_run") and branches == ["main"]:
            continue
        return False
    return True


class WorkflowContractTests(unittest.TestCase):
    def test_the_test_jobs_run_on_every_2x_push(self):
        for name in TEST_WORKFLOWS:
            with self.subTest(workflow=name):
                found = triggers(name)
                # A push trigger with no branch filter runs on every branch.
                self.assertTrue(
                    "push" in found and (found["push"] is None or "2.x" in found["push"]),
                    f"{name} does not run on a 2.x push")
                for job, condition in jobs(name).items():
                    self.assertTrue(
                        "refs/heads/main" not in condition or "refs/heads/2.x" in condition,
                        f"{name} job {job} runs only for main: {condition}")

    def test_every_release_notice_or_dispatch_job_runs_only_for_main(self):
        for name in RELEASE_WORKFLOWS:
            with self.subTest(workflow=name):
                found = jobs(name)
                self.assertTrue(found, f"{name} has no jobs")
                for job, condition in found.items():
                    self.assertTrue(
                        runs_only_for_main(name, condition),
                        f"{name} job {job} can run outside main")

    def test_the_contract_reader_finds_every_workflow_trigger_and_job(self):
        for name in TEST_WORKFLOWS + RELEASE_WORKFLOWS:
            with self.subTest(workflow=name):
                self.assertTrue(triggers(name), f"no triggers read from {name}")
                self.assertTrue(jobs(name), f"no jobs read from {name}")
        self.assertIn("dispatch", jobs("export-kit.yml"))
        self.assertIn("build", jobs("release.yml"))
        self.assertIn("publish", jobs("release.yml"))
        self.assertIn("push", triggers("ci.yml"))
        self.assertIn("gates", jobs("ci.yml"))


if __name__ == "__main__":
    unittest.main()
