"""tools/msgboard/stationery.py: the board data that gives a Message Board letter
its own stationery, icon and sound (docs/messageboard-messages.md).

Each encoder is read back with a decoder written here, not the tool's own: the
RGB5A3 textures, the U8 archives, Nintendo's LZ77 and the DSP-ADPCM sound.
The layout is the one WiiLink's letters carry, which the Wii Menu files (a
copy of one, checked on 2026-10-23, repacked byte for byte with this tool;
WiiLink's art is not kept here)."""

import math
import pathlib
import struct
import sys
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools/msgboard"))

try:
    from PIL import Image
    import stationery as st
    import stationery_themes as themes
except ImportError:     # Pillow missing
    st = None


def rgb5a3_decode(v):
    if v & 0x8000:
        return ((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
    return ((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 7) * 255 // 7)


def tpl_decode(b):
    assert struct.unpack_from(">III", b, 0) == (0x0020AF30, 1, 0x0C)
    io = struct.unpack_from(">I", b, 0x0C)[0]
    h, w, fmt, data = struct.unpack_from(">HHII", b, io)
    assert fmt == 5
    px = {}
    i = data
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            for k in range(16):
                px[(tx + k % 4, ty + k // 4)] = rgb5a3_decode(struct.unpack_from(">H", b, i)[0])
                i += 2
    return w, h, px


def lz_decode(d):
    assert d[0] == 0x10
    size, o, i = int.from_bytes(d[1:4], "little"), bytearray(), 4
    while len(o) < size:
        flags = d[i]
        i += 1
        for b in range(8):
            if len(o) >= size:
                break
            if flags & 0x80 >> b:
                x = d[i] << 8 | d[i + 1]
                i += 2
                for _ in range((x >> 12) + 3):
                    o.append(o[-((x & 0xFFF) + 1)])
            else:
                o.append(d[i])
                i += 1
    return bytes(o)


def u8_decode(a):
    """{path: bytes}; also checks the layout Nintendo's archives have."""
    magic, rootoff, hsize, data = struct.unpack_from(">IIII", a, 0)
    assert (magic, rootoff) == (0x55AA382D, 0x20) and data == 0x20 + hsize and data % 32 == 0
    n = struct.unpack_from(">I", a, 0x28)[0]
    strs, out, stack = 0x20 + 12 * n, {}, []
    for i in range(1, n):
        t, name, d, s = a[0x20 + 12 * i], int.from_bytes(a[0x21 + 12 * i:0x24 + 12 * i], "big"), \
            *struct.unpack_from(">II", a, 0x24 + 12 * i)
        while stack and i >= stack[-1][1]:
            stack.pop()
        nm = a[strs + name:a.index(b"\0", strs + name)].decode()
        if t == 1:
            stack.append((nm, s))
        else:
            assert d % 32 == 0
            out["/".join([x for x, _ in stack] + [nm])] = a[d:d + s]
    return out


def adpcm_decode(data, coefs, count):
    out, h1, h2 = [], 0, 0
    for f in range(0, len(data), 8):
        p, scale = data[f] >> 4, data[f] & 15
        c1, c2 = coefs[p]
        for k in range(14):
            nib = data[f + 1 + k // 2] >> (4 if k % 2 == 0 else 0) & 15
            q = nib - 16 if nib >= 8 else nib
            v = max(-32768, min(32767, ((c1 * h1 + c2 * h2 + 1024) >> 11) + (q << scale)))
            out.append(v)
            h2, h1 = h1, v
            if len(out) == count:
                return out
    return out


@unittest.skipUnless(st, "needs Pillow")
class StationeryTest(unittest.TestCase):
    def test_texture_round_trip(self):
        img = Image.new("RGBA", (64, 144))
        for y in range(144):
            for x in range(64):
                img.putpixel((x, y), (x * 4, y, 255 - x * 4, 255 if (x + y) % 3 else 96))
        w, h, px = tpl_decode(st.tpl(img))
        self.assertEqual((w, h), (64, 144))
        for (x, y), (r, g, b, a) in [((0, 0), img.getpixel((0, 0))), ((63, 143), img.getpixel((63, 143))),
                                     ((10, 20), img.getpixel((10, 20)))]:
            got = px[(x, y)]
            tol = 9 if a >= 0xE0 else 17
            self.assertTrue(all(abs(p - q) <= tol for p, q in zip(got[:3], (r, g, b))), (got, (r, g, b)))
            self.assertEqual(got[3] == 255, a >= 0xE0)

    def test_lz_round_trip(self):
        data = (b"stationery " * 300 + bytes(range(256)) * 20 + b"\x00" * 5000)
        packed = st.lz10(data)
        self.assertEqual(lz_decode(packed), data)
        self.assertLess(len(packed), len(data) // 3)

    def test_sound_round_trip(self):
        n = 8000 * 2
        tone = [int(12000 * math.sin(2 * math.pi * 523 * i / 8000)) for i in range(n)]
        b = st.bns(tone, 8000)
        self.assertEqual(b[:4], b"BNS ")
        self.assertEqual(struct.unpack_from(">HHIHH", b, 4), (0xFEFF, 0x100, len(b), 0x20, 2))
        info = struct.unpack_from(">I", b, 0x10)[0]
        self.assertEqual(b[info:info + 4], b"INFO")
        codec, loop, ch, _, rate = struct.unpack_from(">BBBBH", b, info + 8)
        self.assertEqual((codec, loop, ch, rate), (0, 0, 1, 8000))
        count = struct.unpack_from(">I", b, info + 0x14)[0]
        coefs = [struct.unpack_from(">hh", b, info + 8 + 0x28 + 4 * k) for k in range(8)]
        data = struct.unpack_from(">I", b, 0x18)[0]
        self.assertEqual(b[data:data + 4], b"DATA")
        pcm = adpcm_decode(b[data + 8:data + struct.unpack_from(">I", b, data + 4)[0]], coefs, count)
        noise = sum((x - y) ** 2 for x, y in zip(tone, pcm))
        snr = 10 * math.log10(sum(x * x for x in tone) / max(noise, 1))
        self.assertEqual(count, n)
        self.assertGreater(snr, 30)

    def test_every_theme(self):
        for name, make in themes.THEMES.items():
            with self.subTest(name):
                letter, thumb, music = make()
                arc = st.board(letter, thumb, music, themes.RATE)
                self.assertLessEqual(len(arc), st.BOARD_MAX)
                files = u8_decode(arc)
                want = ["letter_LZ.bin", "thumbnail_LZ.bin"] + (["sound.bns"] if music else [])
                self.assertEqual(list(files), want)
                inner = u8_decode(lz_decode(files["letter_LZ.bin"]))
                self.assertEqual(list(inner), [f"img/my_Letter_{k}.tpl" for k in "acgibdefh"])
                sizes = {k: tpl_decode(v)[:2] for k, v in inner.items()}
                self.assertEqual(sizes["img/my_Letter_a.tpl"], (64, 144))
                self.assertEqual(sizes["img/my_Letter_e.tpl"], (384, 168))
                self.assertEqual(sizes["img/my_Letter_i.tpl"], (64, 64))
                # The middle piece is the letter's middle: (74, 154) is its (10, 10).
                w, h, px = tpl_decode(inner["img/my_Letter_e.tpl"])
                want_px = letter.convert("RGBA").getpixel((74, 154))
                self.assertTrue(all(abs(p - q) <= 17 for p, q in zip(px[(10, 10)][:3], want_px[:3])))
                # The Wii Menu's limits: the letter under 400 KiB unpacked, the icon under 30 KiB.
                self.assertLess(len(lz_decode(files["letter_LZ.bin"])), 0x64000)
                self.assertLess(len(lz_decode(files["thumbnail_LZ.bin"])), 0x7800)
                icon = u8_decode(lz_decode(files["thumbnail_LZ.bin"]))
                self.assertEqual(tpl_decode(icon["img/my_LetterS_b.tpl"])[:2], (144, 96))

    def test_channel_button(self):
        letter, thumb, _ = themes.sunset()
        arc = st.board(letter, thumb, jump=(0x00010001, 0x4F484243))
        jump = u8_decode(arc)["chjump.bin"]
        self.assertEqual(jump[:4], b"ChJp")
        self.assertEqual(struct.unpack_from(">II", jump, 0x10), (0x00010001, 0x4F484243))


if __name__ == "__main__":
    unittest.main()
