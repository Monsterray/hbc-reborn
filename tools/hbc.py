#!/usr/bin/env python3
"""Talk to a running Homebrew Channel over the network.

usage: hbc.py [--wii ADDR] [--json] [--log-port PORT] [--timeout SECONDS] COMMAND ...

  version                     print the running HBC version ("... agent" when
                              an app with sdk/hbc_agent answers instead)
  status                      print HBC's (or the running app's) JSON status
  wait [SECONDS]              wait until HBC answers (default 90 s)
  send FILE [ARG ...]         send a DOL, ELF or ZIP (Wiiload)
  run FILE [ARG ...]          register for logs, send FILE, print its output
  exit                        ask the running agent app to exit to HBC, and
                              wait for HBC
  key KEYS                    send controller presses to the running agent app:
                              h (HOME: opens or closes its overlay), u d l r
                              (D-pad), a, b; e.g. `key hrra`
  screen FILE.png             save what the running agent app shows on the TV
  crash [--elf FILE] [--clear]
                              print the crash an agent app reported (with
                              source lines from FILE via addr2line), or
                              clear it
  log                         register for logs and print app output until ^C
  ls REMOTE                   list a directory, e.g. sd:/apps
  get REMOTE [LOCAL]          download a file
  get -r REMOTEDIR [LOCALDIR] download a directory tree
  put LOCAL REMOTE            upload a file (parent directories are created)
  put -r LOCALDIR REMOTEDIR   upload a directory tree
  rm REMOTE                   delete a file or an empty directory
  rm -r REMOTE                delete a directory tree
  mkdir REMOTE                create a directory and its parents
  sync [--delete] LOCALDIR REMOTEDIR
                              make REMOTEDIR mirror LOCALDIR: upload files whose
                              size or CRC-32 differ and create missing
                              directories; --delete also removes remote files
                              and directories that LOCALDIR lacks

Options:
  --wii ADDR          Wii IPv4 address
  --json              JSON output: version {"version": ...}, status compact,
                      ls [{"name", "type": "f"|"d", "size"}] (0 for directories)
  --log-port PORT     TCP port for app logs (default 4405)
  --timeout SECONDS   how long `run` waits for the app to close its log (300)
  -r, --recursive     get, put, rm: work on a directory tree
  --delete            sync: remove remote entries not present locally
  --elf FILE          crash: the app's ELF, for function names and lines
  --clear             crash: forget the reported crash

Options may also follow the command, except for send and run, where
everything after FILE goes to the app. `--` ends option parsing.

A target ending in "/" names a directory and gets the source's base name
appended: `put app.dol sd:/apps/myapp/` writes sd:/apps/myapp/app.dol, and
`get` into an existing local directory does the same. Without -r a
directory source is an error. Recursive commands list the whole remote tree
before acting and stop if the Wii truncated any listing (256 KiB limit);
plain `ls` prints a warning instead. `rm -r` and `sync --delete` refuse a
device root (sd:/, usb:/, carda:/, cardb:/) and <device>:/apps itself.
`run` and `log` clear the Wii's log target when they exit. Transfers of
256 KiB or more show progress on stderr when it is a terminal.

While an app built with sdk/hbc_agent runs, the file commands, status and
log registration reach the app instead of HBC. `send` and `run` first ask
it to exit to HBC and wait for HBC, so a rebuilt app replaces the running
one. When an agent app that `run` started crashes, `run` waits for HBC and
prints the crash report (exit status 3).

The Wii address comes from --wii, $HBC_WII, $WII_BENCH_IP, or $WIILOAD
("tcp:ADDR"). Only hosts on the Wii's own /16 network are answered.
"""

import argparse
import errno
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

PORT = 4299
LOG_PORT = 4405
TIMEOUT = 15
DEVICES = ("sd", "usb", "carda", "cardb")
PROGRESS_MIN = 256 * 1024
# Waits poll with short connects: while the Wii reboots, a connection attempt
# can go unanswered, and one full TIMEOUT per attempt made each wait 15 s.
POLL_TIMEOUT = 2

# newlib errno values the client acts on
ENOENT, EISDIR = 2, 21


class HBCError(Exception):
    def __init__(self, message, code=None):
        super().__init__(message)
        self.code = code  # the Wii's errno, when the Wii reported one


