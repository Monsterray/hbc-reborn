#!/usr/bin/env python3
"""The slot light library (sdk/slotled) on the bench Wii, as queue jobs.

  check    read-only: tests/slotled_app's `check` (the GPIO banks, the
           saved Slot illumination, what the timer costs, the light left as
           it was), with its report checked here
  demo     for a person at the Wii: every direct effect, named on the TV;
           they watch the slot light
  ios      for a person at the Wii: IOS's levels and flash patterns
           (--first/--last, 0..29), 5 s each, named on the TV
  save N   writes the Wii's SYSCONF (Slot illumination N, 0..2) and reads it
           back. Only on the owner's request: it is the Wii Menu's setting.
           /shared2/sys is copied to the PC before and after (tests/nandget,
           read-only); the job passes only if exactly the one byte changed
           and SYSCONF kept its owner and permissions. The first copy is
           the backup: put it back with tests/nandput if anything is wrong.
  sysconf  read-only: copies /shared2/sys to the PC and prints Slot
           illumination and IPL.IDL (--compare FILE: the bytes that differ
           from an earlier SYSCONF copy)
  install  puts the app on the SD card as sd:/apps/slotled_test, with a
           meta.xml without <ahb_access/>: started from HBC's menu, by a
           person, it runs `check` without AHBPROT (an app sent over the
           network always keeps it)
  fetch    gets and checks the report such a run left

The capture card cannot see the slot light, so `demo` and `ios` are only
as good as the person watching.

usage: tests/wii_slotled.py check | demo | ios [--first N] [--last N] | save N
                            | sysconf [--compare FILE] | install | fetch
queue: wiibench.py add --wii devwii --name slotled-STEP --cwd REPO -- C:/Python312/python.exe tests/wii_slotled.py STEP
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
PROTECTED = ("192.168.8.200", "192.168.8.243")  # real data: never written by this script
PY = sys.executable
HBC = str(root / "tools" / "hbc.py")
APP = root / "tests" / "slotled_app" / "slotled_app.dol"
NANDGET = root / "tests" / "nandget" / "nandget.dol"
RESULT = "sd:/slotled_test/result.txt"
# No <ahb_access/>: HBC reloads IOS without AHBPROT before starting it.
META = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
  <name>slotled test (no AHBPROT)</name>
  <coder>hbc-reborn</coder>
  <short_description>sdk/slotled's check, without AHBPROT</short_description>
  <long_description>Runs tests/slotled_app's read-only check. Its report goes to sd:/slotled_test/result.txt.</long_description>
  <arguments>
    <arg>check</arg>
  </arguments>
</app>
"""


def bench_wii():
    wii = os.environ.get("WII_BENCH_IP")
    if not wii or not os.environ.get("WII_BENCH_JOB_START"):
        sys.exit("run this as a wii-bench queue job")
    if wii.strip() in PROTECTED:
        sys.exit(f"refusing {wii}: a production Wii")
    return wii


def tool(*args, check=True):
    print("$ hbc.py " + " ".join(args), flush=True)
    return subprocess.run([PY, HBC, *args], check=check)


def answers():
    return subprocess.run([PY, HBC, "--timeout", "3", "version"], capture_output=True).returncode == 0


def launch(*args, wait_s=300, dol=APP):
    """Send the app, see HBC go away while it runs, then wait for HBC."""
    # --no-wait: the app replaces HBC, and `send` would wait up to 60 s for a
    # verdict HBC loses when the app ends first. `wait` follows.
    tool("send", "--no-wait", str(dol), *args, check=False)
    deadline = time.monotonic() + 30
    while answers() and time.monotonic() < deadline:
        time.sleep(0.5)
    tool("wait", str(wait_s))


def report(out):
    path = out / "result.txt"
    tool("get", RESULT, str(path))
    text = path.read_text(errors="replace")
    print("-- result.txt --\n" + text, flush=True)
    return text


def sysconf_copy(dest):
    """/shared2/sys/SYSCONF as it is now, through nandget (read-only)."""
    launch("/shared2/sys", "sd:/slbak_sys", dol=NANDGET)
    tool("get", "-r", "sd:/slbak_sys", str(dest))
    print(f"-- {dest.name}/result.txt --\n" + (dest / "result.txt").read_text(errors="replace"), flush=True)
    sc = dest / "SYSCONF"
    data = sc.read_bytes() if sc.exists() else b""
    if len(data) != 0x4000 or data[:4] != b"SCv0" or data[-4:] != b"SCed":
        sys.exit(f"{sc}: not a whole SYSCONF ({len(data)} bytes)")
    return data


def illum(sc):
    """Slot illumination in a SYSCONF image (the last byte of IPL.IDL), or None."""
    if len(sc) != 0x4000 or sc[:4] != b"SCv0" or sc[-4:] != b"SCed":
        return None
    count = int.from_bytes(sc[4:6], "big")
    for i in range(count):
        off = int.from_bytes(sc[6 + 2 * i:8 + 2 * i], "big")
        if sc[off] == (2 << 5 | 6) and sc[off + 1:off + 8] == b"IPL.IDL":
            return sc[off + 9 + sc[off + 8]]
    return None


