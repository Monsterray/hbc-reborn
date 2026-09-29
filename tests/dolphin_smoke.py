#!/usr/bin/env python3
"""Boot the channel DOL in an isolated Dolphin profile and check it over TCP.

Dolphin's IOS socket emulation opens real host sockets, so the guest's
Wiiload listener on TCP 4299 answers on this machine's LAN address. A
matching HBCV reply shows that the DOL booted, reached its menu loop, and
brought up networking.

With --devnet the run also enables an emulated SD card and exercises the
developer protocol through tools/hbc.py: status, mkdir, put, ls, get, rm,
and a network-log round trip with tests/netlog_app. --agent (which implies
the SD card) runs tests/agent_app, built with sdk/hbc_agent, through the
checks in tests/agent_checks.py, including a crash and its report. It turns
on Dolphin's MMU emulation so the app's store to address 0x10 faults.

A .wad image is installed into the profile's NAND and booted from there.
Cheats are enabled (with no codes) so Dolphin does not replace HBC's reload
stub with its own HBReload hook; with --devnet the run then requires the
installed channel to come back after netlog_app exits.

usage: tests/dolphin_smoke.py [--devnet] [--agent] [DOL-or-WAD] [seconds]
Set DOLPHIN to the Dolphin executable if it is not in a standard location.
"""

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

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

config = (root / "channel/channelapp/config.h").read_text()
expected = re.search(r'^#define CHANNEL_VERSION_STR "([^"]+)"', config, re.M)[1]

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
    if os.environ.get("DOLPHIN"):
        return os.environ["DOLPHIN"]
    candidates = {
        "Windows": [r"C:\tools\Dolphin-x64\Dolphin.exe",
                    r"C:\Program Files\Dolphin\Dolphin.exe"],
        "Darwin": ["/Applications/Dolphin.app"],
    }.get(platform.system(), [])
    candidates += [shutil.which("dolphin-emu") or ""]
    for path in candidates:
        if path and os.path.exists(path):
            return path
    raise SystemExit("Dolphin not found; set DOLPHIN to its executable")


def host_address():
    # Dolphin binds the guest socket to the host's outbound address.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.connect(("192.0.2.1", 9))  # no packet is sent
        return udp.getsockname()[0]


class Dolphin:
    def __init__(self, image, sd=False, mmu=False):
        self.dolphin = find_dolphin()
        self.profile = pathlib.Path(tempfile.mkdtemp(prefix="hbc-dolphin-"))
        settings = SETTINGS + (SD_SETTINGS if sd else [])
        if mmu:
            settings.append("Dolphin.Core.MMU=True")
        if str(image).lower().endswith(".wad"):
            settings.append("Dolphin.Core.EnableCheats=True")
        if sd:
            (self.profile / "Load/WiiSDSync/apps").mkdir(parents=True)
        args = ["-b", "-e", str(image), "-u", str(self.profile)]
        for setting in settings:
            args += ["-C", setting]
        if self.dolphin.endswith(".app"):
            # A direct executable launch aborts in Qt/Cocoa startup on macOS.
            self.proc = subprocess.Popen(["open", "-n", "-W", "-a", self.dolphin, "--args"] + args)
        else:
            self.proc = subprocess.Popen([self.dolphin] + args)
        self.address = host_address()
        print(f"dolphin: {self.dolphin}\nprofile: {self.profile}\nimage: {image}")

    def running(self):
        return self.proc.poll() is None

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


def check_devnet(run, log_port=0):
    wii = run.address
    status = hbc.status(wii)
    print("status:", status)
    assert status["version"] == expected, status

    # The SD card mounts shortly after the menu starts.
    deadline = time.monotonic() + 30
    while hbc.status(wii)["device"] != "sd":
        assert time.monotonic() < deadline, "SD card did not mount"
        time.sleep(1)

    payload = os.urandom(100_000) + b"end"
    hbc.file_request(wii, "M", "sd:/hbctest/sub")
    hbc.file_request(wii, "P", "sd:/hbctest/sub/blob.bin", len(payload), payload)
    listing = hbc.file_request(wii, "L", "sd:/hbctest/sub").decode()
    assert f"f {len(payload)} blob.bin" in listing, listing
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
    app = (root / "tests/netlog_app/netlog_app.dol").read_bytes()
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

    hbc.file_request(wii, "D", "sd:/hbctest/sub/blob.bin")
    assert "blob.bin" not in hbc.file_request(wii, "L", "sd:/hbctest/sub").decode()
    hbc.file_request(wii, "D", "sd:/hbctest/sub")
    hbc.file_request(wii, "D", "sd:/hbctest")
    print("devnet files: PASS")

    app = root / "tests/netlog_app/netlog_app.dol"
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
    image = pathlib.Path(args[0] if args else
                         root / "channel/channelapp/channelapp-channel.dol").resolve()
    seconds = int(args[1]) if len(args) > 1 else 60
    if not image.is_file():
        raise SystemExit(f"no such image: {image}")

    run = Dolphin(image, sd=devnet or agent, mmu=agent)
    expected_faults = []
    result = 1
    try:
        wait_version(run, seconds)
        print(f"PASS: HBC {expected} answered on {run.address}:4299")
        if image.suffix.lower() == ".wad":
            title = run.profile / "Wii/title/00010001/4f484243/content"
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
    if result == 0:
        shutil.rmtree(run.profile, ignore_errors=True)
    else:
        print(f"kept profile for inspection: {run.profile}")
    sys.exit(result)


if __name__ == "__main__":
    main()