# The Wii reports newlib errno values, which differ from the host's.
WII_ERRNO = {1: "EPERM", 2: "ENOENT", 5: "EIO", 6: "ENXIO", 9: "EBADF", 11: "EAGAIN",
             12: "ENOMEM", 13: "EACCES", 16: "EBUSY", 17: "EEXIST", 20: "ENOTDIR",
             21: "EISDIR", 22: "EINVAL", 23: "ENFILE", 24: "EMFILE", 27: "EFBIG",
             28: "ENOSPC", 30: "EROFS", 71: "EPROTO", 77: "EBADMSG", 88: "ENOSYS",
             90: "ENOTEMPTY", 91: "ENAMETOOLONG", 104: "ECONNRESET", 116: "ETIMEDOUT"}


def wii_error(code):
    name = WII_ERRNO.get(code, f"errno {code}")
    host = getattr(errno, name, None)
    return f"{name} ({os.strerror(host)})" if host else name


def wii_address(arg):
    for value in (arg, os.environ.get("HBC_WII"), os.environ.get("WII_BENCH_IP"),
                  os.environ.get("WIILOAD", "").removeprefix("tcp:")):
        if value:
            return value
    raise SystemExit("set the Wii address with --wii or $HBC_WII")


def connect(wii, timeout=TIMEOUT):
    conn = socket.create_connection((wii, PORT), timeout=timeout)
    conn.settimeout(timeout)
    return conn


def recv_exact(conn, n):
    data = bytearray()
    while len(data) < n:
        chunk = conn.recv(min(n - len(data), 65536))
        if not chunk:
            raise HBCError("connection closed early")
        data += chunk
    return bytes(data)


def check_status(status, what="Wii returned"):
    if status < 0:
        raise HBCError(f"{what} {wii_error(-status)}", -status)


def recv_reply(conn):
    status, length = struct.unpack(">iI", recv_exact(conn, 8))
    check_status(status)
    return recv_exact(conn, length)


def request(wii, header, payload=b"", timeout=TIMEOUT):
    """Send one 16-byte request (plus payload) and return the reply body."""
    with connect(wii, timeout) as conn:
        conn.sendall(header.ljust(16, b"\0") + payload)
        return recv_reply(conn)


def version(wii, timeout=TIMEOUT):
    with connect(wii, timeout) as conn:
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


AGENT_SUFFIX = " agent"


def is_agent(text):
    """True when a version reply comes from an app's agent, not HBC."""
    return text.endswith(AGENT_SUFFIX)


def hbc_wait(wii, seconds=90):
    """Wait until HBC itself (not an agent app) answers; return its version."""
    deadline, last = time.monotonic() + seconds, None
    while time.monotonic() < deadline:
        try:
            last = version(wii, POLL_TIMEOUT)
            if not is_agent(last):
                return last
        except (OSError, HBCError) as exc:
            last = exc
        time.sleep(0.5)
    raise HBCError(f"HBC did not answer within {seconds} s: {last}")


def exit_app(wii, seconds=90):
    """Ask a running agent app to exit, then wait for HBC. Returns whether an
    app was running."""
    try:
        running = is_agent(version(wii))
    except (OSError, HBCError):
        return False
    if not running:
        return False
    request(wii, b"HBCX")
    hbc_wait(wii, seconds)
    return True


def devkit_tool(name):
    """A devkitPPC binutils program: on PATH, else in the usual install places."""
    found = shutil.which(name)
    if found:
        return found
    dirs = [os.path.join(os.environ[var], sub) for var, sub in
            (("DEVKITPPC", "bin"), ("DEVKITPRO", "devkitPPC/bin")) if os.environ.get(var)]
    dirs += ["/opt/devkitpro/devkitPPC/bin", "C:/devkitPro/devkitPPC/bin"]
    for d in dirs:
        found = shutil.which(name, path=d)
        if found:
            return found
    return None


