#!/usr/bin/env python3
"""Develop for the Wii from a PC, through the Homebrew Channel's network tools.

Run `python3 tools/hbc.py` for an overview and `hbc.py help COMMAND` for the
details of one command. docs/devnet.md describes the protocol underneath.
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
    raise SystemExit("hbc.py: which Wii? Set HBC_WII to its address, or pass --wii ADDRESS.\n"
                     "HBC shows the address on its HOME menu, under HBC > Network.")


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


def foreign_app_check(wii):
    """In a bench-queue job (tools/wii-bench sets WII_BENCH_JOB_START), refuse to exit an
    agent app that was already running when the job started: it is someone else's test,
    maybe from another workstation. An app this job started is younger than the job."""
    start = os.environ.get("WII_BENCH_JOB_START")
    if not start:
        return
    st = status(wii)
    job_s = time.time() - float(start)
    if st.get("uptime_ms", 0) / 1000 > job_s + 2:
        raise HBCError(f"Wii busy: {st.get('app') or 'an app'} is running, started before this "
                       f"bench job; not exiting it")


def exit_app(wii, seconds=90):
    """Ask a running agent app to exit, then wait for HBC. Returns whether an
    app was running."""
    try:
        running = is_agent(version(wii))
    except (OSError, HBCError):
        return False
    if not running:
        return False
    foreign_app_check(wii)
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


def lastlog(wii):
    """The last app's kept output from HBC, or a running agent app's output so
    far ("live"): {"app", "why", "uptime_ms", "text"}; None when there is none."""
    try:
        body = request(wii, b"HBCL")
    except HBCError as exc:
        code = exc.code
        if code == 2:   # newlib's ENOENT: nothing kept
            return None
        if code == 88:  # newlib's ENOSYS: an HBC or agent older than protocol 4
            raise HBCError("this HBC or app agent has no kept log (protocol 4, HBC 1.9.0)") from exc
        raise
    head, _, text = body.partition(b"\n")
    word = head.decode("ascii", "replace").split(" ", 4)
    if len(word) < 4 or word[0] != "HBCL":
        raise HBCError(f"unexpected lastlog reply {head[:40]!r}")
    return {"why": word[2], "uptime_ms": int(word[3]), "app": word[4] if len(word) > 4 else "",
            "text": text.decode("utf-8", "replace")}


def crash_report(crash, elf=None):
    """Format a crash reply's fields, with source lines when elf is given."""
    addrs = [crash["pc"], crash["lr"]] + crash.get("frames", [])
    where = {}
    tool = devkit_tool("powerpc-eabi-addr2line")
    if elf and tool:
        out = subprocess.run([tool, "-f", "-C", "-p", "-e", elf] + [f"0x{a}" for a in addrs],
                             capture_output=True, text=True).stdout.splitlines()
        where = dict(zip(addrs, out))
    after = f"after {crash['uptime_ms'] / 1000:.1f} s"
    kind = crash.get("kind", "exception")
    if kind == "fatal":
        # The code is the app's own: shown, never interpreted.
        lines = [f"{crash['app']} stopped {after}: {crash.get('reason') or 'fatal'} "
                 f"(fatal, app code {crash.get('code', 0)} = {crash.get('code', 0):#x})"]
    elif kind == "hang":
        lines = [f"{crash['app']} hung {after}: {crash.get('reason') or 'hang'}"]
    else:
        lines = [f"{crash['app']} crashed {after}: {crash['name']} exception ({crash['exception']})"]
    for label, key in (("pc", "pc"), ("lr", "lr")):
        lines.append(f"  {label:5} {crash[key]}  {where.get(crash[key], '')}".rstrip())
    if kind == "exception":
        lines.append(f"  dar   {crash['dar']}  dsisr {crash['dsisr']}  sp {crash['sp']}  "
                     f"msr {crash['msr']}  cr {crash['cr']}  ctr {crash['ctr']}")
    else:
        lines.append(f"  sp    {crash['sp']}")
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
    if len(keys) > 64 or any(k not in b"udlrabh12w" for k in keys):
        raise HBCError("keys are up to 64 of u d l r a b h 1 2 w")
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


def upload_name(path):
    """What the Message Board's play log calls an upload: the app's folder for
    an apps/<name>/boot.dol (or .elf), else the file's name without .dol/.elf."""
    base = os.path.basename(path)
    stem, ext = os.path.splitext(base)
    if ext.lower() in (".dol", ".elf"):
        if stem.lower() == "boot":
            parent = os.path.basename(os.path.dirname(os.path.abspath(path)))
            if parent:
                return parent
        return stem
    return base


