#!/usr/bin/env python3
"""Develop for the Wii from a PC, through the Homebrew Channel's network tools.

Run `python3 tools/hbc.py` for an overview and `hbc.py help COMMAND` for the
details of one command. docs/devnet.md describes the protocol underneath.
"""

import argparse
import datetime
import errno
import json
import os
import select
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

PORT = int(os.environ.get("HBC_PORT", "4299"))   # another port only for tests (wiibench's fake Wii)
# In a wii-bench job, the port the bench gave its Wii (4300, 4301, ...): two jobs on one PC,
# each on its own Wii, never listen on the same port.
LOG_PORT = int(os.environ.get("WII_BENCH_LOG_PORT") or 4405)
TIMEOUT = 15
DEVICES = ("sd", "usb", "carda", "cardb", "usb2")
PROGRESS_MIN = 256 * 1024
# Waits poll with short connects: while the Wii reboots, a connection attempt
# can go unanswered, and one full TIMEOUT per attempt made each wait 15 s.
POLL_TIMEOUT = 2
# The Wii reads and CRCs a whole file before it answers a checksum request
# (op C): allow for this rate on top of TIMEOUT, below a slow SD card's.
CHECKSUM_BYTES_PER_S = 1 << 20

# newlib errno values the client acts on
ENOENT, EACCES, EEXIST, EXDEV, EISDIR, ENOSYS = 2, 13, 17, 18, 21, 88


class HBCError(Exception):
    def __init__(self, message, code=None):
        super().__init__(message)
        self.code = code  # the Wii's errno, when the Wii reported one


# The Wii reports newlib errno values, which differ from the host's.
WII_ERRNO = {1: "EPERM", 2: "ENOENT", 5: "EIO", 6: "ENXIO", 9: "EBADF", 11: "EAGAIN",
             12: "ENOMEM", 13: "EACCES", 16: "EBUSY", 17: "EEXIST", 18: "EXDEV", 20: "ENOTDIR",
             21: "EISDIR", 22: "EINVAL", 23: "ENFILE", 24: "EMFILE", 27: "EFBIG",
             28: "ENOSPC", 30: "EROFS", 71: "EPROTO", 77: "EBADMSG", 88: "ENOSYS",
             90: "ENOTEMPTY", 91: "ENAMETOOLONG", 104: "ECONNRESET", 116: "ETIMEDOUT",
             134: "ENOTSUP"}


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


def recv_reply(conn, what="Wii returned"):
    status, length = struct.unpack(">iI", recv_exact(conn, 8))
    check_status(status, what)
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


WAYS_OUT = {
    "not_checked": None,
    "stub": "through the reload stub (IOS answered)",
    "es_launch": "HBC launched through ES (an IOS module did not answer)",
    "reset": "system reset through HW_RESETS (IOS did not answer)",
    "stm_reset": "system reset through STM (IOS did not answer)",
    "stub_anyway": "through the reload stub, after the other ways failed",
}


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
    elif kind == "assert":
        lines = [f"{crash['app']} stopped {after}: assertion failed: {crash.get('reason')}"]
    elif kind == "abort":
        lines = [f"{crash['app']} stopped {after}: {crash.get('reason') or 'abort() called'}"]
    elif kind == "stack":
        lines = [f"{crash['app']} stopped {after}: the main thread overran its stack "
                 f"({crash.get('reason')})"]
    else:
        lines = [f"{crash['app']} crashed {after}: {crash['name']} exception ({crash['exception']})"]
        # An agent whose crash hook faulted too says where (HBC 1.10.2 on).
        if crash.get("reason"):
            lines[0] += f"; {crash['reason']}"
    # How the agent got the Wii back (HBC 1.10.14 on; docs/devnet.md).
    way = WAYS_OUT.get(crash.get("way_out", "not_checked"))
    if way:
        lines.append(f"  left  {way}")
    for label, key in (("pc", "pc"), ("lr", "lr")):
        lines.append(f"  {label:5} {crash[key]}  {where.get(crash[key], '')}".rstrip())
    if kind == "exception" or (kind == "stack" and crash.get("exception")):
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


INFO_PAGES = ["System", "Video", "Storage", "USB", "Network"]


def set_transfer_style(wii, style):
    """Force HBC's long-name style: scroll, bounce or truncate (HBCT)."""
    name = style.encode()
    if style not in ("scroll", "bounce", "truncate"):
        raise HBCError(f"transfer styles are scroll, bounce and truncate, not {style!r}")
    request(wii, b"HBCT" + name + bytes(12 - len(name)))