def crash_report(crash, elf=None):
    """Format a crash reply's fields, with source lines when elf is given."""
    addrs = [crash["pc"], crash["lr"]] + crash.get("frames", [])
    where = {}
    tool = devkit_tool("powerpc-eabi-addr2line")
    if elf and tool:
        out = subprocess.run([tool, "-f", "-C", "-p", "-e", elf] + [f"0x{a}" for a in addrs],
                             capture_output=True, text=True).stdout.splitlines()
        where = dict(zip(addrs, out))
    lines = [f"{crash['app']} crashed after {crash['uptime_ms'] / 1000:.1f} s: "
             f"{crash['name']} exception ({crash['exception']})"]
    for label, key in (("pc", "pc"), ("lr", "lr")):
        lines.append(f"  {label:5} {crash[key]}  {where.get(crash[key], '')}".rstrip())
    lines.append(f"  dar   {crash['dar']}  dsisr {crash['dsisr']}  sp {crash['sp']}  "
                 f"msr {crash['msr']}  cr {crash['cr']}  ctr {crash['ctr']}")
    for i, frame in enumerate(crash.get("frames", [])):
        lines.append(f"  #{i:<3} {frame}  {where.get(frame, '')}".rstrip())
    if elf and not tool:
        lines.append("  (install devkitPPC or put powerpc-eabi-addr2line on PATH for source lines)")
    return "\n".join(lines)


def yuyv_png(width, height, data):
    """A PNG (8-bit RGB) of a Wii YUYV framebuffer, with only the standard library."""
    rows = bytearray()
    clamp = lambda v: 0 if v < 0 else 255 if v > 255 else v
    for y in range(height):
        rows.append(0)
        line = data[y * width * 2:(y + 1) * width * 2]
        for x in range(0, width * 2, 4):
            y0, cb, y1, cr = line[x], line[x + 1], line[x + 2], line[x + 3]
            d, e = cb - 128, cr - 128
            for yy in (y0, y1):
                c = 298 * (yy - 16)
                rows += bytes((clamp((c + 409 * e + 128) >> 8),
                               clamp((c - 100 * d - 208 * e + 128) >> 8),
                               clamp((c + 516 * d + 128) >> 8)))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def screen(wii):
    """(width, height, YUYV bytes) of what an agent app shows."""
    body = request(wii, b"HBCP", timeout=30)
    width, height = struct.unpack(">II", body[:8])
    if len(body) != 8 + width * height * 2:
        raise HBCError(f"short picture: {len(body)} bytes for {width}x{height}")
    return width, height, body[8:]


def send_keys(wii, keys):
    keys = keys.encode("ascii")
    if len(keys) > 64 or any(k not in b"udlrabh" for k in keys):
        raise HBCError("keys are up to 64 of u d l r a b h")
    with connect(wii) as conn:
        conn.sendall((b"HBCK" + struct.pack(">H", len(keys))).ljust(16, b"\0") + keys)
        recv_reply(conn)


def relaunch_wait(wii, expected, seconds=90):
    """After a send, wait for the old program to stop answering, then for
    HBC `expected` to answer; a same-version rebuild cannot pass early."""
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            version(wii, POLL_TIMEOUT)
        except (OSError, HBCError):
            break
        time.sleep(0.2)
    deadline, last = time.monotonic() + seconds, None
    while time.monotonic() < deadline:
        try:
            last = version(wii, POLL_TIMEOUT)
            if last == expected:
                return
        except (OSError, HBCError) as exc:
            last = exc
        time.sleep(0.5)
    raise HBCError(f"HBC {expected} did not answer within {seconds} s: {last}")


def status(wii):
    return json.loads(request(wii, b"HBCS"))


def wait(wii, seconds):
    deadline, last = time.monotonic() + seconds, None
    while time.monotonic() < deadline:
        try:
            return version(wii, POLL_TIMEOUT)
        except (OSError, HBCError) as exc:
            last = exc
            time.sleep(1)
    raise HBCError(f"HBC did not answer within {seconds} s: {last}")


def file_header(op, remote, size=0, flags=0):
    path = remote.encode("utf-8")
    return (b"HBCF" + op.encode() + bytes((flags,)) + struct.pack(">HI", len(path), size)
            ).ljust(16, b"\0") + path


def file_request(wii, op, remote, size=0, payload=b""):
    """A protocol 1 file request: raw payload in, raw reply body out."""
    with connect(wii) as conn:
        conn.sendall(file_header(op, remote, size) + payload)
        return recv_reply(conn)


FRAME = 64 * 1024
FLAG_COMPRESS = 0x01
_proto = {}


