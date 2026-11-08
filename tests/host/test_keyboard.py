"""Build channel/channelapp/source/keyboard.c on the host against a fake USB
(tests/host/keyboard_host) and run its checks: a rollover report keeps the keys
held and is not replayed, a keyboard whose open failed is tried again, a
device that is not a keyboard is opened once, an unplugged device's ID is
forgotten, and a read IOS never gave back after shutdown is reported."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
HOST = root / "tests/host/keyboard_host"


@unittest.skipUnless(CC, "no C compiler")
class KeyboardTest(unittest.TestCase):
    def test_keyboard(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp, "keyboard_test.exe" if os.name == "nt" else "keyboard_test")
            subprocess.run([CC, "-O2", "-Wall", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                            "-I", str(HOST / "include"), "-I", str(root / "tests/host/keymap_host/include"),
                            str(HOST / "test.c"), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], capture_output=True, text=True)
        lines = out.stdout.splitlines()
        self.assertTrue(lines, out.stderr)
        for line in lines:
            with self.subTest(check=line.split(":")[0]):
                self.assertTrue(line.startswith("ok "), line)
        self.assertEqual(out.returncode, 0, out.stdout)


if __name__ == "__main__":
    unittest.main()
