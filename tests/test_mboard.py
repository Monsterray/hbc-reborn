"""The Message Board library (sdk/mboard) on PC images.

Play history: the same sessions as tests/test_cdblog.py must give the same
bytes as the reference the Wii Menu accepted (tools/msgboard/cdb_log.py).

Receive box: a post must leave what the SDK's NWC24CommitMsg() leaves for a
game's message to its own Wii (docs/mboard.md), checked here with a FAT
reader of its own; a post it refuses must leave both files as they were.
Set MBOARD_REAL_BOX to a folder holding a Wii's wc24recv.ctl and .mbx to
also post into a copy of those."""

import base64
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

CC = test_cdblog.CC
BOX_SIZE = 0x700000
CTL_SIZE = 0x8000
FAT1, FAT_LEN = 0x20, 0x7000
FAT2 = FAT1 + FAT_LEN
ROOT = FAT2 + FAT_LEN
DATA = ROOT + 0x1000
CL = 0x200
NCLUSTERS = (BOX_SIZE - DATA) // CL + 2

WII = 1234567890123456
DOMAIN = "@wii.com"
APP, GROUP = 0x4F484243, 0x3031  # 'OHBC', "01"
UTC, LOCAL = "2026-10-22T18:30:05", "2026-10-22T11:30:05"
E_IO, E_FORMAT, E_FULL, E_ARG, E_NO_ID = -1, -2, -3, -5, -7


def empty_ctl():
    """As the SDK's CreateCtrlFile() makes it: every entry free, chained
    through its app_id word."""
    ctl = bytearray(CTL_SIZE)
    struct.pack_into(">4s9I", ctl, 0, b"WcTf", 4, 0, 255, 0, CTL_SIZE, 1, 0x80, 0, BOX_SIZE)
    ctl[0x58:0x7f] = b"0" * 39
    for off in range(0x80, CTL_SIZE, 0x80):
        struct.pack_into(">I", ctl, off + 0x0C, (off + 0x80) % CTL_SIZE)
    return ctl


def lfn_entry(short, name):
    s = 0
    for ch in short:
        s = (((s & 1) << 7) | (s >> 1)) + ch & 0xff
    units = (name.encode("utf-16-le") + b"\0\0").ljust(26, b"\xff")
    e = bytearray(32)
    e[0], e[11], e[13] = 0x41, 0x0f, s
    e[1:11], e[14:26], e[28:32] = units[0:10], units[10:22], units[22:26]
    return e


def dir_entry(short, attr, cluster, size):
    e = bytearray(32)
    e[0:11], e[11], e[13] = short, attr, 1
    struct.pack_into("<HI", e, 26, cluster, size)
    return e


def empty_mbx(with_mb=True):
    """As the Wii Menu leaves an empty receive box (the Dolphin profile's
    copy): a FAT16 VFF holding only the folder "mb"."""
    img = bytearray(BOX_SIZE)
    img[0:16] = bytes.fromhex("56464620feff010000700000" "00200000")
    for fat in (FAT1, FAT2):
        img[fat:fat + 4] = b"\xf0\xff\xff\xff"
    if with_mb:
        for fat in (FAT1, FAT2):
            img[fat + 4:fat + 6] = b"\xff\xff"
        img[ROOT:ROOT + 32] = lfn_entry(b"MB         ", "mb")
        img[ROOT + 32:ROOT + 64] = dir_entry(b"MB         ", 0x10, 2, 0)
        img[DATA:DATA + 32] = dir_entry(b".          ", 0x10, 2, 0)
        img[DATA + 32:DATA + 64] = dir_entry(b"..         ", 0x10, 0, 0)
    return img