def hardware(wii, pages=None):
    """DEV > Info's pages (HBCH): {"System": {"Console": "...", ...}, ...};
    pages, a list of their names, for only those."""
    mask = sum(1 << INFO_PAGES.index(p) for p in pages or [])
    try:
        return json.loads(request(wii, struct.pack(">4sH", b"HBCH", mask)))
    except HBCError as exc:
        if exc.code == 88:  # newlib's ENOSYS
            raise HBCError("this HBC or agent is too old for hw (1.9.6 on)") from None
        raise


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


def file_request(wii, op, remote, size=0, payload=b"", timeout=TIMEOUT):
    """A protocol 1 file request: raw payload in, raw reply body out."""
    with connect(wii, timeout) as conn:
        conn.sendall(file_header(op, remote, size) + payload)
        return recv_reply(conn, f"{remote}: Wii returned")


FRAME = 64 * 1024
FLAG_COMPRESS = 0x01
FLAG_REPLACE = 0x02   # op R: replace a file at the destination
PUT_MAX = 512 << 20    # the largest upload the Wii takes (devfile.c DEVFILE_PUT_MAX)
MOVE_SINCE = (1, 10, 14)   # the first HBC with op R
ATTR_SINCE = (1, 10, 15)   # the first with op A, and every attribute in a listing
# FAT's attributes, as op A's bits and the listing's letters (in this order).
ATTR_BITS = {"r": 0x01, "h": 0x02, "s": 0x04, "a": 0x20}
ATTR_NAMES = {"r": "read-only", "h": "hidden", "s": "system", "a": "archive"}
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
                # An answer before the end is a refusal (no room, a bad
                # path): read it now, before the Wii closes and the reset
                # drops it.
                if select.select([conn], [], [], 0)[0]:
                    break
                conn.sendall(frame)
                sent += struct.unpack(">I", frame[:4])[0]
                if progress:
                    progress(sent, len(data))
        except OSError:
            # The Wii may refuse early and close; prefer its status to the reset.
            try:
                recv_reply(conn, f"{remote}: Wii returned")
            except HBCError as exc:
                if exc.code:
                    raise exc from None
            except OSError:
                pass
            raise
        return recv_reply(conn, f"{remote}: Wii returned")


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
        check_status(status, f"{remote}: Wii returned")
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


def checksum(wii, remote, size=0):
    """Return (size, CRC-32) of a remote file (op C). size, when known,
    gives the Wii time to read all of it before it answers."""
    reply = file_request(wii, "C", remote,
                         timeout=TIMEOUT + size / CHECKSUM_BYTES_PER_S)
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


LIST_VERSION = "#list 2"


def parse_entry(line):
    """One "<d|f> <size> <mtime> <attrs> <name>" line (op L, S) as a dict:
    type, size, mtime (seconds by the Wii's clock, see wii_clock_offset),
    attrs (FAT's attributes as letters, "rhsa" order; "" for none; before
    HBC 1.10.15 only "r" was sent), readonly, hidden, system, archive,
    name."""
    kind, size, mtime, flags, name = line.split(" ", 4)
    attrs = "" if flags == "-" else flags
    return {"type": kind, "size": int(size), "mtime": int(mtime), "attrs": attrs,
            "readonly": "r" in attrs, "hidden": "h" in attrs, "system": "s" in attrs,
            "archive": "a" in attrs, "name": name}


def list_entries(wii, remote):
    """Return ([entry dict, see parse_entry], truncated) for a remote folder."""
    lines = file_request(wii, "L", remote).decode("utf-8", "replace").splitlines()
    if not lines or lines[0] != LIST_VERSION:
        raise HBCError(f"{remote}: this HBC or app agent lists folders the old way "
                       "(no file times); update it to HBC 1.10.6 or later")
    entries, truncated = [], False
    for line in lines[1:]:
        if line == "! truncated":
            truncated = True
        elif line:
            entries.append(parse_entry(line))
    return entries, truncated


def list_dir(wii, remote):
    """Return ([(type, size, name)], truncated) for one remote directory
    (list_entries without the times and flags)."""
    entries, truncated = list_entries(wii, remote)
    return [(e["type"], e["size"], e["name"]) for e in entries], truncated


def stat_remote(wii, remote):
    """One remote path's entry dict (op S)."""
    return parse_entry(file_request(wii, "S", remote).decode("utf-8", "replace").strip())


