import importlib.util
import pathlib
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


if __name__ == "__main__":
    unittest.main()
