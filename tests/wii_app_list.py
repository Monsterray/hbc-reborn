#!/usr/bin/env python3
"""HBC's app list while files arrive over the network, and the Custom order.

Runs this tree's channel DOL through the installed HBC, then with file
requests only (no controller input): uploads a burst of files into a test
app folder (two the list shows, ten it does not), and checks HBCS
"app_list": the list reloaded once, in place, and slid to no page (each file
used to slide to the app's page, which looked like the pages cycling). Then
gives the test apps <sort_id>s with hbc.py's set_order, prints the order,
removes them, and ends in the installed HBC through tests/agent_app's exit.

usage: tests/wii_app_list.py [WII-IP]
"""

import os
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

BURST = "sd:/apps/zz-hbctest-burst"
SECOND = "sd:/apps/zz-hbctest-second"


def main():
    wii = hbc.wii_address(sys.argv[1] if len(sys.argv) > 1 else None)
    work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-applist-"))
    snap = work / "channelapp-channel.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    hbc.send(wii, str(snap), [])
    hbc.relaunch_wait(wii, dolphin_smoke.expected)
    time.sleep(3)
    failed = []
    try:
        before = hbc.status(wii)["app_list"]
        print(f"before: sort {before['sort']}, {len(before['order'])} apps, "
              f"slides {before['slides']}, reloads {before['reloads']}", flush=True)
        t = time.monotonic()
        hbc.put_file(wii, f"{BURST}/boot.dol", b"\0" * 256)
        hbc.put_file(wii, f"{BURST}/meta.xml", b'<?xml version="1.0"?>\n<app version="1">\n'
                     b'  <name>zz HBC test burst</name>\n</app>\n')
        for i in range(10):
            hbc.put_file(wii, f"{BURST}/data/{i}.bin", os.urandom(4096))
        hbc.put_file(wii, f"{SECOND}/boot.dol", b"\0" * 256)
        print(f"12 files in {time.monotonic() - t:.1f} s", flush=True)
        time.sleep(3)
        after = hbc.status(wii)["app_list"]
        reloads = after["reloads"] - before["reloads"]
        slides = after["slides"] - before["slides"]
        shown = all(n in after["order"] for n in ("zz-hbctest-burst", "zz-hbctest-second"))
        print(f"after: {reloads} reload(s), {slides} slide(s), both test apps listed: {shown}",
              flush=True)
        if reloads != 1 or slides or not shown:
            failed.append("burst")
        hbc.set_order(wii, "sd:/apps", ["zz-hbctest-second", "zz-hbctest-burst"],
                      out=lambda line: print("  sort_id" + line, flush=True))
        rows = hbc.app_order(wii, "sd:/apps")
        print("Custom order:", [(r[0], r[1]) for r in rows], flush=True)
        names = [r[1] for r in rows]
        if names.index("zz-hbctest-second") > names.index("zz-hbctest-burst"):
            failed.append("order")
    except (OSError, hbc.HBCError, KeyError) as exc:
        print(f"FAIL: {exc}", flush=True)
        failed.append("error")
    finally:
        for path in (BURST, SECOND):
            try:
                hbc.remove_tree(wii, path, out=lambda _: None)
            except (OSError, hbc.HBCError) as exc:
                print(f"cleanup {path}: {exc}", flush=True)
        time.sleep(2)
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
    if failed:
        raise SystemExit(f"failed: {', '.join(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