def wii_clock_offset(wii):
    """Seconds to add to the Wii's file times for PC time, or None when the
    Wii does not say its clock (HBCS "time"). FAT keeps local time with no
    zone, and the Wii's clock is read the same way, so this lines them up
    whatever the zone, give or take a daylight saving change since."""
    st = status(wii)
    return time.time() - st["time"] if "time" in st else None


def wii_time_text(mtime):
    """A Wii file time as the Wii shows it (it keeps local time)."""
    return datetime.datetime.fromtimestamp(mtime, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M")


# ---- App order (meta.xml <sort_id>) ------------------------------------------

SORT_ID_STEP = 10
SORT_ID_RE = re.compile(rb"<sort_id>\s*(-?\d+)\s*</sort_id>")


def meta_sort_id(meta):
    """The <sort_id> in a meta.xml's bytes, or None."""
    m = SORT_ID_RE.search(meta or b"")
    return int(m.group(1)) if m else None


def meta_name(meta):
    m = re.search(rb"<name>\s*(.*?)\s*</name>", meta or b"", re.S)
    return m.group(1).decode("utf-8", "replace") if m else None


def meta_with_sort_id(meta, folder, sort_id):
    """meta.xml's bytes with <sort_id> set (None removes it); a folder with no
    meta.xml gets a minimal one, named after the folder."""
    if meta is None:
        if sort_id is None:
            return None
        name = folder.replace("&", "&amp;").replace("<", "&lt;").encode()
        return (b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                b'<app version="1">\n  <name>' + name + b'</name>\n  <sort_id>' +
                str(sort_id).encode() + b'</sort_id>\n</app>\n')
    if sort_id is None:
        return re.sub(rb"[ \t]*<sort_id>[^<]*</sort_id>[ \t]*\r?\n?", b"", meta)
    tag = b"<sort_id>" + str(sort_id).encode() + b"</sort_id>"
    if re.search(rb"<sort_id>[^<]*</sort_id>", meta):
        return re.sub(rb"<sort_id>[^<]*</sort_id>", tag, meta, count=1)
    m = re.search(rb"(\r?\n?)([ \t]*)</app>", meta)
    if not m:
        raise HBCError("meta.xml has no </app>")
    nl = m.group(1) or b"\n"
    indent = re.search(rb"\n([ \t]+)<", meta)
    indent = indent.group(1) if indent else b"  "
    return meta[:m.start()] + m.group(1) + indent + tag + nl + m.group(2) + meta[m.start(2):]


def app_order(wii, apps_dir):
    """[(sort_id or None, folder, name, meta bytes or None)] for each app
    folder, in HBC's Custom order (ids first, lowest first; then by name)."""
    entries, _ = list_dir(wii, apps_dir)
    rows = []
    for kind, _, folder in entries:
        if kind != "d":
            continue
        try:
            meta = get_file(wii, remote_join(apps_dir, folder + "/meta.xml"))
        except HBCError:
            meta = None
        rows.append((meta_sort_id(meta), folder, meta_name(meta), meta))
    rows.sort(key=lambda r: (r[0] is None, r[0] or 0, (r[2] or r[1]).lower()))
    return rows


def set_order(wii, apps_dir, folders, out=print):
    """Give the folders <sort_id> 10, 20, 30... in the order given."""
    rows = {r[1].lower(): r for r in app_order(wii, apps_dir)}
    for i, folder in enumerate(folders):
        row = rows.get(folder.lower())
        if not row:
            raise HBCError(f"no app folder {folder!r} in {apps_dir}")
        sort_id = (i + 1) * SORT_ID_STEP
        if row[0] == sort_id:
            continue
        put_file(wii, remote_join(apps_dir, row[1] + "/meta.xml"),
                 meta_with_sort_id(row[3], row[1], sort_id))
        out(f"{sort_id:5} {row[1]}")


def clear_order(wii, apps_dir, out=print):
    for sort_id, folder, _, meta in app_order(wii, apps_dir):
        if sort_id is not None:
            put_file(wii, remote_join(apps_dir, folder + "/meta.xml"),
                     meta_with_sort_id(meta, folder, None))
            out(f"cleared {folder}")


def remote_tree(wii, top):
    """List a whole remote tree before anything acts on it. Returns
    (dirs, parents first; {file: (size, mtime)}), paths relative to top
    joined by "/"."""
    dirs, files, pending = [], {}, [""]
    while pending:
        rel = pending.pop()
        path = remote_join(top, rel)
        entries, truncated = list_entries(wii, path)
        if truncated:
            raise HBCError(f"the Wii truncated the listing of {path} (256 KiB limit); "
                           "refusing to act on a partial listing")
        for e in entries:
            sub = f"{rel}/{e['name']}" if rel else e["name"]
            if e["type"] == "d":
                dirs.append(sub)
                pending.append(sub)
            else:
                files[sub] = (e["size"], e["mtime"])
    return dirs, files


def bottom_up(dirs):
    return sorted(dirs, key=lambda d: d.count("/"), reverse=True)


def remote_is_dir(wii, remote):
    """True for a directory, False for a file; raises if it is missing. Op S
    (HBC 1.10.6) answers at once; an older HBC is asked with op C, which
    reads a file whole (a big one can take longer than the timeout)."""
    try:
        return stat_remote(wii, remote)["type"] == "d"
    except HBCError as exc:
        if exc.code != ENOSYS:
            raise HBCError(f"{remote}: {exc}", exc.code) from None
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


def file_crc(path):
    """CRC-32 of a local file, read 1 MiB at a time."""
    crc = 0
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            crc = zlib.crc32(chunk, crc)
    return crc


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


def remove_tree(wii, remote, out=print, force=False):
    """Delete a remote tree: files first, then directories bottom-up. force
    clears read-only on each entry first."""
    refuse_protected(remote, "recursively delete")
    remote = remote_norm(remote)
    if not remote_is_dir(wii, remote):
        remove_one(wii, remote, force)
        return 1
    dirs, files = remote_tree(wii, remote)
    for rel in sorted(files):
        remove_one(wii, remote_join(remote, rel), force)
    for rel in bottom_up(dirs) + [""]:
        remove_one(wii, remote_join(remote, rel), force)
    count = len(files) + len(dirs) + 1
    out(f"rm: {count} entries deleted")
    return count


def remote_exists(wii, remote):
    try:
        stat_remote(wii, remote)
        return True
    except HBCError as exc:
        if exc.code == ENOENT:
            return False
        raise


def needs_version(wii, since, what):
    """Refuse, before sending anything, an HBC or agent older than since."""
    found = re.match(r"(\d+)\.(\d+)\.(\d+)", version(wii))
    if found and tuple(map(int, found.groups())) < since:
        raise HBCError(f"this HBC or app agent ({found.group(0)}) cannot {what}: it needs HBC "
                       f"{'.'.join(map(str, since))}, or an agent app built with it", ENOSYS)


def set_attrs(wii, remote, add="", remove=""):
    """Set (add) and clear (remove) FAT attributes, letters from "rhsa", on
    one remote path (op A); returns its attributes after, as letters."""
    value = sum(ATTR_BITS[c] for c in add)
    mask = value | sum(ATTR_BITS[c] for c in remove)
    letters = file_request(wii, "A", remote, value | mask << 8).decode("ascii", "replace")
    return "" if letters == "-" else letters


def attrib(wii, paths, add="", remove="", recursive=False, out=print):
    """hbc.py attrib: print, or change and print, each path's attributes."""
    needs_version(wii, ATTR_SINCE, "show or change attributes")
    for top in paths:
        targets = [remote_norm(top)]
        if recursive and remote_is_dir(wii, top):
            dirs, files = remote_tree(wii, top)
            targets += [remote_join(remote_norm(top), rel) for rel in sorted(dirs) + sorted(files)]
        for path in targets:
            letters = set_attrs(wii, path, add, remove)
            if out is None:
                print(json.dumps({"path": path, "attrs": letters}))
            else:
                out(f"{''.join(c if c in letters else '-' for c in 'rhsa')}  {path}")


def remove_one(wii, remote, force=False):
    """Delete one file or empty folder. A read-only one gives EACCES, with a
    hint; force then clears read-only and deletes it, as del /f does (only
    when the delete was refused for that: a folder that is not empty keeps
    its read-only)."""
    try:
        file_request(wii, "D", remote)
    except HBCError as exc:
        if exc.code != EACCES:
            raise
        if not force:
            raise HBCError(f"{exc}: it is read-only (hbc.py attrib -r {remote}, or rm --force)",
                           exc.code) from None
        set_attrs(wii, remote, remove="r")
        file_request(wii, "D", remote)


def move(wii, src, dst, replace=False, out=print):
    """Rename or move src to dst on the Wii (op R, HBC 1.10.14). A dst ending
    in "/", or a folder that is there, means "into that folder", keeping the
    name; missing folders above dst are made. An existing dst is refused
    unless replace (a file only). Between devices (sd: to usb:) it copies
    each file, checks its CRC-32, and only then deletes the source. Returns
    the destination as it ended up."""
    refuse_protected(src, "move")
    # Ask first: an older HBC or agent answers op R with ENOSYS without reading
    # the destination that follows the request, and closing on unread data
    # can reset the connection before its answer arrives.
    needs_version(wii, MOVE_SINCE, "rename or move")
    payload = dst.encode("utf-8")
    if len(payload) >= 256:
        raise HBCError(f"{dst}: a path on the Wii is under 256 bytes")
    try:
        with connect(wii) as conn:
            conn.sendall(file_header("R", src, len(payload), FLAG_REPLACE if replace else 0) + payload)
            final = recv_reply(conn, f"{src}: Wii returned").decode("utf-8", "replace")
    except HBCError as exc:
        if exc.code == ENOSYS:
            raise HBCError("this HBC or app agent cannot rename or move (it needs HBC 1.10.14, "
                           "or an agent app built with it)", exc.code) from None
        if exc.code != EXDEV:
            raise
        final = move_across(wii, src, dst, replace)
    out(f"{src} -> {final}")
    return final


def move_across(wii, src, dst, replace):
    """A move between devices: copy every file, check its CRC-32, and only
    then delete the source. Everything is checked first: a read-only entry
    in the source (it could not be deleted after the copy) or a file over
    the Wii's upload limit refuses the move before anything is written. If a
    copy fails, what was copied is removed again and the source is kept."""
    src = remote_norm(src)
    if dst.endswith("/") or (remote_exists(wii, dst) and remote_is_dir(wii, dst)):
        dst = remote_join(remote_norm(dst), remote_base(src))
    top = stat_remote(wii, src)
    is_dir = top["type"] == "d"
    if remote_exists(wii, dst) and (not replace or is_dir or remote_is_dir(wii, dst)):
        raise HBCError(f"{dst}: Wii returned {wii_error(EEXIST)}", EEXIST)
    entries = {src: top}
    if is_dir:
        dirs, rels = remote_tree(wii, src)
        files = {rel: remote_join(src, rel) for rel in rels}
        for rel in sorted(dirs) + sorted(rels):
            entries[remote_join(src, rel)] = stat_remote(wii, remote_join(src, rel))
    else:
        dirs, files = [], {"": src}
    for path, e in entries.items():
        if e["readonly"]:
            raise HBCError(f"{path} is read-only, so the move could not delete it after "
                           f"copying; nothing was copied (hbc.py attrib --recursive -r {src})", EACCES)
        if e["type"] == "f" and e["size"] > PUT_MAX:
            raise HBCError(f"{path}: {e['size']} bytes, over the {PUT_MAX >> 20} MiB a move "
                           "between devices can copy; nothing was copied", 27)
    made = []
    try:
        if is_dir:
            for rel in [""] + sorted(dirs):
                file_request(wii, "M", remote_join(dst, rel) if rel else dst)
            made.append(dst)
        for rel, path in sorted(files.items()):
            target = remote_join(dst, rel) if rel else dst
            data = get_file(wii, path, progress=Progress(path))
            put_file(wii, target, data, progress=Progress(target))
            if not is_dir:
                made.append(target)
            if checksum(wii, target) != (len(data), zlib.crc32(data)):
                raise HBCError(f"{target}: the copy does not match {path}")
    except (OSError, HBCError) as exc:
        for path in made:
            try:
                remove_tree(wii, path, out=lambda *a: None)
            except (OSError, HBCError):
                pass
        raise HBCError(f"{exc}; the copy was removed again and {src} is kept",
                       getattr(exc, "code", None)) from None
    if is_dir:
        remove_tree(wii, src, out=lambda *a: None)
    else:
        file_request(wii, "D", src)
    return dst


# FAT keeps file times in 2 s steps.
FAT_TIME_STEP = 2


def sync(wii, local, remote, delete=False, out=print, checksum_all=False):
    """Make remote mirror local: upload new or changed files, create missing
    directories and, with delete, remove the rest. A file of the same size
    that the Wii wrote after the local one last changed is taken as the
    same without reading it; otherwise (or with checksum_all) the CRC-32s
    decide."""
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
    failed = []
    offset = None if checksum_all else wii_clock_offset(wii)
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
        # One file that fails (a timeout, a full card) does not stop the rest.
        try:
            size = os.path.getsize(path)
            rsize, rtime = rfiles.get(rel, (None, 0))
            if rsize == size and offset is not None and \
                    rtime + offset >= os.path.getmtime(path) + FAT_TIME_STEP:
                skipped += 1   # the Wii's copy is newer: no need to read it
                continue
            if rsize == size and checksum(wii, target, size) == (size, file_crc(path)):
                skipped += 1
                continue
            data = read_file(path)
            put_file(wii, target, data, progress=Progress(target))
        except (OSError, HBCError) as exc:
            out(f"{target}: failed: {exc}")
            failed.append(target)
            continue
        out(f"{path} -> {target} ({len(data)} bytes)")
        uploaded += 1
        nbytes += len(data)
    out(f"sync: {uploaded} uploaded ({nbytes} bytes), {skipped} unchanged, {deleted} deleted"
        + (f", {len(failed)} failed" if failed else ""))
    if failed:
        raise HBCError(f"sync: {len(failed)} file(s) failed: {', '.join(failed)}")
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
    hw                  the Wii's hardware and settings, as DEV > Info shows them
    transfer-style [S]  how HBC shows a long name it is moving (for testing)
    version             just the version (ends in " agent" for an app, below)
    wait [SECONDS]      wait until HBC answers, after a reboot (default 90)
  Running apps
    run FILE [ARGS...]  send a .dol, .elf or .zip and print what it prints
    send FILE [ARGS...] send it without waiting for output
    log                 print what apps started on the Wii print, until Ctrl+C
  Files (sd:/..., usb:/...)
    ls PATH             list a folder (sizes, dates)
    stat PATH           one file or folder: size, date, attributes
    attrib [+r|-r..] P  show or change attributes (read-only, hidden, ...)
    get PATH [LOCAL]    download (-r for a folder)
    put LOCAL PATH      upload (-r for a folder; missing folders are made)
    sync LOCAL PATH     upload only what changed (--delete removes extras)
    rm PATH             delete a file or empty folder (-r a whole folder,
                        --force read-only ones too)
    mkdir PATH          make a folder
    mv FROM TO          rename or move a file or folder (also between devices)
    order DIR [APP...]  HBC's Custom app order: show it, or set it (meta.xml)
  Apps built with the in-app agent (sdk/hbc_agent.h)
    exit                ask the running app to go back to HBC
    key KEYS            press buttons on it: h (HOME), u d l r, a, b, 1, 2, w
    screen FILE.png     save a picture of what the TV shows
    crash [--elf ELF]   show the last crash: where, and why
    lastlog             the last app's final output (or a running app's so far)

