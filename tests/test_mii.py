"""The Mii library (sdk/mii) on the PC, against a model of its own.

The bit fields are decoded here from docs/mii.md's table, the CRC is
Python's binascii.crc_hqx (CRC-16/XMODEM), and the database and Wii Remote
layouts are built here byte by byte, so the C code is checked against
something it does not share.

Set MII_REAL_DB to a copy of a Wii's /shared2/menu/FaceLib/RFL_DB.dat to
also check the library on real data (a scratch copy is edited, never the
file; nothing from it is printed). `tests/wii_mii.py backup` makes one."""

import binascii
import datetime
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import test_cdblog  # noqa: E402
from dolphin_mboard import test_mii  # noqa: E402

CC = test_cdblog.CC
OK, E_FORMAT, E_FULL, E_ARG, E_INVALID, E_EXISTS, E_EMPTY, E_CRC = 0, -2, -3, -5, -6, -7, -8, -9
DB_FILE_SIZE, DB_SIZE, DB_CRC, NODES, HDATA = 0xBE6C0, 0x1F1E0, 0x1F1DE, 0x1D08, 0x1F1E0
BLOCK = 0x2F0
REAL_DB = os.environ.get("MII_REAL_DB", "")

# docs/mii.md: (offset, [(name, bits), ...]) from the top bit down.
WORDS = [
    (0x00, [("_p0", 1), ("sex", 1), ("birth_month", 4), ("birth_day", 5), ("fav_color", 4), ("favorite", 1)]),
    (0x20, [("face_type", 3), ("face_color", 3), ("face_tex", 4), ("_p1", 3), ("local_only", 1), ("kind", 2)]),
    (0x22, [("hair_type", 7), ("hair_color", 3), ("hair_flip", 1), ("_p2", 5)]),
    (0x24, [("brow_type", 5), ("brow_rotate", 5), ("_p3", 6)]),
    (0x26, [("brow_color", 3), ("brow_scale", 4), ("brow_y", 5), ("brow_x", 4)]),
    (0x28, [("eye_type", 6), ("eye_rotate", 5), ("eye_y", 5)]),
    (0x2A, [("eye_color", 3), ("eye_scale", 4), ("eye_x", 4), ("_p4", 5)]),
    (0x2C, [("nose_type", 4), ("nose_scale", 4), ("nose_y", 5), ("_p5", 3)]),
    (0x2E, [("mouth_type", 5), ("mouth_color", 2), ("mouth_scale", 4), ("mouth_y", 5)]),
    (0x30, [("glass_type", 4), ("glass_color", 3), ("glass_scale", 4), ("glass_y", 5)]),
    (0x32, [("mustache", 2), ("beard_type", 2), ("beard_color", 3), ("beard_scale", 4), ("beard_y", 5)]),
    (0x34, [("mole", 1), ("mole_scale", 4), ("mole_y", 5), ("mole_x", 5), ("_p6", 1)]),
]

# RFLiCheckValidInfo's limits: (field, max, the reason mii_check gives above it)
LIMITS = [("face_type", 7, "face type"), ("face_color", 5, "face colour"), ("face_tex", 11, "face features"),
          ("hair_type", 71, "hair type"), ("hair_color", 7, "hair colour"), ("hair_flip", 1, "hair flip"),
          ("eye_type", 47, "eye type"), ("eye_color", 5, "eye colour"), ("eye_scale", 7, "eye scale"),
          ("eye_rotate", 7, "eye rotation"), ("eye_x", 12, "eye X"), ("eye_y", 18, "eye Y"),
          ("brow_type", 23, "eyebrow type"), ("brow_color", 7, "eyebrow colour"),
          ("brow_scale", 8, "eyebrow scale"), ("brow_rotate", 11, "eyebrow rotation"),
          ("brow_x", 12, "eyebrow X"), ("brow_y", 18, "eyebrow Y"),
          ("nose_type", 11, "nose type"), ("nose_scale", 8, "nose scale"), ("nose_y", 18, "nose Y"),
          ("mouth_type", 23, "mouth type"), ("mouth_color", 2, "mouth colour"),
          ("mouth_scale", 8, "mouth scale"), ("mouth_y", 18, "mouth Y"),
          ("mustache", 3, "mustache"), ("beard_type", 3, "beard type"), ("beard_color", 7, "beard colour"),
          ("beard_scale", 8, "beard scale"), ("beard_y", 16, "beard Y"),
          ("glass_type", 8, "glasses type"), ("glass_color", 5, "glasses colour"),
          ("glass_scale", 7, "glasses scale"), ("glass_y", 20, "glasses Y"),
          ("mole_scale", 8, "mole scale"), ("mole_x", 16, "mole X"), ("mole_y", 30, "mole Y"),
          ("fav_color", 11, "favourite colour")]


