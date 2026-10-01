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
        self.dirs = {"sd:/"}
        self.log = None
        self.log_ports = []
        self.uploads = []
        self.upload_names = []  # HBCA: the play log's name for the next upload
        self.requests = []  # (op, path) of every HBCF request
        self.truncate = False
        self.agent = False  # an agent app answers instead of HBC
        self.agent_uptime_ms = 1000
        self.crash = None
        self.lastlog = None  # (why, uptime_ms, app, text) for HBCL
        self.exits = 0
        self.keys = b""
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
            conn.sendall(b"1.5.0 agent\0" if self.agent else b"1.2.0\0")
        elif magic == b"HBCS":
            st = {"version": "1.2.0", "proto": 2, "log": self.log}
            if self.agent:
                st = {"agent": True, "version": "1.5.0", "proto": 3, "log": self.log,
                      "app": "otherapp", "uptime_ms": self.agent_uptime_ms}
            elif self.crash is not None:
                st.update(proto=4, crash=self.crash or None,
                          lastlog={"why": self.lastlog[0], "bytes": len(self.lastlog[3])}
                          if self.lastlog else None)
            self.reply(conn, 0, json.dumps(st).encode())
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
            n = struct.unpack(">H", hdr[4:6])[0]
            self.upload_names.append(self.recv(conn, n).decode())
            self.reply(conn, 0)
        elif magic == b"HAXX":
            args_len, size, size_un = struct.unpack(">HII", hdr[6:16])
            data = self.recv(conn, size)
            args = self.recv(conn, args_len)
            self.uploads.append((zlib.decompress(data) if size_un else data, args))
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
                self.reply(conn, 0)
            elif op in "GgC" and path not in self.files:
                self.reply(conn, -(EISDIR if path in self.dirs and op == "C" else ENOENT))
            elif op == "G":
                self.reply(conn, 0, self.files[path])
            elif op == "C":
                data = self.files[path]
                self.reply(conn, 0, struct.pack(">II", len(data), zlib.crc32(data)))
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
                lines = "".join(f"d {p.rsplit('/', 1)[1]}\n" if kind == "d" else
                                f"f {len(self.files[p])} {p.rsplit('/', 1)[1]}\n"
                                for kind, p in self.children(path))
                if self.truncate:
                    lines += "! truncated\n"
                self.reply(conn, 0, lines.encode())
            elif op == "D":
                if path in self.files:
                    del self.files[path]
                    self.reply(conn, 0)
                elif path in self.dirs:
                    if self.children(path):
                        self.reply(conn, -ENOTEMPTY)
                    else:
                        self.dirs.discard(path)
                        self.reply(conn, 0)
                else:
                    self.reply(conn, -ENOENT)
            elif op == "M":
                self.make_dirs(path)
                self.reply(conn, 0)
            else:
                self.reply(conn, -88)  # newlib ENOSYS


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
        self.assertTrue(out.getvalue().startswith("hbc.py sync [--delete] LOCAL PATH"))
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
        self.assertIn(f"f {len(data)} data.bin", listing)
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
        self.assertEqual({op for op, _ in self.fake.requests[start:]}, {"L", "C"})
        self.assertIn("sd:/apps/trunc/a.bin", self.fake.files)
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(err):
            hbc.main(["--wii", WII, "ls", "sd:/apps/trunc"])
        self.assertEqual(out.getvalue(), "f 1 a.bin\n")
        self.assertIn("warning", err.getvalue())

    def test_json_output(self):
        hbc.put_file(WII, "sd:/apps/js/f.bin", b"12345")
        hbc.file_request(WII, "M", "sd:/apps/js/sub")
        self.assertEqual(json.loads(self.cli("--json", "version")), {"version": "1.2.0"})
        text = self.cli("status", "--json")
        self.assertEqual(json.loads(text)["proto"], 2)
        self.assertNotIn(" ", text.strip())
        self.assertEqual(json.loads(self.cli("--json", "ls", "sd:/apps/js")),
                         [{"name": "sub", "type": "d", "size": 0},
                          {"name": "f.bin", "type": "f", "size": 5}])


if __name__ == "__main__":
    unittest.main()