Options (before or after the command)
    --wii ADDR          the Wii's address, if $HBC_WII is not set
    --json              machine-readable output (status, version, ls, crash)
    --log-port PORT     the PC port apps send output to (default 4405; in a
                        wii-bench job, $WII_BENCH_LOG_PORT)
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

Lists a folder: "d DATE TIME NAME" for folders, "f SIZE DATE TIME NAME" for
files, the modification time as the Wii's clock had it, and after it the
FAT attributes set, if any: r (read-only), h (hidden), s (system), a
(archive). --json gives each entry's type, size, mtime (seconds, by the
Wii's clock), attrs, readonly, hidden, system, archive and name. Paths look like sd:/apps or
usb:/data (devices: sd, usb, usb2, carda, cardb).""",
    "stat": """\
hbc.py stat PATH

One file or folder on the Wii: its size, when it was last changed (by the
Wii's clock) and its attributes (read-only, hidden, system, archive).
--json for scripts.""",
    "attrib": """\
hbc.py attrib [--recursive] [+r|-r] [+h|-h] [+s|-s] [+a|-a] PATH...

Shows a file's or folder's FAT attributes, as Windows' attrib does: r
read-only, h hidden, s system, a archive. With + or - it sets or clears
them first; letters combine (+rh). --recursive also changes everything
inside a folder.

    hbc.py attrib sd:/apps/myapp            show them
    hbc.py attrib -r sd:/apps/myapp         make it writable
    hbc.py attrib --recursive -r sd:/apps   every app, inside too

A read-only file cannot be written over or deleted, and a read-only folder
cannot be deleted, until -r. HBC 1.10.15 on, and agent apps built with it.""",
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
hbc.py sync [--delete] [--checksum] LOCAL PATH

Makes the folder PATH on the Wii match LOCAL: uploads files that differ and
makes missing folders. A file of the same size that the Wii wrote after the
local copy last changed is taken as the same without reading it (a disc
image is not read again); otherwise the CRC-32s decide. --checksum compares
every same-size file by CRC-32, for when the Wii's copy may have been
changed on the Wii since. With --delete it also removes what LOCAL does not
have. It refuses to delete from a device root or from <device>:/apps
itself. A file that fails is reported and the rest still sync.

    hbc.py sync dist/myapp sd:/apps/myapp""",
    "rm": """\
hbc.py rm [-r] [--force] PATH

Deletes a file or an empty folder, or with -r a whole folder. Device roots
(sd:/) and <device>:/apps itself are refused. A read-only entry is
refused too, unless --force, which clears read-only first (as del /f
does).""",
    "mkdir": """\
hbc.py mkdir PATH

Makes a folder, and any missing folders above it.""",
    "mv": """\
hbc.py mv [--replace] FROM TO

Renames or moves a file or folder. A TO ending in "/", or a folder that is
there, means "into that folder", keeping the name:

    hbc.py mv sd:/apps/old sd:/apps/new             rename a folder
    hbc.py mv sd:/boot.dol sd:/apps/myapp/          move a file into a folder
    hbc.py mv sd:/saves usb:/backup/                move to another device

Missing folders above TO are made. If TO is already there, it is refused,
unless --replace (which replaces a file, never a folder). A folder cannot go
into itself. On one device the Wii renames in place, at once, whatever the
size. Between devices hbc.py copies each file, checks its CRC-32, and
deletes the source only after that; a file there may be up to 512 MiB, a
read-only one is refused before anything is copied, a failed copy is
removed again, and the copies get new times and only the archive
attribute. Device roots and <device>:/apps itself
are refused. An app renamed under sd:/apps shows in HBC's list at once.
Also "rename". HBC 1.10.14 on, and agent apps built with it.""",
    "order": """\
hbc.py order [--clear] APPS [FOLDER ...]

HBC's Custom sort (Options > Sort applications by: Custom) puts apps in the
order of the <sort_id> number in their meta.xml, lowest first; apps without
one follow, by name. With only APPS (such as sd:/apps), order prints the
apps in that order with their ids. With folders, it gives them 10, 20, 30...
in the order named, editing (or making) each meta.xml; the others keep
theirs. --clear removes every <sort_id> under APPS. The list on the Wii
updates once the writes stop.

    hbc.py order sd:/apps
    hbc.py order sd:/apps wiixplorer usbloader_gx nintendont""",
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
    "transfer-style": """\
hbc.py transfer-style [scroll|bounce|truncate]

While hbc.py moves a file, HBC shows it at the bottom of the screen. A name
too long for that line scrolls round, bounces from end to end, or is cut
short with "...": HBC picks one at random each time it starts. This sets
the one to use until HBC restarts, for testing; with no name it prints the
current one (HBCS transfer_style). HBC 1.10.4 on.""",
    "hw": """\
hbc.py hw [--json] [PAGE ...]

The Wii's hardware and settings, as the HOME overlay's DEV > Info pages
show them: console, region, serial and console ID, Wii Menu and IOS
versions, CPU; video mode, cable and TV settings; SD, USB, NAND and memory
cards; USB devices; IP, gateway, DNS, MAC and the connection in use. Name
pages (System, Video, Storage, USB, Network) for only those. HBC 1.9.6 and
agent apps built with it answer (HBCH).""",
    "lastlog": """\
hbc.py lastlog

An agent app's output (stdout and stderr) is kept as it stops, whether by
exit(), a crash, hbc_agent_fatal() or a hang, and HBC shows the last 4 KiB
of it here. While an agent app runs, this prints its output so far.""",
}


