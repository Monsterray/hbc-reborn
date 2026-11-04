#!/usr/bin/env python3
"""tests/slotled_app's `check` in Dolphin: sdk/slotled's timer, its GPIO
writes and its SYSCONF read run without a fault, and the report says what
a Wii's would (tests/wii_slotled.py checks it the same way).

Dolphin keeps the GPIO register but shows no slot light, so this proves the
code runs, not what the light looks like: that is tests/wii_slotled.py demo.

usage: tests/dolphin_slotled.py      (after make -C tests/slotled_app)"""

import pathlib
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import dolphin_smoke  # noqa: E402
import wii_slotled  # noqa: E402

APP = root / "tests" / "slotled_app" / "slotled_app.dol"


def main():
    if not APP.exists():
        sys.exit(f"{APP} is missing: make -C tests/slotled_app")
    run = dolphin_smoke.Dolphin(APP, sd=True, net=False)
    result = run.profile / "Load" / "WiiSDSync" / "slotled_test" / "result.txt"
    time.sleep(25)
    # A normal close, so Dolphin copies its SD card back to the folder.
    subprocess.run(["taskkill", "/PID", str(run.proc.pid)], capture_output=True)
    try:
        run.proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        run.stop()
    text = result.read_text(errors="replace") if result.exists() else ""
    print("-- result.txt --\n" + (text or "(none)"))
    ok = bool(text) and wii_slotled.check_report(text)
    faults = run.faults()
    for line in faults:
        print("fault: " + line[:160])
    ok = ok and not faults
    run.close()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
