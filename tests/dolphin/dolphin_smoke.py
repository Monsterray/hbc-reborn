#!/usr/bin/env python3
"""Boot the channel DOL in an isolated Dolphin profile and check it over TCP.

Dolphin's IOS socket emulation opens real host sockets, so the guest's
Wiiload listener on TCP 4299 answers on this machine's LAN address. A
matching HBCV reply shows that the DOL booted, reached its menu loop, and
brought up networking.

With --devnet the run also enables an emulated SD card and exercises the
developer protocol through tools/hbc.py: status, mkdir, put, ls, get, rm,
and a network-log round trip with tests/apps/netlog_app. --agent (which implies
the SD card) runs tests/apps/agent_app, built with sdk/hbc_agent, through the
checks in tests/lib/agent_checks.py, including a crash and its report. It turns
on Dolphin's MMU emulation so the app's store to address 0x10 faults. It
boots the retail WAD by default and refuses a DOL, since the agent checks
need each app's exit to return to an installed HBC.

A .wad image is installed into the profile's NAND and booted from there.
Cheats are enabled (with no codes) so Dolphin does not replace HBC's reload
stub with its own HBReload hook; with --devnet the run then requires the
installed channel to come back after netlog_app exits.

usage: tests/dolphin/dolphin_smoke.py [--devnet] [--agent] [DOL-or-WAD] [seconds]
Dolphin is found by tools/wii_paths.py: set DOLPHIN, or name it once in
~/.wii-paths.json, if it is not on PATH or in the standard install.

Each run's profile (a copy of the NAND, about 500 MB) is deleted when the
script exits, whether the run passes or fails. HBC_KEEP_PROFILE=1 keeps it
for inspection. A profile older than a day, left by a run that was killed,
is deleted when the next run starts.
"""

import atexit
import os
import pathlib
import platform
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import hbc  # noqa: E402

config = (root / "channel/channelapp/config.h").read_text()
expected = re.search(r'^#define CHANNEL_VERSION_STR "([^"]+)"', config, re.M)[1]
# What the last build reports: the release plus its git build tag (Makefile).
build_tag = root / "channel/channelapp/build_tag.h"
if build_tag.exists():
    expected = re.search(r'"([^"]+)"', build_tag.read_text())[1]

KEEP_PROFILE = os.environ.get("HBC_KEEP_PROFILE") == "1"
PROFILE_PREFIX = "hbc-dolphin-"
STALE_PROFILE_S = 24 * 3600


def remove_profile(path):
    """Delete a profile folder. Dolphin's files can stay locked for a
    moment after it exits on Windows, so try a few times."""
    for _ in range(10):
        shutil.rmtree(path, ignore_errors=True)
        if not os.path.exists(path):
            return True
        time.sleep(0.5)
    return False


def sweep_stale_profiles():
    """Delete profiles older than a day: runs that were killed before
    they could delete their own."""
    now = time.time()
    for path in pathlib.Path(tempfile.gettempdir()).glob(PROFILE_PREFIX + "*"):
        try:
            old = now - path.stat().st_mtime > STALE_PROFILE_S
        except OSError:
            continue
        if old and path.is_dir():
            remove_profile(path)


# Every setting is passed on the command line, so no Dolphin.ini is written.
SETTINGS = [
    "Dolphin.Interface.ConfirmStop=False",
    "Dolphin.Interface.UsePanicHandlers=False",
    "Dolphin.Analytics.PermissionAsked=True",
    "Dolphin.Analytics.Enabled=False",
    "Dolphin.Core.CPUThread=True",
    "Dolphin.Core.DSPHLE=True",
    "Logger.Options.WriteToFile=True",
    "Logger.Options.Verbosity=4",
    "Logger.Logs.MASTER=True",
    "Logger.Logs.BOOT=True",
    "Logger.Logs.OSREPORT=True",
    "Logger.Logs.IOS=True",
    "Logger.Logs.IOS_NET=True",
]
SD_SETTINGS = [
    "Dolphin.Core.WiiSDCard=True",
    "Dolphin.Core.WiiSDCardAllowWrites=True",
    "Dolphin.Core.WiiSDCardEnableFolderSync=True",
]


