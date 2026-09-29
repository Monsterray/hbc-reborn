#!/usr/bin/env python3
"""Talk to a running Homebrew Channel over the network.

usage: hbc.py [--wii ADDR] COMMAND ...

  version                 print the running HBC version
  status                  print HBC's JSON status
  wait [SECONDS]          wait until HBC answers (default 90 s)
  send FILE [ARG ...]     send a DOL, ELF or ZIP (Wiiload)
  run FILE [ARG ...]      register for logs, send FILE, print its output
  log                     register for logs and print app output until ^C
  ls REMOTE               list a directory, e.g. sd:/apps
  get REMOTE [LOCAL]      download a file
  put LOCAL REMOTE        upload a file (parent directories are created)
  rm REMOTE               delete a file or an empty directory
  mkdir REMOTE            create a directory

The Wii address comes from --wii, $HBC_WII, $WII_BENCH_IP, or $WIILOAD
("tcp:ADDR"). Only hosts on the Wii's own /16 network are answered.
"""

import argparse
import errno
import json
import os
import socket
import struct
import sys
import threading
import time
import zlib

PORT = 4299
LOG_PORT = 4405
TIMEOUT = 15


class HBCError(Exception):
    pass


def wii_address(arg):
    for value in (arg, os.environ.get("HBC_WII"), os.environ.get("WII_BENCH_IP"),
                  os.environ.get("WIILOAD", "").removeprefix("tcp:")):
        if value:
            return value
    raise SystemExit("set the Wii address with --wii or $HBC_WII")


def connect(wii):
    conn = socket.create_connection((wii, PORT), timeout=TIMEOUT)
    conn.settimeout(TIMEOUT)
    return conn


def recv_exact(conn, n):
    data = bytearray()
    while len(data) < n:
        chunk = conn.recv(min(n - len(data), 65536))
        if not chunk:
            raise HBCError("connection closed early")
        data += chunk
    return bytes(data)


def recv_reply(conn):
    status, length = struct.unpack(">iI", recv_exact(conn, 8))
    if status < 0:
        name = errno.errorcode.get(-status, str(-status))
        raise HBCError(f"Wii returned {name} ({os.strerror(-status)})")
    return recv_exact(conn, length)


def request(wii, header, payload=b""):
    """Send one 16-byte request (plus payload) and return the reply body."""
    with connect(wii) as conn:
        conn.sendall(header.ljust(16, b"\0") + payload)
        return recv_reply(conn)


def version(wii):
    with connect(wii) as conn:
        conn.sendall(b"HBCV" + bytes(12))
        reply = bytearray()
        while b"\0" not in reply:
            chunk = conn.recv(64)
            if not chunk:
                break
            reply += chunk
    text = bytes(reply).split(b"\0", 1)[0].decode("ascii", "replace")
    if not text:
        raise HBCError("no version reply (older HBC?)")
    return text


def status(wii):
    return json.loads(request(wii, b"HBCS"))


def wait(wii, seconds):
    deadline, last = time.monotonic() + seconds, None
    while time.monotonic() < deadline:
        try:
            return version(wii)
        except (OSError, HBCError) as exc:
            last = exc
            time.sleep(1)
    raise HBCError(f"HBC did not answer within {seconds} s: {last}")


def file_request(wii, op, remote, size=0, payload=b""):
    path = remote.encode("utf-8")
    header = b"HBCF" + op.encode() + b"\0" + struct.pack(">HI", len(path), size)
    return request(wii, header, path + payload)


def send(wii, path, args):
    data = open(path, "rb").read()
    packed = zlib.compress(data, 6)
    if path.lower().endswith(".zip") or len(packed) >= len(data):
        packed, unpacked_len = data, 0
    else:
        unpacked_len = len(data)
    argv = b"".join(a.encode("utf-8") + b"\0" for a in [os.path.basename(path)] + args)
    if len(argv) + 1 > 1024:
        raise HBCError("arguments are longer than 1023 bytes")
    argv += b"\0"
    header = b"HAXX" + bytes((0, 5)) + struct.pack(">HII", len(argv), len(packed), unpacked_len)
    with connect(wii) as conn:
        conn.sendall(header + packed + argv)


class LogServer:
    """Accept app log connections and copy them to stdout."""

    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.done = threading.Event()

    def serve(self, once=False):
        while True:
            conn, (addr, _) = self.sock.accept()
            print(f"[hbc log] {addr} connected", file=sys.stderr, flush=True)
            with conn:
                try:
                    while chunk := conn.recv(4096):
                        sys.stdout.buffer.write(chunk)
                        sys.stdout.flush()
                except ConnectionResetError:
                    pass  # IOS closes sockets with a reset
            print(f"[hbc log] {addr} closed", file=sys.stderr, flush=True)
            if once:
                self.done.set()
                return

    def register(self, wii):
        request(wii, b"HBCN" + struct.pack(">H", self.port))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--wii", help="Wii IPv4 address")
    parser.add_argument("--log-port", type=int, default=LOG_PORT)
    parser.add_argument("--timeout", type=float, default=300,
                        help="seconds `run` waits for the app to finish")
    parser.add_argument("command")
    parser.add_argument("args", nargs="*")
    opts = parser.parse_args()
    cmd, args = opts.command, opts.args
    wii = wii_address(opts.wii)

    def need(n, usage):
        if len(args) < n:
            raise SystemExit(f"usage: hbc.py {cmd} {usage}")

    try:
        if cmd == "version":
            print(version(wii))
        elif cmd == "status":
            print(json.dumps(status(wii), indent=2))
        elif cmd == "wait":
            print(wait(wii, float(args[0]) if args else 90))
        elif cmd == "send":
            need(1, "FILE [ARG ...]")
            send(wii, args[0], args[1:])
        elif cmd in ("run", "log"):
            if cmd == "run":
                need(1, "FILE [ARG ...]")
            server = LogServer(opts.log_port)
            server.register(wii)
            print(f"[hbc log] listening on port {server.port}", file=sys.stderr, flush=True)
            if cmd == "log":
                server.serve()
            thread = threading.Thread(target=server.serve, args=(True,), daemon=True)
            thread.start()
            send(wii, args[0], args[1:])
            if not server.done.wait(opts.timeout):
                raise HBCError(f"the app did not close its log within {opts.timeout} s")
        elif cmd == "ls":
            need(1, "REMOTE")
            sys.stdout.write(file_request(wii, "L", args[0]).decode("utf-8", "replace"))
        elif cmd == "get":
            need(1, "REMOTE [LOCAL]")
            data = file_request(wii, "G", args[0])
            local = args[1] if len(args) > 1 else os.path.basename(args[0].rstrip("/"))
            open(local, "wb").write(data)
            print(f"{args[0]} -> {local} ({len(data)} bytes)")
        elif cmd == "put":
            need(2, "LOCAL REMOTE")
            data = open(args[0], "rb").read()
            file_request(wii, "P", args[1], len(data), data)
            print(f"{args[0]} -> {args[1]} ({len(data)} bytes)")
        elif cmd == "rm":
            need(1, "REMOTE")
            file_request(wii, "D", args[0])
        elif cmd == "mkdir":
            need(1, "REMOTE")
            file_request(wii, "M", args[0])
        else:
            parser.error(f"unknown command {cmd!r}")
    except (OSError, HBCError) as exc:
        raise SystemExit(f"hbc.py: {exc}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