def proto(wii):
    if wii not in _proto:
        try:
            _proto[wii] = status(wii).get("proto", 1)
        except (OSError, HBCError, ValueError):
            _proto[wii] = 1
    return _proto[wii]


def frames(data, level=6):
    """Yield protocol 2 frames: raw length, wire length, CRC-32, wire bytes."""
    for i in range(0, len(data), FRAME):
        raw = data[i:i + FRAME]
        wire = zlib.compress(raw, level) if level else raw
        if len(wire) >= len(raw):
            wire = raw
        yield struct.pack(">III", len(raw), len(wire), zlib.crc32(raw)) + wire


def put_file(wii, remote, data, level=6, progress=None):
    """Upload data; protocol 2 checks every frame's CRC on the Wii.
    `progress(done, total)` is called as frames are sent."""
    if proto(wii) < 2:
        reply = file_request(wii, "P", remote, len(data), data)
        if progress:
            progress(len(data), len(data))
        return reply
    with connect(wii) as conn:
        try:
            conn.sendall(file_header("p", remote, len(data)))
            sent = 0
            for frame in frames(data, level):
                conn.sendall(frame)
                sent += struct.unpack(">I", frame[:4])[0]
                if progress:
                    progress(sent, len(data))
        except OSError:
            # The Wii may refuse early and close; prefer its status to the reset.
            try:
                recv_reply(conn)
            except HBCError as exc:
                if exc.code:
                    raise exc from None
            except OSError:
                pass
            raise
        return recv_reply(conn)


def get_file(wii, remote, compress=True, progress=None):
    """Download a file, verifying every frame's CRC (protocol 2).
    `progress(done, total)` is called as frames arrive."""
    if proto(wii) < 2:
        data = file_request(wii, "G", remote)
        if progress:
            progress(len(data), len(data))
        return data
    with connect(wii) as conn:
        conn.sendall(file_header("g", remote, flags=FLAG_COMPRESS if compress else 0))
        status, size = struct.unpack(">iI", recv_exact(conn, 8))
        check_status(status)
        out = bytearray()
        while True:
            raw_len, wire_len, crc = struct.unpack(">III", recv_exact(conn, 12))
            if not raw_len:
                if crc:
                    check_status(struct.unpack(">i", struct.pack(">I", crc))[0],
                                 "Wii read failed:")
                break
            if raw_len > FRAME or wire_len > raw_len or len(out) + raw_len > size:
                raise HBCError("malformed frame from the Wii")
            wire = recv_exact(conn, wire_len)
            raw = zlib.decompress(wire) if wire_len < raw_len else wire
            if len(raw) != raw_len or zlib.crc32(raw) != crc:
                raise HBCError(f"CRC mismatch at offset {len(out)}")
            out += raw
            if progress:
                progress(len(out), size)
        if len(out) != size:
            raise HBCError(f"short download: {len(out)} of {size} bytes")
        return bytes(out)


def checksum(wii, remote):
    """Return (size, CRC-32) of a remote file (op C)."""
    reply = file_request(wii, "C", remote)
    if len(reply) != 8:
        raise HBCError(f"malformed checksum reply for {remote}")
    return struct.unpack(">II", reply)


class Progress:
    """An in-place progress line on stderr, shown only for large transfers
    and only when stderr is a terminal."""

    def __init__(self, label, stream=None):
        self.stream = stream or sys.stderr
        self.label = label
        isatty = getattr(self.stream, "isatty", None)
        self.tty = bool(isatty and isatty())
        self.start = time.monotonic()
        self.shown = 0

    def __call__(self, done, total):
        if not self.tty or total < PROGRESS_MIN:
            return
        now = time.monotonic()
        if done < total and now - self.shown < 0.1:
            return
        self.shown = now
        rate = done / 1e6 / max(now - self.start, 1e-6)
        self.stream.write(f"\r{self.label}: {100 * done // total:3d}% {rate:6.2f} MB/s")
        if done >= total:
            self.stream.write("\n")
        self.stream.flush()


# Remote paths and trees


def remote_norm(remote):
    """Drop trailing slashes, except a device root's."""
    remote = remote.rstrip("/")
    return remote + "/" if remote.endswith(":") else remote


def remote_join(top, rel):
    return f"{top.rstrip('/')}/{rel}" if rel else top