def check_saved(after, text, value, before=None):
    """The save's report, and the SYSCONF it left (and the one before, if known)."""
    ok = True

    def need(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what, flush=True)
        ok &= bool(cond)

    old = re.search(r"^before: (-?\d+)", text, re.M)
    need(re.search(rf"slotled_save_level\({value}\): 0$", text, re.M), "slotled_save_level returned 0")
    need(re.search(rf"^SYSCONF now: {value}$", text, re.M), "the app read the new level back")
    attrs = re.findall(r"^attributes (?:before|after): (.*)$", text, re.M)
    need(len(attrs) == 2 and attrs[0] == attrs[1] and attrs[0].startswith("0:"), "owner and permissions unchanged")
    need(illum(after) == value, f"the SYSCONF left holds {value}")
    if before is not None:
        diff = [i for i in range(0x4000) if before[i] != after[i]]
        if old and int(old.group(1)) == value:
            need(not diff, "no byte changed: the level was already set")
        else:
            need(len(diff) == 1, f"exactly one byte changed: {[hex(i) for i in diff[:8]]}")
    return ok


def check_report(text):
    ok = True

    def need(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what, flush=True)
        ok = ok and cond

    need("slotled_init: 0" in text, "slotled_init found the pin")
    need(re.search(r"IPL\.IDL: [23] bytes", text) is not None, "IPL.IDL read")
    m = re.search(r"slotled_saved_level: (-?\d+)", text)
    need(m is not None and 0 <= int(m.group(1)) <= 2, "the saved Slot illumination is 0..2")
    m = re.search(r"busy loop: (\d+) off, (\d+) pulsing", text)
    need(m is not None and int(m.group(2)) < int(m.group(1)), "the timer ran while pulsing")
    m = re.search(r"bad blink: (-?\d+); pulse still running: (\w+)", text)
    need(m is not None and int(m.group(1)) < 0 and m.group(2) == "yes", "a bad argument leaves the effect running")
    need(re.search(r"^slotled_get: 128$", text, re.M) is not None, "slotled_get reads the level set")
    m = re.search(r"at 255: OUT ([0-9a-f]+), busy (\d)", text)
    need(m is not None and int(m.group(1), 16) & 0x20 and m.group(2) == "0", "255 is on, and the timer stopped")
    m = re.search(r"at 0: OUT ([0-9a-f]+), busy (\d)", text)
    need(m is not None and not int(m.group(1), 16) & 0x20 and m.group(2) == "0", "0 is off, and the timer stopped")
    m = re.search(r"slotled_wait: (\d+) ms", text)
    need(m is not None and 580 <= int(m.group(1)) <= 640, "three 200 ms blinks take 600 ms")
    m = re.search(r"after a flash over 24: pin on in (\d+) of (\d+) samples", text)
    # Level 24 is on for 35 us of each 4 ms period: about 1%.
    need(m is not None and 0 < int(m.group(1)) and int(m.group(1)) * 20 < int(m.group(2)),
         "a flash hands back to the dim level")
    m = re.search(r"pin (on|off) at the start, (on|off) now", text)
    need(m is not None and m.group(1) == m.group(2), "the light is left as it was found")
    need(text.rstrip().endswith("done"), "the app finished")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=("check", "demo", "ios", "save", "sysconf", "install", "fetch"))
    ap.add_argument("value", nargs="?", type=int)
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, default=29)
    ap.add_argument("--compare", type=pathlib.Path)
    a = ap.parse_args()
    bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    out.mkdir(parents=True, exist_ok=True)
    if a.step == "check":
        launch("check")
        return 0 if check_report(report(out)) else 1
    if a.step == "sysconf":
        data = sysconf_copy(out / "now")
        print(f"Slot illumination: {illum(data)}", flush=True)
        if a.compare:
            old = a.compare.read_bytes()
            diff = [i for i in range(0x4000) if old[i] != data[i]]
            print(f"{len(diff)} byte(s) differ from {a.compare}:", flush=True)
            for i in diff[:64]:
                print(f"  {i:#06x}: {old[i]:02x} -> {data[i]:02x}", flush=True)
        return 0
    if a.step == "install":
        app = out / "slotled_test"
        app.mkdir(exist_ok=True)
        (app / "boot.dol").write_bytes(APP.read_bytes())
        (app / "meta.xml").write_bytes(META.encode())
        tool("put", "-r", str(app), "sd:/apps/slotled_test")
        return 0
    if a.step == "fetch":
        text = report(out)
        return 0 if check_report(text) else 1
    if a.step == "demo":
        print("Watch the slot light: each effect is named on the TV.", flush=True)
        launch("demo")
    elif a.step == "ios":
        print("Watch the slot light: each pattern is named on the TV.", flush=True)
        launch("ios", str(a.first), str(a.last), wait_s=600)
    else:
        if a.value is None or not 0 <= a.value <= 2:
            sys.exit("save takes 0, 1 or 2")
        if not NANDGET.exists():
            sys.exit("build tests/nandget first: nothing was run")
        before = sysconf_copy(out / "before")
        launch("save", str(a.value))
        text = report(out)
        after = sysconf_copy(out / "after")
        return 0 if check_saved(after, text, a.value, before) and text.rstrip().endswith("done") else 1
    text = report(out)
    return 0 if text.rstrip().endswith("done") else 1


if __name__ == "__main__":
    sys.exit(main())
