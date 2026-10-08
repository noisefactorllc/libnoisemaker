import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import fetch_dawn  # noqa: E402

SHA = re.compile(r"^[0-9a-f]{40}$")


class LockTests(unittest.TestCase):
    def test_lock_pins_the_chromium_153_dawn(self):
        lock = fetch_dawn.load_lock(ROOT / "third_party" / "dawn.lock.json")
        self.assertEqual(lock["dawn"]["commit"], "50c9f7b4ee3fef0bdc9166056098271ea85ef9fc")

    def test_every_dependency_has_path_repository_and_full_commit(self):
        lock = fetch_dawn.load_lock(ROOT / "third_party" / "dawn.lock.json")
        self.assertTrue(lock["dependencies"])
        for dep in lock["dependencies"]:
            self.assertTrue(dep["path"].startswith("third_party/"), dep)
            self.assertTrue(dep["repository"].startswith("https://"), dep)
            self.assertRegex(dep["commit"], SHA)


class GitlinkTests(unittest.TestCase):
    def test_parse_gitlink_reads_the_commit(self):
        out = "160000 commit dd67f5ca84f65ebb88ac0ea0fe2c1d58663e519f\tthird_party/abseil-cpp\n"
        self.assertEqual(
            fetch_dawn.parse_gitlink(out, "third_party/abseil-cpp"),
            "dd67f5ca84f65ebb88ac0ea0fe2c1d58663e519f",
        )

    def test_parse_gitlink_rejects_a_tree(self):
        out = "040000 tree 0123456789012345678901234567890123456789\tthird_party/abseil-cpp\n"
        with self.assertRaises(SystemExit):
            fetch_dawn.parse_gitlink(out, "third_party/abseil-cpp")

    def test_gitlink_reads_a_real_repository(self):
        sha = "1" * 40
        with tempfile.TemporaryDirectory() as tmp:
            repo = pathlib.Path(tmp) / "r"
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            subprocess.run(["git", "-C", str(repo), "update-index", "--add", "--cacheinfo",
                            f"160000,{sha},third_party/x"], check=True)
            subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.com",
                            "commit", "-q", "-m", "x"], check=True)
            self.assertEqual(fetch_dawn.gitlink(repo / ".git", "HEAD", "third_party/x"), sha)


if __name__ == "__main__":
    unittest.main()
