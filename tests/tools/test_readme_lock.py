import importlib.util
import os
import pathlib
import shutil
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).resolve().parents[2] / "tools" / "check-readme-lock.py"
spec = importlib.util.spec_from_file_location("check_readme_lock", TOOL)
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

NUMBERED = "It tracks Noisemaker 1.0.295, the version locked in `parity/reference.json`."
LOCK_ONLY = "It tracks the Noisemaker release locked in `parity/reference.json`."


class SentenceTests(unittest.TestCase):
    def test_the_sentence_may_span_a_line_break(self):
        text = ("runs [Noisemaker](https://noisemaker.app) programs.\n\n"
                "It tracks Noisemaker 1.0.295,\nthe version locked in "
                "`parity/reference.json`.\n\n## Requirements")
        self.assertEqual(
            check.sentence_of(text),
            "It tracks Noisemaker 1.0.295, the version locked in "
            "`parity/reference.json`.")

    def test_the_url_in_the_paragraph_does_not_break_the_match(self):
        self.assertIsNotNone(check.sentence_of(
            "It tracks the release locked in `parity/reference.json`."))

    def test_a_release_number_is_found_without_its_punctuation(self):
        self.assertEqual(check.numbers_in(NUMBERED), ["1.0.295"])

    def test_a_sentence_without_a_number_has_none(self):
        self.assertEqual(check.numbers_in(LOCK_ONLY), [])


class EvaluateTests(unittest.TestCase):
    def test_a_sentence_written_with_the_then_locked_release_passes(self):
        self.assertEqual(check.evaluate(NUMBERED, "1.0.295", []), [])

    def test_a_sentence_written_with_a_release_the_lock_did_not_hold_fails(self):
        errors = check.evaluate(NUMBERED, "1.0.312", [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("tracks 1.0.295 but the lock held 1.0.312", errors[0])

    def test_a_sentence_naming_the_lock_without_a_release_passes(self):
        self.assertEqual(check.evaluate(LOCK_ONLY, "9.9.9", []), [])

    def test_a_sentence_naming_neither_fails(self):
        errors = check.evaluate("It tracks the release the build fetches.",
                                "1.2.3", [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("names neither the locked release", errors[0])

    def test_a_missing_sentence_fails(self):
        errors = check.evaluate(None, "1.2.3", [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("no 'It tracks ...' sentence", errors[0])

    def test_a_lock_move_that_left_a_numbered_sentence_unchanged_fails(self):
        errors = check.evaluate(NUMBERED, "1.0.295", True)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("left the tracking sentence unchanged", errors[0])

    def test_a_lock_move_that_updated_the_sentence_passes(self):
        updated = "It tracks Noisemaker 1.0.312, the version locked in `parity/reference.json`."
        self.assertEqual(check.evaluate(updated, "1.0.312", False), [])

    def test_a_lock_move_that_left_a_lock_only_sentence_unchanged_passes(self):
        self.assertEqual(check.evaluate(LOCK_ONLY, "1.0.312", True), [])


class FakeGitTests(unittest.TestCase):
    """history_state against a fake git, per shallow-fetch scenario.

    Each test gets a temp root, a PATH whose "git" is a wrapper around
    tests/tools/test_readme_lock_gitfake.py, and a scenario name in
    NM_FAKE_SCENARIO. The check resolves "git" with shutil.which, which
    honors PATHEXT on Windows, so the wrapper is an extensionless sh script
    for POSIX plus a git.bat companion that Windows resolves; without a
    shell, CreateProcess appends only .exe to an extensionless name, so
    nothing would reach the fake otherwise. The wrappers invoke the test
    interpreter directly, so the fake needs nothing else on PATH.
    state_path lives inside the temp root, so state never leaks between
    tests. The wrapper is restored in tearDown.
    """

    FAKE = pathlib.Path(__file__).resolve().parent / "test_readme_lock_gitfake.py"

    def setUp(self):
        self.old_path = os.environ.get("PATH", "")
        self.old_scenario = os.environ.get("NM_FAKE_SCENARIO")
        self.bin = pathlib.Path(tempfile.mkdtemp(prefix="readme-lock-git-bin-"))
        self.root = tempfile.mkdtemp(prefix="readme-lock-git-root-")
        python = pathlib.Path(sys.executable).resolve()
        wrapper = self.bin / "git"
        wrapper.write_text(
            "#!/bin/sh\nexec '%s' '%s' \"$@\"\n" % (python, self.FAKE))
        wrapper.chmod(0o755)
        (self.bin / "git.bat").write_text(
            '@"%s" "%s" %%*\r\n' % (python, self.FAKE))
        os.environ["PATH"] = f"{self.bin}{os.pathsep}{self.old_path}"
        os.environ["NM_FAKE_SCENARIO"] = "not-shallow"

    def tearDown(self):
        os.environ["PATH"] = self.old_path
        if self.old_scenario is None:
            del os.environ["NM_FAKE_SCENARIO"]
        else:
            os.environ["NM_FAKE_SCENARIO"] = self.old_scenario
        shutil.rmtree(self.bin, ignore_errors=True)
        shutil.rmtree(self.root, ignore_errors=True)

    def run_scenario(self, scenario):
        os.environ["NM_FAKE_SCENARIO"] = scenario
        return check.history_state(self.root,
                                   "tools/check-readme-lock.py")

    def test_a_fetch_that_unshallows_does_not_fail_the_check(self):
        self.assertEqual(self.run_scenario("fetch-success-unshallows"),
                         (None, False))

    def test_a_fetch_that_leaves_the_clone_shallow_fails_closed(self):
        self.assertEqual(self.run_scenario("fetch-success-still-shallow"),
                         (None, True))

    def test_a_failed_shallow_fetch_fails_closed(self):
        self.assertEqual(self.run_scenario("fetch-fail"), (None, True))

    def test_a_clone_that_is_not_shallow_never_fetches(self):
        self.assertEqual(self.run_scenario("not-shallow"), (None, False))
        self.assertFalse(
            (pathlib.Path(self.root) / ".fake-git-state").exists())


if __name__ == "__main__":
    unittest.main()
