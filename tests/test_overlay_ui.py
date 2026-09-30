"""Build tests/overlay_preview/preview.c with the host compiler and check that the
overlay's scripted walk through every menu reaches each page and closes."""

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


@unittest.skipUnless(CC, "no C compiler")
class OverlayUITest(unittest.TestCase):
    def test_scripted_walk(self):
        agent = root / "sdk/hbc_agent"
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp, "preview.exe" if os.name == "nt" else "preview")
            subprocess.run([CC, "-O1", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                            "-I", str(agent), str(root / "tests/overlay_preview/preview.c"),
                            str(agent / "ov_ui.c"), str(agent / "ov_draw.c"), "-o", str(exe)],
                           check=True, env=dict(os.environ, TMP=tmp, TEMP=tmp, TMPDIR=tmp))
            out = subprocess.run([str(exe), tmp], check=True, capture_output=True,
                                 text=True).stdout
        states = dict(re.findall(r"^(\d\d-[\w-]+): (menu .*)$", out, re.M))
        menu = lambda name: int(re.search(r"menu (\d+)", states[name])[1])
        self.assertEqual(menu("02-exit"), 2)          # Exit grew the strip
        self.assertEqual(menu("04-dev-actions"), 1)
        self.assertEqual(menu("06-dev-log"), 1)
        self.assertEqual(menu("07-wiimote"), 3)
        self.assertEqual(menu("12-settings"), 3)
        self.assertIn("act 13 0", out)                # Find on remote 1
        self.assertIn("act 16 0", out)                # Test started
        self.assertIn("act 18 0", out)                # Calibrate started
        self.assertIn("act 6 0", out)                 # Shot
        self.assertIn("hover 104", out)               # the pointer over WiiMote
        self.assertEqual(menu("16-pointer-press"), 3)  # A with it opened WiiMote
        self.assertIn("closed after HOME: yes", out)


if __name__ == "__main__":
    unittest.main()
