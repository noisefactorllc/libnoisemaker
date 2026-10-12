import json
import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "build_dawn.py"


def dry_run(*args):
    result = subprocess.run(
        [sys.executable, str(TOOL), "--dry-run", "--prefix", "X", *args],
        capture_output=True, text=True, check=True,
    )
    return result.stdout.splitlines()


class DryRunTests(unittest.TestCase):
    def test_configure_uses_the_shared_options_with_install_on(self):
        configure = dry_run()[0]
        self.assertIn(" -C ", configure)
        self.assertIn(str(pathlib.Path("cmake") / "DawnOptions.cmake"), configure)
        self.assertIn("-DDAWN_ENABLE_INSTALL=ON", configure)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", configure)

    def test_builds_and_installs_webgpu_dawn_in_release(self):
        lines = dry_run()
        self.assertTrue(any("--target webgpu_dawn" in line and "--config Release" in line for line in lines))
        self.assertTrue(any(" --install " in line for line in lines))

    def test_stamps_the_prefix_with_the_lock_commit(self):
        lock = json.loads((ROOT / "third_party" / "dawn.lock.json").read_text())
        self.assertEqual(dry_run()[-1], f"stamp X/.nm-commit {lock['dawn']['commit']}")

    def test_passes_the_generator_and_architectures(self):
        configure = dry_run("--generator", "Ninja", "--arch", "ARM64", "--osx-arch", "x86_64",
                            "--osx-deployment-target", "13.0")[0]
        self.assertIn("-G Ninja", configure)
        self.assertIn("-A ARM64", configure)
        self.assertIn("-DCMAKE_OSX_ARCHITECTURES=x86_64", configure)
        self.assertIn("-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0", configure)


if __name__ == "__main__":
    unittest.main()
