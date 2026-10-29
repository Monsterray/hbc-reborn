#!/usr/bin/env python3
"""HBC's Mii picker on a Wii, as a queue job: this tree's HBC (channel DOL,
through the installed HBC), with the Wii's own Mii database and the faces
on its SD card (sd:/mii/RFL_Res.dat; `hbc.py mii-faces` first when they
are missing). Nobody holds the Wii Remote, so HBC's menu is pressed from
the PC (`hbc.py buttons`):

  m          A on the avatar: the picker opens on the avatar's page, with
             every Mii in the database; all their heads get drawn
  + ... +    every page in turn, back to the first: a picture of each
  (a second)  the focused coin shows its Mii's name
  r a        another Mii: the avatar becomes it

HBC keeps 60 frames a second and keeps answering throughout. Pictures go
to the job's run folder: picker_page_N.png (HBC's own picture) and tv_*.png
(the capture card). Reads the database only; a channel DOL boot saves no
settings, so the installed HBC keeps its own avatar. Ends in the installed
HBC.

queue: wiibench.py add --wii dev --name mii-picker --cwd REPO --timeout 900 -- <python> tests/wii_mii_picker.py
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
from wii_avatar import avatar  # noqa: E402


def picker(wii):
    return hbc.status(wii)["mii_picker"]


def until(what, seconds, step=0.3):
    deadline = time.monotonic() + seconds
    while True:
        got = what()
        if got or time.monotonic() > deadline:
            return got
        time.sleep(step)


def picture(wii, out, name):
    w, h, data = hbc.screen(wii)
    (out / f"{name}.png").write_bytes(hbc.yuyv_png(w, h, data))


def main():
    wii = wm.bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-miipick-")) / "hbc-miipick.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    failed = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

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
        check(av.get("state") == "ready", f"the avatar stands ({av.get('state')})")
        before = av.get("slot")

        hbc.send_buttons(wii, "m")
        t0 = time.monotonic()
        p = until(lambda: picker(wii)["open"] and picker(wii)["pages"] and picker(wii), 15)
        check(bool(p), f"the picker opens ({picker(wii)})")
        if p:
            print(f"picker: {p}", flush=True)
            p = until(lambda: (lambda q: q["heads"] >= q["miis"] and q)(picker(wii)), 120, 1)
            p = p or picker(wii)
            check(p["heads"] == p["miis"],
                  f"every head drawn: {p['heads']} of {p['miis']} in {time.monotonic() - t0:.1f} s")
            av = hbc.status(wii)["avatar"]
            check(16000 <= av["interval_us"] <= 17500,
                  f"60 frames a second with the picker open ({av['interval_us']} us)")
            time.sleep(1.3)
            p = picker(wii)
            check(p["tip"] == p["focus"] >= 0,
                  f"a second on the focused coin shows its name ({p['tip']}, {p['focus']})")
            first = p["page"]
            for k in range(p["pages"]):
                picture(wii, out, f"picker_page_{p['page']}")
                if k == 0:
                    wm.still(out, "tv_picker")
                hbc.send_buttons(wii, "+")
                time.sleep(0.8)
                p = picker(wii)
            check(p["page"] == first, f"+ turned through all {p['pages']} pages and back ({p['page']})")

            # Another Mii than the avatar's: right (from the page's last, on
            # to the next page's first).
            hbc.send_buttons(wii, "r")
            time.sleep(0.5)
            hbc.send_buttons(wii, "a")
            closed = until(lambda: not picker(wii)["open"], 10)
            check(bool(closed), "A on a Mii closes the picker")
            av = until(lambda: (lambda a: a["state"] == "ready" and a)(hbc.status(wii)["avatar"]), 30)
            av = av or hbc.status(wii)["avatar"]
            print(f"avatar now: {av}", flush=True)
            check(av["state"] == "ready" and (p["miis"] < 2 or av["slot"] != before),
                  f"the avatar is the Mii chosen (slot {before} -> {av['slot']}, {av['mii']})")
            time.sleep(2)
            picture(wii, out, "avatar_chosen")
            wm.still(out, "tv_avatar_chosen")
    except (OSError, hbc.HBCError, KeyError) as exc:
        check(False, f"error: {exc!r}")
        try:
            wm.still(out, "tv_error")
        except Exception as still_exc:   # the capture card is a picture, not the test
            print(f"tv still: {still_exc}", flush=True)
    finally:
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
        time.sleep(8)
        print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
        shutil.rmtree(snap.parent, ignore_errors=True)
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