def find_dolphin():
    """Dolphin as tools/wii_paths.py finds it: $DOLPHIN, ~/.wii-paths.json, PATH, the
    standard install. On macOS the app bundle is run through `open`."""
    import wii_paths
    if (platform.system() == "Darwin" and not wii_paths._env_path("DOLPHIN")
            and os.path.exists("/Applications/Dolphin.app")):
        return "/Applications/Dolphin.app"
    try:
        return str(wii_paths.dolphin())
    except wii_paths.NotFound as e:
        raise SystemExit(str(e))


def host_address():
    # Dolphin binds the guest socket to the host's outbound address.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.connect(("192.0.2.1", 9))  # no packet is sent
        return udp.getsockname()[0]


def port_taken(address, port=hbc.PORT):
    """What already answers on this PC's LAN address at the Wiiload port, or
    None. Dolphin binds the guest's port 4299 there, so only one Dolphin on
    this PC can run HBC or an agent app at a time: a second one's HBC never
    binds it, and the test talks to the first one's guest instead."""
    try:
        with socket.create_connection((address, port), timeout=1) as conn:
            conn.settimeout(2)
            conn.sendall(b"HBCV" + bytes(12))
            return conn.recv(64).split(b"\0")[0].decode(errors="replace").strip() or "something"
    except OSError:
        return None


def wad_title(image):
    """The title ID in a WAD's TMD, as Dolphin's NAND path parts (e.g.
    00010001, 4f484243): OHBC by default, LULZ for a build made to replace
    the original Homebrew Channel."""
    data = pathlib.Path(image).read_bytes()
    align = lambda n: (n + 63) & ~63
    hdr, certs, crl, tik = (int.from_bytes(data[i:i + 4], "big") for i in (0, 8, 12, 16))
    tmd = align(hdr) + align(certs) + align(crl) + align(tik)
    tid = data[tmd + 0x18C:tmd + 0x194]
    return tid[:4].hex(), tid[4:].hex()


NO_HOST_INPUT = {
    "Config/WiimoteNew.ini": "[Wiimote1]\nDevice = DSUClient/0/none\n",
    "Config/GCPadNew.ini": "[GCPad1]\nDevice = DSUClient/0/none\n",
}


class Dolphin:
    def __init__(self, image, sd=False, mmu=False, extra=(), files=None, net=True):
        """extra: more -C settings; files: {path in the profile: text or
        bytes}, such as a controller mapping (tests/dolphin/dolphin_meta.py) or a
        NAND file (tests/dolphin/dolphin_mii_picker.py). net=False for a run that
        never talks to the guest over TCP, which may go beside another
        Dolphin."""
        if net:
            taken = port_taken(host_address())
            if taken:
                raise SystemExit(f"port {hbc.PORT} on {host_address()} already answers "
                                 f"({taken!r}): another Dolphin on this PC runs HBC or an "
                                 "agent app, and only one can hold the port. Wait for it.")
        self.dolphin = find_dolphin()
        sweep_stale_profiles()
        self.profile = pathlib.Path(tempfile.mkdtemp(prefix=PROFILE_PREFIX))
        self.proc = None
        # whatever way the script ends: stop Dolphin, delete the profile
        atexit.register(self.close)
        # Dolphin's default Wii Remote 1 and GameCube pad 1 are this PC's
        # keyboard and mouse, and the pad tests turn on background input: a
        # click anywhere on the desktop was A in HBC, and once launched an app
        # mid-test. Both go to a device that does not exist unless a test
        # maps them (dolphin_meta.PAD_MAP).
        files = {**NO_HOST_INPUT, **(files or {})}
        for rel, text in files.items():
            (self.profile / rel).parent.mkdir(parents=True, exist_ok=True)
            if isinstance(text, bytes):
                (self.profile / rel).write_bytes(text)
            else:
                (self.profile / rel).write_text(text)
        settings = SETTINGS + (SD_SETTINGS if sd else []) + list(extra)
        if mmu:
            settings.append("Dolphin.Core.MMU=True")
        if str(image).lower().endswith(".wad"):
            settings.append("Dolphin.Core.EnableCheats=True")
        if sd:
            (self.profile / "Load/WiiSDSync/apps").mkdir(parents=True, exist_ok=True)
        args = ["-b", "-e", str(image), "-u", str(self.profile)]
        for setting in settings:
            args += ["-C", setting]
        if self.dolphin.endswith(".app"):
            # A direct executable launch aborts in Qt/Cocoa startup on macOS.
            self.proc = subprocess.Popen(["open", "-n", "-W", "-a", self.dolphin, "--args"] + args,
                                         cwd=self.profile)
        else:
            # cwd: the GPU driver writes its logs (NVIDIA Corporation/umdlogs)
            # into the working folder, so keep them in the profile
            self.proc = subprocess.Popen([self.dolphin] + args, cwd=self.profile)
        self.address = host_address()
        print(f"dolphin: {self.dolphin}\nprofile: {self.profile}\nimage: {image}")

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def close(self):
        """Stop Dolphin and delete the profile (HBC_KEEP_PROFILE=1 keeps it).
        Runs again at exit; the second call does nothing."""
        if self.running():
            self.stop()
        if not self.profile.exists():
            return
        if KEEP_PROFILE:
            print(f"kept profile (HBC_KEEP_PROFILE=1): {self.profile}")
        elif not remove_profile(self.profile):
            print(f"could not delete profile {self.profile}: delete it by hand")

    def stop(self):
        if self.dolphin.endswith(".app"):
            subprocess.run(["pkill", "-f", str(self.profile)])
        self.proc.kill()  # this run's process only
        self.proc.wait()

    def faults(self, expected=()):
        """Log lines that suggest a fault, except those matching a pattern in
        expected (a deliberate crash)."""
        log = self.profile / "Logs/dolphin.log"
        if not log.exists():
            return []
        return [line for line in log.read_text(errors="replace").splitlines()
                if re.search(r"Unknown instruction|Invalid (read|write)|PANIC|Exception", line)
                and not any(re.search(p, line) for p in expected)]


