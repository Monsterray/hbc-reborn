"""sdk/mii's drawing (miidraw.c) on the PC, through tests/miidraw_host:

- a synthetic RFL_Res.dat built here (no Nintendo data), with RFL's archive
  sizes: a default Mii builds, every part it should have has triangles,
  each expression's face is composited where RFL puts the eyes and mouth,
  and a damaged resource is refused;
- with MII_REAL_RES (your own RFL_Res.dat, from HOME > HBC > Save Mii faces)
  and MII_REAL_DB (your RFL_DB.dat): every Mii in the database builds.
"""

import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import test_cdblog  # noqa: E402

CC = test_cdblog.CC

# RFL's archives in order, with the file counts a Wii's copy has.
ARCHIVES = [("beard", 4), ("eye", 50), ("eyebrow", 24), ("faceline", 8), ("facetex", 12),
            ("forehead", 72), ("glass", 1), ("glasstex", 10), ("hair", 72), ("mask", 8),
            ("mole", 2), ("mouth", 25), ("mustache", 4), ("nose", 12), ("nline", 12),
            ("nlinetex", 12), ("cap", 72), ("captex", 72)]
TAGS = {"beard": b"berd", "faceline": b"face", "forehead": b"frhd", "glass": b"glas",
        "hair": b"hair", "mask": b"mask", "nose": b"nose", "nline": b"nsln", "cap": b"cap_"}
NO_TEXCOORDS = {"nose", "forehead", "hair", "beard"}


def shape(name, empty=False):
    out = TAGS[name]
    if name == "faceline":
        out += struct.pack(">9f", 0, 24.5, 26, 0, 0, 0, 0, 0, 0)   # nose, beard, hair anchors
    if empty:
        return out + struct.pack(">H", 0)
    out += struct.pack(">H", 3) + struct.pack(">9h", 0, 0, 0, 256 * 10, 0, 0, 0, 256 * 10, 0)
    out += struct.pack(">H", 1) + struct.pack(">3h", 0, 0, 16384)
    tex = name not in NO_TEXCOORDS
    if tex:
        out += struct.pack(">H", 3) + struct.pack(">6h", 0, 0, 8192, 0, 0, 8192)
    per = [0, 0, 0] if tex else [0, 0]
    verts = []
    for v in range(3):
        verts += [v, 0] + ([v] if tex else [])
    return out + bytes([1, 3, 0x90] + verts)


def texture(fmt, w, h, fill, wrap=0):
    bw, bh, bits = {0: (8, 8, 4), 2: (8, 4, 8), 5: (4, 4, 16)}[fmt]
    size = ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bits // 8
    head = struct.pack(">BBHHBB", fmt, 1, w, h, wrap, 0) + bytes(0x1C - 8) + struct.pack(">I", 0x20)
    return head + (fill * size)[:size]


def resource(cut=None):
    body = []
    for name, n in ARCHIVES:
        files = []
        for i in range(n):
            if name in TAGS:
                files.append(shape(name, empty=(name in ("beard", "cap") and i == 0)))
            elif name in ("eye", "mouth", "facetex"):
                # RGB5A3 opaque: red 0, green full, blue full (white and colour 1)
                files.append(texture(5, 16, 16, struct.pack(">H", 0x83FF)))
            elif name == "glasstex":
                files.append(texture(2, 8, 8, b"\xff", wrap=2))
            else:
                files.append(texture(0, 8, 8, b"\xff"))
        offs, at = [], 0
        for f in files:
            offs.append(at)
            at += len(f)
        offs.append(at)
        body.append(struct.pack(">HH", n, max(len(f) for f in files)) +
                    struct.pack(f">{n + 1}I", *offs) + b"".join(files))
    head, offs = 4 + 4 * len(body), []
    at = head
    for b in body:
        offs.append(at)
        at += len(b)
    data = struct.pack(">HH", len(body), 0x39D) + struct.pack(f">{len(offs)}I", *offs) + b"".join(body)
    return data[:cut] if cut else data


def read_rgba(path):
    data = path.read_bytes()
    head, body = data.split(b"\n", 1)
    w, h, ws, wt = map(int, head.split())
    return w, h, body


@unittest.skipUnless(CC, "no C compiler")
class MiiDrawTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = pathlib.Path(cls.tmp.name)
        cls.exe = cls.dir / ("miidraw_host.exe" if os.name == "nt" else "miidraw_host")
        subprocess.run([CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-D_CRT_SECURE_NO_WARNINGS", "-DMII_NO_OGC", "-I", str(root / "sdk/mii"),
                        str(root / "sdk/mii/mii.c"), str(root / "sdk/mii/miidraw.c"),
                        str(root / "tests/miidraw_host/main.c"), "-o", str(cls.exe)],
                       check=True, env=dict(os.environ, TMP=cls.tmp.name, TEMP=cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_host(self, res, *args):
        out = self.dir / "out"
        out.mkdir(exist_ok=True)
        for f in out.iterdir():
            f.unlink()
        r = subprocess.run([str(self.exe), str(res), *map(str, args), str(out)],
                           capture_output=True, text=True, timeout=120)
        return r, out

    def test_synthetic_default_mii(self):
        res = self.dir / "RFL_Res.dat"
        res.write_bytes(resource())
        r, out = self.run_host(res)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        parts = dict(line.split(": ", 1) for line in r.stdout.splitlines() if line.startswith("part "))
        for name in ("faceline", "mask", "nose", "noseline", "hair", "forehead", "torso", "hips",
                     "leg", "shoe", "arm", "hand", "neck"):
            self.assertFalse(parts[f"part {name}"].startswith("0 vertices"), (name, r.stdout))
        self.assertTrue(parts["part beard"].startswith("0 vertices"))   # the default has none
        self.assertTrue(parts["part glass"].startswith("0 vertices"))
        # Each expression's face: 128 square, the eyes and the mouth drawn
        # where RFL puts them (eyes about 18-30 of 64 down, mouth about 29-35),
        # clear above the eyebrows.
        for e in range(7):
            w, h, px = read_rgba(out / f"mask_{e}.rgba")
            self.assertEqual((w, h, len(px)), (128, 128, 128 * 128 * 4))
            alpha = lambda x, y: px[4 * (y * w + x) + 3]
            covered = [y for y in range(h) if any(alpha(x, y) for x in range(w))]
            self.assertTrue(covered, e)
            self.assertGreater(min(covered), 20, e)
            self.assertLess(max(covered), 100, e)
        self.assertTrue((out / "mii.obj").stat().st_size > 1000)

    def test_damaged_resource_refused(self):
        good = resource()
        for bad in (good[:100], good[:len(good) // 2], b"\0\x12\x03\x9c" + good[4:],
                    b"\0\x11" + good[2:]):
            res = self.dir / "bad.dat"
            res.write_bytes(bad)
            r, _ = self.run_host(res)
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertTrue(r.stdout.startswith("res: ") or r.stdout.count("model: "), r.stdout)

    @unittest.skipUnless(os.environ.get("MII_REAL_RES") and os.environ.get("MII_REAL_DB"),
                         "set MII_REAL_RES and MII_REAL_DB to your own files")
    def test_real_database(self):
        built = 0
        for slot in range(100):
            r, out = self.run_host(os.environ["MII_REAL_RES"], os.environ["MII_REAL_DB"], slot)
            if r.returncode == 1 and "db: no Mii there" in r.stdout:
                continue   # an empty slot
            self.assertEqual(r.returncode, 0, (slot, r.stdout))
            built += 1
        self.assertGreater(built, 0)
        print(f"{built} Miis built")


if __name__ == "__main__":
    unittest.main()
