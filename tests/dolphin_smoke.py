#!/usr/bin/env python3
"""Boot the channel DOL in an isolated Dolphin profile and check it over TCP.

Dolphin's IOS socket emulation opens real host sockets, so the guest's
Wiiload listener on TCP 4299 answers on this machine's LAN address. A
matching HBCV reply shows that the DOL booted, reached its menu loop, and
brought up networking.

With --devnet the run also enables an emulated SD card and exercises the
developer protocol through tools/hbc.py: status, mkdir, put, ls, get, rm,
and a network-log round trip with tests/netlog_app.

A .wad image is installed into the profile's NAND and booted from there.
Cheats are enabled (with no codes) so Dolphin does not replace HBC's reload
stub with its own HBReload hook; with --devnet the run then requires the
installed channel to come back after netlog_app exits.

usage: tests/dolphin_smoke.py [--devnet] [DOL-or-WAD] [seconds]
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
    def __init__(self, image, sd=False):
        self.dolphin = find_dolphin()
        self.profile = pathlib.Path(tempfile.mkdtemp(prefix="hbc-dolphin-"))
        settings = SETTINGS + (SD_SETTINGS if sd else [])
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

    def faults(self):
        log = self.profile / "Logs/dolphin.log"
        if not log.exists():
            return []
        return [line for line in log.read_text(errors="replace").splitlines()
                if re.search(r"Unknown instruction|Invalid (read|write)|PANIC|Exception", line)]


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
        conn, _ = server.sock.accept()
        with conn:
            try:
                while chunk := conn.recv(4096):
                    output.extend(chunk)
            except ConnectionResetError:
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
    args = [a for a in args if a != "--devnet"]
    image = pathlib.Path(args[0] if args else
                         root / "channel/channelapp/channelapp-channel.dol").resolve()
    seconds = int(args[1]) if len(args) > 1 else 60
    if not image.is_file():
        raise SystemExit(f"no such image: {image}")

    run = Dolphin(image, sd=devnet)
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
        result = 0
    except (AssertionError, OSError, hbc.HBCError) as exc:
        print(f"FAIL: {exc!r}")
    finally:
        run.stop()

    for line in run.faults()[:20]:
        print("log:", line)
        result = 1
    if result == 0:
        shutil.rmtree(run.profile, ignore_errors=True)
    else:
        print(f"kept profile for inspection: {run.profile}")
    sys.exit(result)


if __name__ == "__main__":
    main()
