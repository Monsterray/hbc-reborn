"""HBC's Message Board play log writer (channel/channelapp/source/cdblog.c)
against the reference the Wii Menu accepted in Dolphin
(tools/msgboard/cdb_log.py): the same sessions, on the same image, must give
the same bytes. The image is built here, as the Wii Menu leaves an empty
cdb.vff (docs/messageboard.md); a real one stays on the Wii."""

import importlib.util
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
spec = importlib.util.spec_from_file_location("cdb_log", root / "tools/msgboard/cdb_log.py")
ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ref)

WIIID = bytes.fromhex("0123456789abcdef")


def empty_vff():
    img = bytearray(0x01400000)
    img[0:16] = bytes.fromhex("564646 20 feff 0100 01400000 00200000".replace(" ", ""))
    for fat in (ref.FAT1, ref.FAT2):
        img[fat:fat + 6] = bytes.fromhex("f0ffffffffff")  # media, reserved, cluster 2: cdb.conf
    lfn = bytearray(32)
    name = "cdb.conf".encode("utf-16-le") + b"\0\0" + b"\xff" * 8
    lfn[0], lfn[11], lfn[13] = 0x41, 0x0f, 0x83
    lfn[1:11], lfn[14:26], lfn[28:32] = name[0:10], name[10:22], name[22:26]
    img[ref.ROOT:ref.ROOT + 32] = lfn
    conf = bytearray(32)
    conf[0:11] = b"CDB~1   CON"
    conf[13] = 1
    struct.pack_into("<HI", conf, 26, 2, 4)
    img[ref.ROOT + 32:ref.ROOT + 64] = conf
    return img


SESSIONS = [  # name, id, start, end, now
    ("Homebrew Channel", "OHBC", "2026-10-01T09:00:00", "2026-10-01T09:12:30", "2026-10-01T09:12:31"),
    ("Wii64", "HBWII6", "2026-10-01T09:12:40", "2026-10-01T10:05:00", "2026-10-01T10:05:02"),
    ("Homebrew Channel", "OHBC", "2026-10-01T10:05:02", "2026-10-01T10:06:10", "2026-10-01T10:06:11"),
] + [(f"App number {i:02d} with a long name", f"HBA{i:03d}", "2026-10-01T11:00:00",
      f"2026-10-01T11:{i:02d}:00", f"2026-10-01T11:{i:02d}:01") for i in range(1, 12)] + [
    ("Homebrew Channel", "OHBC", "2026-10-02T08:00:00", "2026-10-02T08:30:00", "2026-10-02T08:30:01"),
]


def tm(t):
    return time.strptime(t, "%Y-%m-%dT%H:%M:%S")


@unittest.skipUnless(CC, "no C compiler")
class CdbLogTest(unittest.TestCase):
    def test_same_bytes_as_the_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            exe = tmp / "cdblog_host"
            subprocess.run([CC, "-O2", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                            "-I", str(root / "channel/channelapp/source"),
                            str(root / "tests/cdblog_host/main.c"),
                            str(root / "channel/channelapp/source/cdblog.c"), "-o", str(exe)],
                           check=True)
            img_c = tmp / "c.vff"
            img_c.write_bytes(empty_vff())
            img_py = empty_vff()
            results = []
            for name, tid, start, end, now in SESSIONS:
                out = subprocess.run([str(exe), str(img_c), name, tid, start, end, now, WIIID.hex()],
                                     check=True, capture_output=True, text=True).stdout.strip()
                try:
                    py = ref.log(img_py, name, tid, tm(start), tm(end), tm(now), WIIID)
                except SystemExit as exc:
                    py = str(exc)
                results.append((name, int(out), py))
                self.assertEqual(bytes(img_py), img_c.read_bytes(), f"after {name} ({out}, {py})")
            codes = [r[1] for r in results]
            # Created, added, combined (HBC twice on one day), then apps 1-10
            # bring the day to 12 titles, so app 11 is refused; then a new day.
            self.assertEqual(codes[:3], [2, 0, 1])
            self.assertEqual(codes[3:13], [0] * 10)
            self.assertEqual(codes[13], -3)
            self.assertEqual(codes[14], 2)

            # The day's message, as the Wii Menu would show it.
            data = None
            for c in range(2, 200):
                off = ref.DATA + (c - 2) * ref.CL
                if img_py[off:off + 8] == b"CDBFILE\x02" and struct.unpack_from(">I", img_py, off + 0x70)[0] == 1:
                    v = ref.Vff(img_py)
                    data = img_py[off:off + 0x2000]
                    break
            self.assertIsNotNone(data)
            text = data[0x548:].decode("utf-16-be", "replace")
            self.assertIn("Homebrew Channel\n     00:13", text)  # 12.5 + 1.1 min, combined
            self.assertIn("Wii64\n     00:52", text)
            self.assertEqual(struct.unpack_from(">I", data, 0x74)[0], 12)


if __name__ == "__main__":
    unittest.main()
