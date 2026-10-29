"""sdk/mii's effects (miifx.c) on the PC, through tests/miifx_host:

- synthetic REFF and REFT files built here (no Nintendo data), one feature
  each: forms and even spreads, rate, start, interval, life, speed and drag,
  gravity, spin, the curves (linear, smooth, step, random), the flip-book,
  child emitters, the emitter's own curves, placing by a matrix, the pools;
- damaged and cut files refused without a crash;
- with MII_REAL_EFFECTS set to a folder holding your own
  effect_nigaoe.breff and .breft (the Mii Channel's Character.szs/effect):
  every emitter loads, and a few facts about how they play.
"""

import math
import os
import pathlib
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import test_cdblog  # noqa: E402

CC = test_cdblog.CC

REAL_NAMES = ["Nc_sneeze01", "Nc_talk01", "nc_talk01_b", "Nc_bodyslip01", "Nc_run_smoke01", "Nc_favor_pants01",
              "Nc_favor_pants01fm", "Nc_favor_pants02", "Nc_favor_pants02fm", "Nc_favor_on01", "Nc_change_parts01",
              "Nc_zzz01", "Nc_slapsmoke01", "Nc_slapsmoke02"]

# ---- A small REFF / REFT writer (version 5), as FORMAT.md lays them out ----

TAU = 2 * math.pi


def project(magic, name, entries, proj_size):
    """The file: header, block, project, name table, then each entry's data."""
    proj = struct.pack(">III", proj_size, 0, 0) + struct.pack(">HH", len(name) + 1, 0) + name + b"\0"
    proj = proj.ljust(proj_size, b"\0")
    table = b""
    for n, data in entries:
        table += struct.pack(">H", len(n) + 1) + n + b"\0" + struct.pack(">II", 0, 0)
    tsize = 8 + len(table)
    head, at, blobs = b"", tsize, b""
    for n, data in entries:
        size = len(data) - (0x20 if magic == b"REFT" else 0)   # a texture's size is its pixels'
        head += struct.pack(">H", len(n) + 1) + n + b"\0" + struct.pack(">II", at, size)
        at += len(data)
        blobs += data
    body = proj + struct.pack(">IHH", tsize, len(entries), 0) + head + blobs
    total = 0x18 + len(body)
    return magic + struct.pack(">HHIHH", 0xFEFF, 5, total, 0x10, 1) + magic + struct.pack(">I", total - 0x18) + body