def remote_base(remote):
    """The last name of a remote path; a device root gives the device name."""
    return remote.rstrip("/").rsplit("/", 1)[-1].replace(":", "")


def protected(remote):
    """True for a device root or <device>:/apps, which must never be emptied."""
    dev, sep, rest = remote.lower().partition(":")
    parts = [p for p in rest.split("/") if p not in ("", ".")]
    return bool(sep) and dev in DEVICES and parts in ([], ["apps"])


def refuse_protected(remote, what):
    if protected(remote):
        raise HBCError(f"refusing to {what} {remote}: a device root or its apps directory")


def list_dir(wii, remote):
    """Return ([(type, size, name)], truncated) for one remote directory."""
    text = file_request(wii, "L", remote).decode("utf-8", "replace")
    entries, truncated = [], False
    for line in text.splitlines():
        if line == "! truncated":
            truncated = True
        elif line.startswith("d "):
            entries.append(("d", 0, line[2:]))
        elif line.startswith("f "):
            size, _, name = line[2:].partition(" ")
            entries.append(("f", int(size), name))
    return entries, truncated


def remote_tree(wii, top):
    """List a whole remote tree before anything acts on it. Returns
    (dirs, parents first; {file: size}), paths relative to top joined by "/"."""
    dirs, files, pending = [], {}, [""]
    while pending:
        rel = pending.pop()
        path = remote_join(top, rel)
        entries, truncated = list_dir(wii, path)
        if truncated:
            raise HBCError(f"the Wii truncated the listing of {path} (256 KiB limit); "
                           "refusing to act on a partial listing")
        for kind, size, name in entries:
            sub = f"{rel}/{name}" if rel else name
            if kind == "d":
                dirs.append(sub)
                pending.append(sub)
            else:
                files[sub] = size
    return dirs, files


def bottom_up(dirs):
    return sorted(dirs, key=lambda d: d.count("/"), reverse=True)


def remote_is_dir(wii, remote):
    """True for a directory, False for a file; raises if it is missing."""
    try:
        checksum(wii, remote)
        return False
    except HBCError as exc:
        if exc.code == EISDIR:
            return True
        raise HBCError(f"{remote}: {exc}", exc.code) from None


def local_tree(local):
    """Return (dirs, parents first; {file: local path}) relative to local."""
    dirs, files = [], {}
    for dirpath, dirnames, filenames in os.walk(local):
        dirnames.sort()
        rel = os.path.relpath(dirpath, local).replace(os.sep, "/")
        rel = "" if rel == "." else rel
        if rel:
            dirs.append(rel)
        for name in sorted(filenames):
            files[f"{rel}/{name}" if rel else name] = os.path.join(dirpath, name)
    return dirs, files


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def upload(wii, local, remote, out=print):
    data = read_file(local)
    put_file(wii, remote, data, progress=Progress(remote))
    out(f"{local} -> {remote} ({len(data)} bytes)")
    return len(data)


def download(wii, remote, local, out=print):
    data = get_file(wii, remote, progress=Progress(remote))
    os.makedirs(os.path.dirname(local) or ".", exist_ok=True)
    with open(local, "wb") as f:
        f.write(data)
    out(f"{remote} -> {local} ({len(data)} bytes)")
    return len(data)


def put_tree(wii, local, remote, out=print):
    """Upload a directory tree. Files bring their parents, so only empty
    directories need a mkdir."""
    remote = remote_norm(remote)
    dirs, files = local_tree(local)
    inner = list(files) + dirs
    for rel in dirs if inner else [""]:
        if not any(p.startswith(rel + "/") for p in inner):
            file_request(wii, "M", remote_join(remote, rel))
    total = sum(upload(wii, path, remote_join(remote, rel), out) for rel, path in files.items())
    out(f"put: {len(files)} files ({total} bytes)")


def get_tree(wii, remote, local, out=print):
    remote = remote_norm(remote)
    dirs, files = remote_tree(wii, remote)
    os.makedirs(local, exist_ok=True)
    for rel in dirs:
        os.makedirs(os.path.join(local, *rel.split("/")), exist_ok=True)
    total = 0
    for rel in sorted(files):
        total += download(wii, remote_join(remote, rel), os.path.join(local, *rel.split("/")),
                          out)
    out(f"get: {len(files)} files ({total} bytes)")


