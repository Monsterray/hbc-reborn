"""The slot light library (sdk/slotled) on a PC: brightness, the effect
builders and the step engine, and the SYSCONF patcher.

Times are what slotled's timer counts on the Wii (4 ms periods), so a
level here is the level the light shows at that moment there."""

import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import test_cdblog  # noqa: E402

CC = test_cdblog.CC
E_ARG, E_FORMAT, E_TOO_LONG = -2, -5, -6


def sysconf(entries):
    """A SYSCONF as the Wii lays it out: "SCv0", the count, the offsets
    (one more than the count: the end), the entries, zero padding and
    "SCed" at the end of 0x4000 bytes."""
    body = b""
    first = 6 + 2 * (len(entries) + 1)
    offsets = []
    for typ, name, value in entries:
        offsets.append(first + len(body))
        head = bytes([typ << 5 | (len(name) - 1)]) + name
        if typ == 2:
            head += bytes([len(value) - 1])
        body += head + value
    offsets.append(first + len(body))
    data = b"SCv0" + struct.pack(">H", len(entries)) + struct.pack(">%dH" % len(offsets), *offsets)
    data += body
    return data + bytes(0x4000 - 4 - len(data)) + b"SCed"


def entries(idl):
    return [(3, b"IPL.LNG", b"\x01"), (2, b"IPL.NIK", b"\x00W\x00i\x00i" + bytes(16)),
            (2, b"IPL.IDL", idl), (5, b"IPL.CB", b"\0\0\0\7")]


