#!/usr/bin/env python3
"""HOME > HBC > Restart network, on a Wii or in Dolphin.

Runs this tree's channel DOL through the installed HBC, opens its HOME
overlay with `hbc.py key`, moves to "Restart network" in the HBC slot's
menu, checking HBCS overlay_ui before every A (menu 4 is the HBC slot's,
focus 207 its eighth item; a press elsewhere could launch BootMii), presses
A, and waits for HBCS net_starts (times the network came up) to go up: the
restart can be quicker than a poll can see a gap. Ends
in the installed HBC through tests/agent_app's exit.

usage: tests/wii_net_restart.py [WII-IP] [--dolphin]
"""

import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402

MENU_SLOT0, RESTART = 4, 207   # sdk/hbc_agent/ov_ui.c; home.c's items


def ui(wii):
    return hbc.status(wii).get("overlay_ui") or {}


def key(wii, k):
    hbc.send_keys(wii, k)
    time.sleep(0.6)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    wii = hbc.wii_address(args[0] if args else None)
    if "--dolphin" not in sys.argv:
        work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-net-"))
        shutil.copy(root / "channel/channelapp/channelapp-channel.dol", work / "hbc.dol")
        hbc.send(wii, str(work / "hbc.dol"), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
        shutil.rmtree(work, ignore_errors=True)
    time.sleep(3)
    if "net_starts" not in hbc.status(wii):
        raise SystemExit("FAIL: this HBC has no HBCS net_starts (an older build?)")
    try:
        key(wii, "h")
        key(wii, "l")
        state = ui(wii)
        print(f"bar: {state}", flush=True)
        key(wii, "a")
        for _ in range(10):
            state = ui(wii)
            if state.get("menu") != MENU_SLOT0 or state.get("focus") == RESTART:
                break
            # Down a row; where down goes nowhere (the right column above
            # the last row's single button), left.
            key(wii, "d")
            if ui(wii).get("focus") == state.get("focus"):
                key(wii, "l")
        state = ui(wii)
        print(f"menu: {state}", flush=True)
        if state.get("menu") != MENU_SLOT0 or state.get("focus") != RESTART:
            key(wii, "h")   # close it; nothing pressed
            raise SystemExit(f"FAIL: not on Restart network ({state})")
        st = hbc.status(wii)
        before, ip = st["net_starts"], st["ip"]
        hbc.send_keys(wii, "a")
        t = time.monotonic()
        while time.monotonic() - t < 60:
            try:
                st = hbc.status(wii)
                if st["net_starts"] > before:
                    break
            except (OSError, hbc.HBCError):
                pass
            time.sleep(0.5)
        else:
            raise SystemExit(f"FAIL: no restart within 60 s (net_starts still {before})")
        print(f"restarted: net_starts {before} -> {st['net_starts']}, back "
              f"{time.monotonic() - t:.1f} s after A; IP {ip} -> {st['ip']}", flush=True)
    except BaseException:
        # Never leave HOME open: an upload that arrives while it is open
        # waits for it to close, and HBC answers nothing until then.
        try:
            if ui(wii):
                key(wii, "h")
        except (OSError, hbc.HBCError):
            pass
        raise
    finally:
        if "--dolphin" not in sys.argv:
            hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
            time.sleep(8)
            print(f"back in HBC {hbc.hbc_wait(wii, 90)}")
    print("PASS")


if __name__ == "__main__":
    main()