UPLOAD_YES = 1  # HBCA flag: install a ZIP without asking


def send(wii, path, args, name=None, yes=False):
    """Wiiload path to HBC. `name` (default upload_name(path)) is what HBC's
    play log calls it, unless the app's own agent names it. `yes` installs a
    ZIP without the question on the Wii (HBC 1.9.5 on)."""
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
    label = (name or upload_name(path)).encode("utf-8")[:63]
    flags = UPLOAD_YES if yes else 0
    if label or flags:
        try:
            request(wii, struct.pack(">4sHH", b"HBCA", len(label), flags), label)
        except HBCError:
            pass  # an HBC before 1.9.1 (or an agent app): no name, nothing else changes
    with connect(wii) as conn:
        conn.sendall(header + packed + argv)


def upload_seq(wii):
    """The count of uploads HBC has a result for (HBCS "upload"), or None
    when HBC is too old to say (protocol 5 and before)."""
    try:
        st = status(wii)
    except (OSError, HBCError):
        return None
    if st.get("agent") or st.get("proto", 0) < 6:
        return None
    return (st.get("upload") or {}).get("seq", 0)


def upload_outcome(wii, before, seconds=60):
    """What became of the upload after `before` (upload_seq): HBC's result,
    {"result": "launched"} once an app answers or HBC stops answering.
    HBC's own popups close after 10 s, so an error comes back in time."""
    deadline, silent = time.monotonic() + seconds, None
    while time.monotonic() < deadline:
        try:
            st = json.loads(request(wii, b"HBCS", timeout=POLL_TIMEOUT))
            silent = None
        except (OSError, HBCError):
            # HBC answers throughout, popups included; a launched app
            # without an agent does not answer at all.
            silent = silent or time.monotonic()
            if time.monotonic() - silent >= 10:
                return {"result": "launched"}
            time.sleep(0.5)
            continue
        if st.get("agent"):
            return {"result": "launched"}
        up = st.get("upload")
        if up and up.get("seq", 0) > before:
            return up
        time.sleep(0.2)
    raise HBCError(f"HBC gave no result for the upload within {seconds} s")


def check_outcome(up):
    """Raise for an upload HBC could not use; return its result."""
    if up["result"] == "error":
        raise HBCError(f"the Wii could not load it ({up.get('error')}): {up.get('text')}")
    if up["result"] == "declined":
        raise HBCError("the ZIP was not installed: it was declined on the Wii, "
                       "or nobody answered (send --yes installs without asking)")
    return up["result"]


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


# ---- Help ---------------------------------------------------------------

OVERVIEW = """\
hbc.py - work on a Wii from your PC through the Homebrew Channel (HBC).

Tell it where your Wii is, once per terminal (the address is on HBC's
HOME menu, under HBC > Network):

    export HBC_WII=192.168.1.50          (Windows: set HBC_WII=192.168.1.50)

or add --wii 192.168.1.50 to each command.

Common tasks
    Run an app and watch its output     hbc.py run myapp.dol [ARGS...]
    Copy an app to the SD card          hbc.py put -r dist/myapp sd:/apps/myapp
    Update it, changed files only       hbc.py sync dist/myapp sd:/apps/myapp
    Fetch a file the app wrote          hbc.py get sd:/apps/myapp/save.dat
    See what the Wii is running         hbc.py status

Commands
  The Wii
    status              what is running: version, IOS, memory, SD card, network
    version             just the version (ends in " agent" for an app, below)
    wait [SECONDS]      wait until HBC answers, after a reboot (default 90)
  Running apps
    run FILE [ARGS...]  send a .dol, .elf or .zip and print what it prints
    send FILE [ARGS...] send it without waiting for output
    log                 print what apps started on the Wii print, until Ctrl+C
  Files (sd:/..., usb:/...)
    ls PATH             list a folder
    get PATH [LOCAL]    download (-r for a folder)
    put LOCAL PATH      upload (-r for a folder; missing folders are made)
    sync LOCAL PATH     upload only what changed (--delete removes extras)
    rm PATH             delete a file or empty folder (-r for a whole folder)
    mkdir PATH          make a folder
  Apps built with the in-app agent (sdk/hbc_agent.h)
    exit                ask the running app to go back to HBC
    key KEYS            press buttons on it: h (HOME), u d l r, a, b, 1, 2, w
    screen FILE.png     save a picture of what the TV shows
    crash [--elf ELF]   show the last crash: where, and why
    lastlog             the last app's final output (or a running app's so far)

Options (before or after the command)
    --wii ADDR          the Wii's address, if $HBC_WII is not set
    --json              machine-readable output (status, version, ls, crash)
    --log-port PORT     the PC port apps send output to (default 4405)
    --timeout SECONDS   how long run waits for the app to finish (default 300)

More: hbc.py help COMMAND    (for example: hbc.py help sync)
"""

