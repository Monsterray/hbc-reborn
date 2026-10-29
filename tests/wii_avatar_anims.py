#!/usr/bin/env python3
"""Every animation of the Mii Channel body, played by HBC's avatar on a
Wii, as a queue job: to choose which the avatar should use. This tree's HBC
(channel DOL, through the installed HBC), with the Wii's own Mii database
and the faces and bodies on its SD card (`hbc.py mii-faces` first when they
are missing). For each animation in HBCS "anims" (`hbc.py anim NAME`):

  anim_NAME.png   HBC's own pictures of the avatar while it plays, about
                  three a second, side by side
  tv_NAME.mp4     the TV through the capture card, from a second before
  anims.txt       each one's length, and the frames whose posed body had a
                  wild vertex (HBCS "bad": counted only in an HBC built with
                  EXTRA_CFLAGS=-DHBC_DIAG_GPU; always 0 otherwise)

Ends in the installed HBC.

queue: wiibench.py add --wii dev --name avatar-anims --cwd REPO --timeout 1800 -- <venv python> tests/wii_avatar_anims.py [NAME ...]
"""

import io
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageDraw

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
import wii_mboard as wm  # noqa: E402
from wii_avatar import avatar  # noqa: E402

# The avatar's corner of HBC's 640x480 picture: left of the app list, feet
# above the logo.
CROP = (0, 150, 240, 420)
LONGEST = 20     # seconds: an animation still playing after this is cut off
# Left out unless named: bodySlap_03 flings the body 2737 units (every other
# stays within 150), and HBC stopped answering the network while it played.
SKIP = {"bodySlap_03"}
TV_S = 12        # seconds of TV for each


def picture(wii):
    w, h, data = hbc.screen(wii)
    return Image.open(io.BytesIO(hbc.yuyv_png(w, h, data))).convert("RGB")


def sheet(frames, path):
    """The avatar's corner of each picture, side by side, 8 to a row, with
    its time."""
    if not frames:
        return
    cw, ch = CROP[2] - CROP[0], CROP[3] - CROP[1]
    cols = min(8, len(frames))
    rows = (len(frames) + cols - 1) // cols
    out = Image.new("RGB", (cols * cw, rows * ch), "black")
    draw = ImageDraw.Draw(out)
    for i, (t, im) in enumerate(frames):
        x, y = i % cols * cw, i // cols * ch
        out.paste(im.crop(CROP), (x, y))
        draw.text((x + 4, y + 4), f"{t:.1f} s", fill="yellow")
    out.save(path)


def main():
    wii = wm.bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-anims-")) / "hbc-anims.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    failed = []
    notes = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    try:
        hbc.send(wii, str(snap), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
        av = avatar(wii)
        if not av.get("anims"):
            ok, text = hbc.mii_faces(wii)
            print(f"no Mii Channel body yet; Save Mii faces: {text}", flush=True)
            time.sleep(3)
            av = avatar(wii)
        names = sys.argv[1:] or [n for n in av.get("anims", []) if n not in SKIP]
        check(av.get("state") == "ready" and bool(names),
              f"the avatar stands with the Mii Channel body ({len(av.get('anims', []))} animations)")
        for name in names:
            bad = hbc.status(wii)["avatar"]["bad"]
            tv = subprocess.Popen([sys.executable, wm.CAPTURE, "video", str(TV_S), "--no-audio",
                                   "--out", str(out / f"tv_{name}.mp4")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(3)   # the card warms up
            hbc.play_anim(wii, name)
            t0 = time.monotonic()
            frames, seen = [], False
            while time.monotonic() - t0 < LONGEST:
                shown = hbc.status(wii)["avatar"]["anim"]
                if shown == name:
                    seen = True
                elif seen or time.monotonic() - t0 > 2:
                    break
                frames.append((time.monotonic() - t0, picture(wii)))
            length = time.monotonic() - t0
            sheet(frames, out / f"anim_{name}.png")
            bad = hbc.status(wii)["avatar"]["bad"] - bad
            line = f"{name}: {'played' if seen else 'NOT PLAYED'}, about {length:.1f} s, " \
                   f"{len(frames)} pictures, {bad} wild frames"
            print(line, flush=True)
            notes.append(line)
            check(seen, f"{name} plays")
            try:
                tv.wait(TV_S + 30)
            except subprocess.TimeoutExpired:
                tv.kill()
            time.sleep(1)
        av = hbc.status(wii)["avatar"]
        check(16000 <= av["interval_us"] <= 17500, f"60 frames a second after ({av['interval_us']} us)")
    except (OSError, hbc.HBCError, KeyError) as exc:
        check(False, f"error: {exc!r}")
        try:
            wm.still(out, "tv_error")
        except Exception as still_exc:   # the capture card is a picture, not the test
            print(f"tv still: {still_exc}", flush=True)
    finally:
        (out / "anims.txt").write_text("\n".join(notes) + "\n")
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
        time.sleep(8)
        print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
        shutil.rmtree(snap.parent, ignore_errors=True)
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
