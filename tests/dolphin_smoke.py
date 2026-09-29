#!/usr/bin/env python3
"""Boot the channel DOL in an isolated Dolphin profile and read its version.

Dolphin's IOS socket emulation opens real host sockets, so the guest's
Wiiload listener on TCP 4299 answers on this machine's LAN address. A matching HBCV reply
shows that the DOL booted, reached its menu loop, and brought up networking.

usage: tests/dolphin_smoke.py [DOL-or-WAD] [seconds]
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
import time

root = pathlib.Path(__file__).resolve().parents[1]
image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                     root / "channel/channelapp/channelapp-channel.dol").resolve()
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 60

config = (root / "channel/channelapp/config.h").read_text()
expected = re.search(r'^#define CHANNEL_VERSION_STR "([^"]+)"', config, re.M)[1]


def find_dolphin():
    if os.environ.get("DOLPHIN"):
        return os.environ["DOLPHIN"]
    system = platform.system()
    candidates = {
        "Windows": [r"C:\tools\Dolphin-x64\Dolphin.exe",
                    r"C:\Program Files\Dolphin\Dolphin.exe"],
        "Darwin": ["/Applications/Dolphin.app"],
    }.get(system, [])
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


def query_version(address):
    with socket.create_connection((address, 4299), timeout=2) as conn:
        conn.settimeout(2)
        conn.sendall(b"HBCV" + bytes(12))
        reply = conn.recv(64)
    return reply.split(b"\0", 1)[0].decode("ascii", "replace")


# Every setting is passed on the command line, so no Dolphin.ini is written.
settings = [
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

if not image.is_file():
    raise SystemExit(f"no such image: {image}")
dolphin = find_dolphin()
profile = pathlib.Path(tempfile.mkdtemp(prefix="hbc-dolphin-"))
args = ["-b", "-e", str(image), "-u", str(profile)]
for setting in settings:
    args += ["-C", setting]

if dolphin.endswith(".app"):
    # A direct executable launch aborts in Qt/Cocoa startup on macOS.
    proc = subprocess.Popen(["open", "-n", "-W", "-a", dolphin, "--args"] + args)
else:
    proc = subprocess.Popen([dolphin] + args)

print(f"dolphin: {dolphin}\nprofile: {profile}\nimage: {image}")
address = host_address()
result, last = 1, "no reply"
deadline = time.monotonic() + seconds
try:
    while time.monotonic() < deadline and proc.poll() is None:
        try:
            actual = query_version(address)
            if actual == expected:
                print(f"PASS: HBC {actual} answered on {address}:4299")
                result = 0
                break
            last = f"version {actual!r}, expected {expected}"
        except OSError as exc:
            last = str(exc)
        time.sleep(2)
    else:
        print(f"FAIL: {last}")
finally:
    if dolphin.endswith(".app"):
        subprocess.run(["pkill", "-f", str(profile)])
    proc.kill()  # this run's process only
    proc.wait()

log = profile / "Logs/dolphin.log"
if log.exists():
    faults = [line for line in log.read_text(errors="replace").splitlines()
              if re.search(r"Unknown instruction|Invalid (read|write)|PANIC|Exception", line)]
    for line in faults[:20]:
        print("log:", line)
    if faults:
        result = 1
if result == 0:
    shutil.rmtree(profile, ignore_errors=True)
else:
    print(f"kept profile for inspection: {profile}")
sys.exit(result)