COMMAND_HELP = {
    "status": """\
hbc.py status

Prints what the Wii is running as JSON: the HBC (or agent app) version, the
IOS and its revision, whether AHBPROT is open, free memory, the mounted
device and the devices seen, the Wii's IP, the log target, the last file
transfer's timing, and any crash an agent app reported. With --json the
output is one compact line, for scripts.""",
    "version": """\
hbc.py version

Prints the running version, such as "1.7.0". An app built with the in-app
agent answers "1.7.0 agent" instead, so scripts can tell the two apart.""",
    "wait": """\
hbc.py wait [SECONDS]

Waits until HBC answers (default 90 s) and prints its version. Use it after
rebooting the Wii or after an app returns to HBC.""",
    "run": """\
hbc.py run [--name NAME] [--yes] FILE [ARGS...]

Sends FILE (a .dol, .elf or .zip) to the Wii, starts it, and prints what it
prints until it exits (or --timeout seconds pass). Arguments after FILE go
to the app as argv; put -- before any that start with "-". If the Wii
cannot load it, run says why and exits with status 1; a .zip is installed
and run returns (--yes, as for send).

The Wii Message Board's play log lists the app by the name its agent gives
itself (sdk/hbc_agent.h), else by --name, else by its folder for an
apps/NAME/boot.dol, else by the file's name.

The app's output reaches the PC if it uses sdk/hbc_netlog.h. The PC must
accept incoming connections on --log-port (4405 by default): allow it once
in your firewall.

If an agent app is already running, run asks it to exit to HBC first, so
each run replaces the last. If an agent app crashes, run prints the crash
report and exits with status 3.

    hbc.py run build/myapp.dol level2
    hbc.py run build/myapp.dol -- --verbose""",
    "send": """\
hbc.py send [--name NAME] [--yes] FILE [ARGS...]

Sends FILE to the Wii and starts it, without waiting for output: Wiiload,
like the devkitPro wiiload tool. A .zip is installed to the SD card after
you confirm on the Wii, or at once with --yes. --name is the play log's
name for it (see run).

send waits for HBC's verdict: if the Wii cannot load it (not a Wii app, a
broken transfer, a ZIP declined or not answered within HBC's 10 s), send
says why and exits with status 1. HBC 1.9.5 on; older ones are not asked.""",
    "log": """\
hbc.py log

Tells HBC to send the output of the apps it starts to this PC, then prints
it until you press Ctrl+C. Use it when you start apps from the Wii itself.
HBC remembers the PC for later apps too, until log exits.""",
    "ls": """\
hbc.py ls PATH

Lists a folder: "d NAME" for folders, "f SIZE NAME" for files. Paths look
like sd:/apps or usb:/data (devices: sd, usb, carda, cardb).""",
    "get": """\
hbc.py get [-r] PATH [LOCAL]

Downloads a file, or with -r a whole folder. LOCAL defaults to the file's
name in the current folder; an existing local folder gets the file inside
it. Every 64 KiB is checked with a CRC-32 and compressed when that helps.

    hbc.py get sd:/apps/myapp/save.dat
    hbc.py get -r sd:/apps/myapp backup/myapp""",
    "put": """\
hbc.py put [-r] LOCAL PATH

Uploads a file, or with -r a whole folder. Missing folders on the Wii are
made. A PATH ending in "/" means "into this folder": put app.dol
sd:/apps/myapp/ writes sd:/apps/myapp/app.dol. A broken transfer leaves no
half-written file. An app put under sd:/apps shows in HBC's list at once.""",
    "sync": """\
hbc.py sync [--delete] LOCAL PATH

Makes the folder PATH on the Wii match LOCAL: uploads files whose size or
CRC-32 differ and makes missing folders. With --delete it also removes what
LOCAL does not have. It refuses to delete from a device root or from
<device>:/apps itself.

    hbc.py sync dist/myapp sd:/apps/myapp""",
    "rm": """\
hbc.py rm [-r] PATH

Deletes a file or an empty folder, or with -r a whole folder. Device roots
(sd:/) and <device>:/apps itself are refused.""",
    "mkdir": """\
hbc.py mkdir PATH

Makes a folder, and any missing folders above it.""",
    "exit": """\
hbc.py exit

Asks the running agent app to exit to HBC, and waits until HBC answers.""",
    "key": """\
hbc.py key KEYS

Presses buttons on the running agent app, one after another: h (HOME, which
opens and closes the agent's overlay), u d l r (the D-pad), a, b, 1 and 2, and w
(plays speaker.wav on the Test page). Use it
to drive the overlay from a script, or from the PC. HBC's own HOME menu is
the same overlay, so this works on HBC too.

    hbc.py key h          open the HOME overlay
    hbc.py key rra        move right twice and press A""",
    "screen": """\
hbc.py screen FILE.png

Saves a picture of what the TV shows right now, from an agent app or from
HBC, as a PNG.""",
    "crash": """\
hbc.py crash [--elf ELF] [--clear]

After an agent app crashes, the Wii returns to HBC with a report of what
happened: the exception, the registers, and the chain of calls. This prints
it; with --elf the app's .elf adds function names and source lines (it
needs devkitPPC's powerpc-eabi-addr2line). --clear forgets the report.
Besides exceptions, an app can stop itself with hbc_agent_fatal() (its own
code and a reason), and the agent's watchdog reports a hang. The last lines
of the app's output follow the report.""",
    "lastlog": """\
hbc.py lastlog

An agent app's output (stdout and stderr) is kept as it stops, whether by
exit(), a crash, hbc_agent_fatal() or a hang, and HBC shows the last 4 KiB
of it here. While an agent app runs, this prints its output so far.""",
}


