#!/usr/bin/env python3
"""A network cleanup that never returns: HBC carries on, or reloads if set to.

Requires a WAD built with HBC_TEST_STUCK_NET_CLEANUP, where Restart network
wedges the TCP thread inside its cleanup while it holds its mutex. Never put
that build on a Wii. HOME > HBC > Restart network must take HBC off the
network. By default HBC then keeps running without it: Dolphin's log shows
no IOS reload and no second launch of the channel for 40 s.
With --reload, `hbc.py net-hang reload` is set first: after the 5 s watchdog
the log must show one IOS reload and one more launch of the installed
channel, which answers again and stays up: one relaunch, no loop.
Usage: python3 tests/dolphin/dolphin_network_recovery.py [--reload] TEST-WAD
"""
import pathlib
import shutil
import subprocess
import sys
import time

import dolphin_smoke as smoke

hbc = smoke.hbc
LAUNCH = "BootstrapPPC: /title/00010001/4f484243/"
IOS_START = "Starting IOS 000000010000003a"  # IOS58


def key(wii, buttons):
    hbc.send_keys(wii, buttons)
    time.sleep(0.7)


def count(run, text):
    log = run.profile / "Logs/dolphin.log"
    return log.read_text(errors="replace").count(text) if log.exists() else 0


def main():
    reload = sys.argv[1] == "--reload"
    image = pathlib.Path(sys.argv[-1]).resolve()
    assert image.suffix == ".wad", "requires an installed channel"
    run = smoke.Dolphin(image)
    passed = False
    try:
        smoke.wait_version(run, 60)
        time.sleep(2)
        launches, ios_starts = count(run, LAUNCH), count(run, IOS_START)
        assert launches == 1, launches
        assert hbc.status(run.address)["net_hang"] == "wait"
        if reload:
            hbc.set_net_hang(run.address, "reload")
            assert hbc.status(run.address)["net_hang"] == "reload"
        for buttons in ("h", "l", "a", "d", "d"):
            key(run.address, buttons)
        state = hbc.status(run.address)["overlay_ui"]
        assert state["menu"] == 4 and state["focus"] == 207, state  # Restart network
        hbc.send_keys(run.address, "a")
        pressed = time.monotonic()

        # The wedged thread never reopens the listener.
        time.sleep(3)
        try:
            hbc.version(run.address, timeout=2)
            raise AssertionError("HBC still answers: the cleanup did not wedge")
        except (OSError, hbc.HBCError):
            pass
        assert count(run, LAUNCH) == launches, "relaunched before the watchdog"

        if not reload:
            # No reload: the menu carries on without the network.
            while time.monotonic() - pressed < 40:
                assert count(run, LAUNCH) == launches, "relaunched with net-hang wait"
                assert count(run, IOS_START) == ios_starts, "IOS reloaded with net-hang wait"
                time.sleep(0.5)
            assert not run.faults(), run.faults()[:10]
            print("PASS: wedged cleanup left HBC running, no IOS reload, no relaunch", flush=True)
            passed = True
            return

        deadline = pressed + 90
        while time.monotonic() < deadline:
            if count(run, LAUNCH) > launches:
                break
            time.sleep(0.5)
        else:
            raise AssertionError("net-hang reload did not relaunch the channel")
        relaunched = time.monotonic() - pressed
        # Only after the 5 s watchdog.
        assert relaunched >= 4.5, relaunched
        assert count(run, IOS_START) == ios_starts + 1, "no fresh IOS before the relaunch"
        smoke.wait_version(run, 60)
        time.sleep(10)
        assert count(run, LAUNCH) == launches + 1, "relaunch loop"
        assert hbc.version(run.address) == smoke.expected
        assert not run.faults(), run.faults()[:10]
        print(f"PASS: wedged cleanup recovered through a fresh IOS and one channel relaunch "
              f"{relaunched:.0f} s after Restart network; no loop", flush=True)
        passed = True
    finally:
        if run.dolphin.endswith(".app"):
            subprocess.run(["pkill", "-KILL", "-f", str(run.profile)], check=False)
        run.stop()
        if passed:
            shutil.rmtree(run.profile, ignore_errors=True)
        else:
            print(f"Retained profile: {run.profile}", flush=True)


if __name__ == "__main__":
    main()
