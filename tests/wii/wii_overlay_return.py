#!/usr/bin/env python3
"""HBC's HOME overlay right after an app returns to HBC, against later.

Runs tests/apps/agent_app for a few seconds from the HBC the Wii runs, and as soon
as HBC is back opens and closes the overlay (HOME only: nothing in HBC's
menus is chosen). Prints the opening's HBCS "overlay" cost and HBC's startup
steps (the play log's NAND write is one of them). This test owns the opening
right after a return; wii_overlay_speed owns the first and later openings
(timing), so a later opening here is only taken with --later.

usage: tests/wii/wii_overlay_return.py [--later] [WII-IP]
"""

import pathlib
import sys
import time

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import agent_checks  # noqa: E402
import hbc  # noqa: E402


def open_close(wii, label, seconds=4):
    hbc.send_keys(wii, "h")
    time.sleep(seconds)
    hbc.send_keys(wii, "h")
    time.sleep(1.5)
    st = hbc.status(wii)
    print(f"{label}: {st.get('overlay')}", flush=True)
    return st


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    wii = hbc.wii_address(args[0] if args else None)
    print(f"HBC {hbc.hbc_wait(wii, 90)}", flush=True)
    hbc.send(wii, str(root / "tests/apps/agent_app/agent_app.dol"), ["stay", "8"])
    agent_checks.wait_agent(wii)
    back = hbc.hbc_wait(wii, 90)
    t0 = time.monotonic()
    print(f"back in HBC {back}", flush=True)
    time.sleep(1)
    st = open_close(wii, f"opening {time.monotonic() - t0:.0f} s after the return")
    print(f"  startup: {st.get('startup')}", flush=True)
    if "--later" in sys.argv:
        time.sleep(25)
        open_close(wii, f"opening {time.monotonic() - t0:.0f} s after the return")


if __name__ == "__main__":
    main()