def wait_version(run, seconds):
    deadline, last = time.monotonic() + seconds, "no reply"
    while time.monotonic() < deadline and run.running():
        try:
            actual = hbc.version(run.address)
            if actual == expected:
                return
            last = f"version {actual!r}, expected {expected}"
        except (OSError, hbc.HBCError) as exc:
            last = str(exc)
        time.sleep(2)
    raise AssertionError(last)


def bottom_luma(wii):
    """Mean luma of the bottom tenth of the TV picture."""
    width, height, data = hbc.screen(wii)
    rows = data[width * 2 * (height * 9 // 10):]
    return sum(rows[0::2]) / (len(rows) // 2)


def check_home(wii):
    """HBC's HOME menu is the agent's overlay, driven with `hbc.py key`. HBC
    takes HOME only from its app list, so let its start-up finish first."""
    time.sleep(3)
    before = bottom_luma(wii)
    hbc.send_keys(wii, "h")
    time.sleep(1.5)
    shown = bottom_luma(wii)
    assert abs(shown - before) > 4, f"HOME showed no strip (luma {before:.0f} -> {shown:.0f})"
    # Left of Exit is the HBC slot; its menu holds the old HOME menu.
    hbc.send_keys(wii, "la")
    time.sleep(2)
    menu = hbc.screen(wii)[2]
    # B closes the menu, HOME the overlay; HBC keeps answering throughout.
    hbc.send_keys(wii, "bh")
    time.sleep(2.5)
    assert hbc.version(wii, 5)
    after = bottom_luma(wii)
    assert abs(after - before) < 4, f"the overlay did not close (luma {before:.0f} -> {after:.0f})"
    assert menu != hbc.screen(wii)[2]
    print(f"HBC HOME menu (agent overlay): PASS (strip luma {before:.0f} -> {shown:.0f} -> {after:.0f})")


def check_devnet(run, log_port=0):
    wii = run.address
    status = hbc.status(wii)
    print("status:", status)
    assert status["version"] == expected, status
    check_home(wii)

    # The SD card mounts shortly after the menu starts.
    deadline = time.monotonic() + 30
    while hbc.status(wii)["device"] != "sd":
        assert time.monotonic() < deadline, "SD card did not mount"
        time.sleep(1)

    payload = os.urandom(100_000) + b"end"
    hbc.file_request(wii, "M", "sd:/hbctest/sub")
    hbc.file_request(wii, "P", "sd:/hbctest/sub/blob.bin", len(payload), payload)
    listing = hbc.file_request(wii, "L", "sd:/hbctest/sub").decode()
    assert any(e["name"] == "blob.bin" and e["size"] == len(payload)
               for e in map(hbc.parse_entry, listing.splitlines()[1:])), listing
    assert hbc.file_request(wii, "G", "sd:/hbctest/sub/blob.bin") == payload
    for bad in ("sd:/hbctest/../x", "sd:relative", "nand:/x", "sd:/a\\b"):
        try:
            hbc.file_request(wii, "L", bad)
        except hbc.HBCError:
            continue
        raise AssertionError(f"accepted bad path {bad!r}")
    # Protocol 2: framed, CRC-checked, compressed where it helps.
    hbc._proto.pop(wii, None)
    assert hbc.proto(wii) >= 2
    # The session's first framed download, of three frames: its end frame
    # goes in a buffer slot that never held data (a NULL write before 1.7.0).
    first = os.urandom(140_000)
    hbc.put_file(wii, "sd:/hbctest/sub/first.bin", first)
    assert hbc.get_file(wii, "sd:/hbctest/sub/first.bin") == first
    hbc.file_request(wii, "D", "sd:/hbctest/sub/first.bin")
    samples = {"random": os.urandom(300_001), "zeros": bytes(200_000),
               "text": b"hbc devnet " * 30_000, "tiny": b"x", "exact": os.urandom(65536)}
    for name, blob in samples.items():
        path = f"sd:/hbctest/sub/{name}.bin"
        hbc.put_file(wii, path, blob)
        up = hbc.status(wii)["last"]
        assert hbc.get_file(wii, path) == blob, name
        down = hbc.status(wii)["last"]
        assert hbc.file_request(wii, "G", path) == blob, f"{name} (protocol 1 read)"
        print(f"  {name}: {len(blob)} bytes, wire up {up['wire']} / down {down['wire']}")
        hbc.file_request(wii, "D", path)

    # A frame with a wrong CRC must fail the upload and leave no file behind.
    bad = bytearray(next(hbc.frames(b"corrupt me" * 100, level=0)))
    bad[8] ^= 0xff
    with socket.create_connection((wii, hbc.PORT), timeout=15) as conn:
        conn.sendall(hbc.file_header("p", "sd:/hbctest/sub/bad.bin", 1000) + bytes(bad))
        try:
            hbc.recv_reply(conn)
            raise AssertionError("the Wii accepted a frame with a bad CRC")
        except hbc.HBCError as exc:
            assert "EBADMSG" in str(exc), exc
    assert "bad.bin" not in hbc.file_request(wii, "L", "sd:/hbctest/sub").decode()
    print("devnet framed transfers: PASS")

    # Status reports real memory, stack, and timing numbers.
    st = hbc.status(wii)
    for key in ("heap_free", "tcp_stack_used", "tcp_stack_size", "init_ms", "scan_ms"):
        assert key in st, (key, st)
    assert 0 < st["tcp_stack_used"] < st["tcp_stack_size"], st
    print(f"  heap {st['heap_free']} free, tcp stack "
          f"{st['tcp_stack_used']} of {st['tcp_stack_size']}, init {st['init_ms']} ms, "
          f"scan {st['scan_ms']} ms")

    # A client that connects and closes without a request must not stall the
    # loader (it used to spin for the 10 s receive timeout).
    socket.create_connection((wii, hbc.PORT), timeout=5).close()
    start = time.monotonic()
    hbc.status(wii)
    stall = time.monotonic() - start
    assert stall < 4, f"status took {stall:.1f} s after a bare connect"
    print(f"  bare connect then status: {stall:.2f} s")

    # Op C returns the size and CRC-32 that the client computes.
    blob = os.urandom(70_000)
    hbc.put_file(wii, "sd:/hbctest/sub/crc.bin", blob)
    assert hbc.checksum(wii, "sd:/hbctest/sub/crc.bin") == (len(blob), zlib.crc32(blob))
    hbc.file_request(wii, "D", "sd:/hbctest/sub/crc.bin")

    # A put to a path ending in "/" names a directory, not a file.
    try:
        hbc.file_request(wii, "P", "sd:/hbctest/sub/", 3, b"abc")
        raise AssertionError("put to a trailing slash was accepted")
    except hbc.HBCError as exc:
        assert "EISDIR" in str(exc), exc

    # An app put under sd:/apps/ shows up in the menu without a restart, and
    # goes away when it is removed.
    before = hbc.status(wii)["apps"]
    app = (root / "tests/apps/netlog_app/netlog_app.dol").read_bytes()
    hbc.put_file(wii, "sd:/apps/hbctestapp/boot.dol", app)
    deadline = time.monotonic() + 5
    while hbc.status(wii)["apps"] != before + 1:
        assert time.monotonic() < deadline, "the uploaded app did not appear"
        time.sleep(0.2)
    hbc.remove_tree(wii, "sd:/apps/hbctestapp", out=lambda *a: None)
    deadline = time.monotonic() + 5
    while hbc.status(wii)["apps"] != before:
        assert time.monotonic() < deadline, "the removed app did not go away"
        time.sleep(0.2)
    print(f"  app list followed a put and a removal ({before} -> {before + 1} -> {before})")

    # sync uploads only what changed; get -r brings the tree back intact.
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp, "src")
        (src / "data").mkdir(parents=True)
        (src / "a.bin").write_bytes(os.urandom(5000))
        (src / "data" / "b.txt").write_bytes(b"hello " * 1000)
        quiet = lambda *a: None
        first = hbc.sync(wii, str(src), "sd:/hbctest/sync", out=quiet)
        again = hbc.sync(wii, str(src), "sd:/hbctest/sync", out=quiet)
        assert first[0] == 2 and again[0] == 0 and again[1] == 2, (first, again)
        (src / "a.bin").write_bytes(os.urandom(5000))
        changed = hbc.sync(wii, str(src), "sd:/hbctest/sync", out=quiet)
        assert changed[0] == 1, changed
        dst = pathlib.Path(tmp, "dst")
        hbc.get_tree(wii, "sd:/hbctest/sync", str(dst), out=quiet)
        for rel in ("a.bin", "data/b.txt"):
            assert (dst / rel).read_bytes() == (src / rel).read_bytes(), rel
        hbc.remove_tree(wii, "sd:/hbctest/sync", out=quiet)
    print("devnet tools (checksum, sync, trees, app list, EOF): PASS")

    # Op E: the Wii walks a tree, or matches a pattern, itself. A deletion
    # must also leave the folder it emptied listing correctly.
    for rel in ("a.log", "B.LOG", "keep.txt", "dir/x.log", "dir/deeper/y.bin"):
        hbc.file_request(wii, "P", f"sd:/hbctest/rm/{rel}", 1, b"x")
    hbc.file_request(wii, "M", "sd:/apps")   # so the patterns below have it to refuse
    for i in range(60):
        hbc.file_request(wii, "P", f"sd:/hbctest/rm/many/f{i:02}.bin", 1, b"x")
    quiet = lambda *a: None
    assert hbc.remove_glob(wii, "sd:/hbctest/rm/*.log", out=quiet) == 2
    assert hbc.remove_glob(wii, "sd:/hbctest/rm/ma*", recursive=True, out=quiet) == 61
    listing = hbc.file_request(wii, "L", "sd:/hbctest/rm").decode()
    assert "keep.txt" in listing and "dir" in listing and "many" not in listing, listing
    assert not hbc._no_tree_delete, "the Wii refused op E"
    # rm -r with a pattern at a device's top: hbc.py refuses it before it
    # asks the Wii (no errno), and the Wii refuses it too, with EPERM before
    # anything went; a "." component is EINVAL in any request.
    for refused in ("sd:/*", "sd:/AP?S", "sd:/./AP?S"):
        try:
            hbc.remove_glob(wii, refused, recursive=True, out=quiet)
            raise AssertionError(f"{refused} was deleted")
        except hbc.HBCError as exc:
            assert exc.code is None and "takes no wildcard" in str(exc), (refused, exc)
    for refused, code in (("sd:/*", 1), ("sd:/AP?S", 1), ("sd:/./AP?S", 22)):
        try:
            hbc.remote_delete(wii, refused, True, glob=True)   # the Wii itself
            raise AssertionError(f"{refused} was deleted")
        except hbc.HBCError as exc:
            assert exc.code == code, (refused, exc)
    assert hbc.remove_tree(wii, "sd:/hbctest/rm", out=quiet) == 6   # 3 files, dir, deeper, rm
    for refused, code in (("sd:/", 1), ("sd:/apps", 1), ("sd:/APPS/", 1), ("sd:/./apps", 22)):
        try:
            hbc.remote_delete(wii, refused, True)   # the Wii itself, not hbc.py
            raise AssertionError(f"{refused} was deleted")
        except hbc.HBCError as exc:
            assert exc.code == code, (refused, exc)
    assert hbc.file_request(wii, "S", "sd:/apps")
    print("devnet tree and pattern delete: PASS")

    hbc.file_request(wii, "D", "sd:/hbctest/sub/blob.bin")
    assert "blob.bin" not in hbc.file_request(wii, "L", "sd:/hbctest/sub").decode()
    hbc.file_request(wii, "D", "sd:/hbctest/sub")
    hbc.file_request(wii, "D", "sd:/hbctest")
    print("devnet files: PASS")

    import agent_checks   # the rename checks, shared with the agent's
    agent_checks.check_move(wii, "sd:/hbcmv", apps=True)
    agent_checks.check_attrs(wii, "sd:/hbcattr")

    app = root / "tests/apps/netlog_app/netlog_app.dol"
    server = hbc.LogServer(log_port)
    server.register(wii)
    assert hbc.status(wii)["log"].endswith(f":{server.port}")
    output = bytearray()

    def serve():
        deadline = time.monotonic() + 60
        while True:  # LogServer's socket times out so Ctrl-C works on Windows
            try:
                conn, _ = server.sock.accept()
                break
            except TimeoutError:
                if time.monotonic() > deadline:
                    return
        conn.settimeout(30)
        with conn:
            try:
                while chunk := conn.recv(4096):
                    output.extend(chunk)
            except (ConnectionResetError, TimeoutError):
                pass  # IOS closes sockets with a reset
        server.done.set()

    threading.Thread(target=serve, daemon=True).start()
    hbc.send(wii, str(app), ["hello", "world"])
    assert server.done.wait(60), f"no complete log; got {bytes(output)!r}"
    text = output.decode()
    print(text, end="")
    assert "argv[1]=hello" in text and "argv[2]=world" in text and "done" in text, text
    print("devnet log: PASS")


def main():
    args = sys.argv[1:]
    devnet = "--devnet" in args
    agent = "--agent" in args
    args = [a for a in args if a not in ("--devnet", "--agent")]
    default = ("channel/title/channel_retail.wad" if agent else
               "channel/channelapp/channelapp-channel.dol")
    image = pathlib.Path(args[0] if args else root / default).resolve()
    seconds = int(args[1]) if len(args) > 1 else 60
    if not image.is_file():
        raise SystemExit(f"no such image: {image}")
    if agent and image.suffix.lower() != ".wad":
        # A DOL boot has no installed title for the reload stub to launch, so
        # HBC keeps Dolphin's HBReload hook at 0x80001800. The app's exit then
        # asks Dolphin to stop, the guest ignores the power event, and the CPU
        # runs on from 0x80001800 into garbage: HBC never comes back.
        raise SystemExit("--agent needs the retail WAD (make -C channel): an app's exit "
                         "cannot return to a DOL-booted HBC in Dolphin")

    run = Dolphin(image, sd=devnet or agent, mmu=agent)
    expected_faults = []
    result = 1
    try:
        wait_version(run, seconds)
        print(f"PASS: HBC {expected} answered on {run.address}:4299")
        # HBCU discovery: asked directly, since a broadcast need not reach
        # Dolphin's host socket. One HBC answers, by its menu's name.
        found = hbc.find_wiis(3, [run.address])
        assert [(w["agent"], w["name"]) for w in found] == [(False, "Homebrew Channel")], found
        print(f"PASS: HBCU discovery answered from {found[0]['address']}")
        if image.suffix.lower() == ".wad":
            title = run.profile.joinpath("Wii/title", *wad_title(image), "content")
            apps = sorted(p.name for p in title.glob("*.app")) if title.exists() else []
            assert apps, f"no installed content under {title}"
            print(f"PASS: WAD installed to NAND ({', '.join(apps)})")
        if devnet:
            check_devnet(run)
            if image.suffix.lower() == ".wad":
                # netlog_app exits through HBC's own stub, which relaunches the title.
                wait_version(run, 60)
                print(f"PASS: app exit returned to the installed HBC {expected}")
        if agent:
            import agent_checks
            # The deliberate crash's store to 0x10.
            expected_faults = [r"0x00000010", r"DSI"]
            crash = agent_checks.check_agent(run.address)
            assert crash, "HBC reported no crash"
            wait_version(run, 60)
            print("agent: PASS")
        result = 0
    except (AssertionError, OSError, hbc.HBCError) as exc:
        print(f"FAIL: {exc!r}")
    finally:
        run.stop()

    for line in run.faults(expected_faults)[:20]:
        print("log:", line)
        result = 1
    run.close()
    sys.exit(result)


if __name__ == "__main__":
    main()
