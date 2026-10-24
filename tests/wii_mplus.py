#!/usr/bin/env python3
"""The agent's overlay with a MotionPlus remote, on a Wii.

Runs tests/agent_app (an app that never uses MotionPlus itself), or with
--hbc this tree's channel DOL (HBC's HOME menu is the agent's overlay), opens
the HOME overlay with `hbc.py key h`, goes to WiiMote > (the remote) > More >
Test, checking HBCS overlay_ui's focus before every A, and then checks HBCS
"remotes" for that remote:

  - Test switched its MotionPlus on: "mplus" is "active", "mplus_ours" true,
    and "gyro" gives the three rates (about 8000 each at rest);
  - closing the overlay (HOME) switched it back off: "mplus" is "present"
    (found, no longer the expansion) and "mplus_ours" false, so the app has
    the remote back as it left it.

A remote with no MotionPlus is reported as "none" and the test says so. Put
a Wii Remote Plus (or one with a MotionPlus fitted) on the Wii first; leave it
still for the gyro figures. Ends in HBC through the agent's exit.

usage: tests/wii_mplus.py [WII-IP] [--chan N] [--seconds S] [--hbc]
"""

import argparse
import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
sys.path.insert(0, str(root / "tests"))
import hbc  # noqa: E402
import dolphin_smoke  # noqa: E402  (expected: this tree's version)

MENU_WM = 3                                        # sdk/hbc_agent/ov_ui.c
ID_WIIMOTE, ID_FIND, ID_MORE, ID_TEST = 104, 140, 150, 171


def ui(wii):
    return hbc.status(wii).get("overlay_ui") or {}


def key(wii, k):
    hbc.send_keys(wii, k)
    time.sleep(0.6)


def remote(wii, chan):
    for r in hbc.status(wii).get("remotes") or []:
        if r.get("chan") == chan:
            return r
    return None


def focus_to(wii, target, steps):
    """Press keys from `steps(focus)` until the focus is target; never A."""
    for _ in range(14):
        f = ui(wii).get("focus")
        if f == target:
            return True
        k = steps(f)
        if not k:
            return False
        key(wii, k)
    return ui(wii).get("focus") == target


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wii", nargs="?")
    ap.add_argument("--chan", type=int, default=0, help="the remote, 0-3 (default 0: player 1)")
    ap.add_argument("--seconds", type=float, default=3, help="how long to read the gyro")
    ap.add_argument("--hbc", action="store_true", help="HBC's own HOME menu (this tree's DOL)")
    a = ap.parse_args()
    wii = hbc.wii_address(a.wii)
    chan = a.chan
    failed = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    if a.hbc:
        work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-mplus-"))
        shutil.copy(root / "channel/channelapp/channelapp-channel.dol", work / "hbc.dol")
        hbc.send(wii, str(work / "hbc.dol"), [])
        print(f"running HBC {hbc.relaunch_wait(wii, dolphin_smoke.expected)}", flush=True)
        shutil.rmtree(work, ignore_errors=True)
        time.sleep(3)
    else:
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["stay", "240"])
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                if hbc.is_agent(hbc.version(wii, hbc.POLL_TIMEOUT)):
                    break
            except (OSError, hbc.HBCError):
                pass
            time.sleep(0.5)
        time.sleep(2)
    try:
        before = remote(wii, chan)
        print(f"remote {chan} before: {before}", flush=True)
        if not before:
            raise SystemExit(f"FAIL: no remote on channel {chan} (connect one and run again)")
        check("mplus" in before, "the agent reports MotionPlus per remote (this tree's agent)")

        key(wii, "h")
        ok = focus_to(wii, ID_WIIMOTE, lambda f: "r" if f is not None and f < ID_WIIMOTE else "l")
        if not ok:
            raise SystemExit(f"FAIL: could not reach WiiMote on the bar ({ui(wii)})")
        key(wii, "a")

        def to_more(f):
            if f is None:
                return None
            if ID_FIND <= f < ID_FIND + 4:
                n = f - ID_FIND
                return "d" if n < chan else "u" if n > chan else "r"
            if ID_MORE <= f < ID_MORE + 4:
                n = f - ID_MORE
                return "d" if n < chan else "u" if n > chan else None
            return "u"
        if ui(wii).get("menu") != MENU_WM or not focus_to(wii, ID_MORE + chan, to_more):
            raise SystemExit(f"FAIL: could not reach remote {chan}'s More ({ui(wii)})")
        key(wii, "a")
        moves = iter("dlrudl")
        if not focus_to(wii, ID_TEST, lambda f: next(moves, None)):
            raise SystemExit(f"FAIL: could not reach Test ({ui(wii)})")
        key(wii, "a")                                  # Test: the agent switches MotionPlus on

        t0 = time.monotonic()
        r = remote(wii, chan)
        while time.monotonic() - t0 < 6 and r and r.get("mplus") != "active":
            time.sleep(0.4)
            r = remote(wii, chan)
        print(f"remote {chan} on Test after {time.monotonic() - t0:.1f} s: {r}", flush=True)
        if r and r.get("mplus") == "none":
            print("this remote has no MotionPlus: the probe found none", flush=True)
        check(r is not None and r.get("mplus") == "active", "Test switched the MotionPlus on")
        check(r is not None and r.get("mplus_ours") is True, "the agent knows it switched it on")
        samples = []
        t1 = time.monotonic()
        while time.monotonic() - t1 < a.seconds and r and r.get("mplus") == "active":
            if r.get("gyro"):
                samples.append(r["gyro"])
            time.sleep(0.25)
            r = remote(wii, chan)
        if samples:
            axes = list(zip(*samples))
            spread = [max(x) - min(x) for x in axes]
            mean = [sum(x) // len(x) for x in axes]
            print(f"gyro over {len(samples)} reads: mean pitch/roll/yaw {mean}, spread {spread}", flush=True)
            check(all(0 < m < 16384 for m in mean), "the gyro reads in range (14 bits)")
            check(len({tuple(s) for s in samples}) > 1, "the gyro updates (the values move)")

        key(wii, "h")                                  # close: the agent switches it back off
        t2 = time.monotonic()
        r = remote(wii, chan)
        while time.monotonic() - t2 < 6 and r and (r.get("mplus") == "active" or r.get("mplus_ours")):
            time.sleep(0.4)
            r = remote(wii, chan)
        print(f"remote {chan} after the overlay closed: {r}", flush=True)
        check(not ui(wii), "the overlay closed")
        check(r is not None and r.get("mplus") != "active" and r.get("mplus_ours") is False,
              "closing switched it back off: the app has the remote as it left it")
    except BaseException:
        try:
            if ui(wii):
                key(wii, "h")
        except (OSError, hbc.HBCError):
            pass
        raise
    finally:
        try:
            if a.hbc:
                # Back to the installed HBC: an agent app's exit returns there.
                hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
                time.sleep(8)
            else:
                hbc.exit_app(wii)                      # hbc.py exit: the agent leaves to HBC
                time.sleep(4)
        except (OSError, hbc.HBCError):
            pass
        print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
