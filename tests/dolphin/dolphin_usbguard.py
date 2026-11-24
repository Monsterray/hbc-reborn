#!/usr/bin/env python3
"""tests/apps/usbguard_app's `check` in Dolphin: with a healthy USB (Dolphin has
no drive and nothing stuck), every sdk/usbguard wrapper answers as the plain
call would. No STUCK phase, no write to the EHCI ports, the IOS reload
works, and the report is written.

usage: tests/dolphin/dolphin_usbguard.py   (after make -C tests/apps/usbguard_app)"""

import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import dolphin_smoke  # noqa: E402

APP = root / "tests" / "apps" / "usbguard_app" / "usbguard_app.dol"
RUN_SECONDS = 25  # the longest the app is given; its checks take a few seconds


def check_report(text):
    """True when the report shows a healthy run."""
    ok = True

    def need(cond, what):
        nonlocal ok
        if not cond:
            print("FAIL: " + what)
            ok = False

    need("done" in text.split("\n"), "the app did not finish")
    need("stuck phase seen: 0" in text, "a STUCK phase on a healthy USB")
    need("  phase " not in text, "on_wait saw a phase change")
    need(not re.search(r": (STUCK|FREED|NO_WORKER) in", text), "a guard result")
    # The guarded reload answers as the plain one does.
    m = re.search(r"^reload IOS: (-?\d+) in", text, re.M)
    p = re.search(r"^plain reload IOS: (-?\d+) in", text, re.M)
    need(m and p and m.group(1) == p.group(1), "the guarded IOS reload differs from the plain one")
    m = re.search(r"mount usb: (\S+) in (\d+) ms", text)
    need(m and m.group(1) in ("0", "NO_DRIVE"), "the mount said neither mounted nor no drive")
    need(m and int(m.group(2)) < 5000, "the mount took 5 s or more")
    need("check CUT_OFF" not in text, "a port reads cut off")
    need("blocked 1" not in text, "a worker reads blocked")
    return ok


def main():
    if not APP.exists():
        sys.exit(f"{APP} is missing: make -C tests/apps/usbguard_app")
    run = dolphin_smoke.Dolphin(APP, sd=True, net=False)
    result = run.profile / "Load" / "WiiSDSync" / "usbguard_test" / "result.txt"
    # result.txt exists on the host only after Dolphin closes normally and
    # copies its SD card back, so it cannot be polled while the app runs. The
    # app writes it just before it ends (three seconds at most to its exit):
    # wait for Dolphin to end by itself, and close it after the limit.
    try:
        run.proc.wait(timeout=RUN_SECONDS)
    except subprocess.TimeoutExpired:
        pass
    # A normal close, so Dolphin copies its SD card back to the folder.
    subprocess.run(["taskkill", "/PID", str(run.proc.pid)], capture_output=True)
    try:
        run.proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        run.stop()
    text = result.read_text(errors="replace") if result.exists() else ""
    print("-- result.txt --\n" + (text or "(none)"))
    ok = bool(text) and check_report(text)
    faults = run.faults()
    for line in faults:
        print("fault: " + line[:160])
    ok = ok and not faults
    run.close()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