def decode(raw):
    out = {}
    for off, fields in WORDS:
        w = struct.unpack_from(">H", raw, off)[0]
        used = 0
        for name, bits in fields:
            used += bits
            out[name] = w >> (16 - used) & ((1 << bits) - 1)
    out["height"], out["build"] = raw[0x16], raw[0x17]
    return out


def encode(base, **changes):
    """base with fields changed, in this file's own packing."""
    raw = bytearray(base)
    for off, fields in WORDS:
        vals = decode(raw)
        if not any(n in changes for n, _ in fields):
            continue
        w, used = 0, 0
        for name, bits in fields:
            used += bits
            w |= (changes.get(name, vals[name]) & ((1 << bits) - 1)) << (16 - used)
        struct.pack_into(">H", raw, off, w)
    for k in ("height", "build"):
        if k in changes:
            raw[0x16 if k == "height" else 0x17] = changes[k]
    if "name" in changes:
        raw[2:0x16] = changes["name"].encode("utf-16-be").ljust(20, b"\0")
    if "id" in changes:
        raw[0x18:0x20] = changes["id"]
    return bytes(raw)


def crc(data):
    return binascii.crc_hqx(bytes(data), 0)


FREE_NODE = bytes(8) + bytes.fromhex("7fff7fff")   # RFLiClearTableData: next and prev -1


def empty_db(isolation=False):
    """RFL_DB.dat as the Wii Menu's format leaves it (RFL_Format.c): every
    hidden node free, its next and prev -1, as on the bench Wii."""
    f = bytearray(DB_FILE_SIZE)
    f[0:4] = b"RNOD"
    struct.pack_into(">I", f, 0x1CEC, 0x80000000 if isolation else 0)
    f[0x1D00:0x1D04] = b"RNHD"
    struct.pack_into(">hh", f, 0x1D04, -1, -1)
    f[NODES:NODES + 10000 * 12] = FREE_NODE * 10000
    struct.pack_into(">H", f, DB_CRC, crc(f[:DB_CRC]))
    return f


def node(f, i):
    nid = bytes(f[NODES + i * 12:NODES + i * 12 + 8])
    a, b = struct.unpack_from(">HH", f, NODES + i * 12 + 8)

    def s15(v):
        v &= 0x7fff
        return v - 0x8000 if v & 0x4000 else v
    return nid, a >> 15, s15(a), s15(b)


def mii_with(i, **kw):
    """test_mii() with its own creation ID."""
    return encode(test_mii(), id=struct.pack(">II", 0x80000000 | (0x100 + i), 0xC0FFEE00), **kw)