class Fat:
    """Just enough FAT16 to list a folder and read a file."""

    def __init__(self, img):
        self.img = img

    def next(self, c):
        return struct.unpack_from("<H", self.img, FAT1 + 2 * c)[0]

    def chain(self, c):
        out = []
        while 2 <= c < 0xfff8:
            out.append(c)
            c = self.next(c)
        return out

    def entries(self, cluster):
        if cluster == 0:
            areas = [(ROOT, 0x1000)]
        else:
            areas = [(DATA + (c - 2) * CL, CL) for c in self.chain(cluster)]
        lfn = None
        for start, size in areas:
            for off in range(start, start + size, 32):
                e = self.img[off:off + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xe5:
                    lfn = None
                elif e[11] == 0x0f:
                    lfn = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le").split("\0")[0]
                else:
                    yield {"short": bytes(e[0:11]), "long": lfn, "attr": e[11],
                           "cluster": struct.unpack_from("<H", e, 26)[0],
                           "size": struct.unpack_from("<I", e, 28)[0]}
                    lfn = None

    def read(self, ent):
        data = b"".join(self.img[DATA + (c - 2) * CL:DATA + (c - 1) * CL]
                        for c in self.chain(ent["cluster"]))
        return data[:ent["size"]]

    def free(self):
        return sum(1 for c in range(2, NCLUSTERS) if self.next(c) == 0)


def expected_message(msg_id, title, body, utc=UTC):
    t = datetime.datetime.strptime(utc, "%Y-%m-%dT%H:%M:%S")
    unix = int(t.replace(tzinfo=datetime.timezone.utc).timestamp())
    minutes = (unix + 2208988800) // 60
    addr = f"w{WII:016d}{DOMAIN}"
    head = (f"Date: {t.day:02d} {t.strftime('%b')} {t.year} {t:%H:%M:%S} -0000\r\n"
            f"From: {addr}\r\nTo: {addr}\r\n"
            f"Message-Id: <{msg_id:05X}{WII >> 32:08X}{WII & 0xffffffff:08X}{minutes:08X}{DOMAIN}>\r\n"
            f"X-Wii-AppId: 2-{APP:08X}-{GROUP:04X}\r\nX-Wii-Cmd: 00040001\r\n")
    alt = title.encode("utf-16-be") + b"\0\0"
    head += "X-Wii-AltName:" + "".join(
        " " + base64.b64encode(alt[i:i + 42]).decode() + "\r\n" for i in range(0, len(alt), 42))
    head += ("X-Wii-MB-NoReply: -\r\nMIME-Version: 1.0\r\n"
             "Content-Type: text/plain; charset=utf-16be\r\n"
             "Content-Transfer-Encoding: 8bit\r\n\r\n")
    return head.encode() + body.encode("utf-16-be"), minutes


@unittest.skipUnless(CC, "no C compiler")
class MboardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = pathlib.Path(cls.tmp.name)
        cls.exe = cls.dir / "mboard_host"
        subprocess.run([CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-D_CRT_SECURE_NO_WARNINGS", "-DMBOARD_NO_OGC",
                        "-I", str(root / "sdk/mboard"), str(root / "tests/mboard_host/main.c"),
                        str(root / "sdk/mboard/mboard.c"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def host(self, *args):
        out = subprocess.run([str(self.exe), *map(str, args)], check=True,
                             capture_output=True, text=True).stdout.strip()
        return int(out)

    # ---- Play history -----------------------------------------------------

    def test_play_same_bytes_as_the_reference(self):
        ref = test_cdblog.ref
        img_c = self.dir / "cdb.vff"
        img_c.write_bytes(test_cdblog.empty_vff())
        img_py = test_cdblog.empty_vff()
        codes = []
        for name, tid, start, end, now in test_cdblog.SESSIONS:
            codes.append(self.host("play", img_c, name, tid, start, end, now,
                                   test_cdblog.WIIID.hex()))
            try:
                ref.log(img_py, name, tid, test_cdblog.tm(start), test_cdblog.tm(end),
                        test_cdblog.tm(now), test_cdblog.WIIID)
            except SystemExit:
                pass
            self.assertEqual(bytes(img_py), img_c.read_bytes(), f"after {name}")
        self.assertEqual(codes[:3], [2, 0, 1])
        self.assertEqual(codes[3:13], [0] * 10)
        self.assertEqual(codes[13], E_FULL)
        self.assertEqual(codes[14], 2)

    # ---- Receive box ------------------------------------------------------

    def box(self, ctl=None, mbx=None, name="box"):
        c, m = self.dir / f"{name}.ctl", self.dir / f"{name}.mbx"
        c.write_bytes(ctl if ctl is not None else empty_ctl())
        m.write_bytes(mbx if mbx is not None else empty_mbx())
        return c, m

    def post(self, c, m, title="WiiStation", body="Played Final Fantasy VII\nfor 42 minutes.",
             utc=UTC, wii=WII, domain=DOMAIN):
        return self.host("post", c, m, wii, domain, f"{APP:08x}", f"{GROUP:04x}", utc, LOCAL,
                         title.encode().hex(), body.encode().hex())

    def check_box(self, ctl, mbx, posts):
        """posts: [(id, title, body)] in the order they were made."""
        fat = Fat(mbx)
        self.assertEqual(mbx[FAT1:FAT1 + FAT_LEN], mbx[FAT2:FAT2 + FAT_LEN], "FAT copies differ")
        mb = [e for e in fat.entries(0) if e["short"] == b"MB         "]
        self.assertEqual(len(mb), 1)
        self.assertEqual(mb[0]["attr"], 0x10)
        files = {e["long"]: e for e in fat.entries(mb[0]["cluster"]) if e["attr"] == 0x20}
        total = 0
        for i, (msg_id, title, body) in enumerate(posts):
            name = f"r{msg_id:07d}.msg"
            self.assertIn(name, files)
            data = fat.read(files[name])
            want, minutes = expected_message(msg_id, title, body)
            # The file is zero-padded to 32 bytes; the entry has the exact length.
            self.assertEqual(data, want + bytes(-len(want) % 32))
            total += len(want)
            e = ctl[0x80 * (i + 1):0x80 * (i + 2)]
            f = struct.unpack(">8IQIIBBH5I2I8I3II", bytes(e))
            body_at = want.index(b"\r\n\r\n", want.index(b"Content-Transfer-Encoding")) + 4
            frm = want.index(b"From: ") + 6
            to = want.index(b"To: ") + 4
            ct = want.index(b"utf-16be")
            te = want.index(b"8bit")
            alen = len(f"w{WII:016d}{DOMAIN}")
            text = len(body.encode("utf-16-be"))
            self.assertEqual(f[0:8], (msg_id, 0x200329, len(want), APP, body_at, 0, 0x40001, 0))
            self.assertEqual(f[8:14], (WII, minutes, 0, 1, 0, GROUP))
            self.assertEqual(f[14:19], (frm | alen << 20, to | alen << 20, 0, ct | 9 << 20, te | 5 << 20))
            self.assertEqual(f[19:21], (body_at, text))
            self.assertEqual(f[21:29], (0, 0, 0, 0, 0, 0, 0x10000, 0x10000))
            self.assertEqual(f[29:33], (text, 0, 0x80000000, 0))
        n = len(posts)
        num, cap, tot, size, nxt, free, oldest, space = struct.unpack_from(">8I", ctl, 8)
        self.assertEqual((num, cap, tot, size), (n, 255, total, CTL_SIZE))
        self.assertEqual(nxt, posts[-1][0] + 1 if posts else 1)
        self.assertEqual(free, 0x80 * (n + 1))
        self.assertEqual(oldest, posts[0][0] if posts else 0)
        # As the VF library counts it: the last cluster is never used.
        self.assertEqual(space, (fat.free() - 1) * CL)

    def test_post(self):
        c, m = self.box()
        self.assertEqual(self.post(c, m), 0)
        posts = [(1, "WiiStation", "Played Final Fantasy VII\nfor 42 minutes.")]
        self.check_box(c.read_bytes(), m.read_bytes(), posts)
        self.assertEqual(self.post(c, m, "Wii — Station ✓", "Crash 🦊 0:07"), 0)
        posts.append((2, "Wii — Station ✓", "Crash 🦊 0:07"))
        self.check_box(c.read_bytes(), m.read_bytes(), posts)

    def test_post_makes_the_folder(self):
        c, m = self.box(mbx=empty_mbx(with_mb=False))
        self.assertEqual(self.post(c, m), 0)
        mbx = m.read_bytes()
        self.assertEqual(mbx[ROOT:ROOT + 32], lfn_entry(b"MB         ", "mb"))
        self.check_box(c.read_bytes(), mbx, [(1, "WiiStation", "Played Final Fantasy VII\nfor 42 minutes.")])

    def test_long_message_spans_clusters(self):
        c, m = self.box()
        body = ("Played for a long while. " * 40)[:1000]
        title = "A long sender name that is cut at 34 units"
        self.assertEqual(self.post(c, m, title, body + "and more"), 0)
        self.check_box(c.read_bytes(), m.read_bytes(), [(1, title[:34], body)])

    def test_a_failed_control_write_is_retried_in_place(self):
        """The message file is written first; if the control file never gets
        its entry, the next post takes the same id and reuses the file."""
        c, m = self.box()
        self.assertEqual(self.post(c, m, body="first"), 0)
        free_once = Fat(m.read_bytes()).free()
        c.write_bytes(empty_ctl())
        self.assertEqual(self.post(c, m, body="second try"), 0)
        self.check_box(c.read_bytes(), m.read_bytes(), [(1, "WiiStation", "second try")])
        self.assertEqual(Fat(m.read_bytes()).free(), free_once)

    def test_refusals_change_nothing(self):
        def ctl_with(**kw):
            ctl = empty_ctl()
            where = {"magic": 0, "version": 4, "num": 8, "free": 0x1c, "space": 0x24, "next": 0x18}
            for k, v in kw.items():
                struct.pack_into(">I", ctl, where[k], v)
            return ctl

        used = empty_ctl()
        struct.pack_into(">I", used, 0x80, 7)  # the free chain's first entry is in use
        dup = empty_ctl()
        struct.pack_into(">I", dup, 0x100, 1)  # a message already has the next id
        small = bytearray(0x200000)
        small[0:16] = bytes.fromhex("56464620feff010000200000" "00200000")
        cases = [
            ("full", ctl_with(num=255), None, E_FULL),
            ("no free entry", ctl_with(free=0), None, E_FULL),
            ("no room", ctl_with(space=0x4000), None, E_FULL),
            ("magic", ctl_with(magic=0x42524b4e), None, E_FORMAT),
            ("version", ctl_with(version=3), None, E_FORMAT),
            ("chain", ctl_with(free=0x84), None, E_FORMAT),
            ("chain in use", used, None, E_FORMAT),
            ("duplicate id", dup, None, E_FORMAT),
            ("not a VFF", None, bytearray(BOX_SIZE), E_FORMAT),
            ("FAT12", None, small, E_FORMAT),
        ]
        for name, ctl, mbx, want in cases:
            with self.subTest(name):
                c, m = self.box(ctl, mbx, name="refused")
                before = (c.read_bytes(), m.read_bytes())
                self.assertEqual(self.post(c, m), want)
                self.assertEqual((c.read_bytes(), m.read_bytes()), before)
        c, m = self.box(name="refused")
        before = (c.read_bytes(), m.read_bytes())
        self.assertEqual(self.post(c, m, wii=0), E_NO_ID)
        self.assertEqual(self.post(c, m, domain="wii.com"), E_NO_ID)
        self.assertEqual(self.post(c, m, body=""), E_ARG)
        self.assertEqual((c.read_bytes(), m.read_bytes()), before)

    @unittest.skipUnless(os.environ.get("MBOARD_REAL_BOX"), "MBOARD_REAL_BOX not set")
    def test_post_into_a_real_box(self):
        real = pathlib.Path(os.environ["MBOARD_REAL_BOX"])
        ctl0, mbx0 = (real / "wc24recv.ctl").read_bytes(), (real / "wc24recv.mbx").read_bytes()
        c, m = self.box(ctl0, mbx0, name="real")
        self.assertEqual(self.post(c, m), 0)
        ctl, mbx = c.read_bytes(), m.read_bytes()
        n0, n1 = struct.unpack_from(">I", ctl0, 8)[0], struct.unpack_from(">I", ctl, 8)[0]
        self.assertEqual(n1, n0 + 1)
        # The free space the VF library recorded before, less this message's clusters.
        used = -(-(len(expected_message(1, "WiiStation", "Played Final Fantasy VII\nfor 42 minutes.")[0])) // CL)
        self.assertEqual(struct.unpack_from(">I", ctl, 0x24)[0],
                         struct.unpack_from(">I", ctl0, 0x24)[0] - used * CL)
        self.assertEqual(ctl[0x80:struct.unpack_from(">I", ctl0, 0x1c)[0]],
                         ctl0[0x80:struct.unpack_from(">I", ctl0, 0x1c)[0]], "entries before the new one changed")
        msg_id = struct.unpack_from(">I", ctl, 0x18)[0] - 1
        fat = Fat(mbx)
        mb = next(e for e in fat.entries(0) if e["short"] == b"MB         ")
        files = {e["long"]: fat.read(e) for e in fat.entries(mb["cluster"]) if e["attr"] == 0x20}
        want = expected_message(msg_id, "WiiStation", "Played Final Fantasy VII\nfor 42 minutes.")[0]
        self.assertEqual(files[f"r{msg_id:07d}.msg"], want + bytes(-len(want) % 32))
        # Every message already there reads back unchanged.
        old = Fat(mbx0)
        mb0 = next(e for e in old.entries(0) if e["short"] == b"MB         ")
        for e in old.entries(mb0["cluster"]):
            if e["attr"] == 0x20:
                self.assertEqual(files[e["long"]], old.read(e))


if __name__ == "__main__":
    unittest.main()