@unittest.skipUnless(CC, "no C compiler")
class SlotledTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = pathlib.Path(cls.tmp.name)
        cls.exe = cls.dir / "slotled_host"
        subprocess.run([CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-D_CRT_SECURE_NO_WARNINGS", "-DSLOTLED_NO_OGC",
                        "-I", str(root / "sdk/slotled"), str(root / "tests/host/slotled_host/main.c"),
                        str(root / "sdk/slotled/slotled.c"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_host(self, *args):
        return subprocess.run([str(self.exe), *map(str, args)], check=True,
                              capture_output=True, text=True).stdout.split("\n")[:-1]

    def duty(self, level):
        return int(self.run_host("duty", level)[0])

    def steps(self, *args):
        out = [l for l in self.run_host("steps", *args) if not l.startswith("chars")]
        if out and out[0].startswith("error"):
            return int(out[0].split()[1])
        return [tuple(map(int, l.split())) for l in out]

    def at(self, kind, repeat, start, times):
        out = self.run_host("at", *kind, "--", repeat, start, *times)
        return [tuple(map(int, l.split())) for l in out if not l.startswith("chars")]

    # ---- Brightness -------------------------------------------------------

    def test_duty_is_level_squared(self):
        self.assertEqual(self.duty(0), 0)
        self.assertEqual(self.duty(255), 4000)
        # Half the level is a quarter of the on-time.
        self.assertAlmostEqual(self.duty(128), 4000 * 128 * 128 / 65025, delta=1)

    def test_duty_never_too_short_to_see_or_to_time(self):
        self.assertEqual(self.duty(1), 20)
        # Within 100 us of the period's end the off-edge alarm would race
        # the next period: full on instead.
        self.assertEqual(self.duty(254), 4000)
        duties = [self.duty(level) for level in range(256)]
        self.assertEqual(duties, sorted(duties))

    # ---- Builders ---------------------------------------------------------

    def test_blink(self):
        self.assertEqual(self.steps("blink", 200, 100, 300), [(200, 0, 100), (0, 0, 300)])
        self.assertEqual(self.steps("blink", 200, 100, 0), [(200, 0, 100)])
        self.assertEqual(self.steps("blink", 256, 100, 100), E_ARG)
        self.assertEqual(self.steps("blink", 255, 0, 100), E_ARG)

    def test_pulse_fades_up_and_down(self):
        self.assertEqual(self.steps("pulse", 10, 250, 2001), [(250, 1, 1000), (10, 1, 1001)])

    def test_heartbeat(self):
        self.assertEqual(self.steps("heart", 255, 1000),
                         [(255, 0, 80), (0, 1, 140), (255, 0, 80), (0, 1, 300), (0, 0, 400)])
        self.assertEqual(len(self.steps("heart", 255, 600)), 4)
        self.assertEqual(self.steps("heart", 255, 599), E_ARG)

    def test_morse_timing(self):
        # SOS: dot 1, dash 3, 1 between marks, 3 between letters, 7 after.
        u = 50
        marks = [1, 1, 1, 3, 3, 3, 1, 1, 1]
        gaps = [1, 1, 3, 1, 1, 3, 1, 1, 7]
        want = []
        for m, g in zip(marks, gaps):
            want += [(255, 0, m * u), (0, 0, g * u)]
        self.assertEqual(self.steps("morse", "sos", 255, u), want)

    def test_morse_words_and_unknown_characters(self):
        out = self.run_host("steps", "morse", "E  E#E", 9, 10)
        self.assertEqual(out[0], "chars 6")
        steps = [tuple(map(int, l.split())) for l in out[1:]]
        # Spaces and '#' are word gaps; several in a row are still one.
        self.assertEqual(steps, [(9, 0, 10), (0, 0, 70), (9, 0, 10), (0, 0, 70),
                                 (9, 0, 10), (0, 0, 70)])

    def test_morse_too_long_or_too_slow(self):
        self.assertEqual(self.steps("morse", "0" * 60, 255, 10), E_TOO_LONG)
        self.assertEqual(self.steps("morse", "E", 255, 9363), E_ARG)   # 7 units over 65535 ms

    # ---- The step engine --------------------------------------------------

    def test_fade_from_the_level_shown_at_start(self):
        # A pulse starts from whatever was showing, not from `lo`.
        got = self.at(("pulse", 0, 200, 1000), 0, 100, [0, 250, 500, 750, 1000, 1500])
        self.assertEqual(got, [(100, 0), (150, 0), (200, 0), (100, 0), (0, 0), (200, 0)])

    def test_repeat_ends_on_the_last_level(self):
        got = self.at(("blink", 255, 100, 100), 3, 0, [0, 99, 100, 599, 600, 10 ** 12])
        self.assertEqual(got, [(255, 0), (255, 0), (0, 0), (0, 0), (0, 1), (0, 1)])

    def test_forever(self):
        got = self.at(("blink", 255, 100, 100), 0, 0, [200 * 10 ** 10, 200 * 10 ** 10 + 100])
        self.assertEqual(got, [(255, 0), (0, 0)])

    def test_zero_length_steps(self):
        self.assertEqual(self.at(("raw", "10,0,0", "20,0,0"), 1, 5, [0, 1]), [(20, 1), (20, 1)])
        self.assertEqual(self.at(("raw", "10,0,0", "20,1,10"), 1, 5, [0, 5]), [(10, 0), (15, 0)])

    # ---- SYSCONF ----------------------------------------------------------

    def patch(self, data, set_to):
        path = self.dir / "SYSCONF"
        path.write_bytes(data)
        old = int(self.run_host("sysconf", path, set_to)[0])
        return old, path.read_bytes()

    def test_reads_and_sets_only_that_byte(self):
        for idl, at in ((b"\x01\x02", 1), (b"\x01\x00\x02", 2)):   # libogc/Dolphin, WiiBrew
            data = sysconf(entries(idl))
            old, new = self.patch(data, 1)
            self.assertEqual(old, 2)
            want = bytearray(data)
            want[data.index(b"IPL.IDL") + 7 + 1 + at] = 1
            self.assertEqual(new, bytes(want))

    def test_read_only(self):
        data = sysconf(entries(b"\x00\x01"))
        self.assertEqual(self.patch(data, -1), (1, data))

    def test_refuses_what_the_wii_would_not_write(self):
        good = sysconf(entries(b"\x00\x01"))
        for bad in (b"SCv1" + good[4:],                         # magic
                    sysconf(entries(b"\x00\x01")[:2]),          # no IPL.IDL
                    sysconf(entries(b"\x00\x03")),              # value out of range
                    sysconf(entries(b"\x00")),                  # one byte
                    sysconf([(3, b"IPL.IDL", b"\x01")]),        # not an array
                    good[:40]):                                 # offsets past the end
            self.assertEqual(self.patch(bad, 2), (E_FORMAT, bad))


if __name__ == "__main__":
    unittest.main()
