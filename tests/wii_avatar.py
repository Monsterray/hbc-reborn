#!/usr/bin/env python3
"""HBC's avatar on a Wii, as a queue job: this tree's HBC (channel DOL,
through the installed HBC), with the Wii's own Mii database and the faces
on its SD card (sd:/mii/RFL_Res.dat: HOME > HBC > Save Mii faces, or
`hbc.py mii-faces`, which this runs first when they are missing). Checks
HBCS "avatar" (ready; the CPU time it takes; HBC at 60 frames a second
for a minute) and saves a series of TV pictures (`hbc.py screen`) to the job's
run folder; ends in the installed HBC.

queue: wiibench.py add --wii dev --name avatar --cwd REPO --timeout 900 -- <python> tests/wii_avatar.py
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
import wii_mboard as wm  # noqa: E402


def avatar(wii, seconds=60):
    """HBCS "avatar" once it has stopped loading."""
    deadline = time.monotonic() + seconds
    while True:
        av = hbc.status(wii).get("avatar") or {}
        if av.get("state") != "loading" or time.monotonic() > deadline:
            return av
        time.sleep(1)


def main():
    wii = wm.bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-avatar-")) / "hbc-avatar.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    failed = []
    try:
        hbc.send(wii, str(snap), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
        av = avatar(wii)
        if av.get("state") == "none":
            ok, text = hbc.mii_faces(wii)
            print(f"no avatar yet; Save Mii faces: {text}", flush=True)
            time.sleep(2)
            av = avatar(wii)
        print(f"avatar: {av}", flush=True)
        if av.get("state") != "ready":
            failed.append(f"avatar {av.get('state')}")
        else:
            # A minute of it: frames keep coming at 60 a second, HBC answers.
            # (Before 1.10.21's display lists it overran the GX FIFO here.)
            last, at = av["frames"], time.monotonic()
            for k in range(12):
                time.sleep(5)
                av = hbc.status(wii)["avatar"]
                now = time.monotonic()
                rate = (av["frames"] - last) / (now - at)
                print(f"after {5 * (k + 1)} s: {av}, {rate:.1f} frames a second", flush=True)
                if not 16000 <= av["interval_us"] <= 17500:
                    failed.append(f"frames every {av['interval_us']} us: not 60 a second")
                    break
                # The status request's own time falls between the samples.
                if not 50 <= rate <= 63:
                    failed.append(f"{rate:.1f} frames a second")
                    break
                last, at = av["frames"], now
            for k in range(10):
                w, h, data = hbc.screen(wii)
                (out / f"avatar_{k}.png").write_bytes(hbc.yuyv_png(w, h, data))
                time.sleep(0.7)
            print(f"10 pictures in {out}", flush=True)
    finally:
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
        time.sleep(8)
        print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
        shutil.rmtree(snap.parent, ignore_errors=True)
    for f in failed:
        print("FAIL", f)
    if failed:
        raise SystemExit(1)
    print("PASS")


if __name__ == "__main__":
    main()
