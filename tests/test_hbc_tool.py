"""Check tools/hbc.py against a fake HBC that follows docs/devnet.md."""

import json
import pathlib
import socket
import struct
import sys
import tempfile
import threading
import unittest
import zlib

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

ENOENT, EINVAL = 2, 22


class FakeHBC:
    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.files = {}
        self.log = None
        self.uploads = []
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            conn, (addr, _) = self.sock.accept()
            with conn:
                self.handle(conn, addr)

    @staticmethod
    def recv(conn, n):
        data = b""
        while len(data) < n:
            data += conn.recv(n - len(data))
        return data

    def reply(self, conn, status, payload=b""):
        conn.sendall(struct.pack(">iI", status, len(payload)) + payload)

    def handle(self, conn, addr):
        hdr = self.recv(conn, 16)
        magic = hdr[:4]
        if magic == b"HBCV":
            conn.sendall(b"1.2.0\0")
        elif magic == b"HBCS":
            self.reply(conn, 0, json.dumps({"version": "1.2.0", "proto": 2,
                                             "log": self.log}).encode())
        elif magic == b"HBCN":
            self.log = f"{addr}:{struct.unpack('>H', hdr[4:6])[0]}"
            self.reply(conn, 0)
        elif magic == b"HAXX":
            args_len, size, size_un = struct.unpack(">HII", hdr[6:16])
            data = self.recv(conn, size)
            args = self.recv(conn, args_len)
            self.uploads.append((zlib.decompress(data) if size_un else data, args))
        elif magic == b"HBCF":
            op = chr(hdr[4])
            path_len, size = struct.unpack(">HI", hdr[6:12])
            path = self.recv(conn, path_len).decode()
            if ".." in path:
                self.reply(conn, -EINVAL)
            elif op == "P":
                self.files[path] = self.recv(conn, size)
                self.reply(conn, 0)
            elif op == "G":
                if path in self.files:
                    self.reply(conn, 0, self.files[path])
                else:
                    self.reply(conn, -ENOENT)
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
                self.files[path] = bytes(data)
                self.reply(conn, 0)
            elif op == "g":
                data = self.files.get(path)
                if data is None:
                    self.reply(conn, -ENOENT)
                    return
                conn.sendall(struct.pack(">iI", 0, len(data)))
                for frame in hbc.frames(data, level=6 if hdr[5] & 1 else 0):
                    conn.sendall(frame)
                conn.sendall(bytes(12))
            elif op == "L":
                lines = "".join(f"f {len(v)} {k.rsplit('/', 1)[1]}\n"
                                for k, v in self.files.items() if k.startswith(path + "/"))
                self.reply(conn, 0, lines.encode())
            elif op == "D":
                self.reply(conn, 0 if self.files.pop(path, None) is not None else -ENOENT)
            else:
                self.reply(conn, 0)


class HBCToolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fake = FakeHBC()
        hbc.PORT = cls.fake.port

    def test_version_and_status(self):
        self.assertEqual(hbc.version("127.0.0.1"), "1.2.0")
        self.assertEqual(hbc.status("127.0.0.1")["version"], "1.2.0")

    def test_file_round_trip(self):
        data = bytes(range(256)) * 300
        hbc.file_request("127.0.0.1", "P", "sd:/apps/x/data.bin", len(data), data)
        self.assertEqual(hbc.file_request("127.0.0.1", "G", "sd:/apps/x/data.bin"), data)
        listing = hbc.file_request("127.0.0.1", "L", "sd:/apps/x").decode()
        self.assertIn(f"f {len(data)} data.bin", listing)
        hbc.file_request("127.0.0.1", "D", "sd:/apps/x/data.bin")
        with self.assertRaisesRegex(hbc.HBCError, "ENOENT"):
            hbc.file_request("127.0.0.1", "G", "sd:/apps/x/data.bin")
        with self.assertRaisesRegex(hbc.HBCError, "EINVAL"):
            hbc.file_request("127.0.0.1", "L", "sd:/../x")

    def test_framed_round_trip(self):
        for data in (b"", b"a", bytes(200_000), bytes(range(256)) * 700):
            hbc.put_file("127.0.0.1", "sd:/f.bin", data)
            self.assertEqual(self.fake.files["sd:/f.bin"], data)
            self.assertEqual(hbc.get_file("127.0.0.1", "sd:/f.bin"), data)

    def test_frames_compress_only_when_smaller(self):
        zeros = list(hbc.frames(bytes(70_000)))
        self.assertEqual(len(zeros), 2)
        raw_len, wire_len, _ = struct.unpack(">III", zeros[0][:12])
        self.assertEqual(raw_len, 65536)
        self.assertLess(wire_len, 1000)
        noise = list(hbc.frames(bytes((i * 7919) & 0xff ^ (i >> 8) for i in range(4096)) ))
        raw_len, wire_len, _ = struct.unpack(">III", noise[0][:12])
        self.assertLessEqual(wire_len, raw_len)

    def test_send_compresses_and_terminates_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = pathlib.Path(tmp, "app.dol")
            app.write_bytes(bytes(4096))
            hbc.send("127.0.0.1", str(app), ["a", "b c"])
            zipped = pathlib.Path(tmp, "app.zip")
            zipped.write_bytes(bytes(4096))
            hbc.send("127.0.0.1", str(zipped), [])
        for _ in range(100):
            if len(self.fake.uploads) >= 2:
                break
            threading.Event().wait(0.05)
        (data, args), (zdata, zargs) = self.fake.uploads[-2:]
        self.assertEqual(data, bytes(4096))
        self.assertEqual(args, b"app.dol\0a\0b c\0\0")
        self.assertEqual(zdata, bytes(4096))  # ZIPs are sent as they are
        self.assertEqual(zargs, b"app.zip\0\0")

    def test_log_registration(self):
        server = hbc.LogServer(0)
        server.register("127.0.0.1")
        self.assertTrue(hbc.status("127.0.0.1")["log"].endswith(f":{server.port}"))
        server.sock.close()


if __name__ == "__main__":
    unittest.main()
