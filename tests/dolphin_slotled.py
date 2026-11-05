#!/usr/bin/env python3
"""tests/slotled_app's `check` in Dolphin: sdk/slotled's timer, its GPIO
writes and its SYSCONF read run without a fault, and the report says what
a Wii's would (tests/wii_slotled.py checks it the same way).

Dolphin keeps the GPIO register but shows no slot light, so this proves the
code runs, not what the light looks like: that is tests/wii_slotled.py demo.

`save N` runs the save instead, on the run's own throwaway NAND, and checks
the report and the SYSCONF Dolphin is left with.

usage: tests/dolphin_slotled.py [save N]   (after make -C tests/slotled_app)"""

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
    save = sys.argv[1:3] if sys.argv[1:2] == ["save"] else None
    if save and (len(save) != 2 or save[1] not in ("0", "1", "2")):
        sys.exit("usage: tests/dolphin_slotled.py [save 0|1|2]")
    files = {"Load/WiiSDSync/slotled_test/args.txt": " ".join(save) + "\n"} if save else None
    run = dolphin_smoke.Dolphin(APP, sd=True, net=False, files=files)
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
    if save:
        sc = run.profile / "Wii" / "shared2" / "sys" / "SYSCONF"
        data = sc.read_bytes() if sc.exists() else b""
        ok = bool(text) and wii_slotled.check_saved(data, text, int(save[1]))
    else:
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
