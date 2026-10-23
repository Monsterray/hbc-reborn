#!/usr/bin/env python3
"""Why User Agreements might not open (tests/eulacheck), as a queue job.

Sends tests/eulacheck/eulacheck.dol (make -C tests/eulacheck) to the job's
Wii, waits for HBC to come back, and prints the app's report: the Wii
Menu's version, the console's region, country and language, each hidden
EULA channel (HAKx) installed with the IOS it needs and how many of its
contents are on the NAND, and that IOS. Read-only: the app uses ES's title
list and the settings, and writes only sd:/eulacheck/result.txt.

usage: tests/wii_eulacheck.py
queue: wiibench.py add --wii lrwii --name eula --cwd REPO -- python tests/wii_eulacheck.py
"""

import os
import pathlib
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

DOL = root / "tests" / "eulacheck" / "eulacheck.dol"


def main():
    wii = hbc.wii_address(None)
    print(f"Wii {os.environ.get('WII_BENCH_WII') or wii}: HBC {hbc.version(wii)}", flush=True)
    hbc.send(wii, str(DOL), [])
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:     # the app takes HBC's place while it runs
        try:
            hbc.version(wii, hbc.POLL_TIMEOUT)
        except (OSError, hbc.HBCError):
            break
        time.sleep(0.3)
    hbc.hbc_wait(wii, 120)
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT") or tempfile.mkdtemp()) / "eula_result.txt"
    out.write_bytes(hbc.get_file(wii, "sd:/eulacheck/result.txt"))
    report = out.read_text(errors="replace")
    print(report, flush=True)
    sys.exit(0 if "done" in report else 1)


if __name__ == "__main__":
    main()