def remove_tree(wii, remote, out=print):
    """Delete a remote tree: files first, then directories bottom-up."""
    refuse_protected(remote, "recursively delete")
    remote = remote_norm(remote)
    if not remote_is_dir(wii, remote):
        file_request(wii, "D", remote)
        return 1
    dirs, files = remote_tree(wii, remote)
    for rel in sorted(files):
        file_request(wii, "D", remote_join(remote, rel))
    for rel in bottom_up(dirs) + [""]:
        file_request(wii, "D", remote_join(remote, rel))
    count = len(files) + len(dirs) + 1
    out(f"rm: {count} entries deleted")
    return count


def sync(wii, local, remote, delete=False, out=print):
    """Make remote mirror local: upload new or changed files (size, then
    CRC-32), create missing directories and, with delete, remove the rest."""
    if not os.path.isdir(local):
        raise HBCError(f"{local} is not a directory")
    if delete:
        refuse_protected(remote, "sync --delete into")
    remote = remote_norm(remote)
    try:
        rdirs, rfiles = remote_tree(wii, remote)
        exists = True
    except HBCError as exc:
        if exc.code != ENOENT:
            raise
        rdirs, rfiles, exists = [], {}, False
    ldirs, lfiles = local_tree(local)
    rdirset, ldirset = set(rdirs), set(ldirs)
    clash = [f for f in lfiles if f in rdirset] + [d for d in ldirs if d in rfiles]
    if clash and not delete:
        raise HBCError(f"{remote_join(remote, clash[0])} is a file on one side and a "
                       "directory on the other; use --delete to replace it")
    uploaded = skipped = deleted = nbytes = 0
    if delete:
        for rel in sorted(f for f in rfiles if f not in lfiles):
            file_request(wii, "D", remote_join(remote, rel))
            del rfiles[rel]
            deleted += 1
        for rel in bottom_up(d for d in rdirs if d not in ldirset):
            file_request(wii, "D", remote_join(remote, rel))
            rdirset.discard(rel)
            deleted += 1
    if not exists:
        file_request(wii, "M", remote)
    for rel in ldirs:
        if rel not in rdirset:
            file_request(wii, "M", remote_join(remote, rel))
    for rel, path in lfiles.items():
        target = remote_join(remote, rel)
        data = read_file(path)
        if rfiles.get(rel) == len(data) and \
                checksum(wii, target) == (len(data), zlib.crc32(data)):
            skipped += 1
            continue
        put_file(wii, target, data, progress=Progress(target))
        out(f"{path} -> {target} ({len(data)} bytes)")
        uploaded += 1
        nbytes += len(data)
    out(f"sync: {uploaded} uploaded ({nbytes} bytes), {skipped} unchanged, {deleted} deleted")
    return uploaded, skipped, deleted, nbytes


def send(wii, path, args):
    data = read_file(path)
    # The PC has CPU to spare, and the Wii's inflate cost does not depend on the level.
    packed = zlib.compress(data, 9)
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

    def __init__(self, port, wii=None):
        self.wii = wii
        self.agent = False  # the app that connected answers as an agent
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # On Windows SO_REUSEADDR would let a second `log` share the port.
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.sock.listen(4)
        self.sock.settimeout(0.5)  # short waits keep ^C working on Windows
        self.port = self.sock.getsockname()[1]
        self.done = threading.Event()

    @staticmethod
    def write(chunk):
        out = getattr(sys.stdout, "buffer", None)
        if out:
            out.write(chunk)
        else:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()

    def serve(self, once=False):
        while True:
            try:
                conn, (addr, _) = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return  # the socket was closed
            print(f"[hbc log] {addr} connected", file=sys.stderr, flush=True)
            if once and self.wii:
                threading.Thread(target=self.probe_agent, daemon=True).start()
            with conn:
                conn.settimeout(0.5)
                while True:
                    try:
                        chunk = conn.recv(4096)
                    except socket.timeout:
                        continue
                    except ConnectionResetError:
                        break  # IOS closes sockets with a reset
                    if not chunk:
                        break
                    self.write(chunk)
            print(f"[hbc log] {addr} closed", file=sys.stderr, flush=True)
            if once:
                self.done.set()
                return

    def probe_agent(self, seconds=5):
        """Note whether the app that connected runs an agent, so `run` can
        wait for its crash report."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and not self.done.is_set():
            try:
                if is_agent(version(self.wii, POLL_TIMEOUT)):
                    self.agent = True
                    return
            except (OSError, HBCError):
                pass
            time.sleep(0.5)

    def register(self, wii):
        request(wii, b"HBCN" + struct.pack(">H", self.port))

    @staticmethod
    def unregister(wii):
        """Clear the Wii's log target (port 0), ignoring errors."""
        try:
            request(wii, b"HBCN" + struct.pack(">H", 0), timeout=3)
        except (OSError, HBCError):
            pass

    def close(self):
        self.sock.close()


