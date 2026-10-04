#!/usr/bin/env python3
"""HBC's HOME overlay right after an app returns to HBC, against later.

Runs tests/agent_app for a few seconds from the HBC the Wii runs, and as soon
as HBC is back opens and closes the overlay (HOME only: nothing in HBC's
menus is chosen), then again once HBC has settled. Prints each opening's
HBCS "overlay" cost and HBC's startup steps (the play log's NAND write is
one of them).

usage: tests/wii_overlay_return.py [WII-IP]
"""

import pathlib
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
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
    wii = hbc.wii_address(sys.argv[1] if len(sys.argv) > 1 else None)
    print(f"HBC {hbc.hbc_wait(wii, 90)}", flush=True)
    hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["stay", "8"])
    agent_checks.wait_agent(wii)
    back = hbc.hbc_wait(wii, 90)
    t0 = time.monotonic()
    print(f"back in HBC {back}", flush=True)
    time.sleep(1)
    st = open_close(wii, f"opening {time.monotonic() - t0:.0f} s after the return")
    print(f"  startup: {st.get('startup')}", flush=True)
    time.sleep(25)
    open_close(wii, f"opening {time.monotonic() - t0:.0f} s after the return")


if __name__ == "__main__":
    main()
