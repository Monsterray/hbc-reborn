#!/usr/bin/env python3
"""How fast HBC's HOME overlay draws: the first opening after HBC starts,
then later ones (HBCS "overlay": frames drawn, average and worst time each).

Sends this tree's channel DOL through the installed HBC, opens and closes
the overlay with HOME only (no A or D-pad presses: nothing in HBC's menus is
ever chosen), and returns to the installed HBC through tests/agent_app's
exit.

usage: tests/wii_overlay_speed.py [WII-IP]
"""

import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import agent_checks  # noqa: E402
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402


def open_close(wii, label, seconds=4):
    hbc.send_keys(wii, "h")
    time.sleep(seconds)
    hbc.send_keys(wii, "h")
    time.sleep(1.5)
    cost = hbc.status(wii).get("overlay")
    print(f"{label}: {cost}", flush=True)
    return cost


def main():
    wii = hbc.wii_address(sys.argv[1] if len(sys.argv) > 1 else None)
    work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-ovspeed-"))
    snap = work / "channelapp-channel.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    hbc.send(wii, str(snap), [])
    hbc.relaunch_wait(wii, dolphin_smoke.expected)
    t0 = time.monotonic()
    print(f"HBC {hbc.version(wii)}", flush=True)
    time.sleep(3)   # HBC takes HOME once its menu is up
    first = open_close(wii, f"first opening, {time.monotonic() - t0:.0f} s after start")
    second = open_close(wii, "second opening, right after")
    time.sleep(20)
    third = open_close(wii, f"third opening, {time.monotonic() - t0:.0f} s after start")
    # Back to the installed HBC through an app's exit.
    hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            if hbc.version(wii, hbc.POLL_TIMEOUT) != dolphin_smoke.expected:
                break
        except (OSError, hbc.HBCError):
            break
        time.sleep(0.5)
    print(f"back in HBC {hbc.hbc_wait(wii, 90)}")
    shutil.rmtree(work, ignore_errors=True)
    return first, second, third


if __name__ == "__main__":
    main()
