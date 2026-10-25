"""tools/mii_res.py against a synthetic RFL_Res.dat built here (no Nintendo
data): the archive table, a faceline and a nose shape, an I4 and an RGB5A3
texture, OBJ and PNG output, and refusal of a damaged file."""

import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import mii_res  # noqa: E402


def archive(files):
    offs, at = [], 0
    for f in files:
        offs.append(at)
        at += len(f)
    offs.append(at)
    return struct.pack(">HH", len(files), max([len(f) for f in files] or [0])) + \
        struct.pack(f">{len(offs)}I", *offs) + b"".join(files)


def table(rows, frac):
    return struct.pack(">H", len(rows)) + b"".join(
        struct.pack(f">{len(r)}h", *[round(v * (1 << frac)) for v in r]) for r in rows)


def faceline():
    anchors = struct.pack(">9f", 0, 30, 5, 0, 10, 4, 0, 40, 0)
    pos = table([(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0)], 8)
    nrm = table([(0, 0, 1)], 14)
    txc = table([(0, 0), (1, 0), (1, 1), (0, 1)], 13)
    prims = bytes([1, 4, 0x80]) + bytes(sum(([v, 0, v] for v in range(4)), []))
    return b"face" + anchors + pos + nrm + txc + prims


def nose():   # no texcoords: two indexes per vertex
    pos = table([(0, 0, 0), (1, 0, 0), (0, 1, 0)], 8)
    nrm = table([(0, 0, 1)], 14)
    return b"nose" + pos + nrm + bytes([1, 3, 0x90, 0, 0, 1, 0, 2, 0])


def tex_header(fmt, w, h):
    return struct.pack(">BBHH", fmt, 1, w, h) + bytes(0x1C - 6) + struct.pack(">I", 0x20)


def i4_tex():   # one 8x8 block: left half 0xF, right half 0x0
    return tex_header(0, 8, 8) + bytes([0xFF, 0xFF, 0x00, 0x00] * 8)


def rgb5a3_tex():   # one 4x4 block: opaque red, then translucent blue
    return tex_header(5, 4, 4) + struct.pack(">8H", *[0xFC00] * 8) + struct.pack(">8H", *[0x400F] * 8)


def resource(**override):
    files = {name: [] for name in mii_res.ARCHIVES}
    files.update(faceline=[faceline()], nose=[nose(), b"nose" + struct.pack(">H", 0)],
                 eye=[i4_tex()], facetex=[rgb5a3_tex()])
    files.update(override)
    body = [archive(files[n]) for n in mii_res.ARCHIVES]
    head = 4 + 4 * len(body)
    offs, at = [], head
    for b in body:
        offs.append(at)
        at += len(b)
    return struct.pack(">HH", len(body), 0x39D) + struct.pack(f">{len(offs)}I", *offs) + b"".join(body)


class MiiResTest(unittest.TestCase):
    def test_archives(self):
        arcs, version = mii_res.archives(resource())
        self.assertEqual(version, 0x39D)
        self.assertEqual(len(arcs), 18)
        self.assertEqual(len(arcs["nose"]), 2)
        self.assertEqual(arcs["beard"], [])

    def test_faceline(self):
        anchors, pos, nrm, txc, prims = mii_res.shape("faceline", faceline())
        self.assertEqual(anchors[0], (0.0, 30.0, 5.0))
        self.assertEqual(pos[2], (10.0, 10.0, 0.0))
        self.assertEqual(nrm, [(0.0, 0.0, 1.0)])
        self.assertEqual(txc[1], (1.0, 0.0))
        self.assertEqual(prims, [("quads", [(0, 0, 0), (1, 0, 1), (2, 0, 2), (3, 0, 3)])])
        obj = mii_res.obj("faceline 0", (anchors, pos, nrm, txc, prims))
        self.assertIn("f 1/1/1 2/2/1 3/3/1", obj)
        self.assertEqual(obj.count("\nf "), 2)

    def test_nose_and_empty(self):
        _, pos, _, txc, prims = mii_res.shape("nose", nose())
        self.assertEqual((len(pos), txc), (3, []))
        self.assertEqual(prims[0][1], [(0, 0), (1, 0), (2, 0)])
        self.assertEqual(mii_res.shape("nose", b"nose\0\0")[1], [])
        with self.assertRaises(mii_res.ResError):
            mii_res.shape("beard", nose())

    def test_textures(self):
        fmt, w, h, px = mii_res.texture(i4_tex())
        self.assertEqual((fmt, w, h), ("I4", 8, 8))
        self.assertEqual(px[0][0], (255, 255, 255, 255))
        self.assertEqual(px[0][7], (0, 0, 0, 0))
        fmt, _, _, px = mii_res.texture(rgb5a3_tex())
        self.assertEqual(fmt, "RGB5A3")
        self.assertEqual(px[0][0], (255, 0, 0, 255))
        self.assertEqual(px[3][3], (0, 0, 255, 146))

    def test_png(self):
        data = mii_res.png(2, 1, [[(1, 2, 3, 4), (5, 6, 7, 8)]])
        self.assertTrue(data.startswith(b"\x89PNG"))
        idat = data[data.index(b"IDAT") + 4:data.index(b"IEND") - 8]
        self.assertEqual(zlib.decompress(idat), b"\0\1\2\3\4\5\6\7\x08")

    def test_dump(self):
        with tempfile.TemporaryDirectory() as tmp:
            mii_res.dump(resource(), pathlib.Path(tmp))
            names = sorted(p.name for p in pathlib.Path(tmp).iterdir())
        self.assertEqual(names, ["eye_000.png", "faceline_000.obj", "facetex_000.png", "nose_000.obj"])

    def test_refuses(self):
        good = resource()
        for bad in (good[:20], struct.pack(">H", 17) + good[2:], good[:2] + struct.pack(">H", 0x39C) + good[4:],
                    good[:-10]):
            with self.assertRaises(mii_res.ResError):
                mii_res.archives(bad)


if __name__ == "__main__":
    unittest.main()