@unittest.skipUnless(CC, "no C compiler")
class MiiLibrary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = pathlib.Path(tempfile.mkdtemp(prefix="mii-"))
        cls.exe = cls.dir / "mii_host"
        subprocess.run([CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-D_CRT_SECURE_NO_WARNINGS", "-DMII_NO_OGC",
                        "-I", str(root / "sdk/mii"), str(root / "tests/mii_host/main.c"),
                        str(root / "sdk/mii/mii.c"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def host(self, *args):
        out = subprocess.run([str(self.exe), *map(str, args)], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout.splitlines()

    def check(self, raw):
        code, why = self.host("check", raw.hex())[0].split(" ", 1)
        return int(code), why

    # ---- a Mii -------------------------------------------------------------

    def test_fields_match_the_documented_layout(self):
        for raw in (test_mii(), bytes(range(74)), bytes([0xff] * 74), os.urandom(74)):
            got = dict(line.split("=", 1) for line in self.host("fields", raw.hex()))
            want = decode(raw)
            for k, v in want.items():
                if not k.startswith("_"):
                    self.assertEqual(int(got[k]), v, k)
            self.assertEqual(got["name"], raw[2:0x16].hex())
            self.assertEqual(got["creator"], raw[0x36:0x4A].hex())
            self.assertEqual(got["id"], raw[0x18:0x20].hex())
            self.assertEqual(got["pack"], raw.hex(), "pack(unpack(x)) is x, padding bits too")

    def test_the_menu_accepted_mii_passes(self):
        self.assertEqual(self.check(test_mii()), (OK, "-"))

    def test_each_limit(self):
        base = test_mii()
        for name, top, what in LIMITS:
            bits = next(b for _, fs in WORDS for n, b in fs if n == name)
            self.assertEqual(self.check(encode(base, **{name: top}))[0], OK, name)
            if top + 1 < 1 << bits:
                self.assertEqual(self.check(encode(base, **{name: top + 1})),
                                 (E_INVALID, f"{what} above {top}"), name)
        self.assertEqual(self.check(encode(base, brow_y=2)), (E_INVALID, "eyebrow Y below 3"))
        self.assertEqual(self.check(encode(base, brow_y=3))[0], OK)
        for h in (128, 129, 255):
            self.assertEqual(self.check(encode(base, height=h))[0], OK if h <= 128 else E_INVALID)
            self.assertEqual(self.check(encode(base, build=h))[0], OK if h <= 128 else E_INVALID)

    def test_birthday_name_and_id(self):
        base = test_mii()
        for (m, d), ok in {(0, 0): True, (2, 29): True, (2, 30): False, (4, 31): False, (12, 31): True,
                           (0, 5): False, (5, 0): False, (13, 1): False}.items():
            self.assertEqual(self.check(encode(base, birth_month=m, birth_day=d))[0], OK if ok else E_INVALID,
                             (m, d))
        self.assertEqual(self.check(encode(base, name="")), (E_INVALID, "no name"))
        self.assertEqual(self.check(encode(base, id=bytes(8))), (E_INVALID, "no creation ID (mii_make_id)"))
        special = encode(base, id=bytes.fromhex("0000000500000001"))
        self.assertEqual(self.check(special)[0], E_INVALID)
        self.assertEqual(self.check(encode(special, local_only=1))[0], OK)

    def test_default_and_random_miis_pass(self):
        for sex in (0, 1):
            raw = bytes.fromhex(self.host("default", sex)[0])
            self.assertEqual(decode(raw)["sex"], sex)
            self.assertEqual(raw[2:8].decode("utf-16-be"), "Mii")
            self.assertEqual(self.check(encode(raw, id=bytes.fromhex("8000000100000001")))[0], OK)
        seen = {k: set() for k in ("sex", "hair_type", "eye_type", "glass_type", "beard_type", "face_color")}
        for sex, age in ((-1, -1), (0, 0), (0, 2), (1, 1)):
            for line in self.host("random", sex, age, 12345, 300):
                raw, code, _ = line.split(" ", 2)
                self.assertEqual(int(code), OK, line)
                d = decode(bytes.fromhex(raw))
                if sex >= 0:
                    self.assertEqual(d["sex"], sex)
                if age == 0:
                    self.assertEqual(d["beard_type"] + d["mustache"], 0, "no beards on children")
                for k in seen:
                    seen[k].add(d[k])
        self.assertEqual(seen["sex"], {0, 1})
        self.assertGreater(len(seen["hair_type"]), 50)
        self.assertGreater(len(seen["eye_type"]), 40)
        self.assertGreater(len(seen["glass_type"]), 5)
        self.assertEqual(len(seen["face_color"]), 6)
        a = self.host("random", -1, -1, 7, 3)
        self.assertEqual(a, self.host("random", -1, -1, 7, 3), "the same seed, the same Miis")
        self.assertNotEqual(a, self.host("random", -1, -1, 8, 3))

    def test_names(self):
        for text, kept, back in (("Mii", 3, "Mii"), ("Zoë", 3, "Zoë"), ("ねこ", 2, "ねこ"),
                                 ("ABCDEFGHIJKL", 10, "ABCDEFGHIJ"), ("hi\U0001F600", 4, "hi\U0001F600"),
                                 ("ABCDEFGHI\U0001F600", 9, "ABCDEFGHI"), ("", 0, "")):
            n, h = (self.host("name", text.encode().hex())[0].split(" ") + [""])[:2]
            self.assertEqual((int(n), bytes.fromhex(h).decode()), (kept, back), text)

    def test_creation_id(self):
        # 2026-10-23 12:00:00 local, as the Wii's time base (60.75 MHz from 2000).
        secs = (datetime.date(2026, 10, 23) - datetime.date(2000, 1, 1)).days * 86400 + 12 * 3600
        ticks = secs * 60750000
        for mac in (bytes.fromhex("0017ab123456"), bytes.fromhex("e00c7f123456")):
            nintendo = mac[:3] == bytes.fromhex("0017ab")
            home = (sum(mac[:3]) & (0xff if nintendo else 0x7f)) << 24 | int.from_bytes(mac[3:], "big")
            idb, flags, since, home_word = self.host("id", ticks, mac.hex())[0].split(" ")
            word0 = ((ticks - 11505369649262175) >> 2) // 60750000 & 0x0FFFFFFF | 0x80000000
            self.assertEqual(bytes.fromhex(idb), struct.pack(">II", word0, home))
            self.assertEqual(int(flags, 16), 0x80000000)
            self.assertEqual(int(home_word, 16), home)
            # 4-second units since 2006-01-01: within 4 s of the real gap.
            gap = secs - (datetime.date(2006, 1, 1) - datetime.date(2000, 1, 1)).days * 86400
            self.assertLessEqual(abs(int(since) - gap), 4)

    def test_store_and_import(self):
        raw = test_mii()
        out = bytes.fromhex(self.host("store", raw.hex())[0])
        self.assertEqual(out, raw + struct.pack(">H", crc(raw)))
        self.assertEqual(crc(out), 0)
        self.assertEqual(self.host("import", out.hex())[0], f"0 {raw.hex()}")
        self.assertEqual(self.host("import", raw.hex())[0], f"0 {raw.hex()}")
        bad = out[:-1] + bytes([out[-1] ^ 1])
        self.assertEqual(self.host("import", bad.hex())[0].split()[0], str(E_CRC))
        self.assertEqual(self.host("import", out[:75].hex())[0].split()[0], str(E_ARG))

    # ---- the database ---------------------------------------------------------

    def db(self, f, *ops, name="db"):
        path = self.dir / name
        path.write_bytes(f)
        lines = self.host("db", path, *ops)
        return lines, bytearray(path.read_bytes())

    def assert_sealed(self, f):
        self.assertEqual(len(f), DB_FILE_SIZE)
        self.assertEqual(struct.unpack_from(">H", f, DB_CRC)[0], crc(f[:DB_CRC]))

    def test_refuses_a_database_it_cannot_trust(self):
        good = empty_db()
        self.assertEqual(self.db(good, "count")[0][:2], ["attach 0", "count 0 0"])
        bad_crc = bytearray(good)
        bad_crc[0x10] ^= 1
        short = good[:DB_FILE_SIZE - 0x40]
        bad_magic = bytearray(good)
        bad_magic[0:4] = b"RNOX"
        struct.pack_into(">H", bad_magic, DB_CRC, crc(bad_magic[:DB_CRC]))
        for f in (bad_crc, short, bad_magic):
            lines, after = self.db(f, "add", test_mii().hex())
            self.assertEqual(lines, [f"attach {E_FORMAT}"])
            self.assertEqual(after, f, "a refused file is left as it was")

    def test_slots(self):
        a, b = mii_with(1), mii_with(2)
        lines, f = self.db(empty_db(), "add", a.hex(), "add", b.hex(), "find", a[0x18:0x20].hex(),
                           "put", 5, a.hex(), "put", 0, a.hex(), "count", "get", 1, "get", 7,
                           "rm", 1, "rm", 1, "count", "put", 100, a.hex(), "add", encode(a, brow_y=1).hex())
        self.assertEqual(lines, ["attach 0", "add 0", "add 1", "find 0", f"put {E_EXISTS}", "put 0", "count 2 0",
                                 f"get 0 {b.hex()}", f"get {E_EMPTY} ", "rm 0", f"rm {E_EMPTY}", "count 1 0",
                                 f"put {E_ARG}", f"add {E_INVALID}", "dirty"])
        self.assert_sealed(f)
        self.assertEqual(bytes(f[4:4 + 74]), a)
        self.assertEqual(bytes(f[4 + 74:4 + 148]), bytes(74), "a removed slot is all zero")
        self.assertEqual(f[4 + 148:DB_CRC], empty_db()[4 + 148:DB_CRC])

    def test_a_full_database(self):
        ops = []
        for i in range(101):
            ops += ["add", mii_with(i).hex()]
        lines, f = self.db(empty_db(), *ops)
        self.assertEqual(lines[1:101], [f"add {i}" for i in range(100)])
        self.assertEqual(lines[101], f"add {E_FULL}")
        self.assert_sealed(f)

    def test_another_wiis_mii_is_kept_local(self):
        mine = mii_with(1)                                       # second word C0FFEE00
        theirs = encode(mii_with(2), id=struct.pack(">II", 0x80000102, 0x12345678))
        lines, f = self.db(empty_db(), "home", "c0ffee00", "add", mine.hex(), "add", theirs.hex())
        self.assertEqual(lines[2:4], ["add 0", "add 1"])
        self.assertEqual(decode(f[4:])["local_only"], 0)
        self.assertEqual(decode(f[4 + 74:])["local_only"], 1)
        lines, f = self.db(empty_db(), "add", theirs.hex())        # home unknown: as given
        self.assertEqual(decode(f[4:])["local_only"], 0)

    def test_hidden_database(self):
        a, b, c = mii_with(1), mii_with(2, sex=0), mii_with(3)
        lines, f = self.db(empty_db(), "hput", a.hex(), "hput", b.hex(), "hput", c.hex(), "hlist",
                           "hput", b.hex(), "hlist", "hget", 1, "hfind", c[0x18:0x20].hex(),
                           "hput", encode(a, local_only=1).hex())
        self.assertEqual(lines[1:10], ["hput 0", "hput 1", "hput 2", "hlist 0 1 2", "hput 1", "hlist 0 1 2",
                                       "hget 0 " + (b[:0x36] + bytes(20)).hex()
                                       .replace(b[:2].hex(), struct.pack(">H", struct.unpack(">H", b[:2])[0]
                                                                        & ~0x3fe1).hex(), 1),
                                       "hfind 2", f"hput {E_INVALID}"])
        self.assertEqual(lines[10], "dirty 0 1 2")
        self.assert_sealed(f)
        self.assertEqual(struct.unpack_from(">hh", f, 0x1D04), (0, 2))
        self.assertEqual(node(f, 0), (a[0x18:0x20], 1, 1, -1))
        self.assertEqual(node(f, 1), (b[0x18:0x20], 0, 2, 0))
        self.assertEqual(node(f, 2), (c[0x18:0x20], 1, -1, 1))
        h = f[HDATA + 0x40:HDATA + 0x80]
        self.assertEqual(struct.unpack(">H", h[:2])[0] & 0x3fe1, 0, "no birthday, not favourite")
        self.assertEqual(h[2:0x36], b[2:0x36])
        self.assertEqual(h[0x36:], bytes(10))

        # Removing the middle one relinks; adding to the 100 slots takes it out.
        lines, g = self.db(f, "hrm", 1, "hlist", "add", a.hex(), "hlist", "count", "hput", a.hex())
        self.assertEqual(lines[1:7], ["hrm 0", "hlist 0 2", "add 0", "hlist 2", "count 1 1", f"hput {E_EXISTS}"])
        self.assertEqual(node(g, 0), (bytes(8), 0, -1, -1), "a freed node as RFL leaves it")
        self.assertEqual(bytes(g[NODES:NODES + 12]), FREE_NODE)
        self.assertEqual(node(g, 2), (c[0x18:0x20], 1, -1, -1))
        self.assertEqual(struct.unpack_from(">hh", g, 0x1D04), (2, 2))
        self.assert_sealed(g)

    def test_a_full_hidden_database_drops_the_oldest(self):
        f = empty_db()
        # 10,000 nodes chained 0..9999 (oldest 0), ids 1..10000.
        for i in range(10000):
            struct.pack_into(">II", f, NODES + i * 12, 0x80000000 | (i + 1), 0x0BADF00D)
            struct.pack_into(">HH", f, NODES + i * 12 + 8, (i + 1 if i < 9999 else 0x7fff),
                             (i - 1) & 0x7fff if i else 0x7fff)
            f[HDATA + i * 0x40 + 0x18:HDATA + i * 0x40 + 0x20] = f[NODES + i * 12:NODES + i * 12 + 8]
        struct.pack_into(">hh", f, 0x1D04, 0, 9999)
        struct.pack_into(">H", f, DB_CRC, crc(f[:DB_CRC]))
        new = mii_with(77)
        lines, g = self.db(f, "count", "hput", new.hex(), "count", "hfind", "800000010badf00d")
        self.assertEqual(lines[1:5], ["count 0 10000", "hput 0", "count 0 10000", f"hfind {E_EMPTY}"])
        self.assertEqual(struct.unpack_from(">hh", g, 0x1D04), (1, 0))
        self.assertEqual(node(g, 0)[0], new[0x18:0x20])
        self.assertEqual(node(g, 0)[2:], (-1, 9999))
        self.assertEqual(node(g, 1)[3], -1)
        self.assertEqual(node(g, 9999)[2], 0)
        self.assert_sealed(g)

    # ---- a Wii Remote --------------------------------------------------------------

    def remote(self, mem, *ops):
        src, dst = self.dir / "rin", self.dir / "rout"
        src.write_bytes(mem)
        lines = self.host("remote", src, dst, *ops)
        return lines, dst.read_bytes()

    def test_remote_from_new(self):
        a = mii_with(1, favorite=1)
        lines, out = self.remote(bytes(2 * BLOCK), "put", 3, a.hex(), "get", 3, "get", 4)
        self.assertEqual(lines[0], "parse 0 copy -1 secret 000")
        stored = encode(a, favorite=0)
        self.assertEqual(lines[1:], ["put 0", f"get 0 {stored.hex()}", f"get {E_EMPTY} "])
        block = out[:BLOCK]
        self.assertEqual(out[BLOCK:], block)
        self.assertEqual(block[:4], b"RNCD")
        self.assertEqual(block[8 + 3 * 74:8 + 4 * 74], stored)
        self.assertEqual(struct.unpack_from(">HH", block, 0x2EC), (0, crc(block[:0x2EE])))
        self.assertEqual(crc(block), 0)

    def built(self, slots, secret=0, deleted=0, magic=b"RNCD", good_crc=True):
        b = bytearray(BLOCK)
        b[0:4] = magic
        struct.pack_into(">H", b, 4, secret)
        for i, raw in slots.items():
            b[8 + i * 74:8 + (i + 1) * 74] = raw
        struct.pack_into(">H", b, 0x2EC, deleted)
        struct.pack_into(">H", b, 0x2EE, crc(b[:0x2EE]) ^ (0 if good_crc else 1))
        return bytes(b)

    def test_remote_copies(self):
        a, b = mii_with(1), mii_with(2)
        first, second = self.built({0: a}, secret=0x001), self.built({1: b})
        lines, _ = self.remote(first + second, "get", 0, "get", 1)
        self.assertEqual(lines, ["parse 0 copy 0 secret 001", f"get 0 {a.hex()}", f"get {E_EMPTY} "])
        broken = self.built({0: a}, good_crc=False)
        lines, _ = self.remote(broken + second, "get", 0, "get", 1)
        self.assertEqual(lines, ["parse 0 copy 1 secret 000", f"get {E_EMPTY} ", f"get 0 {b.hex()}"])
        lines, _ = self.remote(self.built({0: a}, magic=b"XXXX") + bytes(BLOCK))
        self.assertEqual(lines, ["parse 0 copy -1 secret 000"])

    def test_remote_drops_what_rfl_drops(self):
        a, b, c = mii_with(1), mii_with(2), encode(mii_with(3), brow_y=0)
        lines, out = self.remote(self.built({0: a, 1: b, 2: c}, secret=0x007, deleted=0x002) + bytes(BLOCK),
                                 "get", 0, "get", 1, "get", 2, "rm", 0)
        self.assertEqual(lines, ["parse 0 copy 0 secret 001", f"get 0 {a.hex()}", f"get {E_EMPTY} ",
                                 f"get {E_EMPTY} ", "rm 0"])
        self.assertEqual(out[:BLOCK], self.built({}))

    # ---- the 2026-10-24 audit: each defect it found -----------------------------

    def test_flag_bits_are_checked_not_cut(self):
        base = test_mii()
        for args, why in ((["favorite=2"], "favourite above 1"), (["local_only=2"], "local only above 1"),
                          (["kind=4"], "kind above 3")):
            line = self.host("check", base.hex(), *args)[0]
            self.assertEqual(line, f"{E_INVALID} {why}", args)
        # A special ID with local_only 2 would pack as 0 and fail later: refused now.
        special = encode(base, id=bytes.fromhex("0000000500000001"))
        self.assertEqual(self.host("check", special.hex(), "local_only=2")[0].split()[0], str(E_INVALID))

    def test_a_temporary_id_is_not_special(self):
        # RFLiIsSpecialID excludes temporary IDs: RFL's check passes such a Mii.
        temp = encode(test_mii(), id=bytes.fromhex("a000000500000001"))
        self.assertEqual(self.check(temp), (OK, "-"))
        lines, _ = self.db(empty_db(), "add", temp.hex(), "hput", temp.hex())
        self.assertEqual(lines[1:3], [f"add {E_INVALID}", f"hput {E_INVALID}"], "never stored on a Wii")

    def test_names_that_are_not_utf8(self):
        fffd = "�"
        for raw, kept, back in ((b"\xe3", 1, fffd), (b"A\xc0\x80B", 4, f"A{fffd}{fffd}B"),
                                (b"\xed\xa0\x80", 1, fffd), (b"\xf4\x90\x80\x80", 1, fffd),
                                (b"\xf8x", 2, f"{fffd}x"), (b"\xe3\x81x", 2, f"{fffd}x"),
                                (b"\xe3\x81\x82", 1, "あ")):
            n, h = (self.host("name", raw.hex())[0].split(" ") + [""])[:2]
            self.assertEqual((int(n), bytes.fromhex(h).decode()), (kept, back), raw)

    def test_free_slots(self):
        lines, _ = self.db(empty_db(), "free", "add", mii_with(1).hex(), "free", "rm", 0, "free")
        self.assertEqual(lines[1:6], ["free 0", "add 0", "free 1", "rm 0", "free 0"])
        lines, _ = self.remote(self.built({0: mii_with(1), 2: mii_with(2)}) + bytes(BLOCK), "free")
        self.assertEqual(lines[1], "free 1")
        full = self.built({i: mii_with(i) for i in range(10)})
        lines, _ = self.remote(full + bytes(BLOCK), "free")
        self.assertEqual(lines[1], f"free {E_FULL}")

    def test_a_damaged_remote_is_not_an_empty_one(self):
        a = mii_with(1)
        both_bad = self.built({0: a}, good_crc=False) + self.built({0: a}, good_crc=False)
        lines, _ = self.remote(both_bad)
        self.assertEqual(lines[0], f"parse {E_FORMAT} copy -1 secret 000")
        lines, _ = self.remote(self.built({0: a}, good_crc=False) + bytes(BLOCK))
        self.assertEqual(lines[0].split()[1], str(E_FORMAT))
        lines, _ = self.remote(bytes(2 * BLOCK))
        self.assertEqual(lines[0], "parse 0 copy -1 secret 000", "a new Remote: an empty set")

    def broken_links(self, change):
        """A database with three hidden Miis (0 <- 1 <- 2), then change(f)
        breaks it, and the CRC is made right again."""
        lines, f = self.db(empty_db(), "hput", mii_with(1).hex(), "hput", mii_with(2).hex(),
                           "hput", mii_with(3).hex())
        self.assertEqual(lines[1:4], ["hput 0", "hput 1", "hput 2"])
        change(f)
        struct.pack_into(">H", f, DB_CRC, crc(f[:DB_CRC]))
        return f

    def test_hidden_links_a_crc_does_not_cover(self):
        def node_field(i, off, v):
            return lambda f: struct.pack_into(">H", f, NODES + i * 12 + off, v)
        cases = {
            "prev past the table": node_field(0, 10, 10016),
            "next past the table": node_field(2, 8, 12000),
            "a loop": node_field(2, 8, 0),
            "a prev that does not point back": node_field(1, 10, 2),
            "a used node off the list": lambda f: struct.pack_into(">H", f, NODES + 1 * 12 + 8, 0x7fff),
            "the tail not the list's end": lambda f: struct.pack_into(">h", f, 0x1D06, 1),
            "head past the table": lambda f: struct.pack_into(">h", f, 0x1D04, 10000),
        }
        for what, change in cases.items():
            f = self.broken_links(change)
            lines, after = self.db(f, "hrm", 0, name="broken")
            self.assertEqual(lines, [f"attach {E_FORMAT}"], what)
            self.assertEqual(after, f, f"{what}: nothing written")
        lines, _ = self.db(self.broken_links(lambda f: None), "hlist", "count")
        self.assertEqual(lines[:3], ["attach 0", "hlist 0 1 2", "count 0 3"], "the unbroken file")

    # ---- real data -------------------------------------------------------------------

    @unittest.skipUnless(REAL_DB and pathlib.Path(REAL_DB).is_file(), "no RFL_DB.dat copy (MII_REAL_DB)")
    def test_a_real_database(self):
        real = pathlib.Path(REAL_DB).read_bytes()
        lines, after = self.db(real, "count", "iso", name="real")
        self.assertEqual(lines[0], "attach 0")
        self.assertEqual(after, real, "only reading changes nothing")
        used = [i for i in range(100) if any(real[4 + i * 74 + 0x18:4 + i * 74 + 0x20])]
        self.assertEqual(lines[1], f"count {len(used)} {lines[1].split()[2]}")
        for i in used:
            raw = real[4 + i * 74:4 + (i + 1) * 74]
            got = dict(line.split("=", 1) for line in self.host("fields", raw.hex()))
            self.assertEqual(got["pack"], raw.hex(), f"slot {i} round trip")
            self.assertEqual(self.check(raw)[0], OK, f"slot {i} passes the Wii's check")
        # An edit on a scratch copy: one Mii in, then out again, leaves the file as it was.
        new = mii_with(99)
        lines, f = self.db(real, "add", new.hex(), name="real")
        slot = int(lines[1].split()[1])
        self.assertNotIn(slot, used)
        self.assert_sealed(f)
        lines, g = self.db(f, "rm", slot, name="real")
        self.assertEqual(bytes(g), real)
        # Into the hidden database and out: the table and slots as they were
        # (only the hidden Mii's data stays, as RFL leaves it).
        lines, h = self.db(real, "hput", new.hex(), name="real")
        i = int(lines[1].split()[1])
        lines, h = self.db(h, "hrm", i, name="real")
        self.assertEqual(lines[1], "hrm 0")
        self.assertEqual(bytes(h[:HDATA]), real[:HDATA])
        self.assertEqual(bytes(h[HDATA + (i + 1) * 0x40:]), real[HDATA + (i + 1) * 0x40:])


if __name__ == "__main__":
    unittest.main()
