"""Check tools/hbc.py against a fake HBC that follows docs/devnet.md."""

import contextlib
import io
import json
import os
import pathlib
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
import zlib

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

ENOENT, ENOTDIR, EISDIR, EINVAL, ENOTEMPTY = 2, 20, 21, 22, 90
WII = "127.0.0.1"


def parent(path):
    head = path.rsplit("/", 1)[0]
    return head + "/" if head.endswith(":") else head


class FakeHBC:
    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.files = {}
        self.mtimes = {}       # path -> the fake Wii's file time
        self.clock = 0         # the fake Wii's clock minus the PC's (HBCS "time")
        self.old_listing = False
        self.dirs = {"sd:/"}
        self.log = None
        self.log_ports = []
        self.uploads = []
        self.upload_names = []  # HBCA: the play log's name for the next upload
        self.upload_flags = []  # HBCA's flags (protocol 6)
        # Protocol 6: what HBC makes of each upload, for HBCS "upload"; None
        # acts as an older HBC that does not say.
        self.next_result = None
        self.upload = None
        self.requests = []  # (op, path) of every HBCF request
        self.truncate = False
        self.agent = False  # an agent app answers instead of HBC
        self.agent_uptime_ms = 1000
        self.crash = None
        self.lastlog = None  # (why, uptime_ms, app, text) for HBCL
        self.exits = 0
        self.keys = b""
        self.no_rename = False  # an HBC or agent before op R (1.10.14)
        self.version = "1.2.0"   # what HBCV answers
        self.attrs = {}          # path -> FAT attribute bits (op A, listings)
        self.full = set()        # paths whose put fails with ENOSPC
        self.mii_faces = (0, b"Saved Mii faces to sd:/mii/RFL_Res.dat (671 KB)")  # HBCM's reply
        self.lock = threading.Lock()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            conn, (addr, _) = self.sock.accept()
            with conn, self.lock:
                try:
                    self.handle(conn, addr)
                except OSError:
                    pass

    @staticmethod
    def recv(conn, n):
        data = b""
        while len(data) < n:
            chunk = conn.recv(n - len(data))
            if not chunk:
                raise ConnectionError("client closed")
            data += chunk
        return data

    def reply(self, conn, status, payload=b""):
        conn.sendall(struct.pack(">iI", status, len(payload)) + payload)

    def letters(self, path):
        bits = self.attrs.get(path, 0)
        return "".join(c for c, b in (("r", 1), ("h", 2), ("s", 4), ("a", 0x20)) if bits & b) or "-"

    def entry(self, kind, path):
        size = len(self.files[path]) if kind == "f" else 0
        return f"{kind} {size} {self.mtimes.get(path, 0)} {self.letters(path)} {path.rsplit('/', 1)[1]}\n"

    def make_dirs(self, path):
        while path not in self.dirs:
            self.dirs.add(path)
            path = parent(path)

    def children(self, path):
        return ([("d", p) for p in sorted(self.dirs) if p != path and parent(p) == path] +
                [("f", p) for p in sorted(self.files) if parent(p) == path])

    def send_log(self, text):
        addr, port = self.log.rsplit(":", 1)
        with socket.create_connection((addr, int(port)), timeout=5) as log:
            log.sendall(text)

    def handle(self, conn, addr):
        hdr = self.recv(conn, 16)
        magic = hdr[:4]
        if magic == b"HBCV":
            conn.sendall(b"1.5.0 agent\0" if self.agent else self.version.encode() + b"\0")
        elif magic == b"HBCS":
            st = {"version": "1.2.0", "proto": 2, "log": self.log,
                  "time": int(time.time()) + self.clock}
            if self.next_result is not None:
                st.update(proto=6, upload=self.upload)
            if self.agent:
                st = {"agent": True, "version": "1.5.0", "proto": 3, "log": self.log,
                      "app": "otherapp", "uptime_ms": self.agent_uptime_ms}
            elif self.crash is not None:
                st.update(proto=4, crash=self.crash or None,
                          lastlog={"why": self.lastlog[0], "bytes": len(self.lastlog[3])}
                          if self.lastlog else None)
            self.reply(conn, 0, json.dumps(st).encode())
        elif magic == b"HBCM":
            self.reply(conn, *self.mii_faces)
        elif magic == b"HBCX" and self.agent:
            self.reply(conn, 0)
            self.agent = False  # the app exits to HBC
            self.exits += 1
        elif magic == b"HBCK":
            n = struct.unpack(">H", hdr[4:6])[0]
            self.keys += self.recv(conn, n)
            self.reply(conn, 0)
        elif magic == b"HBCP":
            # 4x2 pixels: a red pair and a white pair on each row.
            frame = bytes([81, 90, 81, 240, 235, 128, 235, 128]) * 2
            conn.sendall(struct.pack(">iIII", 0, 8 + len(frame), 4, 2) + frame)
        elif magic == b"HBCH":
            self.reply(conn, 0, json.dumps({"System": {"Console": "Wii RVL-001(USA)"},
                                            "USB": {"057e:0305": "Device"}}).encode())
        elif magic == b"HBCC":
            self.crash = {}
            self.reply(conn, 0)
        elif magic == b"HBCL":
            if self.lastlog:
                why, up, app, text = self.lastlog
                self.reply(conn, 0, f"HBCL 1 {why} {up} {app}\n{text}".encode())
            else:
                self.reply(conn, -2)  # ENOENT
        elif magic == b"HBCN":
            port = struct.unpack(">H", hdr[4:6])[0]
            self.log_ports.append(port)
            self.log = f"{addr}:{port}" if port else None
            self.reply(conn, 0)
        elif magic == b"HBCA":
            n, flags = struct.unpack(">HH", hdr[4:8])
            self.upload_names.append(self.recv(conn, n).decode())
            self.upload_flags.append(flags)
            self.reply(conn, 0)
        elif magic == b"HAXX":
            args_len, size, size_un = struct.unpack(">HII", hdr[6:16])
            data = self.recv(conn, size)
            args = self.recv(conn, args_len)
            self.uploads.append((zlib.decompress(data) if size_un else data, args))
            if self.next_result is not None:
                seq = (self.upload or {}).get("seq", 0) + 1
                self.upload = dict(self.next_result, seq=seq, ago_ms=0)
            if self.log and b"log-me" in args:
                threading.Thread(target=self.send_log, args=(b"hello from the app\n",),
                                 daemon=True).start()
        elif magic == b"HBCF":
            op = chr(hdr[4])
            path_len, size = struct.unpack(">HI", hdr[6:12])
            path = self.recv(conn, path_len).decode()
            self.requests.append((op, path))
            if op in "pP" and path.endswith("/"):
                self.reply(conn, -EISDIR)  # refused before any data is read
                conn.shutdown(socket.SHUT_WR)
                while conn.recv(4096):
                    pass
                return
            if len(path) > 4 and path.endswith("/") and path[-2] != ":":
                path = path[:-1]
            if ".." in path or "//" in path:
                self.reply(conn, -EINVAL)
            elif op in "pP" and path in self.dirs:
                self.reply(conn, -EISDIR)
            elif op == "P":
                self.make_dirs(parent(path))
                self.files[path] = self.recv(conn, size)
                self.mtimes[path] = int(time.time()) + self.clock
                self.reply(conn, 0)
            elif op in "GgC" and path not in self.files:
                self.reply(conn, -(EISDIR if path in self.dirs and op == "C" else ENOENT))
            elif op == "G":
                self.reply(conn, 0, self.files[path])
            elif op == "C":
                data = self.files[path]
                self.reply(conn, 0, struct.pack(">II", len(data), zlib.crc32(data)))
            elif op == "p" and path in self.full:
                self.reply(conn, -28)   # ENOSPC, before any data; then read on, as HBC does
                conn.shutdown(socket.SHUT_WR)
                while conn.recv(65536):
                    pass
            elif op == "p":
                data, left = bytearray(), size
                while left:
                    raw_len, wire_len, crc = struct.unpack(">III", self.recv(conn, 12))
                    wire = self.recv(conn, wire_len)
                    raw = zlib.decompress(wire) if wire_len < raw_len else wire
                    if zlib.crc32(raw) != crc:
                        self.reply(conn, -77)  # newlib EBADMSG
                        return
                    data += raw
                    left -= raw_len
                self.make_dirs(parent(path))
                self.files[path] = bytes(data)
                self.mtimes[path] = int(time.time()) + self.clock
                self.reply(conn, 0)
            elif op == "g":
                data = self.files[path]
                conn.sendall(struct.pack(">iI", 0, len(data)))
                for frame in hbc.frames(data, level=6 if hdr[5] & 1 else 0):
                    conn.sendall(frame)
                conn.sendall(bytes(12))
            elif op == "L":
                if path not in self.dirs:
                    self.reply(conn, -(ENOTDIR if path in self.files else ENOENT))
                    return
                if self.old_listing:   # HBC 1.10.5 and older
                    lines = "".join(f"d {p.rsplit('/', 1)[1]}\n" if kind == "d" else
                                    f"f {len(self.files[p])} {p.rsplit('/', 1)[1]}\n"
                                    for kind, p in self.children(path))
                else:
                    lines = "#list 2\n" + "".join(self.entry(kind, p)
                                                   for kind, p in self.children(path))
                if self.truncate:
                    lines += "! truncated\n"
                self.reply(conn, 0, lines.encode())
            elif op == "S":
                if path in self.files or path in self.dirs:
                    kind = "f" if path in self.files else "d"
                    self.reply(conn, 0, self.entry(kind, path).encode())
                else:
                    self.reply(conn, -ENOENT)
            elif op == "D":
                if path in self.dirs and self.children(path):
                    self.reply(conn, -ENOTEMPTY)
                elif (path in self.files or path in self.dirs) and self.attrs.get(path, 0) & 1:
                    self.reply(conn, -13)   # EACCES: read-only
                elif path in self.files:
                    del self.files[path]
                    self.reply(conn, 0)
                elif path in self.dirs:
                    self.dirs.discard(path)
                    self.reply(conn, 0)
                else:
                    self.reply(conn, -ENOENT)
            elif op == "A":
                value, mask = size & 0xff, (size >> 8) & 0xff
                if mask & ~0x27 or size >> 16:
                    self.reply(conn, -EINVAL)
                elif path not in self.files and path not in self.dirs:
                    self.reply(conn, -ENOENT)
                else:
                    cur = self.attrs.get(path, 0)
                    self.attrs[path] = (cur & ~mask) | (value & mask)
                    self.reply(conn, 0, self.letters(path).encode())
            elif op == "M":
                self.make_dirs(path)
                self.reply(conn, 0)
            elif op == "R" and not self.no_rename:
                self.rename(conn, path, self.recv(conn, size).decode(), hdr[5])
            elif op == "R":
                # No op R. A real old HBC leaves the destination unread (which
                # is why hbc.py asks its version first); read it here so the
                # test sees the answer, not a reset.
                self.recv(conn, size)
                self.reply(conn, -88)
            else:
                self.reply(conn, -88)  # newlib ENOSYS

    def rename(self, conn, src, dst, flags):
        """Op R as devfile.c's file_move() does it."""
        into = dst.endswith("/")
        if into and dst[-2] != ":":
            dst = dst[:-1]
        if into or (dst in self.dirs and dst.lower() != src.lower()):
            dst = dst.rstrip("/") + "/" + src.rsplit("/", 1)[1]
        is_dir = src in self.dirs
        if not is_dir and src not in self.files:
            return self.reply(conn, -ENOENT)
        if dst.split(":")[0] != src.split(":")[0]:
            return self.reply(conn, -18)   # newlib EXDEV
        if is_dir and dst.lower().startswith(src.lower() + "/"):
            return self.reply(conn, -EINVAL)
        if dst != src and (dst in self.files or dst in self.dirs):
            if not flags & 2:
                return self.reply(conn, -17)   # EEXIST
            if dst in self.dirs:
                return self.reply(conn, -EISDIR)
            if is_dir:
                return self.reply(conn, -ENOTDIR)
            if self.attrs.get(dst, 0) & 1:
                return self.reply(conn, -13)   # EACCES: read-only kept
            del self.files[dst]
        self.make_dirs(parent(dst))
        if is_dir:
            for p in [p for p in self.files if p.startswith(src + "/")]:
                self.files[dst + p[len(src):]] = self.files.pop(p)
            for p in [p for p in self.dirs if p == src or p.startswith(src + "/")]:
                self.dirs.discard(p)
                self.dirs.add(dst + p[len(src):])
        elif dst != src:
            self.files[dst] = self.files.pop(src)
        self.reply(conn, 0, dst.encode())


class HBCToolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fake = FakeHBC()
        hbc.PORT = cls.fake.port

    def setUp(self):
        self.fake.truncate = False
        self.fake.agent = False
        self.fake.agent_uptime_ms = 1000
        self.fake.crash = None
        self.fake.lastlog = None
        self.fake.exits = 0
        self.fake.next_result = None
        self.fake.upload = None
        self.fake.no_rename = False
        self.fake.version = "1.2.0"
        self.fake.attrs = {}
        self.fake.full = set()
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = pathlib.Path(tmp.name)

    def cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            hbc.main(["--wii", WII, *argv])
        return out.getvalue()

    def make_tree(self, base, files):
        for rel, data in files.items():
            path = pathlib.Path(base, *rel.split("/"))
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def remote(self, prefix):
        return {k: v for k, v in self.fake.files.items() if k.startswith(prefix)}

    def test_mii_faces(self):
        self.fake.version = "1.10.20"
        self.assertEqual(self.cli("mii-faces"), "Saved Mii faces to sd:/mii/RFL_Res.dat (671 KB)\n")
        # A failure prints HBC's own message and exits 1.
        self.fake.mii_faces = (-13, b"Mii faces: needs AHBPROT (HBC's own IOS)")
        try:
            with self.assertRaises(SystemExit) as caught:
                self.cli("mii-faces")
            self.assertEqual(caught.exception.code, 1)
            self.assertEqual(hbc.mii_faces(WII), (False, "Mii faces: needs AHBPROT (HBC's own IOS)"))
        finally:
            self.fake.mii_faces = (0, b"Saved Mii faces to sd:/mii/RFL_Res.dat (671 KB)")
        # An HBC before 1.10.20 has no HBCM: refused before asking.
        self.fake.version = "1.10.19"
        with self.assertRaises(SystemExit):
            self.cli("mii-faces")

    def test_version_and_status(self):
        self.assertEqual(hbc.version(WII), "1.2.0")
        self.assertEqual(hbc.status(WII)["version"], "1.2.0")

    def test_send_exits_a_running_agent_app_first(self):
        app = self.tmp / "app.dol"
        app.write_bytes(bytes(100))
        self.fake.agent = True
        self.cli("send", str(app))
        self.assertEqual(self.fake.exits, 1)
        self.assertFalse(self.fake.agent)
        # send() returns once the data is sent; the fake records it after.
        deadline = time.monotonic() + 5
        while self.fake.uploads[-1][1] != b"app.dol\0\0" and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(self.fake.uploads[-1][1], b"app.dol\0\0")

    def test_a_bench_job_leaves_an_older_agent_app_running(self):
        # Another workstation's app, running since before this job started.
        app = self.tmp / "app.dol"
        app.write_bytes(bytes(100))
        self.fake.agent = True
        self.fake.agent_uptime_ms = 600_000
        with mock.patch.dict(os.environ, WII_BENCH_JOB_START=str(time.time() - 30)):
            with self.assertRaisesRegex(SystemExit, "Wii busy: otherapp is running"):
                self.cli("send", str(app))
        self.assertEqual(self.fake.exits, 0)
        self.assertTrue(self.fake.agent)

    def test_a_bench_job_exits_an_agent_app_it_started(self):
        app = self.tmp / "app.dol"
        app.write_bytes(bytes(100))
        self.fake.agent = True
        self.fake.agent_uptime_ms = 5_000
        with mock.patch.dict(os.environ, WII_BENCH_JOB_START=str(time.time() - 30)):
            self.cli("send", str(app))
        self.assertEqual(self.fake.exits, 1)

    def test_exit_needs_an_agent_app(self):
        self.fake.agent = True
        self.assertEqual(self.cli("exit").strip(), "1.2.0")
        with self.assertRaisesRegex(SystemExit, "no agent app"):
            self.cli("exit")

    def test_crash_report_and_clear(self):
        self.fake.crash = {"app": "demo", "exception": 3, "name": "DSI", "pc": "80004cac",
                           "lr": "800047a8", "msr": "00009032", "cr": "20002494",
                           "ctr": "00000000", "dar": "00000010", "dsisr": "42000000",
                           "sp": "8008b160", "uptime_ms": 1002, "frames": ["80004004"]}
        out = self.cli("crash")
        self.assertIn("demo crashed after 1.0 s: DSI exception (3)", out)
        self.assertIn("dar   00000010", out)
        self.assertIn("#0   80004004", out)
        self.assertEqual(json.loads(self.cli("crash", "--json"))["pc"], "80004cac")
        self.fake.crash["reason"] = "agent crash hook faulted: pc 80bc74cc dar 00000398"
        self.assertIn("demo crashed after 1.0 s: DSI exception (3); agent crash hook faulted: "
                      "pc 80bc74cc dar 00000398", self.cli("crash"))
        self.cli("crash", "--clear")
        self.assertEqual(self.cli("crash").strip(), "no crash reported")
        self.fake.agent = True
        with self.assertRaisesRegex(SystemExit, "agent app is running"):
            self.cli("crash")

    def test_fatal_and_hang_reports(self):
        base = {"app": "demo", "exception": 0, "name": "unknown", "pc": "80004cac",
                "lr": "800047a8", "msr": "00009032", "cr": "00000000", "ctr": "00000000",
                "dar": "00000000", "dsisr": "00000000", "sp": "8008b160", "uptime_ms": 61500,
                "frames": ["80004004"]}
        self.fake.crash = dict(base, kind="fatal", code=0x81, reason="guest segfault at PC 80012345")
        self.fake.lastlog = ("fatal", 61500, "demo", "loading\nlevel 3\ndemo: fatal (129): guest segfault\n")
        out = self.cli("crash")
        # The app's own code is shown, never interpreted.
        self.assertIn("demo stopped after 61.5 s: guest segfault at PC 80012345 "
                      "(fatal, app code 129 = 0x81)", out)
        self.assertNotIn("dar", out)
        self.assertIn("  last output:\n    loading\n    level 3\n    demo: fatal (129)", out)
        self.fake.crash = dict(base, kind="hang", code=0, reason="no hbc_agent_alive() for 60 s")
        self.assertIn("demo hung after 61.5 s: no hbc_agent_alive() for 60 s", self.cli("crash"))
        self.assertNotIn("  left ", self.cli("crash"))
        # How the agent got the Wii back (HBC 1.10.18 on).
        self.fake.crash["way_out"] = "es_launch"
        self.assertIn("  left  HBC launched through ES (IOS acked every close)",
                      self.cli("crash"))
        self.fake.crash["way_out"] = "stub"
        self.assertIn("  left  through the reload stub (IOS well, ES launched no HBC title)", self.cli("crash"))
        self.fake.crash["way_out"] = "warp"   # from a newer agent: said, not dropped
        self.assertIn("  left  warp (a way out this hbc.py does not know)", self.cli("crash"))

    def test_lastlog(self):
        self.fake.crash = {}
        self.assertIn("no kept log", self.cli("lastlog"))
        self.fake.lastlog = ("exit", 4200, "demo", "hello\nbye\n")
        out = self.cli("lastlog")
        self.assertIn("-- demo (ended by exit, 4.2 s) --\nhello\nbye\n", out)
        self.assertEqual(json.loads(self.cli("lastlog", "--json"))["text"], "hello\nbye\n")

    def test_upload_names_for_the_play_log(self):
        def wait_upload(n):
            deadline = time.monotonic() + 5
            while len(self.fake.uploads) < n and time.monotonic() < deadline:
                time.sleep(0.05)

        folder = self.tmp / "apps" / "Wii64"
        folder.mkdir(parents=True)
        (folder / "boot.dol").write_bytes(bytes(64))
        (self.tmp / "netblock.elf").write_bytes(bytes(64))
        start = len(self.fake.uploads)
        self.cli("send", str(folder / "boot.dol"), "sd:/roms/Mario Kart 64.v64")
        self.cli("send", str(self.tmp / "netblock.elf"))
        self.cli("send", "--name", "WiiStation", str(folder / "boot.dol"), "--diag")
        wait_upload(start + 3)
        # The folder of a boot.dol, else the file; --name over both. The
        # app's arguments (a ROM, an option) are never the name.
        self.assertEqual(self.fake.upload_names[-3:], ["Wii64", "netblock", "WiiStation"])
        self.assertEqual(self.fake.uploads[-1][1], b"boot.dol\0--diag\0\0")

    def test_send_reports_what_hbc_made_of_the_upload(self):
        app = self.tmp / "app.dol"
        app.write_bytes(bytes(64))
        # An HBC that could not use it: send fails with HBC's message.
        self.fake.next_result = {"result": "error", "error": "not_wii_app",
                                 "text": "This is not a valid Wii application"}
        with self.assertRaises(SystemExit) as caught:
            self.cli("send", str(app))
        self.assertIn("not_wii_app", str(caught.exception.code))
        self.assertIn("not a valid Wii application", str(caught.exception.code))
        # A ZIP nobody answered for.
        self.fake.next_result = {"result": "declined", "error": None, "text": ""}
        with self.assertRaises(SystemExit) as caught:
            self.cli("send", str(app))
        self.assertIn("--yes", str(caught.exception.code))
        # --yes asks HBC to install it unasked (HBCA flag 1).
        self.fake.next_result = {"result": "installed", "error": None, "text": "myapp"}
        self.cli("send", "--yes", str(app))
        self.assertEqual(self.fake.upload_flags[-1], 1)
        # A launch is fine, and an older HBC is not asked at all.
        self.fake.next_result = {"result": "launched", "error": None, "text": ""}
        self.cli("send", str(app))
        self.assertEqual(self.fake.upload_flags[-1], 0)
        self.fake.next_result = None
        self.cli("send", str(app))

    def test_hw_prints_the_info_pages(self):
        out = self.cli("hw")
        self.assertIn("System\n  Console          Wii RVL-001(USA)", out)
        self.assertIn("USB\n  057e:0305        Device", out)

    def test_key_sends_presses(self):
        self.fake.keys = b""
        self.cli("key", "hrra")
        self.assertEqual(self.fake.keys, b"hrra")
        with self.assertRaisesRegex(SystemExit, "keys are"):
            self.cli("key", "x")

    def test_screen_writes_png(self):
        out = self.tmp / "tv.png"
        self.assertIn("4x2", self.cli("screen", str(out)))
        png = out.read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack(">II", png[16:24]), (4, 2))
        raw = zlib.decompress(png[png.index(b"IDAT") + 4:png.index(b"IEND") - 8])
        self.assertEqual(raw[0], 0)                 # filter byte, then RGB
        self.assertGreater(raw[1], 200)             # red
        self.assertLess(raw[3], 60)
        self.assertEqual(tuple(raw[7:10]), (255, 255, 255))

    def test_help(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            hbc.main([])
        self.assertIn("Common tasks", out.getvalue())
        self.assertIn("hbc.py help COMMAND", out.getvalue())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            hbc.main(["help", "sync"])
        self.assertTrue(out.getvalue().startswith("hbc.py sync [--delete] [--checksum] LOCAL PATH"))
        for name in ("status", "run", "get", "put", "key", "screen", "crash"):
            self.assertIn(name, hbc.COMMAND_HELP)
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as exc:
            hbc.main(["frob"])
        self.assertEqual(exc.exception.code, 2)

    def test_agent_versions(self):
        self.assertTrue(hbc.is_agent("1.5.0 agent"))
        self.assertFalse(hbc.is_agent("1.5.0"))

    def test_file_round_trip(self):
        data = bytes(range(256)) * 300
        hbc.file_request(WII, "P", "sd:/apps/x/data.bin", len(data), data)
        self.assertEqual(hbc.file_request(WII, "G", "sd:/apps/x/data.bin"), data)
        listing = hbc.file_request(WII, "L", "sd:/apps/x").decode()
        self.assertTrue(listing.startswith("#list 2\n"))
        self.assertRegex(listing, rf"\nf {len(data)} \d+ - data.bin\n")
        e = hbc.stat_remote(WII, "sd:/apps/x/data.bin")
        self.assertEqual((e["type"], e["size"], e["readonly"], e["name"]),
                         ("f", len(data), False, "data.bin"))
        self.assertEqual(hbc.checksum(WII, "sd:/apps/x/data.bin"), (len(data), zlib.crc32(data)))
        hbc.file_request(WII, "D", "sd:/apps/x/data.bin")
        with self.assertRaisesRegex(hbc.HBCError, "ENOENT"):
            hbc.file_request(WII, "G", "sd:/apps/x/data.bin")
        with self.assertRaisesRegex(hbc.HBCError, "EINVAL"):
            hbc.file_request(WII, "L", "sd:/../x")
        with self.assertRaises(hbc.HBCError) as cm:
            hbc.checksum(WII, "sd:/apps/x")
        self.assertEqual(cm.exception.code, EISDIR)

    def test_framed_round_trip(self):
        seen = []
        for data in (b"", b"a", bytes(200_000), bytes(range(256)) * 700):
            hbc.put_file(WII, "sd:/f.bin", data, progress=lambda d, t: seen.append((d, t)))
            self.assertEqual(self.fake.files["sd:/f.bin"], data)
            self.assertEqual(hbc.get_file(WII, "sd:/f.bin"), data)
        self.assertEqual(seen[-1], (179_200, 179_200))
        got = []
        hbc.get_file(WII, "sd:/f.bin", progress=lambda d, t: got.append(d))
        self.assertEqual(got, [65536, 131072, 179_200])

    def test_frames_compress_only_when_smaller(self):
        zeros = list(hbc.frames(bytes(70_000)))
        self.assertEqual(len(zeros), 2)
        raw_len, wire_len, _ = struct.unpack(">III", zeros[0][:12])
        self.assertEqual(raw_len, 65536)
        self.assertLess(wire_len, 1000)
        noise = list(hbc.frames(bytes((i * 7919) & 0xff ^ (i >> 8) for i in range(4096))))
        raw_len, wire_len, _ = struct.unpack(">III", noise[0][:12])
        self.assertLessEqual(wire_len, raw_len)

    def test_progress_only_on_a_tty(self):
        class Tty(io.StringIO):
            def isatty(self):
                return True

        stream = Tty()
        progress = hbc.Progress("x", stream)
        progress(1000, 1000)  # below the threshold
        progress(hbc.PROGRESS_MIN, hbc.PROGRESS_MIN)
        self.assertRegex(stream.getvalue(), r"^\rx: 100% +[\d.]+ MB/s\n$")
        quiet = io.StringIO()
        hbc.Progress("x", quiet)(hbc.PROGRESS_MIN, hbc.PROGRESS_MIN)
        self.assertEqual(quiet.getvalue(), "")

    def test_send_compresses_and_terminates_arguments(self):
        start = len(self.fake.uploads)
        app = self.tmp / "app.dol"
        app.write_bytes(bytes(4096))
        hbc.send(WII, str(app), ["a", "b c"])
        zipped = self.tmp / "app.zip"
        zipped.write_bytes(bytes(4096))
        hbc.send(WII, str(zipped), [])
        for _ in range(100):
            if len(self.fake.uploads) >= start + 2:
                break
            threading.Event().wait(0.05)
        (data, args), (zdata, zargs) = self.fake.uploads[-2:]
        self.assertEqual(data, bytes(4096))
        self.assertEqual(args, b"app.dol\0a\0b c\0\0")
        self.assertEqual(zdata, bytes(4096))  # ZIPs are sent as they are
        self.assertEqual(zargs, b"app.zip\0\0")

    def test_log_registration(self):
        server = hbc.LogServer(0)
        server.register(WII)
        self.assertTrue(hbc.status(WII)["log"].endswith(f":{server.port}"))
        server.unregister(WII)
        self.assertIsNone(hbc.status(WII)["log"])
        with self.assertRaises(OSError):
            hbc.LogServer(server.port)  # a second listener cannot share the port
        server.close()

    def test_run_prints_log_and_unregisters(self):
        app = self.tmp / "app.dol"
        app.write_bytes(bytes(1024))
        out = self.cli("run", str(app), "log-me", "-v")
        self.assertIn("hello from the app", out)
        self.assertEqual(self.fake.uploads[-1][1], b"app.dol\0log-me\0-v\0\0")
        self.assertEqual(self.fake.log_ports[-1], 0)
        self.assertIsNone(self.fake.log)

    def test_run_unregisters_on_timeout(self):
        app = self.tmp / "app.dol"
        app.write_bytes(bytes(1024))
        with self.assertRaisesRegex(SystemExit, "did not close its log"):
            self.cli("--timeout", "0.2", "run", str(app))
        self.assertNotEqual(self.fake.log_ports[-2], 0)
        self.assertEqual(self.fake.log_ports[-1], 0)

    def test_put_to_directory_appends_basename(self):
        local = self.tmp / "boot.dol"
        local.write_bytes(b"dol")
        self.cli("put", str(local), "sd:/apps/slash/")
        self.assertEqual(self.fake.files["sd:/apps/slash/boot.dol"], b"dol")
        with self.assertRaisesRegex(hbc.HBCError, "EISDIR"):
            hbc.put_file(WII, "sd:/apps/slash/", b"x")  # the Wii refuses it
        with self.assertRaisesRegex(SystemExit, "is a directory; use put -r"):
            self.cli("put", str(self.tmp), "sd:/apps/slash")
        with self.assertRaisesRegex(SystemExit, "is a directory; use get -r"):
            self.cli("get", "sd:/apps/slash", str(self.tmp / "x"))

    def test_recursive_put_get_round_trip(self):
        tree = {"boot.dol": b"a" * 100, "meta.xml": b"<app/>",
                "data/one.bin": bytes(300_000), "data/deep/two.bin": b"2"}
        src = self.tmp / "src"
        self.make_tree(src, tree)
        (src / "empty").mkdir()
        self.cli("put", "-r", str(src), "sd:/apps/tree")
        self.assertEqual(self.remote("sd:/apps/tree/"),
                         {f"sd:/apps/tree/{k}": v for k, v in tree.items()})
        self.assertIn("sd:/apps/tree/empty", self.fake.dirs)
        self.cli("get", "-r", "sd:/apps/tree", str(self.tmp / "dst"))
        for rel, data in tree.items():
            self.assertEqual((self.tmp / "dst" / rel).read_bytes(), data)
        self.assertTrue((self.tmp / "dst" / "empty").is_dir())
        self.cli("put", "--recursive", str(src), "sd:/apps/")
        self.assertIn("sd:/apps/src/data/deep/two.bin", self.fake.files)

    def test_recursive_rm_deletes_bottom_up(self):
        self.make_tree(self.tmp, {"a.txt": b"a", "sub/b.txt": b"b", "sub/deeper/c.txt": b"c"})
        hbc.put_tree(WII, str(self.tmp), "sd:/apps/gone", out=lambda _: None)
        start = len(self.fake.requests)
        self.cli("rm", "-r", "sd:/apps/gone/")
        deletes = [p for op, p in self.fake.requests[start:] if op == "D"]
        self.assertEqual(deletes[-3:], ["sd:/apps/gone/sub/deeper", "sd:/apps/gone/sub",
                                        "sd:/apps/gone"])
        self.assertEqual(set(deletes[:3]), {"sd:/apps/gone/a.txt", "sd:/apps/gone/sub/b.txt",
                                            "sd:/apps/gone/sub/deeper/c.txt"})
        self.assertFalse(self.remote("sd:/apps/gone"))
        self.assertNotIn("sd:/apps/gone", self.fake.dirs)

    def test_recursive_rm_refuses_roots_and_apps(self):
        start = len(self.fake.requests)
        for path in ("sd:/", "sd:", "SD:/", "usb:/", "carda:", "cardb:/", "sd:/apps",
                     "sd:/apps/", "SD:/Apps//", "usb:/./apps"):
            with self.assertRaisesRegex(SystemExit, "refusing"):
                self.cli("rm", "-r", path)
        with self.assertRaisesRegex(SystemExit, "refusing"):
            self.cli("sync", "--delete", str(self.tmp), "sd:/apps/")
        self.assertEqual(self.fake.requests[start:], [])
        self.assertFalse(hbc.protected("sd:/apps/myapp"))

    def test_errors_name_the_path(self):
        with self.assertRaisesRegex(SystemExit, "sd:/nosuch: Wii returned ENOENT"):
            self.cli("ls", "sd:/nosuch")
        missing = str(self.tmp / "nosuch.bin")
        with self.assertRaisesRegex(SystemExit, "nosuch.bin: no such file or folder on this PC"):
            self.cli("put", missing, "sd:/x.bin")

    def test_order_sets_and_shows_sort_ids(self):
        meta = b'<?xml version="1.0"?>\n<app version="1">\n\t<name>Beta</name>\n</app>\n'
        hbc.put_file(WII, "sd:/ordertest/beta/meta.xml", meta)
        hbc.put_file(WII, "sd:/ordertest/beta/boot.dol", b"b")
        hbc.put_file(WII, "sd:/ordertest/alpha/boot.dol", b"a")
        hbc.put_file(WII, "sd:/ordertest/gamma/boot.dol", b"g")
        self.cli("order", "sd:/ordertest", "gamma", "beta")
        self.assertEqual(self.fake.files["sd:/ordertest/beta/meta.xml"],
                         meta.replace(b"</app>", b"\t<sort_id>20</sort_id>\n</app>"))
        self.assertEqual(hbc.meta_sort_id(self.fake.files["sd:/ordertest/gamma/meta.xml"]), 10)
        self.assertNotIn("sd:/ordertest/alpha/meta.xml", self.fake.files)
        rows = json.loads(self.cli("--json", "order", "sd:/ordertest"))
        self.assertEqual([(r["sort_id"], r["folder"]) for r in rows],
                         [(10, "gamma"), (20, "beta"), (None, "alpha")])
        self.cli("order", "--clear", "sd:/ordertest")
        self.assertEqual(self.fake.files["sd:/ordertest/beta/meta.xml"], meta)
        with self.assertRaises(SystemExit):
            self.cli("order", "sd:/ordertest", "nosuch")

    def test_sync_by_date_reads_nothing(self):
        src = self.tmp / "dated"
        self.make_tree(src, {"big.img": b"x" * 5000, "small.txt": b"hi"})
        quiet = lambda _: None  # noqa: E731
        old = time.time() - 3600
        for f in src.iterdir():
            os.utime(f, (old, old))   # changed an hour ago, before the upload
        self.fake.clock = -7200   # a Wii two hours behind (another zone): no matter
        try:
            hbc.sync(WII, str(src), "sd:/apps/dated", out=quiet)
            start = len(self.fake.requests)
            self.assertEqual(hbc.sync(WII, str(src), "sd:/apps/dated", out=quiet), (0, 2, 0, 0))
            self.assertNotIn("C", {op for op, _ in self.fake.requests[start:]})
            # Changed here since (same size): the Wii's copy is older, so the
            # CRCs decide, and it goes up.
            (src / "small.txt").write_bytes(b"HI")
            self.assertEqual(hbc.sync(WII, str(src), "sd:/apps/dated", out=quiet)[:2], (1, 1))
            # --checksum reads every same-size file.
            start = len(self.fake.requests)
            hbc.sync(WII, str(src), "sd:/apps/dated", out=quiet, checksum_all=True)
            self.assertEqual(sum(op == "C" for op, _ in self.fake.requests[start:]), 2)
        finally:
            self.fake.clock = 0

    def test_old_listing_says_update(self):
        hbc.put_file(WII, "sd:/apps/old/a.bin", b"a")
        self.fake.old_listing = True
        try:
            with self.assertRaisesRegex(hbc.HBCError, "lists folders the old way"):
                hbc.list_dir(WII, "sd:/apps/old")
        finally:
            self.fake.old_listing = False

    def test_sync_goes_on_past_a_failed_file(self):
        src = self.tmp / "src2"
        self.make_tree(src, {"a.bin": b"a" * 10, "b.bin": b"b" * 20, "c.bin": b"c" * 30})
        quiet = lambda _: None  # noqa: E731
        hbc.sync(WII, str(src), "sd:/apps/sync2", out=quiet)
        (src / "a.bin").write_bytes(b"A" * 10)   # same size: the CRC decides
        (src / "c.bin").write_bytes(b"C" * 30)
        real = hbc.checksum
        seen = []

        def flaky(wii, remote, size=0):
            seen.append((remote, size))
            if remote.endswith("a.bin"):
                raise hbc.HBCError("timed out")
            return real(wii, remote, size)

        lines = []
        with mock.patch.object(hbc, "checksum", flaky):
            with self.assertRaisesRegex(hbc.HBCError, "1 file\\(s\\) failed: sd:/apps/sync2/a.bin"):
                hbc.sync(WII, str(src), "sd:/apps/sync2", out=lines.append)
        self.assertIn(("sd:/apps/sync2/a.bin", 10), seen)   # the size, for the timeout
        self.assertEqual(self.fake.files["sd:/apps/sync2/c.bin"], b"C" * 30)   # went on
        self.assertTrue(any("1 failed" in l for l in lines))

    def test_file_crc_streams(self):
        path = self.tmp / "big.bin"
        data = bytes(range(256)) * 9000   # over 1 MiB
        path.write_bytes(data)
        self.assertEqual(hbc.file_crc(str(path)), zlib.crc32(data))

    def test_sync_uploads_only_changes(self):
        src = self.tmp / "src"
        self.make_tree(src, {"boot.dol": b"one", "data/a.bin": b"a" * 50, "data/b.bin": b"b"})
        quiet = lambda _: None  # noqa: E731
        self.assertEqual(hbc.sync(WII, str(src), "sd:/apps/sync", out=quiet), (3, 0, 0, 54))
        self.assertEqual(hbc.sync(WII, str(src), "sd:/apps/sync", out=quiet), (0, 3, 0, 0))
        (src / "boot.dol").write_bytes(b"two")  # same size, different CRC
        start = len(self.fake.requests)
        self.assertEqual(hbc.sync(WII, str(src), "sd:/apps/sync", out=quiet), (1, 2, 0, 3))
        puts = [p for op, p in self.fake.requests[start:] if op in "pP"]
        self.assertEqual(puts, ["sd:/apps/sync/boot.dol"])
        hbc.put_file(WII, "sd:/apps/sync/extra.txt", b"x")
        hbc.put_file(WII, "sd:/apps/sync/old/stale.bin", b"y")
        out = self.cli("sync", str(src), "sd:/apps/sync", "--delete")
        self.assertIn("sync: 0 uploaded (0 bytes), 3 unchanged, 3 deleted", out)
        self.assertEqual(set(self.remote("sd:/apps/sync/")),
                         {"sd:/apps/sync/boot.dol", "sd:/apps/sync/data/a.bin",
                          "sd:/apps/sync/data/b.bin"})
        self.assertNotIn("sd:/apps/sync/old", self.fake.dirs)

    def test_truncated_listing_stops_recursive_ops(self):
        hbc.put_file(WII, "sd:/apps/trunc/a.bin", b"a")
        self.fake.truncate = True
        start = len(self.fake.requests)
        for argv in (("rm", "-r", "sd:/apps/trunc"),
                     ("get", "-r", "sd:/apps/trunc", str(self.tmp / "t")),
                     ("sync", "--delete", str(self.tmp), "sd:/apps/trunc")):
            with self.assertRaisesRegex(SystemExit, "truncated"):
                self.cli(*argv)
        self.assertEqual({op for op, _ in self.fake.requests[start:]}, {"L", "S"})   # reads only
        self.assertIn("sd:/apps/trunc/a.bin", self.fake.files)
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(err):
            hbc.main(["--wii", WII, "ls", "sd:/apps/trunc"])
        self.assertRegex(out.getvalue(), r"^f 1 \d{4}-\d\d-\d\d \d\d:\d\d a.bin\n$")
        self.assertIn("warning", err.getvalue())

    def test_json_output(self):
        hbc.put_file(WII, "sd:/apps/js/f.bin", b"12345")
        hbc.file_request(WII, "M", "sd:/apps/js/sub")
        self.assertEqual(json.loads(self.cli("--json", "version")), {"version": "1.2.0"})
        text = self.cli("status", "--json")
        self.assertEqual(json.loads(text)["proto"], 2)
        self.assertNotIn(" ", text.strip())
        listing = json.loads(self.cli("--json", "ls", "sd:/apps/js"))
        self.assertEqual([{k: e[k] for k in ("name", "type", "size", "readonly")} for e in listing],
                         [{"name": "sub", "type": "d", "size": 0, "readonly": False},
                          {"name": "f.bin", "type": "f", "size": 5, "readonly": False}])
        self.assertTrue(all(isinstance(e["mtime"], int) for e in listing))

    # ---- mv: op R (HBC 1.10.14) ------------------------------------------------

    def put(self, files):
        self.fake.version = "1.10.15"
        for path, data in files.items():
            hbc.put_file(WII, path, data)

    def test_mv_renames_and_moves(self):
        self.put({"sd:/mvt/a.bin": b"A", "sd:/mvt/dir/x.bin": b"X", "sd:/mvt/dir/sub/y.bin": b"Y"})
        quiet = lambda *a: None
        self.assertEqual(hbc.move(WII, "sd:/mvt/a.bin", "sd:/mvt/b.bin", out=quiet), "sd:/mvt/b.bin")
        self.assertEqual(self.fake.files["sd:/mvt/b.bin"], b"A")
        self.assertNotIn("sd:/mvt/a.bin", self.fake.files)
        # Into a folder: a trailing "/", or a folder that is there.
        self.assertEqual(hbc.move(WII, "sd:/mvt/b.bin", "sd:/mvt/new/", out=quiet), "sd:/mvt/new/b.bin")
        self.assertEqual(hbc.move(WII, "sd:/mvt/new/b.bin", "sd:/mvt/dir", out=quiet),
                         "sd:/mvt/dir/b.bin")
        # A folder moves with everything in it.
        self.assertEqual(hbc.move(WII, "sd:/mvt/dir", "sd:/mvt/moved", out=quiet), "sd:/mvt/moved")
        self.assertEqual(self.fake.files["sd:/mvt/moved/sub/y.bin"], b"Y")
        self.assertFalse([p for p in self.fake.files if p.startswith("sd:/mvt/dir/")])
        with self.assertRaisesRegex(hbc.HBCError, "EINVAL"):
            hbc.move(WII, "sd:/mvt/moved", "sd:/mvt/moved/sub/in", out=quiet)
        hbc.remove_tree(WII, "sd:/mvt", out=quiet)

    def test_mv_replaces_only_when_asked(self):
        self.put({"sd:/mvr/a.bin": b"new", "sd:/mvr/b.bin": b"old", "sd:/mvr/d/keep.bin": b"k"})
        with self.assertRaisesRegex(hbc.HBCError, "EEXIST"):
            hbc.move(WII, "sd:/mvr/a.bin", "sd:/mvr/b.bin", out=lambda *a: None)
        self.assertEqual(self.fake.files["sd:/mvr/b.bin"], b"old")
        out = self.cli("mv", "--replace", "sd:/mvr/a.bin", "sd:/mvr/b.bin")
        self.assertEqual(out.strip(), "sd:/mvr/a.bin -> sd:/mvr/b.bin")
        self.assertEqual(self.fake.files["sd:/mvr/b.bin"], b"new")
        # A folder never replaces a file, even with --replace; and a file "onto"
        # a folder goes into it, as mv does.
        with self.assertRaisesRegex(SystemExit, "ENOTDIR"):
            self.cli("rename", "--replace", "sd:/mvr/d", "sd:/mvr/b.bin")
        self.assertEqual(self.fake.files["sd:/mvr/b.bin"], b"new")
        self.fake.make_dirs("sd:/mvr/e")
        self.assertEqual(hbc.move(WII, "sd:/mvr/b.bin", "sd:/mvr/e", out=lambda *a: None),
                         "sd:/mvr/e/b.bin")
        hbc.remove_tree(WII, "sd:/mvr", out=lambda *a: None)

    def test_mv_between_devices_copies_checks_then_deletes(self):
        self.put({"sd:/mvx/one.bin": os.urandom(70_000), "sd:/mvx/tree/a.bin": b"a" * 5000,
                  "sd:/mvx/tree/deep/b.bin": b"b"})
        self.fake.make_dirs("sd:/mvx/tree/empty")
        one = self.fake.files["sd:/mvx/one.bin"]
        start = len(self.fake.requests)
        self.assertEqual(hbc.move(WII, "sd:/mvx/one.bin", "usb:/mvx/", out=lambda *a: None),
                         "usb:/mvx/one.bin")
        self.assertEqual(self.fake.files["usb:/mvx/one.bin"], one)
        self.assertNotIn("sd:/mvx/one.bin", self.fake.files)
        ops = [op for op, _ in self.fake.requests[start:]]
        self.assertLess(ops.index("C"), ops.index("D"), "checked before the source is deleted")
        hbc.move(WII, "sd:/mvx/tree", "usb:/tree2", out=lambda *a: None)
        self.assertEqual(self.fake.files["usb:/tree2/deep/b.bin"], b"b")
        self.assertIn("usb:/tree2/empty", self.fake.dirs)
        self.assertFalse([p for p in self.fake.files if p.startswith("sd:/mvx/tree")])
        self.assertNotIn("sd:/mvx/tree", self.fake.dirs)
        # An existing destination on the other device is refused there too.
        self.put({"sd:/mvx/c.bin": b"c", "usb:/mvx/c.bin": b"other"})
        with self.assertRaisesRegex(hbc.HBCError, "EEXIST"):
            hbc.move(WII, "sd:/mvx/c.bin", "usb:/mvx/c.bin", out=lambda *a: None)
        self.assertEqual(self.fake.files["sd:/mvx/c.bin"], b"c")

    def test_mv_refusals(self):
        self.fake.version = "1.10.14"
        with self.assertRaisesRegex(hbc.HBCError, "refusing to move"):
            hbc.move(WII, "sd:/apps", "sd:/apps2", out=lambda *a: None)
        with self.assertRaisesRegex(hbc.HBCError, "refusing to move"):
            hbc.move(WII, "usb:/", "sd:/x", out=lambda *a: None)
        with self.assertRaisesRegex(hbc.HBCError, "ENOENT"):
            hbc.move(WII, "sd:/nosuch.bin", "sd:/x.bin", out=lambda *a: None)
        # An HBC before op R: refused before anything is sent. It would answer
        # ENOSYS without reading the destination, and could reset the
        # connection first.
        self.put({"sd:/old.bin": b"o"})
        self.fake.version = "1.10.13"
        start = len(self.fake.requests)
        with self.assertRaisesRegex(hbc.HBCError, r"\(1\.10\.13\) cannot rename or move.*1\.10\.14"):
            hbc.move(WII, "sd:/old.bin", "sd:/new.bin", out=lambda *a: None)
        self.assertEqual(self.fake.requests[start:], [], "nothing sent")
        self.assertIn("sd:/old.bin", self.fake.files)
        # One that claims a new enough version but has no op R still says why.
        self.fake.version = "1.10.14"
        self.fake.no_rename = True
        with self.assertRaisesRegex(hbc.HBCError, "cannot rename or move"):
            hbc.move(WII, "sd:/old.bin", "sd:/new.bin", out=lambda *a: None)

    # ---- attributes: op A, listings, rm (HBC 1.10.15) ------------------------------

    def test_parse_entry_attributes(self):
        e = hbc.parse_entry("f 5 100 rha x.bin")
        self.assertEqual((e["attrs"], e["readonly"], e["hidden"], e["system"], e["archive"]),
                         ("rha", True, True, False, True))
        e = hbc.parse_entry("d 0 100 - dir name")
        self.assertEqual((e["attrs"], e["readonly"], e["name"]), ("", False, "dir name"))
        self.assertTrue(hbc.parse_entry("f 1 1 r old.bin")["readonly"], "HBC 1.10.6-1.10.14")

    def test_attrib_shows_and_changes(self):
        self.fake.version = "1.10.15"
        hbc.put_file(WII, "sd:/att/a.bin", b"a")
        hbc.put_file(WII, "sd:/att/sub/b.bin", b"b")
        self.fake.version = "1.10.15"
        self.assertEqual(self.cli("attrib", "sd:/att/a.bin").strip(), "----  sd:/att/a.bin")
        self.assertEqual(self.cli("attrib", "+rh", "+a", "sd:/att/a.bin").strip(), "rh-a  sd:/att/a.bin")
        self.assertEqual(self.fake.attrs["sd:/att/a.bin"], 0x23)
        self.assertEqual(self.cli("attrib", "-h", "sd:/att/a.bin").strip(), "r--a  sd:/att/a.bin")
        # ls and stat show them; -r is an attribute here, not --recursive.
        self.assertIn(" ra a.bin", self.cli("ls", "sd:/att"))
        self.assertIn(", read-only, archive", self.cli("stat", "sd:/att/a.bin"))
        listing = json.loads(self.cli("--json", "ls", "sd:/att"))
        a = next(e for e in listing if e["name"] == "a.bin")
        self.assertEqual((a["attrs"], a["readonly"], a["archive"], a["hidden"]), ("ra", True, True, False))
        out = self.cli("attrib", "--recursive", "+s", "sd:/att")
        self.assertEqual(sorted(line.split()[1] for line in out.splitlines()),
                         ["sd:/att", "sd:/att/a.bin", "sd:/att/sub", "sd:/att/sub/b.bin"])
        self.assertTrue(all(self.fake.attrs[p] & 4 for p in ("sd:/att", "sd:/att/sub/b.bin")))
        self.cli("attrib", "--recursive", "-rhsa", "sd:/att")
        self.assertFalse(any(self.fake.attrs.values()))
        hbc.remove_tree(WII, "sd:/att", out=lambda *a: None)

    def test_rm_read_only_and_force(self):
        self.fake.version = "1.10.15"
        hbc.put_file(WII, "sd:/ro/a.bin", b"a")
        hbc.put_file(WII, "sd:/ro/sub/b.bin", b"b")
        self.fake.version = "1.10.15"
        self.cli("attrib", "+r", "sd:/ro/a.bin")
        with self.assertRaisesRegex(SystemExit, "EACCES.*read-only.*attrib -r sd:/ro/a.bin"):
            self.cli("rm", "sd:/ro/a.bin")
        self.assertIn("sd:/ro/a.bin", self.fake.files)
        self.cli("rm", "--force", "sd:/ro/a.bin")
        self.assertNotIn("sd:/ro/a.bin", self.fake.files)
        with self.assertRaisesRegex(SystemExit, "ENOTEMPTY"):
            self.cli("rm", "sd:/ro/sub")
        self.cli("attrib", "--recursive", "+r", "sd:/ro")
        with self.assertRaisesRegex(SystemExit, "EACCES"):
            self.cli("rm", "-r", "sd:/ro")
        self.cli("rm", "-r", "--force", "sd:/ro")
        self.assertNotIn("sd:/ro", self.fake.dirs)

    def test_attrib_needs_a_new_enough_wii(self):
        self.fake.version = "1.10.14"
        start = len(self.fake.requests)
        with self.assertRaisesRegex(SystemExit, r"\(1\.10\.14\) cannot show or change attributes.*1\.10\.15"):
            self.cli("attrib", "-r", "sd:/x")
        with self.assertRaisesRegex(SystemExit, "1\\.10\\.15"):
            self.cli("rm", "--force", "sd:/x")
        self.assertEqual(self.fake.requests[start:], [], "nothing sent")

    # ---- the 2026-10-24 audit of mv, attrib and rm --force ---------------------------

    def test_mv_across_refuses_read_only_and_cleans_up(self):
        self.put({"sd:/au/t/a.bin": b"a", "sd:/au/t/b.bin": b"b", "sd:/au/one.bin": b"1"})
        self.fake.attrs["sd:/au/t/b.bin"] = 1
        quiet = lambda *a: None
        start = len(self.fake.requests)
        with self.assertRaisesRegex(hbc.HBCError, "read-only.*nothing was copied"):
            hbc.move(WII, "sd:/au/t", "usb:/au/", out=quiet)
        self.assertFalse([op for op, _ in self.fake.requests[start:] if op in "pPMDR" and op != "R"])
        self.assertIn("sd:/au/t/b.bin", self.fake.files)
        # A copy that fails part way is removed again; the source stays.
        self.fake.attrs.clear()
        self.fake.full = {"usb:/au/t/b.bin"}
        with self.assertRaisesRegex(hbc.HBCError, "ENOSPC.*removed again.*sd:/au/t is kept"):
            hbc.move(WII, "sd:/au/t", "usb:/au/", out=quiet)
        self.assertFalse([p for p in self.fake.files if p.startswith("usb:/au")])
        self.assertNotIn("usb:/au/t", self.fake.dirs)
        self.assertEqual(self.fake.files["sd:/au/t/a.bin"], b"a")
        # A file over the Wii's upload limit is refused before anything moves.
        self.fake.files["sd:/au/big.bin"] = b""
        orig = self.fake.entry
        self.fake.entry = lambda kind, path: (f"f {600 << 20} 0 a big.bin\n" if path == "sd:/au/big.bin"
                                              else orig(kind, path))
        try:
            with self.assertRaisesRegex(hbc.HBCError, "512 MiB"):
                hbc.move(WII, "sd:/au/big.bin", "usb:/au/", out=quiet)
        finally:
            self.fake.entry = orig

    def test_mv_replace_keeps_read_only_and_json(self):
        self.put({"sd:/aj/a.bin": b"a", "sd:/aj/b.bin": b"b"})
        self.fake.attrs["sd:/aj/b.bin"] = 1
        with self.assertRaisesRegex(SystemExit, "EACCES"):
            self.cli("mv", "--replace", "sd:/aj/a.bin", "sd:/aj/b.bin")
        self.assertEqual(self.fake.files["sd:/aj/b.bin"], b"b")
        self.assertEqual(json.loads(self.cli("--json", "mv", "sd:/aj/a.bin", "sd:/aj/c/")),
                         {"from": "sd:/aj/a.bin", "to": "sd:/aj/c/a.bin"})
        with self.assertRaisesRegex(hbc.HBCError, "under 256 bytes"):
            hbc.move(WII, "sd:/aj/c/a.bin", "sd:/" + "x" * 300, out=lambda *a: None)

    def test_attrib_bad_letters_and_json(self):
        self.put({"sd:/ab/a.bin": b"a"})
        for bad in ("+x", "-rq", "+"):
            with self.assertRaisesRegex(SystemExit, "attrib: .*r, h, s, a"):
                self.cli("attrib", bad, "sd:/ab/a.bin")
        self.assertEqual(json.loads(self.cli("--json", "attrib", "+h", "sd:/ab/a.bin")),
                         {"path": "sd:/ab/a.bin", "attrs": "h"})

    def test_rm_force_clears_read_only_only_when_needed(self):
        self.put({"sd:/af/full/x.bin": b"x", "sd:/af/plain.bin": b"p"})
        self.fake.attrs["sd:/af/full"] = 1
        start = len(self.fake.requests)
        self.cli("rm", "--force", "sd:/af/plain.bin")      # not read-only: no op A
        self.assertEqual([op for op, _ in self.fake.requests[start:] if op == "A"], [])
        with self.assertRaisesRegex(SystemExit, "ENOTEMPTY"):
            self.cli("rm", "--force", "sd:/af/full")       # not empty: read-only kept
        self.assertEqual(self.fake.attrs["sd:/af/full"], 1)


if __name__ == "__main__":
    unittest.main()
