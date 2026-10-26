#!/usr/bin/env python3
"""HBC's avatar on a Wii, as a queue job: this tree's HBC (channel DOL,
through the installed HBC), with the Wii's own Mii database and the faces
on its SD card (sd:/mii/RFL_Res.dat: HOME > HBC > Save Mii faces, or
`hbc.py mii-faces`, which this runs first when they are missing). Checks
HBCS "avatar" (ready; the CPU time it takes; HBC at 60 frames a second
for a minute) and saves a series of TV pictures (`hbc.py screen`) to the job's
run folder; ends in the installed HBC.

  --seconds N   how long to watch it (default 60)
  --built       with the library's own body: the Mii Channel bodies on the
                SD card are copied to the run folder, deleted for the run
                and put back after (an installed HBC before 1.10.14 cannot
                rename)

Every 10 s, and when HBC stops answering, the TV through the bench's
capture card goes to the run folder too (tv_*.png), to tell a crash screen
from a hang.

queue: wiibench.py add --wii dev --name avatar --cwd REPO --timeout 900 -- <python> tests/wii_avatar.py [--seconds N] [--built]
"""

import argparse
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


BODIES = ("sd:/mii/maleBody.brres", "sd:/mii/femaleBody.brres")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--built", action="store_true")
    ap.add_argument("--dol", help="another channel DOL than this tree's (a diagnostic build), of this tree's version")
    opts = ap.parse_args()
    wii = wm.bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-avatar-")) / "hbc-avatar.dol"
    shutil.copy(opts.dol or root / "channel/channelapp/channelapp-channel.dol", snap)
    failed = []
    moved = {}
    if opts.built:
        for body in BODIES:
            try:
                moved[body] = hbc.get_file(wii, body)
                (out / body.rsplit("/", 1)[1]).write_bytes(moved[body])
                hbc.remove_one(wii, body)
                print(f"{body}: set aside ({len(moved[body])} bytes)", flush=True)
            except (OSError, hbc.HBCError) as exc:
                print(f"{body}: not set aside ({exc})", flush=True)
                moved.pop(body, None)
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
            # Watch it: frames keep coming at 60 a second, HBC answers.
            # (Before 1.10.21's display lists it overran the GX FIFO here.)
            last, at = av["frames"], time.monotonic()
            for k in range(opts.seconds // 5):
                time.sleep(5)
                if k % 2 == 1:
                    wm.still(out, f"tv_{5 * (k + 1):04d}s")
                try:
                    av = hbc.status(wii)["avatar"]
                except (OSError, hbc.HBCError) as exc:
                    wm.still(out, f"tv_stopped_{5 * (k + 1):04d}s")
                    failed.append(f"HBC stopped answering after {5 * (k + 1)} s ({exc!r}): see tv_stopped_*.png")
                    break
                now = time.monotonic()
                rate = (av["frames"] - last) / (now - at)
                print(f"after {5 * (k + 1)} s: {av}, {rate:.1f} frames a second", flush=True)
                if not 16000 <= av["interval_us"] <= 17500:
                    failed.append(f"frames every {av['interval_us']} us: not 60 a second")
                    break
                # The status request's own time falls between the samples.
                if not 45 <= rate <= 75:   # a slow status reply moves a sample; interval_us is exact
                    failed.append(f"{rate:.1f} frames a second")
                    break
                last, at = av["frames"], now
            for k in range(10):
                w, h, data = hbc.screen(wii)
                (out / f"avatar_{k}.png").write_bytes(hbc.yuyv_png(w, h, data))
                time.sleep(0.7)
            print(f"10 pictures in {out}", flush=True)
    finally:
        if failed and "stopped answering" in failed[-1]:
            raise SystemExit("FAIL " + failed[-1] + "; the Wii needs a power cycle")
        for body, data in moved.items():
            hbc.put_file(wii, body, data)
            print(f"{body}: put back", flush=True)
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
