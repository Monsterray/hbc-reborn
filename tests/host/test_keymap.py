"""Build channel/channelapp/source/keymap.c on the host and check how a USB
keyboard's keys (by HID usage code) read as a Wii Remote's buttons: each key
of the table in keymap.h gives its button alone, a chord gives the union,
and every other key gives nothing."""

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
HOST = root / "tests/host/keymap_host"

B = {"2": 0x0001, "1": 0x0002, "B": 0x0004, "A": 0x0008, "MINUS": 0x0010, "HOME": 0x0080,
     "LEFT": 0x0100, "RIGHT": 0x0200, "DOWN": 0x0400, "UP": 0x0800, "PLUS": 0x1000}
# USB HID usage codes, keyboard page: each key and the button it gives.
KEYS = {
    0x52: "UP", 0x51: "DOWN", 0x50: "LEFT", 0x4f: "RIGHT",
    0x28: "A", 0x58: "A", 0x2c: "A",              # Enter, keypad Enter, Space
    0x2a: "B",                                    # Backspace
    0x1b: "1", 0x1e: "1", 0x1c: "2", 0x1f: "2",  # X, 1, Y, 2
    0x2d: "MINUS", 0x56: "MINUS", 0x4c: "MINUS",  # -, keypad -, Delete
    0x2e: "PLUS", 0x57: "PLUS", 0x49: "PLUS",     # =, keypad +, Insert
    0x29: "HOME", 0x4a: "HOME",                   # Esc, Home
}


@unittest.skipUnless(CC, "no C compiler")
class KeymapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = pathlib.Path(cls.tmp.name, "keymap_test.exe" if os.name == "nt" else "keymap_test")
        subprocess.run([CC, "-O2", "-Wall", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        "-I", str(HOST / "include"), str(HOST / "test.c"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def buttons(self, *usages):
        out = subprocess.run([str(self.exe)] + [f"{u:x}" for u in usages],
                             check=True, capture_output=True, text=True).stdout
        return int(out, 16)

    def test_no_keys_no_buttons(self):
        self.assertEqual(self.buttons(), 0)

    def test_each_key_gives_its_button(self):
        for usage, name in KEYS.items():
            with self.subTest(usage=hex(usage)):
                self.assertEqual(self.buttons(usage), B[name])

    def test_keys_with_no_button_give_nothing(self):
        for usage in range(256):
            if usage not in KEYS:
                with self.subTest(usage=hex(usage)):
                    self.assertEqual(self.buttons(usage), 0)

    def test_chord_is_the_union(self):
        self.assertEqual(self.buttons(0x52, 0x4f, 0x28), B["UP"] | B["RIGHT"] | B["A"])
        # Two keys for one button are still that button.
        self.assertEqual(self.buttons(0x28, 0x2c, 0x58), B["A"])

    def test_print_screen_is_not_a_button(self):
        self.assertEqual(self.buttons(0x46), 0)

    def test_values_match_libogc(self):
        dkp = os.environ.get("DEVKITPRO")
        header = pathlib.Path(dkp, "libogc/include/wiiuse/wpad.h") if dkp else None
        if not header or not header.is_file():
            self.skipTest("no libogc header (set DEVKITPRO)")
        real = dict(re.findall(r"#define WPAD_BUTTON_(\w+)\s+(0x[0-9a-fA-F]+)", header.read_text()))
        for name, value in B.items():
            self.assertEqual(int(real[name], 16), value, name)


if __name__ == "__main__":
    unittest.main()
