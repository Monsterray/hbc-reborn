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

The capture card cannot see the slot light, so `demo` and `ios` are only
as good as the person watching.

usage: tests/wii_slotled.py check | demo | ios [--first N] [--last N] | save N
queue: wiibench.py add --name slotled-STEP --cwd REPO -- C:/Python312/python.exe tests/wii_slotled.py STEP
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
RESULT = "sd:/slotled_test/result.txt"


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


def launch(*args, wait_s=300):
    """Send the app, see HBC go away while it runs, then wait for HBC."""
    tool("send", str(APP), *args, check=False)
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
    m = re.search(r"to the timer\)", text)
    need(m is not None, "the timer ran while pulsing")
    need(re.search(r"at 255: OUT [0-9a-f]*[2367abef][0-9a-f]\b", text) is not None, "255 is on")
    m = re.search(r"slotled_wait: (\d+) ms", text)
    need(m is not None and 580 <= int(m.group(1)) <= 640, "three 200 ms blinks take 600 ms")
    m = re.search(r"pin (on|off) at the start, (on|off) now", text)
    need(m is not None and m.group(1) == m.group(2), "the light is left as it was found")
    need(text.rstrip().endswith("done"), "the app finished")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=("check", "demo", "ios", "save"))
    ap.add_argument("value", nargs="?", type=int)
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, default=29)
    a = ap.parse_args()
    bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    out.mkdir(parents=True, exist_ok=True)
    if a.step == "check":
        launch("check")
        return 0 if check_report(report(out)) else 1
    if a.step == "demo":
        print("Watch the slot light: each effect is named on the TV.", flush=True)
        launch("demo")
    elif a.step == "ios":
        print("Watch the slot light: each pattern is named on the TV.", flush=True)
        launch("ios", str(a.first), str(a.last), wait_s=600)
    else:
        if a.value is None or not 0 <= a.value <= 2:
            sys.exit("save takes 0, 1 or 2")
        launch("save", str(a.value))
    text = report(out)
    return 0 if text.rstrip().endswith("done") else 1


if __name__ == "__main__":
    sys.exit(main())