COMMAND_HELP["rename"] = COMMAND_HELP["mv"]


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
                 "--recursive": ("recursive", ("get", "put", "rm", "attrib")),
                 "--delete": ("delete", ("sync",)),
                 "--checksum": ("checksum", ("sync",)),
                 "--clear": ("clear", ("crash", "order")),
                 "--replace": ("replace", ("mv", "rename")),
                 "--force": ("force", ("rm",)),
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
        if cmd == "attrib" and re.fullmatch(r"[+-][a-zA-Z]*", arg):   # attrib checks the letters
            rest.append(arg)   # +r, -h ...: attributes, not options
            i += 1
            continue
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
        elif cmd == "transfer-style":
            if args:
                set_transfer_style(wii, args[0])
            print(status(wii).get("transfer_style", "unknown"))
        elif cmd == "hw":
            want = [p for p in INFO_PAGES for a in args if a.lower() == p.lower()]
            if len(want) != len(args):
                raise SystemExit(f"hbc.py: hw pages are {', '.join(INFO_PAGES)}")
            pages = hardware(wii, want)
            if opts.json:
                print(json.dumps(pages, indent=1))
            else:
                for page, rows in pages.items():
                    print(page)
                    for label, value in rows.items():
                        print(f"  {label:16} {value}")
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
            entries, truncated = list_entries(wii, args[0])
            if opts.json:
                print(json.dumps(entries))
            else:
                for e in entries:
                    when = wii_time_text(e["mtime"]) + (f" {e['attrs']}" if e["attrs"] else "")
                    print(f"d {when} {e['name']}" if e["type"] == "d" else
                          f"f {e['size']} {when} {e['name']}")
            if truncated:
                print("hbc.py: warning: the Wii truncated this listing (256 KiB limit)",
                      file=sys.stderr)
        elif cmd == "stat":
            need(1, "REMOTE")
            e = stat_remote(wii, args[0])
            if opts.json:
                print(json.dumps(e))
            else:
                print(f"{'folder' if e['type'] == 'd' else 'file'} {args[0]}: "
                      + (f"{e['size']} bytes, " if e["type"] == "f" else "")
                      + f"modified {wii_time_text(e['mtime'])} (the Wii's clock)"
                      + "".join(f", {ATTR_NAMES[c]}" for c in e["attrs"]))
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
            need(1, "[-r] [--force] REMOTE")
            force = bool(flags.get("force"))
            if force:
                needs_version(wii, ATTR_SINCE, "clear read-only (rm --force)")
            if recursive:
                remove_tree(wii, args[0], force=force)
            else:
                remove_one(wii, args[0], force)
        elif cmd == "attrib":
            bad = [a for a in args if a[0] in "+-" and not re.fullmatch(r"[+-][rhsa]+", a)]
            if bad:
                raise SystemExit(f"hbc.py: attrib: {bad[0]!r}: the attributes are +/- with r, h, s, a")
            ops = [a for a in args if a[0] in "+-"]
            paths = [a for a in args if a[0] not in "+-"]
            if not paths:
                need(len(args) + 1, "[--recursive] [+r|-r] [+h|-h] [+s|-s] [+a|-a] PATH...")
            add = "".join(a[1:] for a in ops if a[0] == "+")
            remove = "".join(a[1:] for a in ops if a[0] == "-")
            attrib(wii, paths, add, remove, recursive, out=None if opts.json else print)
        elif cmd == "mkdir":
            need(1, "REMOTE")
            file_request(wii, "M", args[0])
        elif cmd in ("mv", "rename"):
            need(2, "[--replace] FROM TO")
            final = move(wii, args[0], args[1], bool(flags.get("replace")),
                         out=(lambda *a: None) if opts.json else print)
            if opts.json:
                print(json.dumps({"from": args[0], "to": final}))
        elif cmd == "order":
            need(1, "[--clear] APPS [FOLDER ...]")
            if flags.get("clear"):
                clear_order(wii, args[0])
            elif len(args) > 1:
                set_order(wii, args[0], args[1:])
            else:
                rows = app_order(wii, args[0])
                if opts.json:
                    print(json.dumps([{"sort_id": r[0], "folder": r[1], "name": r[2]}
                                      for r in rows]))
                for sort_id, folder, name, _ in [] if opts.json else rows:
                    shown = "" if sort_id is None else sort_id
                    print(f"{shown:>5} {folder}" + (f"  ({name})" if name else ""))
        elif cmd == "sync":
            need(2, "[--delete] LOCALDIR REMOTEDIR")
            sync(wii, args[0], args[1], delete=flags.get("delete", False),
                 checksum_all=flags.get("checksum", False))
        else:
            print_help(cmd)
            raise SystemExit(2)
    except FileNotFoundError as exc:
        if exc.filename:   # a local path; the Wii's errors are HBCError
            raise SystemExit(f"hbc.py: {exc.filename}: no such file or folder on this PC")
        raise SystemExit(f"hbc.py: {exc}")
    except (OSError, HBCError) as exc:
        raise SystemExit(f"hbc.py: {exc}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