def run_outcome(wii, seconds=30):
    """After an agent app closed its log: None if it still runs or exited
    cleanly, else the crash HBC reported."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if is_agent(version(wii, POLL_TIMEOUT)):
                return None  # still running; it closed its log itself
            return status(wii).get("crash")
        except (OSError, HBCError):
            time.sleep(0.5)  # on its way back to HBC
    return None


# Options accepted after the command: flag -> (destination, takes a value)
GLOBAL_FLAGS = {"--wii": ("wii", True), "--log-port": ("log_port", True),
                "--timeout": ("timeout", True), "--json": ("json", False)}
COMMAND_FLAGS = {"-r": ("recursive", ("get", "put", "rm")),
                 "--recursive": ("recursive", ("get", "put", "rm")),
                 "--delete": ("delete", ("sync",)),
                 "--clear": ("clear", ("crash",))}
VALUE_FLAGS = {"--elf": ("elf", ("crash",))}


def split_flags(cmd, args):
    """Pull options out of a command's arguments. For send and run, options
    stop at FILE so the app gets the rest untouched."""
    known = dict(GLOBAL_FLAGS)
    known.update({flag: (dest, False) for flag, (dest, cmds) in COMMAND_FLAGS.items()
                  if cmd in cmds})
    known.update({flag: (dest, True) for flag, (dest, cmds) in VALUE_FLAGS.items()
                  if cmd in cmds})
    flags, rest, i = {}, [], 0
    while i < len(args):
        arg = args[i]
        if arg == "--":
            rest += args[i + 1:]
            break
        if arg in known:
            dest, takes_value = known[arg]
            if takes_value:
                if i + 1 >= len(args):
                    raise SystemExit(f"hbc.py: {arg} needs a value")
                flags[dest] = args[i + 1]
            else:
                flags[dest] = True
            i += 2 if takes_value else 1
            continue
        if cmd in ("send", "run"):
            rest += args[i:]
            break
        if arg.startswith("-") and len(arg) > 1:
            raise SystemExit(f"hbc.py: unknown option {arg!r} for {cmd}")
        rest.append(arg)
        i += 1
    return flags, rest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--wii", help="Wii IPv4 address")
    parser.add_argument("--json", action="store_true",
                        help="JSON output for version, status and ls")
    parser.add_argument("--log-port", type=int, default=LOG_PORT)
    parser.add_argument("--timeout", type=float, default=300,
                        help="seconds `run` waits for the app to finish")
    parser.add_argument("command")
    parser.add_argument("args", nargs=argparse.REMAINDER)
    opts = parser.parse_args(argv)
    cmd = opts.command
    flags, args = split_flags(cmd, opts.args)
    try:
        for dest, conv in (("log_port", int), ("timeout", float)):
            if dest in flags:
                flags[dest] = conv(flags[dest])
    except ValueError as exc:
        raise SystemExit(f"hbc.py: {exc}")
    vars(opts).update(flags)
    recursive = flags.get("recursive", False)
    wii = wii_address(opts.wii)

    def need(n, usage):
        if len(args) < n:
            raise SystemExit(f"usage: hbc.py {cmd} {usage}")

    try:
        if cmd == "version":
            text = version(wii)
            print(json.dumps({"version": text}) if opts.json else text)
        elif cmd == "status":
            st = status(wii)
            print(json.dumps(st, separators=(",", ":")) if opts.json else json.dumps(st, indent=2))
        elif cmd == "wait":
            print(wait(wii, float(args[0]) if args else 90))
        elif cmd == "send":
            need(1, "FILE [ARG ...]")
            if exit_app(wii):
                print("hbc.py: the running app exited to HBC", file=sys.stderr)
            send(wii, args[0], args[1:])
        elif cmd == "exit":
            if not exit_app(wii):
                raise HBCError("no agent app is running (HBC answers itself)")
            print(hbc_wait(wii, 5))
        elif cmd == "key":
            need(1, "KEYS")
            send_keys(wii, "".join(args))
        elif cmd == "screen":
            need(1, "FILE.png")
            width, height, data = screen(wii)
            with open(args[0], "wb") as f:
                f.write(yuyv_png(width, height, data))
            print(f"{args[0]}: {width}x{height}")
        elif cmd == "crash":
            if flags.get("clear"):
                request(wii, b"HBCC")
                return
            st = status(wii)
            if st.get("agent"):
                raise HBCError("an agent app is running; crash reports come from HBC")
            crash = st.get("crash")
            if opts.json:
                print(json.dumps(crash))
            elif crash:
                print(crash_report(crash, flags.get("elf")))
            else:
                print("no crash reported")
        elif cmd in ("run", "log"):
            if cmd == "run":
                need(1, "FILE [ARG ...]")
                if exit_app(wii):
                    print("[hbc log] the running app exited to HBC", file=sys.stderr, flush=True)
                if proto(wii) >= 3:
                    request(wii, b"HBCC")  # so a crash after this is this app's
            server = LogServer(opts.log_port, wii if cmd == "run" else None)
            crashed = None
            try:
                server.register(wii)
                print(f"[hbc log] listening on port {server.port}", file=sys.stderr, flush=True)
                if cmd == "log":
                    server.serve()
                    return
                threading.Thread(target=server.serve, args=(True,), daemon=True).start()
                send(wii, args[0], args[1:])
                deadline = time.monotonic() + opts.timeout
                while not server.done.wait(0.5):  # short waits keep ^C working
                    if time.monotonic() >= deadline:
                        raise HBCError(f"the app did not close its log within {opts.timeout} s")
                if server.agent:
                    crashed = run_outcome(wii)
            finally:
                server.unregister(wii)
                server.close()
            if crashed:
                print(crash_report(crashed), file=sys.stderr)
                raise SystemExit(3)
        elif cmd == "ls":
            need(1, "REMOTE")
            entries, truncated = list_dir(wii, args[0])
            if opts.json:
                print(json.dumps([{"name": name, "type": kind, "size": size}
                                  for kind, size, name in entries]))
            else:
                for kind, size, name in entries:
                    print(f"d {name}" if kind == "d" else f"f {size} {name}")
            if truncated:
                print("hbc.py: warning: the Wii truncated this listing (256 KiB limit)",
                      file=sys.stderr)
        elif cmd == "get":
            need(1, "[-r] REMOTE [LOCAL]")
            base = remote_base(args[0]) or "root"
            local = args[1] if len(args) > 1 else base
            if local.endswith(("/", "\\")) or (not recursive and os.path.isdir(local)):
                local = os.path.join(local, base)
            if recursive:
                get_tree(wii, args[0], local)
                return
            try:
                download(wii, args[0], local)
            except HBCError:
                try:
                    is_dir = remote_is_dir(wii, args[0])
                except (OSError, HBCError):
                    is_dir = False
                if is_dir:
                    raise HBCError(f"{args[0]} is a directory; use get -r") from None
                raise
        elif cmd == "put":
            need(2, "[-r] LOCAL REMOTE")
            local, remote = args[0], args[1]
            if remote.endswith("/"):
                remote += os.path.basename(os.path.normpath(local))
            if os.path.isdir(local):
                if not recursive:
                    raise HBCError(f"{local} is a directory; use put -r")
                put_tree(wii, local, remote)
            else:
                upload(wii, local, remote)
        elif cmd == "rm":
            need(1, "[-r] REMOTE")
            if recursive:
                remove_tree(wii, args[0])
            else:
                file_request(wii, "D", args[0])
        elif cmd == "mkdir":
            need(1, "REMOTE")
            file_request(wii, "M", args[0])
        elif cmd == "sync":
            need(2, "[--delete] LOCALDIR REMOTEDIR")
            sync(wii, args[0], args[1], delete=flags.get("delete", False))
        else:
            parser.error(f"unknown command {cmd!r}")
    except (OSError, HBCError) as exc:
        raise SystemExit(f"hbc.py: {exc}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