def texture(w=8, h=8, fmt=3):
    size = w * h * {2: 1, 3: 2}[fmt]
    return (struct.pack(">IHHIBBHIBBB", 0, w, h, size, fmt, 0, 0, 0, 0, 0, 0).ljust(0x20, b"\0") +
            bytes(range(256)) * (size // 256) + bytes(size % 256))


def breft(names=("t0", "t1")):
    return project(b"REFT", b"test", [(n.encode(), texture()) for n in names], 0x48)


def curve(kind, ctype, mask, keys, frames, flags=0, loop=0, ranges=(), names=(), info=b""):
    """keys: (frame, interp, value): a tuple of numbers, ("rand", entry), a
    texture's name index, or a child's (name index, kind)."""
    comps = bin(mask).count("1")
    out = b""
    for frame, interp, value in keys:
        k = struct.pack(">HHHBB", frame, interp, 0, 0, 0)
        if ctype == 4:
            v = struct.pack(">BBH", 0, 0, value)
        elif ctype == 5:
            v = struct.pack(">hBBBBBBH", 1, 100, 0, 0, 128, value[1], 0, value[0])
        elif isinstance(value, tuple) and value[0] == "rand":
            k = k[:6] + b"\x01\x00"
            v = struct.pack(">H", value[1]).ljust(comps if ctype == 0 else 4 * comps, b"\0")
        elif ctype == 0:
            v = bytes(value)
        else:
            v = struct.pack(f">{comps}f", *value)
        out += (k + v).ljust((8 + len(v) + 1) & ~1, b"\0")
    keytab = struct.pack(">HH", len(keys), 0) + out
    rangetab = b""
    if ranges:
        rangetab = struct.pack(">HH", len(ranges), 0)
        for r in ranges:   # r: per component (base, range), and a sign for rotations
            for base, rng in r[0]:
                rangetab += struct.pack(">BB", base, rng) if ctype == 0 else struct.pack(">ff", base, rng)
            if ctype == 6:
                rangetab += struct.pack(">B3x", r[1])
    nametab = b""
    if names:
        nametab = struct.pack(">HH", len(names), 0) + bytes(4 * len(names))
        for n in names:
            nametab += struct.pack(">H", len(n) + 1) + n.encode() + b"\0"
        nametab = nametab.ljust((len(nametab) + 3) & ~3, b"\0")
    sizes = [len(keytab), len(rangetab), 0, len(nametab), len(info)]
    head = struct.pack(">BBBBBBHHH5I", 0xAC, kind, ctype, mask, flags, loop, 0xF43C, frames, 0, *sizes)
    return head + keytab + rangetab + nametab + info


FIT, LOOP = 0x80, 0x20


def emitter(form=9, common=0x80000038, emit_flags=0, emit_life=1, ptcl_life=10, life_rnd=0, emit=1.0, start=0,
            interval=0, params=(0,) * 6, div=0, vel_rnd=0, radial=0.0, yaxis=0.0, rnd=0.0, normal=0.0, spec=0.0,
            spec_dir=(0, 0, 0), spec_cone=0.0, scale=(1, 1, 1), rot=(0, 0, 0), trans=(0, 0, 0), seed=0, bb=0,
            draw=0x15, size=(10, 10), pscale=(1, 1), prot=(0, 0, 0), size_rnd=0, tex="t0", ptracks=(), init=0,
            etracks=(), color=(255, 255, 255, 255), color2=(0, 0, 0, 0)):
    d = bytearray(0x140)
    struct.pack_into(">II", d, 0, common, emit_flags | form)
    struct.pack_into(">HHbbbbfHHHbb", d, 8, emit_life, ptcl_life, life_rnd, 0, 0, 0, emit, start, 0, interval, 0, 0)
    struct.pack_into(">6f", d, 0x1C, *params)
    struct.pack_into(">Hbb7f3f", d, 0x34, div, vel_rnd, 0, radial, yaxis, rnd, normal, 0, spec, spec_cone, *spec_dir)
    struct.pack_into(">9f", d, 0x60, *scale, *rot, *trans)
    struct.pack_into(">IBBBB", d, 0x88, seed, 3, bb, 0, 1)
    struct.pack_into(">HBBB", d, 0xA0, draw, 7, 7, 0)
    struct.pack_into(">BBBB", d, 0xA0 + 0x58, 1, 4, 5, 0)
    d[0xA0 + 0x64] = 3
    p = bytearray(0x7A)
    struct.pack_into(">4B4B", p, 0, *color, *color2)
    struct.pack_into(">2f2f3f", p, 0x10, *size, *pscale, *prot)
    struct.pack_into(">HBBBb", p, 0x74, 0, 0, 0, 0, size_rnd)
    for n in (tex, "", ""):
        p += struct.pack(">H", len(n) + 1) + n.encode() + b"\0"
    p = bytes(p.ljust((len(p) + 3) & ~3, b"\0"))
    anim = b""
    for tracks, ninit in ((ptracks, init), (etracks, 0)):
        anim += struct.pack(">HH", len(tracks), ninit) + bytes(4 * len(tracks))
        anim += b"".join(struct.pack(">I", len(t)) for t in tracks)
    anim += b"".join(ptracks) + b"".join(etracks)
    return struct.pack(">II", 0, 0x140) + bytes(d) + struct.pack(">I", len(p)) + p + anim


def breff(emitters):
    return project(b"REFF", b"test", [(n.encode(), e) for n, e in emitters], 0x20)


def parse(out):
    frames, cur = [], None
    for line in out.splitlines():
        if line.startswith("frame "):
            cur = []
            frames.append(cur)
        elif line.startswith("p ") and cur is not None:
            f = line.split()
            cur.append({"emitter": int(f[1]), "age": int(f[3]), "life": int(f[5]),
                        "pos": tuple(map(float, f[7:10])), "size": tuple(map(float, f[11:13])),
                        "rot": float(f[14]), "col": tuple(map(int, f[16:20])), "sec": tuple(map(int, f[21:24])),
                        "tex": int(f[25]), "hidden": int(f[27])})
    return frames


def births(frames, emitter=None):
    return [i for i, ps in enumerate(frames) for p in ps if p["age"] == 0 and emitter in (None, p["emitter"])]


@unittest.skipUnless(CC, "no C compiler")
class MiiFxTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = pathlib.Path(cls.tmp.name)
        cls.exe = cls.dir / ("miifx_host.exe" if os.name == "nt" else "miifx_host")
        subprocess.run([CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-D_CRT_SECURE_NO_WARNINGS", "-DMII_NO_OGC", "-I", str(root / "sdk/mii"),
                        str(root / "sdk/mii/mii.c"), str(root / "sdk/mii/miifx.c"),
                        str(root / "tests/miifx_host/main.c"), "-o", str(cls.exe)]
                       + ([] if os.name == "nt" else ["-lm"]),
                       check=True, env=dict(os.environ, TMP=cls.tmp.name, TEMP=cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def host(self, eff, tex, *args, env=None):
        e = self.dir / "t.breff"
        t = self.dir / "t.breft"
        e.write_bytes(eff)
        if tex is not None:
            t.write_bytes(tex)
        return subprocess.run([str(self.exe), str(e), str(t) if tex is not None else "-", *map(str, args)],
                              capture_output=True, text=True, timeout=60, env=dict(os.environ, **(env or {})))

    def play(self, emitters, name, frames, seed=1, env=None, tex=None):
        r = self.host(breff(emitters), tex or breft(), name, frames, seed, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return parse(r.stdout)

    def test_load_and_find(self):
        r = self.host(breff([("a", emitter()), ("b", emitter(tex="t1")), ("c", emitter(tex="none"))]), breft())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        em = re.findall(r"^emitter (\d+) (\S+) .* tex (-?\d+)", r.stdout, re.M)
        self.assertEqual(em, [("0", "a", "0"), ("1", "b", "1"), ("2", "c", "-1")])
        self.assertIn("texture 1 t1 8x8 fmt 3 bytes 128", r.stdout)
        r = self.host(breff([("a", emitter())]), breft(), "nope", 1)
        self.assertEqual(r.returncode, 1)
        # without textures: loads, nothing to draw
        r = self.host(breff([("a", emitter())]), None, "a", 2)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(parse(r.stdout)[0][0]["tex"], -1)

    def test_flip_book(self):
        tr = curve(14, 4, 1, [(0, 0, 0), (1, 0, 1), (2, 0, 0)], 3, FIT, 1, names=["t1", "t0"])
        f = self.play([("s", emitter(ptcl_life=9, ptracks=[tr]))], "s", 10)
        texs = [ps[0]["tex"] for ps in f[:9]]
        self.assertEqual(texs, [1] * 4 + [0] * 4 + [1])   # as Nc_sneeze01: 01 02 01 over its life
        self.assertEqual(f[9], [])   # dead after 9 frames

    def test_even_disc_and_drag(self):
        drag = curve(33, 3, 1, [(0, 0, (0.9,))], 1)
        e = emitter(form=0, emit_flags=1 << 17 | 1 << 18, div=7, params=(20, 100, 0, TAU, 20, 0), radial=4,
                    trans=(0, 15, -20), ptcl_life=30, ptracks=[drag])
        f = self.play([("d", e)], "d", 6)
        self.assertEqual(len(f[0]), 7)
        angles = sorted(math.atan2(p["pos"][2] + 20, p["pos"][0]) % TAU for p in f[0])
        for i, a in enumerate(angles):
            self.assertAlmostEqual(a, TAU * i / 7, places=4)
        for k in range(6):
            want = 20 + 4 * sum(0.9 ** i for i in range(1, k + 1))
            for p in f[k]:
                self.assertAlmostEqual(math.hypot(p["pos"][0], p["pos"][2] + 20), want, places=3)
                self.assertAlmostEqual(p["pos"][1], 15, places=4)

    def test_rate_start_interval_life(self):
        f = self.play([("r", emitter(emit=0.5, emit_life=24, ptcl_life=40))], "r", 30)
        self.assertEqual(births(f), list(range(0, 24, 2)))   # 0.5 a frame: 12, the first at once
        f = self.play([("r", emitter(start=5, interval=2, emit_life=9, ptcl_life=3))], "r", 20)
        self.assertEqual(births(f), [5, 8, 11])
        f = self.play([("r", emitter(emit=3, emit_life=10, ptcl_life=40, life_rnd=50))], "r", 12)
        lives = {p["life"] for ps in f for p in ps}
        self.assertTrue(lives <= set(range(20, 41)) and len(lives) > 5, lives)

    def test_curves(self):
        scale = curve(11, 3, 3, [(0, 0, (1, 1)), (10, 0, (3, 3))], 11)
        smooth = curve(11, 3, 3, [(0, 1, (1, 1)), (10, 1, (3, 3))], 11)
        step = curve(11, 3, 3, [(0, 2, (1, 1)), (10, 2, (3, 3))], 11)
        col = curve(0, 0, 7, [(0, 0, (0, 0, 0)), (10, 0, (200, 100, 50))], 11)
        alpha = curve(1, 0, 1, [(0, 0, (255,)), (5, 0, (255,)), (10, 0, (0,))], 11, FIT, 1)
        sec = curve(2, 0, 7, [(0, 0, (10, 20, 30))], 1)
        f = self.play([("c", emitter(ptcl_life=21, ptracks=[scale, col, alpha, sec]))], "c", 11)
        p = f[5][0]
        self.assertAlmostEqual(p["size"][0], 20, places=3)
        self.assertEqual(p["col"][:3], (100, 50, 25))
        self.assertEqual(p["sec"], (10, 20, 30))
        self.assertEqual(f[10][0]["col"][3], 255)   # stretched over 21 frames: frame 10 of 21 is key 5
        self.assertEqual(f[5][0]["col"][3], 255)
        t = 0.2 * 0.2 * (3 - 2 * 0.2)
        f = self.play([("c", emitter(ptcl_life=21, ptracks=[smooth]))], "c", 3)
        self.assertAlmostEqual(f[2][0]["size"][0], 10 * (1 + 2 * t), places=3)
        f = self.play([("c", emitter(ptcl_life=21, ptracks=[step]))], "c", 11)
        self.assertEqual([round(ps[0]["size"][0]) for ps in f], [10] * 10 + [30])
        f = self.play([("c", emitter(ptcl_life=21, ptracks=[alpha]))], "c", 21)
        self.assertEqual(f[20][0]["col"][3], 0)
        self.assertEqual(f[15][0]["col"][3], 128)   # half way down: frame 15 of 21 is 7.5 of 10

    def test_random_rotation_seeded(self):
        rot = curve(12, 6, 7, [(0, 0, ("rand", 0))], 1,
                    ranges=[([(0, 0), (0, 0), (4 * math.pi, -4 * math.pi / 65536)], 0)])
        e = emitter(emit=20, ptcl_life=5, ptracks=[rot], init=1)
        a = self.play([("r", e)], "r", 3, seed=7)
        b = self.play([("r", e)], "r", 3, seed=7)
        c = self.play([("r", e)], "r", 3, seed=8)
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)
        rots = [p["rot"] for p in a[0]]
        self.assertTrue(all(0 < r <= 4 * math.pi + 1e-4 for r in rots), rots)
        self.assertGreater(max(rots) - min(rots), 2)
        self.assertEqual([p["rot"] for p in a[2]], rots)   # applied at birth only

    def test_gravity_spin_speed(self):
        grav = curve(32, 3, 1, [(0, 0, (2,))], 1, info=struct.pack(">4fBB2x", 2, 0, 1, 0, 0, 1))
        f = self.play([("g", emitter(ptcl_life=10, ptracks=[grav]))], "g", 5)
        self.assertEqual([round(ps[0]["pos"][1], 4) for ps in f], [0, 2, 4, 6, 8])
        grav_v = curve(32, 3, 1, [(0, 0, (1,))], 1, info=struct.pack(">4fBB2x", 1, 0, -1, 0, 0, 0))
        f = self.play([("g", emitter(ptcl_life=10, ptracks=[grav_v]))], "g", 4)
        self.assertEqual([round(ps[0]["pos"][1], 4) for ps in f], [0, -1, -3, -6])   # on the speed: falling
        spin = curve(38, 3, 15, [(0, 0, (0.1, 0, 0, 0))], 1, info=struct.pack(">4fBB2x", 0.1, 0, 0, 0, 1, 1))
        e = emitter(form=0, emit_flags=1 << 17 | 1 << 18, div=4, params=(10, 100, 0, TAU, 10, 0), ptcl_life=20,
                    ptracks=[spin])
        f = self.play([("s", e)], "s", 6)
        for k, ps in enumerate(f):
            for p in ps:
                self.assertAlmostEqual(math.hypot(p["pos"][0], p["pos"][2]), 10, places=3)
        a0 = math.atan2(f[0][0]["pos"][2], f[0][0]["pos"][0])
        a5 = math.atan2(f[5][0]["pos"][2], f[5][0]["pos"][0])
        self.assertAlmostEqual(abs((a5 - a0 + math.pi) % TAU - math.pi), 0.5, places=3)
        # speed along a set direction: spec_dir turns Y; (pi/2, 0, 0) turns it to +Z
        f = self.play([("v", emitter(spec=3, spec_dir=(math.pi / 2, 0, 0), ptcl_life=5))], "v", 3)
        self.assertAlmostEqual(f[2][0]["pos"][2], 6, places=4)
        self.assertAlmostEqual(f[2][0]["pos"][1], 0, places=4)

    def test_emitter_curves_and_matrix(self):
        tr = curve(67, 3, 7, [(0, 0, (0, 0, 0)), (9, 0, (0, 9, 0))], 10)
        f = self.play([("t", emitter(emit=1, emit_life=10, ptcl_life=30, etracks=[tr]))], "t", 10)
        born = {}
        for i, ps in enumerate(f):
            for p in ps:
                if p["age"] == 0:
                    born[i] = p["pos"][1]
        self.assertEqual(born, {i: float(i) for i in range(10)})
        # placed by the app's matrix: 90 degrees about Z, scaled 2, moved 100 along X
        at = "0 -2 0 100  2 0 0 0  0 0 2 0"
        f = self.play([("m", emitter(trans=(10, 0, 0)))], "m", 1, env={"MII_FX_AT": at})
        p = f[0][0]
        self.assertEqual(tuple(round(v, 4) for v in p["pos"]), (100, 20, 0))
        self.assertAlmostEqual(p["size"][0], 20, places=4)

    def test_emission_curve_loop_and_stop(self):
        em = curve(73, 3, 1, [(0, 0, (1,)), (1, 0, (0,)), (10, 0, (0,))], 11, LOOP)
        e = emitter(common=0x8000003C, emit_life=0xFFFF, ptcl_life=3, etracks=[em])
        f = self.play([("z", e)], "z", 45)
        self.assertEqual(births(f), [0, 10, 20, 30, 40])
        r = self.host(breff([("z", e)]), breft(), "z", 45, 1, env={"MII_FX_STOP": "25"})
        f = parse(r.stdout)
        self.assertEqual(births(f), [0, 10, 20])
        self.assertIn("frame 25 live 0 playing 1", r.stdout)
        self.assertIn("frame 26 live 0 playing 0", r.stdout)

    def test_children(self):
        child = curve(26, 5, 1, [(0, 0, (0, 1)), (15, 0, (0, 1)), (30, 0, (0, 1))], 31, names=["kid"])
        carrier = emitter(ptcl_life=40, spec=1, draw=0x415, ptracks=[child])
        kid = emitter(ptcl_life=5, tex="t1")
        f = self.play([("carrier", carrier), ("kid", kid)], "carrier", 40)
        self.assertEqual(births(f, 1), [0, 15, 30])
        kids = {i: p for i, ps in enumerate(f) for p in ps if p["emitter"] == 1 and p["age"] == 0}
        self.assertEqual({i: round(p["pos"][1], 4) for i, p in kids.items()}, {0: 0, 15: 15, 30: 30})
        self.assertTrue(all(p["hidden"] for ps in f for p in ps if p["emitter"] == 0))
        self.assertFalse(any(p["hidden"] for ps in f for p in ps if p["emitter"] == 1))
        # a short-lived carrier starts fewer
        f = self.play([("carrier", emitter(ptcl_life=20, spec=1, draw=0x415, ptracks=[child])), ("kid", kid)],
                      "carrier", 40)
        self.assertEqual(births(f, 1), [0, 15])

    def test_cylinder(self):
        e = emitter(form=7, emit=40, params=(30, 100, 0, TAU, 10, 15), ptcl_life=2)
        f = self.play([("c", e)], "c", 1)
        self.assertEqual(len(f[0]), 40)
        for p in f[0]:
            x, y, z = p["pos"]
            self.assertAlmostEqual((x / 30) ** 2 + (z / 15) ** 2, 1, places=3)
            self.assertLessEqual(abs(y), 5)

    def test_pools(self):
        f = self.play([("p", emitter(form=0, emit_flags=1 << 17, div=200, params=(5, 100, 0, TAU, 5, 0),
                                     ptcl_life=3))], "p", 2)
        self.assertEqual(len(f[0]), 128)
        f = self.play([("p", emitter(emit=50, emit_life=10, ptcl_life=30))], "p", 4)
        self.assertEqual([len(ps) for ps in f], [50, 100, 128, 128])
        self.assertEqual(sorted(p["age"] for p in f[3])[:50], [0] * 50)   # the oldest went

    def test_replaced(self):
        # Every emitter busy: a play takes the oldest one's place, and the
        # state counts it (mii_fx_state.replaced, sdk/mii 1.5.0); so do the
        # children a particle starts in mii_fx_step.
        e = emitter(emit_life=100, ptcl_life=5)
        r = self.host(breff([("e", e)]), breft(), "e", 1, 1, env={"MII_FX_PLAYS": "16"})
        self.assertIn("frame 0 live 16 playing 1 replaced 0", r.stdout)
        r = self.host(breff([("e", e)]), breft(), "e", 1, 1, env={"MII_FX_PLAYS": "20"})
        self.assertIn("frame 0 live 16 playing 1 replaced 4", r.stdout)
        # 20 particles at once, each starting a child: 15 free emitters,
        # then the 16th child takes the oldest's place, the carrier's own.
        # The carrier stops there (16 particles); its slot plays the child.
        child = curve(26, 5, 1, [(0, 0, (0, 1))], 1, names=["kid"])
        carrier = emitter(emit=20, ptcl_life=40, draw=0x415, ptracks=[child])
        r = self.host(breff([("carrier", carrier), ("kid", emitter(ptcl_life=5, tex="t1"))]), breft(), "carrier",
                      1, 1)
        self.assertIn("frame 0 live 32 playing 0 replaced 1", r.stdout)
        f = parse(r.stdout)
        self.assertEqual([sum(p["emitter"] == k for p in f[0]) for k in (0, 1)], [16, 16])

    def test_damaged_refused(self):
        tr = curve(11, 3, 3, [(0, 0, (1, 1)), (10, 0, (3, 3))], 11)
        child = curve(26, 5, 1, [(0, 0, (0, 1))], 1, names=["a"])
        eff = breff([("a", emitter(ptracks=[tr, child], etracks=[tr])), ("b", emitter(form=0, div=3))])
        tex = breft()
        self.assertEqual(self.host(eff, tex).returncode, 0)
        for cut in range(0, len(eff), 7):
            r = self.host(eff[:cut], tex)
            self.assertEqual(r.returncode, 1, (cut, r.stdout, r.stderr))
        for cut in range(0, len(tex), 5):
            r = self.host(eff, tex[:cut])
            self.assertEqual(r.returncode, 1, (cut, r.stdout, r.stderr))
        rng = random.Random(5)
        for i in range(150):
            e, t = bytearray(eff), bytearray(tex)
            for _ in range(rng.randint(1, 6)):
                b = e if rng.random() < 0.8 else t
                b[rng.randrange(len(b))] = rng.choice([0, 0xFF, 0x7F, 0x80, rng.randrange(256)])
            r = self.host(bytes(e), bytes(t), "a", 30)
            self.assertIn(r.returncode, (0, 1), (i, r.stderr))

    @unittest.skipUnless(os.environ.get("MII_REAL_EFFECTS"),
                         "set MII_REAL_EFFECTS to a folder with your own effect_nigaoe.breff and .breft")
    def test_real_effects(self):
        d = pathlib.Path(os.environ["MII_REAL_EFFECTS"])
        eff, tex = d / "effect_nigaoe.breff", d / "effect_nigaoe.breft"

        def run(*args):
            r = subprocess.run([str(self.exe), str(eff), str(tex), *map(str, args)], capture_output=True,
                               text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout

        out = run()
        self.assertEqual(re.findall(r"^emitter \d+ (\S+)", out, re.M), REAL_NAMES)
        self.assertEqual(re.findall(r"^texture \d+ (\S+) (\d+x\d+) fmt (\d)", out, re.M),
                         [("nc_talk", "32x64", "3"), ("nc_splash01", "64x64", "2"), ("nc_splash02", "64x64", "2"),
                          ("nc_kira01", "64x64", "3"), ("nc_smoke03", "64x64", "2"), ("nc_zzz01", "32x32", "3")])
        self.assertNotIn(" tex -1 ", out)
        f = parse(run("Nc_zzz01", 200))
        self.assertEqual(births(f), [0, 53, 106, 159])
        f = parse(run("Nc_bodyslip01", 2))
        self.assertEqual(len(f[0]), 7)
        f = parse(run("Nc_sneeze01", 9))
        self.assertEqual([ps[0]["tex"] for ps in f], [1] * 4 + [2] * 4 + [1])
        talk = REAL_NAMES.index("nc_talk01_b")
        seen = set()
        for seed in range(1, 12):
            b = births(parse(run("Nc_talk01", 60, seed)), talk)
            self.assertEqual(b, [0, 15, 30][:len(b)])
            seen.add(len(b))
        self.assertIn(3, seen)
        for name in REAL_NAMES:
            self.assertIn("frame 59", run(name, 60))
        cursor = d / "nw4r_defcursor_all01.breff"
        if cursor.exists():
            r = subprocess.run([str(self.exe), str(cursor), str(d / "nw4r_defcursor_all01.breft")],
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    @unittest.skipUnless(os.environ.get("MII_REAL_EFFECTS"), "set MII_REAL_EFFECTS")
    def test_real_effects_cut(self):
        d = pathlib.Path(os.environ["MII_REAL_EFFECTS"])
        eff = (d / "effect_nigaoe.breff").read_bytes()
        tex = (d / "effect_nigaoe.breft").read_bytes()
        for cut in range(0, len(eff), 97):
            self.assertEqual(self.host(eff[:cut], tex).returncode, 1, cut)
        for cut in range(0, len(tex), 211):
            self.assertEqual(self.host(eff, tex[:cut]).returncode, 1, cut)


if __name__ == "__main__":
    unittest.main()