def print_help(topic=None):
    if topic in COMMAND_HELP:
        print(COMMAND_HELP[topic])
    else:
        if topic:
            print(f"hbc.py: no command {topic!r}\n")
        print(OVERVIEW, end="")


# Options accepted after the command: flag -> (destination, takes a value)
GLOBAL_FLAGS = {"--wii": ("wii", True), "--log-port": ("log_port", True),
                "--timeout": ("timeout", True), "--json": ("json", False)}
COMMAND_FLAGS = {"-r": ("recursive", ("get", "put", "rm")),
                 "--recursive": ("recursive", ("get", "put", "rm")),
                 "--delete": ("delete", ("sync",)),
                 "--clear": ("clear", ("crash",)),
                 "--yes": ("yes", ("send", "run"))}
VALUE_FLAGS = {"--elf": ("elf", ("crash",)),
               "--name": ("name", ("send", "run"))}


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
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] in ("-h", "--help", "help"):
        print_help(argv[1] if len(argv) > 1 and argv[0] == "help" else None)
        return
    if len(argv) > 1 and argv[1] in ("-h", "--help"):
        print_help(argv[0])
        return
    parser = argparse.ArgumentParser(prog="hbc.py", add_help=False,
                                     usage="hbc.py [OPTIONS] COMMAND ... (hbc.py help)")
    parser.error = lambda msg: (_ for _ in ()).throw(SystemExit(f"hbc.py: {msg} (see: hbc.py help)"))
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
    if cmd not in COMMAND_HELP:
        print_help(cmd)
        raise SystemExit(2)
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
            before = upload_seq(wii)
            send(wii, args[0], args[1:], flags.get("name"), flags.get("yes", False))
            if before is not None:
                result = check_outcome(upload_outcome(wii, before))
                if result != "launched":
                    print(f"hbc.py: {result}", file=sys.stderr)
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
                kept = lastlog(wii) if st.get("lastlog") else None
                if kept and kept["text"].strip():
                    tail = kept["text"].rstrip("\n").splitlines()[-12:]
                    print("  last output:")
                    for line in tail:
                        print(f"    {line}")
            else:
                print("no crash reported")
        elif cmd == "lastlog":
            kept = lastlog(wii)
            if opts.json:
                print(json.dumps(kept))
            elif not kept:
                print("no kept log (the last app had no agent, or HBC restarted since)")
            else:
                state = "running, so far" if kept["why"] == "live" else f"ended by {kept['why']}"
                print(f"-- {kept['app']} ({state}, {kept['uptime_ms'] / 1000:.1f} s) --")
                print(kept["text"], end="" if kept["text"].endswith("\n") else "\n")
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
                before = upload_seq(wii)
                send(wii, args[0], args[1:], flags.get("name"), flags.get("yes", False))
                if before is not None:
                    result = check_outcome(upload_outcome(wii, before))
                    if result != "launched":
                        print(f"[hbc log] {result}", file=sys.stderr, flush=True)
                        return
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
            print_help(cmd)
            raise SystemExit(2)
    except (OSError, HBCError) as exc:
        raise SystemExit(f"hbc.py: {exc}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
