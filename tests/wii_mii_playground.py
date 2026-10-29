#!/usr/bin/env python3
"""HBC's Mii playground on a Wii, as a queue job: this tree's HBC (channel
DOL, through the installed HBC), with the Wii's own Mii database and the
faces and bodies on its SD card (`hbc.py mii-faces` first when they are
missing). Nobody holds the Wii Remote, so it is pressed from the PC
(`hbc.py buttons`):

  p              the playground opens (HOME > HBC > Mii playground does the
                 same); the Mii on the floor at the screen's middle
  r x 6, a       Moves: Slap_00, which throws the Mii up out of its
                 box: drawn whole, over the whole screen; it lands in the
                 Mii Channel's smoke
  + + - - -      bigger, and smaller again
  u r r a d      the Faces tab, then each face in turn: r a
  u l a a        Poses: Stand; Talk, Catnap_00 and the sneeze (bodysnart_01)
                 played (`hbc.py anim`), their effects on the head
  d r x 8, a     Icon: the head alone, talking with its balloons
  b              back to the app list

HBC keeps 60 frames a second throughout. Pictures go to the job's run
folder: face_NAME.png and anim_*.png (HBC's own picture), tv_*.png and
tv_slap.mp4 (the capture card). Ends in the installed HBC; with --stay, in
this tree's, on the app list, for someone to use.

queue: wiibench.py add --wii dev --name mii-playground --cwd REPO --timeout 900 -- <python> tests/wii_mii_playground.py [--stay]
"""

import os
import pathlib
import shutil
import subprocess
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

FACES = ["Normal", "Smile", "Angry", "Sad", "Surprised", "Blink", "Talking"]


def playground(wii):
    return hbc.status(wii)["playground"]


def picture(wii, out, name):
    w, h, data = hbc.screen(wii)
    (out / f"{name}.png").write_bytes(hbc.yuyv_png(w, h, data))


def main():
    wii = wm.bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-miiplay-")) / "hbc-miiplay.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    failed = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    def keys(k, wait=0.4):
        hbc.send_buttons(wii, k)
        time.sleep(wait)
        return playground(wii)

    try:
        hbc.send(wii, str(snap), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
        # Save Mii faces: the bodies and (1.12.2) the effects on the SD card;
        # the avatar loads again with them.
        ok, text = hbc.mii_faces(wii)
        print(f"Save Mii faces: {text}", flush=True)
        time.sleep(4)
        av = avatar(wii)
        check(av.get("state") == "ready" and bool(av.get("anims")),
              f"the avatar stands with the Mii Channel body ({av.get('mii')})")

        pg = keys("p", 1.5)
        print(f"playground: {pg}", flush=True)
        check(pg["open"] and pg["focus"] == "Stop" and pg["y"] == pg["floor"] < 400,
              "the playground opens, the Mii on the floor over the buttons")
        picture(wii, out, "playground")
        wm.still(out, "tv_playground")

        # Slap_00: up out of its box and back down, drawn whole.
        pg = keys("rrrrrr", 1)
        check(pg["focus"] == "Slap_00", f"six right: Slap_00 ({pg['focus']})")
        tv = subprocess.Popen([sys.executable, wm.CAPTURE, "video", "8", "--no-audio",
                               "--out", str(out / "tv_slap.mp4")],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(3)   # the card warms up
        played = playground(wii)["played"]
        keys("a", 0.1)
        check(hbc.status(wii)["avatar"]["anim"] == "bodySlap_00", "A plays it")   # 1.6 s long
        for i in range(4):
            picture(wii, out, f"anim_slap_{i}")
        # It lands at frame 30 and bounces at 60: the Mii Channel's smoke,
        # twice (each puff lasts under half a second: counted, not seen).
        pg = playground(wii)
        check(pg["effects"] and pg["played"] - played >= 2, f"the landing's smoke ({pg['played'] - played} played)")
        tv.wait(60)

        # Slap_03: dropped from 2660 up, drawn all the way down (the depth
        # range follows it), HBC answering throughout.
        played = playground(wii)["played"]
        hbc.play_anim(wii, "bodySlap_03")
        for i in range(5):
            time.sleep(0.35)
            picture(wii, out, f"anim_slap3_{i}")
        time.sleep(1)
        pg = playground(wii)
        check(hbc.status(wii)["avatar"]["anim"] in ("bodySlap_03", "bodyWait") and pg["played"] - played >= 2,
              f"Slap_03 plays and lands in dust ({pg['played'] - played} played)")

        # + and -: its size.
        sizes = [keys(k)["size"] for k in "++---"]
        check(sizes == [11, 12, 11, 10, 9], f"+ and - resize it ({sizes})")
        picture(wii, out, "smaller")
        keys("+")

        # Each face.
        pg = keys("urra", 0.8)
        check(pg["tab"] == "Faces", f"the Faces tab ({pg['tab']})")
        pg = keys("d")
        for face in FACES:
            pg = keys("ra", 1)
            check(pg["focus"] == face and pg["expression"] == FACES.index(face),
                  f"{face} ({pg['focus']}, {pg['expression']})")
            picture(wii, out, f"face_{face.replace(' ', '_')}")
        wm.still(out, "tv_face")

        # The effects on the head: talk's balloons and the zzz over it, the
        # sneeze in front of the face; then the same on its icon.
        keys("ulaa", 0.8)   # Poses, back to Stand
        for anim, when in (("bodyTalk", 1.0), ("bodyCatnap_00", 2.0), ("bodysnart_01", 0.3)):
            played = playground(wii)["played"]
            hbc.play_anim(wii, anim)
            time.sleep(when)
            pg = playground(wii)
            check(pg["played"] > played, f"{anim}: its effect on the head ({pg['particles']} particles)")
            picture(wii, out, f"fx_{anim}")
            wm.still(out, f"tv_fx_{anim}")
        pg = keys("d" + "r" * 8 + "a", 1)
        check(pg["focus"] == "Icon" and pg["pose"] == 8, f"Icon: the head alone ({pg['focus']}, {pg['pose']})")
        played = pg["played"]
        hbc.play_anim(wii, "bodyTalk")
        time.sleep(1)
        pg = playground(wii)
        check(pg["played"] > played, f"talking as an icon ({pg['particles']} particles)")
        picture(wii, out, "icon_talk")
        wm.still(out, "tv_icon_talk")
        hbc.play_anim(wii, "bodyWait")
        keys("l" * 8 + "a", 0.5)   # Stand
        av = hbc.status(wii)["avatar"]
        check(16000 <= av["interval_us"] <= 17500, f"60 frames a second ({av['interval_us']} us)")

        pg = keys("b", 1.5)
        check(not pg["open"] and hbc.status(wii)["avatar"]["state"] == "ready",
              "B goes back to the app list, the avatar beside it")
        picture(wii, out, "back")
    except (OSError, hbc.HBCError, KeyError, subprocess.TimeoutExpired) as exc:
        check(False, f"error: {exc!r}")
        try:
            wm.still(out, "tv_error")
        except Exception as still_exc:   # the capture card is a picture, not the test
            print(f"tv still: {still_exc}", flush=True)
    finally:
        if "--stay" in sys.argv:
            print(f"left running: HBC {hbc.version(wii)}", flush=True)
        else:
            hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
            time.sleep(8)
            print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
        shutil.rmtree(snap.parent, ignore_errors=True)
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
