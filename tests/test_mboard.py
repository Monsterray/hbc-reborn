"""The Message Board library (sdk/mboard) on PC images.

Play history: the same sessions as tests/test_cdblog.py must give the same
bytes as the reference the Wii Menu accepted (tools/msgboard/cdb_log.py).

Receive box: a post must leave what the SDK's NWC24CommitMsg() leaves for a
game's message to its own Wii (docs/mboard.md), checked here with a FAT
reader of its own; a post it refuses must leave both files as they were.
Set MBOARD_REAL_BOX to a folder holding a Wii's wc24recv.ctl and .mbx to
also post into a copy of those.

The full message (mboard_msg: a Mii face, a JPEG, board data, Reply, delay,
filing date) is checked against a rebuild from the SDK's own format strings
(NWC24MsgCommit.c), and parsed back with Python's email package."""

import base64
import datetime
import email
import email.policy
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
OTHER = 6543210987654321                # another Wii, for the send box
SEND_SIZE, SEND_CTL = 0x200000, 0x4000
DOMAIN = "@wii.com"
APP, GROUP = 0x4F484243, 0x3031  # 'OHBC', "01"
UTC, LOCAL = "2026-10-22T18:30:05", "2026-10-22T11:30:05"
E_IO, E_FORMAT, E_FULL, E_ARG, E_NO_ID, E_TOO_BIG = -1, -2, -3, -5, -7, -9


def empty_ctl():
    """As the SDK's CreateCtrlFile() makes it: every entry free, chained
    through its app_id word."""
    ctl = bytearray(CTL_SIZE)
    struct.pack_into(">4s9I", ctl, 0, b"WcTf", 4, 0, 255, 0, CTL_SIZE, 1, 0x80, 0, BOX_SIZE)
    ctl[0x58:0x7f] = b"0" * 39
    for off in range(0x80, CTL_SIZE, 0x80):
        struct.pack_into(">I", ctl, off + 0x0C, (off + 0x80) % CTL_SIZE)
    return ctl


def empty_send():
    """The send box as the bench Wii has it with nothing to send: a 127-entry
    control file and a 2 MiB FAT12 VFF holding only "mb", its FATs as long
    as FAT16 ones (0x2000 bytes: the second at 0x2020, the root at 0x4020)."""
    ctl = bytearray(SEND_CTL)
    struct.pack_into(">4s9I", ctl, 0, b"WcTf", 4, 0, 127, 0, SEND_CTL, 1, 0x80, 0, SEND_SIZE)
    for off in range(0x80, SEND_CTL, 0x80):
        struct.pack_into(">I", ctl, off + 0x0C, (off + 0x80) % SEND_CTL)
    img = bytearray(SEND_SIZE)
    img[0:16] = bytes.fromhex("56464620feff010000200000" "00200000")
    for fat in (0x20, 0x2020):
        img[fat:fat + 6] = bytes.fromhex("f0ffffff0f00")   # FAT12: cluster 2 (mb) ends its chain
    img[0x4020:0x4040] = lfn_entry(b"MB         ", "mb")
    img[0x4040:0x4060] = dir_entry(b"MB         ", 0x10, 2, 0)
    img[0x5020:0x5040] = dir_entry(b".          ", 0x10, 2, 0)
    img[0x5040:0x5060] = dir_entry(b"..         ", 0x10, 0, 0)
    return ctl, img


