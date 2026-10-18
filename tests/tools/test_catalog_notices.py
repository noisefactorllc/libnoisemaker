import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "catalog"))

import check_notices  # noqa: E402

NOTICES = ROOT / "src" / "core" / "catalog" / "catalog-NOTICES.txt"
SEPARATOR = "\n" + check_notices.SEPARATOR + "\n"


def notices_text(names, extra=""):
    entries = [f"{name}\nSource: x\n\nPermission is hereby granted\n" for name in names]
    return SEPARATOR.join(["preamble\n"] + entries) + extra


class NoticesFileTests(unittest.TestCase):
    def test_every_work_has_one_entry_with_its_notice(self):
        entries = check_notices.read_entries(NOTICES.read_text(encoding="utf-8"))
        self.assertEqual(set(entries), set(check_notices.WORKS))
        for name, entry in entries.items():
            self.assertTrue("Permission is hereby granted" in entry or "public domain" in entry, name)


class CheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.catalog = pathlib.Path(self.tmp.name) / "catalog"
        self.notices = pathlib.Path(self.tmp.name) / "catalog-NOTICES.txt"
        self.notices.write_text(notices_text(check_notices.WORKS), encoding="utf-8")

    def shader(self, rel, text):
        path = self.catalog / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def check(self):
        return check_notices.check(self.catalog, self.notices)

    def test_a_notice_of_a_known_work_passes(self):
        self.shader("glsl/synth/noise/noise.glsl", "// PCG PRNG - MIT License\nuvec3 pcg(uvec3 v) {}\n")
        self.shader("wgsl/synth/noise/noise.wgsl",
                    "// 3d cell noise - MIT license\n// Cellular noise in 3D.\n// Copyright (c) Stefan Gustavson\n")
        self.assertEqual(self.check(), [])

    def test_a_notice_of_an_unknown_work_fails(self):
        self.shader("glsl/synth/noise/noise.glsl", "// PCG PRNG - MIT License\nfloat a;\n")
        self.shader("glsl/filter/foo/foo.glsl", "float b;\n// Foo noise - MIT License\n// https://example.com/foo\nfloat c;\n")
        errors = self.check()
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("glsl/filter/foo/foo.glsl:2", errors[0])

    def test_a_work_named_outside_the_comment_block_does_not_cover_a_notice(self):
        self.shader("glsl/filter/foo/foo.glsl", "// PCG PRNG\nfloat a;\nfloat b;\n// Bar - BSD\nfloat c;\n")
        errors = self.check()
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("glsl/filter/foo/foo.glsl:4", errors[0])

    def test_a_missing_entry_and_an_unknown_entry_fail(self):
        names = [name for name in check_notices.WORKS if name != "Wagner"] + ["Unknown work"]
        self.notices.write_text(notices_text(names), encoding="utf-8")
        self.catalog.mkdir()
        errors = self.check()
        self.assertTrue(any("no entry for Wagner" in error for error in errors), errors)
        self.assertTrue(any("'Unknown work' is not a work" in error for error in errors), errors)

    def test_a_cited_source_line_must_hold_a_notice(self):
        self.shader("glsl/filter/tunnel/tunnel.glsl", "/*\n * Based on Inigo Quilez's tunnel shader\n * MIT License\n */\n")
        good = "  shaders/effects/filter/tunnel/glsl/tunnel.glsl:3\n"
        bad = "  shaders/effects/filter/tunnel/glsl/tunnel.glsl:2\n"
        names = list(check_notices.WORKS)
        self.notices.write_text(notices_text(names[:-1]) + SEPARATOR + f"{names[-1]}\npublic domain\n{good}",
                                encoding="utf-8")
        self.assertEqual(self.check(), [])
        self.notices.write_text(notices_text(names[:-1]) + SEPARATOR + f"{names[-1]}\npublic domain\n{bad}",
                                encoding="utf-8")
        errors = self.check()
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("tunnel.glsl:2, which holds no license notice", errors[0])


if __name__ == "__main__":
    unittest.main()
