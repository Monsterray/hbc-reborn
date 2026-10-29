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
                        str(root / "sdk/mii/mii.c"), str(root / "sdk/mii/miidraw.c"), str(root / "sdk/mii/miibody.c"), str(root / "sdk/mii/miifx.c"),
                        str(root / "tests/miidraw_host/main.c"), "-o", str(cls.exe)]
                       + ([] if os.name == "nt" else ["-lm"]),
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
        # mii_model_opts: the normal face and those asked for, at their size.
        self.assertIn("opts: expressions 0x21, 32 px", r.stdout)
        self.check_pack(r.stdout)
        self.check_head_lods(r.stdout, real=False)
        # The cache: the same Mii twice shares everything (each part, the
        # face); faces rebuilt at 32 px replace the 128 px ones in it; with
        # both models freed, nothing is left.
        m = re.search(r"cache: (\d+) of (\d+) parts and the face shared; entries (\d+) (\d+) (\d+) (\d+), "
                      r"users (\d+) (\d+) (\d+) (\d+); bytes (\d+) then (\d+)", r.stdout)
        self.assertTrue(m, r.stdout)
        self.assertEqual(m[1], m[2])
        entries, users = [int(m[k]) for k in range(3, 7)], [int(m[k]) for k in range(7, 11)]
        self.assertGreaterEqual(users[0], 2 * entries[0])   # synthetic parts repeat within a Mii too
        self.assertEqual(entries[2], entries[0])     # 32 px faces in place of the 128 px ones
        self.assertGreater(entries[1], entries[0])   # one model moved: both sizes held
        self.assertEqual((entries[3], users[3]), (0, 0))
        self.assertLess(int(m[12]), int(m[11]))

    def check_head_lods(self, stdout, real):
        # Coarser heads: valid levels, never more triangles than the level
        # before; with the Wii's own faces, each level fewer and the last
        # under 60% of the whole (each level may change a part by half a
        # pixel where it is drawn, so how far they go depends on the Mii).
        m = re.search(r"headlod: (\d+) (\d+) (\d+) (\d+) (\d+) triangles, (\d+) bad", stdout)
        self.assertTrue(m, stdout)
        tris = [int(m[k]) for k in range(1, 6)]
        self.assertEqual(m[6], "0", stdout)
        self.assertEqual(tris, sorted(tris, reverse=True), stdout)
        if real:
            self.assertEqual(len(set(tris)), 5, stdout)
            self.assertLess(tris[3], tris[0] * 0.6, stdout)
        return tris

    def test_head_lods_real_faces(self):
        if not os.environ.get("MII_REAL_RES"):
            self.skipTest("set MII_REAL_RES to your own RFL_Res.dat")
        for sex in ("0", "1"):
            r, _ = self.run_host(os.environ["MII_REAL_RES"], "-d", sex, "64", "64", "5")
            self.assertEqual(r.returncode, 0, r.stdout)
            print("head levels:", self.check_head_lods(r.stdout, real=True))
            # Strips: every triangle back, the same way round, and far fewer
            # vertices sent than as separate triangles.
            m = re.search(r"strips: (\d+) triangles sent as (\d+) vertices \((\d+) as triangles\), (\d+) wrong",
                          r.stdout)
            self.assertTrue(m, r.stdout)
            self.assertEqual(m[4], "0", r.stdout)
            self.assertLess(int(m[2]), int(m[3]) * 0.6, r.stdout)
            print("strips:", m[0])
            # The forehead and nose are the face texture's colour as the Wii
            # draws it, so they do not stand apart from the face.
            self.assertIn("skin: 6 of 6 the face texture's colour", r.stdout)

    def check_pack(self, stdout):
        # Packed meshes: fewer vertices than corners, each within its fixed
        # point's step; RGB5A3 within its bits (4-bit colour: 8; alpha 3 bits: 18).
        m = re.search(r"pack: (\d+) corners, (\d+) vertices; largest error: position ([\d.]+), "
                      r"normal ([\d.]+), coordinate ([\d.]+); rgb5a3 colour (\d+), alpha (\d+)", stdout)
        self.assertTrue(m, stdout)
        corners, verts = int(m[1]), int(m[2])
        self.assertLess(verts, corners)
        self.assertLess(float(m[3]), 0.01)
        self.assertLess(float(m[4]), 0.01)
        self.assertLess(float(m[5]), 0.001)
        self.assertLessEqual(int(m[6]), 8)
        self.assertLessEqual(int(m[7]), 19)
        return corners, verts

    def test_damaged_resource_refused(self):
        good = resource()
        for bad in (good[:100], good[:len(good) // 2], b"\0\x12\x03\x9c" + good[4:],
                    b"\0\x11" + good[2:]):
            res = self.dir / "bad.dat"
            res.write_bytes(bad)
            r, _ = self.run_host(res)
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertTrue(r.stdout.startswith("res: ") or r.stdout.count("model: "), r.stdout)

    def test_body_lods(self):
        # Levels of detail from the user's own bodies: four, each list the
        # body's own vertices, fewer triangles each level (each may move the
        # surface half a pixel where it is drawn), the last under 40%.
        if not os.environ.get("MII_REAL_BODIES"):
            self.skipTest("set MII_REAL_BODIES")
        for name in ("maleBody.brres", "femaleBody.brres"):
            r = self.run_body(pathlib.Path(os.environ["MII_REAL_BODIES"]) / name, MII_LOD="1")
            self.assertEqual(r.returncode, 0, r.stdout)
            full = int(re.search(r"lod 0: (\d+) triangles", r.stdout)[1])
            levels = re.findall(r"lod (\d): (\d+) triangles, (\d+) vertices sent, moved at most ([\d.]+), "
                                r"(\d+) bad", r.stdout)
            self.assertEqual(len(levels), 4, r.stdout)
            last = full
            for level, tris, sent, moved, bad in levels:
                tris = int(tris)
                self.assertEqual(bad, "0", r.stdout)
                self.assertLess(tris, last, r.stdout)
                self.assertLess(int(sent), 3 * tris * 0.7, r.stdout)   # strips, not separate triangles
                last = tris
            self.assertLess(last, full * 0.4, r.stdout)
            print(name, r.stdout.split("lod 0", 1)[1].strip().replace("\n", "; "))

    def test_pose_memo(self):
        # A Mii posed at a frame another Mii was posed at takes the body's
        # remembered pose: the same bones, bit for bit.
        if not os.environ.get("MII_REAL_BODIES"):
            self.skipTest("set MII_REAL_BODIES")
        r = self.run_body(pathlib.Path(os.environ["MII_REAL_BODIES"]) / "maleBody.brres")
        self.assertIn("memo: hits 1, same 1", r.stdout)

    def run_body(self, body, anim=None, frame=0, size=("64", "64"), **extra):
        res = self.dir / "RFL_Res.dat"
        res.write_bytes(resource())
        env = dict(os.environ, MII_BODY=str(body), MII_FRAME=str(frame), **extra)
        if anim:
            env["MII_ANIM"] = anim
        out = self.dir / "out"
        out.mkdir(exist_ok=True)
        return subprocess.run([str(self.exe), str(res), "-d", "0", size[0], size[1], "5", str(out)],
                              capture_output=True, text=True, timeout=120, env=env)

    def test_body_refused(self):
        # Not a BRRES, a BRRES header with nothing behind it, random bytes.
        for data in (b"not a model at all" * 10, b"bres\xfe\xff\0\0" + bytes(56),
                     b"bres\xfe\xff\0\0\0\0\x10\0\0\x10\0\x01" + os.urandom(4000)):
            body = self.dir / "body.brres"
            body.write_bytes(data)
            r = self.run_body(body)
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn("body: ", r.stdout)

    @unittest.skipUnless(os.environ.get("MII_REAL_BODIES"),
                         "set MII_REAL_BODIES to a folder with your own maleBody.brres and femaleBody.brres")
    def test_real_bodies(self):
        folder = pathlib.Path(os.environ["MII_REAL_BODIES"])
        for name in ("maleBody.brres", "femaleBody.brres"):
            body = folder / name
            r = self.run_body(body)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn("body: 20 bones", r.stdout)
            # The Mii Channel's look (sdk/mii 1.4.0): env0 (32 x 32 I8) for
            # the shirt and trousers, the shadow's 64 x 64 texture, its quad
            # (75 square) on shadow_model and its colour from mt_shadow's
            # TEV register 0. The shadow sized by the animation: gone as the
            # Mii is lifted (bodyGetDown scales it to 0.01), wider as it
            # falls flat (bodySlip_00), on the ground throughout.
            self.assertIn("look: env 32x32, shadow shadow_model 64x64 half 37.5 rgb 32 40 38", r.stdout)
            self.assertIn("shadow bodyWait: 120 frames, none 0, width 37.5 to 37.5, y 0.10 to 0.10", r.stdout)
            m = re.search(r"shadow bodyGetDown: \d+ frames, none (\d+), width ([\d.]+) to", r.stdout)
            self.assertTrue(m and int(m.group(1)) > 0 and float(m.group(2)) < 5, r.stdout)
            m = re.search(r"shadow bodySlip_00: \d+ frames, none 0, width [\d.]+ to ([\d.]+), y 0.10 to 0.10",
                          r.stdout)
            self.assertTrue(m and float(m.group(1)) > 45, r.stdout)
            # Where effects go (mii_fx_place): on the head bone, about 108
            # up, its X up the neck and its Y out of the face (+Z); the
            # ground at the feet; for an icon, the chin at the origin, the
            # face to the front.
            m = re.search(r"place: head [-\d.]+ ([\d.]+) [-\d.]+, up -?0.0\d 1.00 -?0.0\d, "
                          r"face -?0.0\d -?0.0\d 1.00, ground y 0.0", r.stdout)
            self.assertTrue(m and 90 < float(m.group(1)) < 130, r.stdout)
            self.assertIn("place icon: head 0.0 0.0 0.0, face 0.00 0.00 1.00", r.stdout)
            effects = os.environ.get("MII_REAL_EFFECTS")
            if effects:
                # The effects there (your own effect files): the sneeze's
                # splash about 110 in front of the face; talk's balloons in
                # front of the mouth; the zzz up over the head.
                r = self.run_body(body, MII_FX_DIR=effects)
                fx = {m.group(1): tuple(map(float, m.group(3, 4, 5))) for m in re.finditer(
                    r"fx (\w+): (\d+) shown, side ([-\d.]+), up ([-\d.]+), forward ([-\d.]+)", r.stdout)}
                self.assertEqual(len(fx), 3, r.stdout)
                side, up, fwd = fx["Nc_sneeze01"]
                self.assertTrue(80 < fwd < 140 and 0 < up < 50, fx)
                side, up, fwd = fx["Nc_talk01"]
                self.assertTrue(fwd > 20 and -20 < up < 80, fx)
                side, up, fwd = fx["Nc_zzz01"]
                self.assertTrue(up > 60, fx)
            # Faces for the animations (sdk/mii's own choice): eyes shut
            # asleep (5), a smile (1), the mouth going (99 then 6),
            # surprise then sorrow slipping (4, 3); none for Wait.
            self.assertIn("faces: Catnap_00 5, Greeting 1, Talk 99 6, Slip_00 4 3, Wait 99, none 99", r.stdout)
            anims =[line for line in r.stdout.splitlines() if line.startswith("anim ")]
            # All 21, Wallow and Slip_00 too (their rotation keys are the
            # 4-byte kind, sdk/mii 1.3.0).
            self.assertGreaterEqual(len(anims), 21, r.stdout)
            names = {line.split(":")[0][5:] for line in anims}
            self.assertLessEqual({"bodyWallow", "bodySlip_00"}, names, r.stdout)
            # The loop flag (CHR0 0x20): Wait, Walk, Run, Talk, Talk_01,
            # Catnap_00 and Wallow go round; the rest play once.
            loops = {line.split(":")[0][5:] for line in anims if line.endswith(", loops")}
            self.assertEqual(loops, {"bodyWait", "bodyWalk", "bodyRun", "bodyTalk", "bodyTalk_01",
                                     "bodyCatnap_00", "bodyWallow"}, r.stdout)
            # mii_anim_frame: Wait wraps (as drawing does), Greeting (96
            # frames, 1.6 s) holds its end and says it is done.
            self.assertIn("frame bodyWait 10.0 s: 600.00\n", r.stdout)
            self.assertIn("frame bodyGreeting 1.5 s: 90.00\n", r.stdout)
            self.assertIn("frame bodyGreeting 1.6 s: 95.99 done\n", r.stdout)
            self.assertIn("frame bodyGreeting 10.0 s: 95.99 done\n", r.stdout)
            # HBC's two animations at thousands of frames: every posed
            # vertex finite and near the body (the GPU is fed them as they
            # are). Not every animation: bodySlap_03 throws the body 2737
            # units.
            # The GPU's way (sdk/mii 2.0): every vertex through its bone's
            # palette matrix lands where CPU skinning puts it; its normal
            # points within 7 degrees (the GPU leaves out the scale's
            # inverse; exact at middle height and build); every primitive
            # of the display lists moved to GX_VTXFMT1.
            for size, least in ((("64", "64"), 0.9999), (("127", "127"), 0.99), (("0", "0"), 0.99)):
                r = self.run_body(body, size=size, MII_PALETTE="1")
                self.assertEqual(r.returncode, 0, r.stdout)
                m = re.search(r"palette: (\d+) poses, objects (\d+), matrices (\d+), worst position ([\d.]+), "
                              r"least normal dot ([\d.]+), primitives (\d+), on VTXFMT1 (\d+)", r.stdout)
                self.assertTrue(m, r.stdout)
                self.assertEqual(int(m.group(2)), 2, m.group(0))
                self.assertLess(float(m.group(4)), 0.01, m.group(0))
                self.assertGreater(float(m.group(5)), least, (size, m.group(0)))
                self.assertGreater(int(m.group(6)), 0, m.group(0))
                self.assertEqual(m.group(6), m.group(7), m.group(0))
            # Cross-fades: a pose faded into itself unchanged (the
            # quaternion round trip), weight 1 is the animation before
            # alone, and a fade from Wait to Greeting moves in small steps
            # (60 of them; the hand travels about 100 units).
            r = self.run_body(body, MII_FADE="1")
            self.assertEqual(r.returncode, 0, r.stdout)
            m = re.search(r"fade: self ([\d.]+), whole ([\d.]+), largest step ([\d.]+)", r.stdout)
            self.assertTrue(m, r.stdout)
            self.assertLess(float(m.group(1)), 0.001, m.group(0))
            self.assertLess(float(m.group(2)), 0.001, m.group(0))
            self.assertLess(float(m.group(3)), 5, m.group(0))
            # Out of the wave as HBC does it (Greeting held at its last
            # frame, Wait fading in over 0.4 s): no frame jumps. Playing
            # the finished wave on wraps it to its start: about 10 units in
            # one frame, the jump the user saw.
            m = re.search(r"out of the wave: largest frame step: wait alone ([\d.]+), held ([\d.]+), "
                          r"played on ([\d.]+)", r.stdout)
            self.assertTrue(m, r.stdout)
            self.assertLess(float(m.group(2)), 2, m.group(0))
            for anim in ("bodyWait", "bodyGreeting", "bodyWallow", "bodySlip_00"):
                r = self.run_body(body, anim, MII_SWEEP="1")
                self.assertEqual(r.returncode, 0, r.stdout)
                m = re.search(r"sweep: (\d+) poses, (\d+) non-finite, largest position ([\d.]+), "
                              r"normal ([\d.]+)", r.stdout)
                self.assertTrue(m, r.stdout)
                self.assertEqual(int(m.group(2)), 0, (anim, m.group(0)))
                self.assertLess(float(m.group(3)), 300, (anim, m.group(0)))
                self.assertLess(float(m.group(4)), 1.01, (anim, m.group(0)))
            for anim, frame in (("bodyWait", 37), ("bodyGreeting", 45), ("bodyWalk", 10), ("bodyRun", 7)):
                r = self.run_body(body, anim, frame)
                self.assertEqual(r.returncode, 0, (anim, r.stdout))
            # Idle stays put: through bodyWait no vertex of the shirt or
            # trousers strays far from where it starts (7.9 at most). Read
            # raw, the right ankle's X (179.1, -179.2, 179.1 degrees) swung
            # the foot a whole turn: 22.6.
            first, worst = None, 0
            for frame in range(0, 120, 4):
                r = self.run_body(body, "bodyWait", frame)
                self.assertEqual(r.returncode, 0, r.stdout)
                vs, on = [], False
                for line in (self.dir / "out" / "mii.obj").read_text().splitlines():
                    if line.startswith("o "):
                        on = line.startswith("o body_")
                    elif on and line.startswith("v "):
                        vs.append(tuple(map(float, line.split()[1:4])))
                if first is None:
                    first = vs
                worst = max(worst, max(sum((a - b) ** 2 for a, b in zip(p, q)) ** 0.5 for p, q in zip(first, vs)))
            self.assertLess(worst, 12, (name, worst))
            # Cut short anywhere: refused, never a crash.
            data = body.read_bytes()
            cut = self.dir / "cut.brres"
            for size in range(64, len(data), len(data) // 23):
                cut.write_bytes(data[:size])
                r = self.run_body(cut)
                self.assertIn(r.returncode, (0, 1), (size, r.stdout, r.stderr))

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
