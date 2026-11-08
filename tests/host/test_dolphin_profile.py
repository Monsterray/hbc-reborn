"""The Dolphin runs' profile cleanup (tests/dolphin/dolphin_smoke.py), without
Dolphin: each run's profile (a NAND copy, about 500 MB) goes when the
script ends, pass or fail, unless HBC_KEEP_PROFILE=1; profiles left by
killed runs go once they are a day old. Before 1.13.1 a failed run kept
its profile, and scripts other than dolphin_smoke.py kept every one:
72 GB of them piled up in the temp folder."""

import os
import pathlib
import sys
import tempfile
import time
import unittest
from unittest import mock

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import dolphin_smoke  # noqa: E402


class ProfileCleanupTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = pathlib.Path(tmp.name)

    def profile(self, name, age_s=0):
        path = self.tmp / (dolphin_smoke.PROFILE_PREFIX + name)
        (path / "Wii/title").mkdir(parents=True)
        (path / "Wii/title/nand.bin").write_bytes(b"x" * 100)
        then = time.time() - age_s
        os.utime(path, (then, then))
        return path

    def run_without_dolphin(self, profile):
        run = dolphin_smoke.Dolphin.__new__(dolphin_smoke.Dolphin)
        run.profile, run.proc, run.dolphin = profile, None, "Dolphin.exe"
        return run

    def test_close_deletes_the_profile(self):
        path = self.profile("a")
        run = self.run_without_dolphin(path)
        run.close()
        self.assertFalse(path.exists())
        run.close()  # again, as atexit does: nothing to do

    def test_keep_profile(self):
        path = self.profile("b")
        with mock.patch.object(dolphin_smoke, "KEEP_PROFILE", True):
            self.run_without_dolphin(path).close()
        self.assertTrue(path.exists())

    def test_sweep_deletes_only_old_profiles(self):
        old = self.profile("old", age_s=2 * 24 * 3600)
        new = self.profile("new", age_s=60)
        other = self.tmp / "hbc-meta-keep"
        other.mkdir()
        os.utime(other, (0, 0))
        with mock.patch.object(dolphin_smoke.tempfile, "gettempdir", lambda: str(self.tmp)):
            dolphin_smoke.sweep_stale_profiles()
        self.assertFalse(old.exists())
        self.assertTrue(new.exists())
        self.assertTrue(other.exists())

    def test_every_dolphin_script_uses_the_class(self):
        # a script that makes a temp profile must hand it to
        # dolphin_smoke.Dolphin, which deletes it at exit
        # (dolphin_mboard.py reuses one fixed profile in Documents)
        for script in root.glob("tests/dolphin/dolphin_*.py"):
            text = script.read_text(encoding="utf-8")
            if 'mkdtemp(prefix="hbc-dolphin-' in text and script.name != "dolphin_smoke.py":
                self.assertIn("dolphin_smoke.Dolphin", text, script.name)


if __name__ == "__main__":
    unittest.main()