class Fat12:
    """The send box's FAT12, read on its own: two FATs of 0x2000 bytes."""
    FAT1, FAT_LEN, ROOT, DATA = 0x20, 0x2000, 0x4020, 0x5020

    def __init__(self, img):
        self.img = img
        self.n = (SEND_SIZE - self.DATA) // CL + 2

    def next(self, c):
        w = struct.unpack_from("<H", self.img, self.FAT1 + c + c // 2)[0]
        return w >> 4 if c & 1 else w & 0xfff

    def chain(self, c):
        out = []
        while 2 <= c < 0xff8:
            out.append(c)
            c = self.next(c)
        return out

    def entries(self, cluster):
        areas = ([(self.ROOT, 0x1000)] if cluster == 0 else
                 [(self.DATA + (c - 2) * CL, CL) for c in self.chain(cluster)])
        lfn = None
        for start, size in areas:
            for off in range(start, start + size, 32):
                e = self.img[off:off + 32]
                if e[0] == 0:
                    return
                if e[11] == 0x0f:
                    lfn = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le").split("\0")[0]
                elif e[0] != 0xe5:
                    yield {"short": bytes(e[0:11]), "long": lfn, "attr": e[11],
                           "cluster": struct.unpack_from("<H", e, 26)[0],
                           "size": struct.unpack_from("<I", e, 28)[0]}
                    lfn = None

    def read(self, ent):
        return b"".join(self.img[self.DATA + (c - 2) * CL:self.DATA + (c - 1) * CL]
                        for c in self.chain(ent["cluster"]))[:ent["size"]]

    def free(self):
        return sum(1 for c in range(2, self.n) if self.next(c) == 0)


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


# A Mii's RFLCharData: a creation ID at 0x18 is what makes it one.
FACE = bytes(range(0x18)) + bytes.fromhex("80aabbcc12345678") + bytes(range(0x20, 0x4a))
JPEG = b"\xff\xd8\xff\xe0" + bytes(range(256)) * 3 + b"\xff\xd9"   # 774 bytes
# A U8 archive's start (Nintendo's board data: chjump.bin, letterhead.arc).
BOARD = bytes.fromhex("55aa382d00000020") + bytes(171)          # 179 bytes


def b64_lines(data, step):
    return [base64.b64encode(data[i:i + step]).decode() for i in range(0, len(data), step)]


def expected_full(msg_id, title, body, face=None, jpeg=None, board=None, reply=False,
                  delay=0, date=None, utc=UTC, to=WII):
    """NWC24CommitMsg()'s bytes for a WII_MENU message to the Wii itself (or,
    with `to`, to another Wii: then the send box's, with its SMTP envelope),
    from its format strings. Returns (bytes, minutes, offsets)."""
    t = datetime.datetime.strptime(utc, "%Y-%m-%dT%H:%M:%S")
    unix = int(t.replace(tzinfo=datetime.timezone.utc).timestamp())
    minutes = (unix + 2208988800) // 60
    addr, to_addr = f"w{WII:016d}{DOMAIN}", f"w{to:016d}{DOMAIN}"
    h = f"MAIL FROM: {addr}\r\nRCPT TO: {to_addr}\r\nDATA\r\n" if to != WII else ""
    h += (f"Date: {t.day:02d} {t.strftime('%b')} {t.year} {t:%H:%M:%S} -0000\r\n"
         f"From: {addr}\r\nTo: {to_addr}\r\n"
         f"Message-Id: <{msg_id:05X}{WII >> 32:08X}{WII & 0xffffffff:08X}{minutes:08X}{DOMAIN}>\r\n"
         f"X-Wii-AppId: 2-{APP:08X}-{GROUP:04X}\r\nX-Wii-Cmd: 00040001\r\n")
    h += "X-Wii-AltName:" + "".join(f" {l}\r\n" for l in b64_lines(title.encode("utf-16-be") + b"\0\0", 42))
    if face:
        h += "X-WiiFace:" + "".join(f" {l}\r\n" for l in b64_lines(face, 42))
    if not reply:
        h += "X-Wii-MB-NoReply: -\r\n"
    if date:
        y, mo, d = date
        h += f"X-Wii-MB-RegDate: {(y - 2000) << 9 | mo << 5 | d:04X}\r\n"
    if delay:
        h += f"X-Wii-MB-Delay: {delay:02X}\r\n"
    h += "MIME-Version: 1.0\r\n"
    atts = [(a, t_, s) for a, t_, s in ((jpeg, "image/jpeg", "jpg"),
                                         (board, "application/x-wii-msgboard", "wii")) if a]
    divider = f"Boundary-NWC24-{minutes:08X}{msg_id:05X}"
    if atts:
        h += f'Content-Type: multipart/mixed;\r\n boundary="{divider}"\r\n'
        start = len(h)
        h += f"\r\n--{divider}\r\n"
    b64 = to != WII   # to another Wii the text goes base64, like the Wii Menu's letters
    h += ("Content-Type: text/plain; charset=utf-16be\r\nContent-Transfer-Encoding: "
          + ("base64" if b64 else "8bit") + "\r\n\r\n")
    if not atts:
        start = len(h)
    out = h.encode()
    body_at = len(out)
    raw = body.encode("utf-16-be")
    if b64:
        full = len(raw) - len(raw) % 57
        out += "".join(l + "\r\n" for l in b64_lines(raw[:full], 57)).encode()
        out += base64.b64encode(raw[full:]) + b"\r\n\r\n"
    else:
        out += raw
    text_len = len(out) - body_at
    spans = []
    for i, (data, mime, suffix) in enumerate(atts):
        name = f"{chr(97 + i)}{msg_id:07d}.{suffix}"
        out += (f"\r\n--{divider}\r\nContent-Type: {mime};\r\n name={name}\r\n"
                "Content-Transfer-Encoding: base64\r\n"
                f"Content-Disposition: attachment;\r\n filename={name}\r\n\r\n").encode()
        at = len(out)
        full = len(data) - len(data) % 57
        out += "".join(l + "\r\n" for l in b64_lines(data[:full], 57)).encode()
        out += base64.b64encode(data[full:]) + b"\r\n\r\n"
        spans.append((at, len(out) - at, len(data), 0x20000 if suffix == "jpg" else 0x30001))
        if i == len(atts) - 1:
            out += f"\r\n--{divider}--\r\n".encode()
    return out, minutes, {"start": start, "body": body_at, "text_len": text_len, "atts": spans}


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
             utc=UTC, wii=WII, domain=DOMAIN, **opts):
        args = []
        for key in ("face", "jpeg", "board"):
            if opts.get(key) is not None:
                f = self.dir / f"part.{key}"
                f.write_bytes(opts[key])
                args.append(f"{key}={f}")
        if opts.get("reply"):
            args.append("reply=1")
        if opts.get("delay"):
            args.append(f"delay={opts['delay']}")
        if opts.get("date"):
            args.append("date=%04d-%02d-%02d" % opts["date"])
        if opts.get("to"):
            args.append(f"to={opts['to']}")
        return self.host("post", c, m, wii, domain, f"{APP:08x}", f"{GROUP:04x}", utc, LOCAL,
                         title.encode().hex(), body.encode().hex(), *args)

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

    # ---- The full message: face, photo, board data, Reply, delay, date ----

    def check_full(self, ctl, mbx, msg_id, slot, title, body, **opts):
        want, minutes, at = expected_full(msg_id, title, body, **opts)
        fat = Fat(mbx)
        mb = next(e for e in fat.entries(0) if e["short"] == b"MB         ")
        files = {e["long"]: fat.read(e) for e in fat.entries(mb["cluster"]) if e["attr"] == 0x20}
        self.assertEqual(files[f"r{msg_id:07d}.msg"], want + bytes(-len(want) % 32))
        e = bytes(ctl[0x80 * slot:0x80 * (slot + 1)])
        f = struct.unpack(">8IQIIBBH5I2I8I3II", e)
        atts = at["atts"]
        flags = 0x200329 | (0x10000 if atts else 0) | (0x1000000 if opts.get("delay") else 0)
        text = len(body.encode("utf-16-be"))
        self.assertEqual(f[0:8], (msg_id, flags, len(want), APP, at["start"], 0, 0x40001, 0))
        self.assertEqual(f[8:14], (WII, minutes, minutes + 60 * opts.get("delay", 0) if opts.get("delay") else 0,
                                   1, len(atts), GROUP))
        self.assertEqual(f[19:21], (at["body"], text))
        pad = [(0, 0, 0, 0x10000)] * (2 - len(atts))
        a = atts + pad
        self.assertEqual(f[21:29], (a[0][0], a[0][1], a[1][0], a[1][1], a[0][2], a[1][2], a[0][3], a[1][3]))
        self.assertEqual(f[29:33], (text, 0, 0x80000000, 0))
        return want

    def test_full_message(self):
        """Every option at once: the bytes the SDK writes, and a MIME
        reader gets each part back."""
        c, m = self.box()
        opts = dict(face=FACE, jpeg=JPEG, board=BOARD, delay=3, date=(2026, 10, 24))
        self.assertEqual(self.post(c, m, "WiiStation", "Played Wii Sports\nfor 5 minutes.", **opts), 0)
        want = self.check_full(c.read_bytes(), m.read_bytes(), 1, 1, "WiiStation",
                               "Played Wii Sports\nfor 5 minutes.", **opts)
        msg = email.message_from_bytes(want, policy=email.policy.compat32)
        self.assertTrue(msg.is_multipart())
        parts = msg.get_payload()
        self.assertEqual([p.get_content_type() for p in parts],
                         ["text/plain", "image/jpeg", "application/x-wii-msgboard"])
        self.assertEqual(parts[0].get_payload(decode=True).decode("utf-16-be"),
                         "Played Wii Sports\nfor 5 minutes.")
        self.assertEqual(parts[1].get_payload(decode=True), JPEG)
        self.assertEqual(parts[2].get_payload(decode=True), BOARD)
        self.assertEqual(parts[1].get_filename(), "a0000001.jpg")
        self.assertEqual(base64.b64decode("".join(msg["X-WiiFace"].split())), FACE)
        self.assertEqual(msg["X-Wii-MB-NoReply"], "-")  # no Reply button unless asked
        self.assertEqual((msg["X-Wii-MB-RegDate"], msg["X-Wii-MB-Delay"]), ("3558", "03"))
        self.check_box_header(c.read_bytes(), m.read_bytes(), [len(want)])

    def test_each_option_alone(self):
        cases = [dict(face=FACE), dict(reply=True), dict(delay=240), dict(date=(2028, 2, 29)),
                 dict(jpeg=JPEG), dict(board=BOARD),
                 dict(jpeg=bytes.fromhex("ffd8ffe0") + bytes(57 * 4 - 4))]  # a whole number of lines
        c, m = self.box()
        sizes = []
        for i, opts in enumerate(cases, 1):
            with self.subTest(opts=sorted(opts)):
                self.assertEqual(self.post(c, m, body=f"option {i}", **opts), 0)
                sizes.append(len(self.check_full(c.read_bytes(), m.read_bytes(), i, i, "WiiStation",
                                                 f"option {i}", **opts)))
        self.check_box_header(c.read_bytes(), m.read_bytes(), sizes)

    def check_box_header(self, ctl, mbx, sizes):
        num, cap, tot, size, nxt, free, oldest, space = struct.unpack_from(">8I", ctl, 8)
        self.assertEqual((num, tot, nxt, free, oldest), (len(sizes), sum(sizes), len(sizes) + 1,
                                                         0x80 * (len(sizes) + 1), 1))
        self.assertEqual(space, (Fat(mbx).free() - 1) * CL)
        self.assertEqual(mbx[FAT1:FAT1 + FAT_LEN], mbx[FAT2:FAT2 + FAT_LEN])

    def test_big_attachments(self):
        """Board data at its limit, and a photo near the SDK's total."""
        c, m = self.box()
        board = BOARD + bytes(0x1E000 - len(BOARD))
        self.assertEqual(self.post(c, m, board=board), 0)
        jpeg = JPEG + bytes(0x20000 - len(JPEG))
        self.assertEqual(self.post(c, m, body="big photo", jpeg=jpeg), 0)
        self.check_full(c.read_bytes(), m.read_bytes(), 2, 2, "WiiStation", "big photo", jpeg=jpeg)

    def test_option_refusals_change_nothing(self):
        bad_face = FACE[:0x18] + bytes(8) + FACE[0x20:]
        cases = [
            ("face with no creation ID", dict(face=bad_face), E_ARG),
            ("not a JPEG", dict(jpeg=b"\x89PNG" + bytes(100)), E_ARG),
            ("board data not U8", dict(board=b"RIFF" + bytes(100)), E_ARG),
            ("board data too big", dict(board=BOARD + bytes(0x1E000 - len(BOARD) + 1)), E_TOO_BIG),
            ("photo and board over the total",
             dict(jpeg=JPEG + bytes(0x245B0 - 0x1E000 - len(JPEG)), board=BOARD + bytes(0x1E000 - len(BOARD))),
             E_TOO_BIG),
            ("delay over 240 h", dict(delay=241), E_ARG),
            ("30 February", dict(date=(2026, 2, 30)), E_ARG),
            ("29 February, not a leap year", dict(date=(2027, 2, 29)), E_ARG),
            ("year 2036", dict(date=(2036, 1, 1)), E_ARG),
            ("month 13", dict(date=(2026, 13, 1)), E_ARG),
        ]
        for name, opts, want in cases:
            with self.subTest(name):
                c, m = self.box(name="refused")
                before = (c.read_bytes(), m.read_bytes())
                self.assertEqual(self.post(c, m, **opts), want)
                self.assertEqual((c.read_bytes(), m.read_bytes()), before)

    # ---- To another Wii: the send box ------------------------------------

    def test_send(self):
        """Two messages into a FAT12 send box: the SMTP envelope and the other
        Wii in To:, TO_SEND in the entry, both FATs packed FAT12, /mb/s*.msg."""
        ctl0, mbx0 = empty_send()
        c, m = self.box(ctl0, mbx0, name="send")
        opts = dict(face=FACE, jpeg=JPEG, board=BOARD, reply=True)
        self.assertEqual(self.post(c, m, "WiiStation", "Hello from the bench\nWii.", to=OTHER, **opts), 0)
        self.assertEqual(self.post(c, m, "WiiStation", "A second one.", to=OTHER), 0)
        ctl, mbx = c.read_bytes(), m.read_bytes()
        self.assertEqual(mbx[0x20:0x2020], mbx[0x2020:0x4020], "FAT copies differ")
        fat = Fat12(mbx)
        mb = next(e for e in fat.entries(0) if e["short"] == b"MB         ")
        files = {e["long"]: e for e in fat.entries(mb["cluster"]) if e["attr"] == 0x20}
        self.assertEqual(sorted(files), ["s0000001.msg", "s0000002.msg"])
        sizes = []
        for i, (body, o) in enumerate([("Hello from the bench\nWii.", opts), ("A second one.", {})], 1):
            want, minutes, at = expected_full(i, "WiiStation", body, to=OTHER, **o)
            self.assertEqual(fat.read(files[f"s{i:07d}.msg"]), want + bytes(-len(want) % 32))
            self.assertTrue(want.startswith(f"MAIL FROM: w{WII:016d}{DOMAIN}\r\nRCPT TO: "
                                            f"w{OTHER:016d}{DOMAIN}\r\nDATA\r\nDate: ".encode()))
            f = struct.unpack(">8IQIIBBH5I2I8I3II", ctl[0x80 * i:0x80 * (i + 1)])
            flags = 0x200319 | (0x10000 if at["atts"] else 0)       # TO_SEND, not TO_RECV
            self.assertEqual(f[0:5], (i, flags, len(want), APP, at["start"]))
            self.assertEqual(f[8:14], (WII, minutes, 0, 1, len(at["atts"]), GROUP))
            to_at = want.index(b"\r\nTo: ") + 6
            self.assertEqual(f[15], to_at | len(f"w{OTHER:016d}{DOMAIN}") << 20)
            te = want.index(b"base64\r\n\r\n")
            self.assertEqual(f[18], te | 7 << 20)                    # "base64"
            self.assertEqual(f[19:21], (at["body"], at["text_len"]))  # the encoded text
            self.assertEqual(f[29], len(body.encode("utf-16-be")))   # its size before encoding
            # The text survives WiiLink's server: send.go deletes every NUL
            # byte (it keeps mail as text), and receive.go makes every LF a
            # CRLF (WiiLink24/Mail-Server). Raw UTF-16 did not: its ASCII lost
            # every other byte and read back as CJK characters.
            served = want.split(b"DATA\r\n", 1)[1].replace(b"\x00", b"")
            served = served.replace(b"\n", b"\r\n").replace(b"\r\r\n", b"\r\n")
            msg = email.message_from_bytes(served, policy=email.policy.compat32)
            part = msg.get_payload()[0] if msg.is_multipart() else msg
            self.assertEqual(part.get_payload(decode=True).decode("utf-16-be"), body)
            sizes.append(len(want))
        num, cap, tot, size, nxt, free, oldest, space = struct.unpack_from(">8I", ctl, 8)
        self.assertEqual((num, cap, tot, size, nxt, free, oldest), (2, 127, sum(sizes), SEND_CTL, 3, 0x180, 1))
        self.assertEqual(space, (fat.free() - 1) * CL)
        # A message's clusters chain through the FAT12 entries and end it.
        chain = fat.chain(files["s0000001.msg"]["cluster"])
        self.assertEqual(len(chain), -(-(sizes[0] + (-sizes[0] % 32)) // CL))
        self.assertEqual(fat.next(chain[-1]), 0xfff)

    def test_send_refusals(self):
        ctl0, mbx0 = empty_send()
        for name, kw, want in [("to itself", dict(to=WII), E_ARG),
                               ("a number of 17 digits", dict(to=10 ** 16), E_ARG)]:
            with self.subTest(name):
                c, m = self.box(ctl0, mbx0, name="sendref")
                before = (c.read_bytes(), m.read_bytes())
                self.assertEqual(self.post(c, m, **kw), want)
                self.assertEqual((c.read_bytes(), m.read_bytes()), before)
        # The receive box's control file is not a send box's.
        c, m = self.box(empty_ctl()[:SEND_CTL], mbx0, name="sendref")
        self.assertEqual(self.post(c, m, to=OTHER), E_FORMAT)

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
