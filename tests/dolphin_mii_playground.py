#!/usr/bin/env python3
"""HBC's Mii playground in Dolphin, driven as a person would: the retail WAD
in a throwaway NAND with one Mii, a synthetic RFL_Res.dat on the SD card (no
Nintendo data: the head draws nothing visible), and, with MII_REAL_BODIES
set to a folder holding your own maleBody.brres and femaleBody.brres (from
the Mii Channel), those bodies on the throwaway SD card too, for the real
animations; with MII_REAL_EFFECTS, a folder with your own effect_nigaoe.breff
and .breft, the effects too. Nothing of Nintendo's is kept: the NAND and the card are deleted
with Dolphin.

  1. `hbc.py buttons p` opens it (HOME > HBC > Mii playground does the
     same); the Mii stands at the screen's middle on the floor.
  2. The menu: a button on the Moves tab plays that animation (with the
     bodies); Poses and Faces tabs, a pose and a face; the D-pad off the end
     of a page turns it, and the page number says so. + and - resize the Mii.
  3. The pointer (scripted: it moves only up and down, at the screen's
     middle, where the Mii is): A and B held on the Mii pick it up, and it
     follows the pointer up; let go, it falls and lands on its feet
     (bodyGetDown); let go near the top of the screen, it lands hard
     (bodySlip_01). It cannot be
     carried down over the buttons: the floor is above them.
  4. B goes back to the app list; the avatar is beside it again.

Pictures go to OUTDIR (default: a temporary folder). Needs the retail WAD
(make -C channel).

usage: tests/dolphin_mii_playground.py [OUTDIR]
"""

import os
import pathlib
import struct
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
import test_mii  # noqa: E402
import test_miidraw  # noqa: E402
from dolphin_meta import PAD_MAP, PAD_SETTINGS, Pad, shot  # noqa: E402

OFF = -1
# The pointer's height (tests/dolphin_meta.py maps the stick's Y to it): on
# the Mii's chest, standing; then higher, and higher still.
CHEST = 0.1
UP = -0.35
TOP = -0.55
LOW = 0.4


def database():
    f = test_mii.empty_db()
    f[4:4 + 74] = test_mii.encode(test_mii.mii_with(0), name="Player")
    struct.pack_into(">H", f, test_mii.DB_CRC, test_mii.crc(f[:test_mii.DB_CRC]))
    return bytes(f)


def status(wii):
    s = hbc.status(wii)
    return s["playground"], s["avatar"]


def hard(lifted, floor=378, tall=260):
    """How a Mii lands, let go `lifted` over the floor: hard (bodySlip_01)
    with its head within 48 of the top of the screen (avatar.c PG_TOP)."""
    return "bodySlip_01" if floor - lifted - tall < 48 else "bodyGetDown"


def until(what, seconds, step=0.2):
    deadline = time.monotonic() + seconds
    while True:
        got = what()
        if got or time.monotonic() > deadline:
            return got
        time.sleep(step)


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="hbc-miiplay-"))
    out.mkdir(parents=True, exist_ok=True)
    files = {"Config/WiimoteNew.ini": PAD_MAP,
             "Wii/shared2/menu/FaceLib/RFL_DB.dat": database(),
             "Load/WiiSDSync/mii/RFL_Res.dat": test_miidraw.resource()}
    bodies = os.environ.get("MII_REAL_BODIES")
    if bodies:
        for name in ("maleBody.brres", "femaleBody.brres"):
            files[f"Load/WiiSDSync/mii/{name}"] = (pathlib.Path(bodies) / name).read_bytes()
    effects = os.environ.get("MII_REAL_EFFECTS")
    if effects:
        for name in ("effect_nigaoe.breff", "effect_nigaoe.breft"):
            files[f"Load/WiiSDSync/mii/{name}"] = (pathlib.Path(effects) / name).read_bytes()
    dolphin = dolphin_smoke.Dolphin(root / "channel/title/channel_retail.wad", sd=True,
                                    extra=PAD_SETTINGS, files=files)
    wii = dolphin.address
    failed = []
    pad = Pad()

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    def steps(s):
        pad.send(s)
        pad.done()

    def keys(k, wait=0.4):
        hbc.send_buttons(wii, k)
        time.sleep(wait)
        return status(wii)[0]

    try:
        print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}", flush=True)
        steps(f"stick 0 {OFF}; wait 1")
        av = until(lambda: (lambda a: a["state"] != "loading" and a)(hbc.status(wii)["avatar"]), 60)
        check(av and av["state"] == "ready", f"the avatar stands ({av})")
        anims = av.get("anims", []) if av else []
        print(f"animations: {anims}", flush=True)

        # 1. Open.
        hbc.send_buttons(wii, "p")
        pg = until(lambda: (lambda p: p["open"] and p)(status(wii)[0]), 10) or status(wii)[0]
        print(f"playground: {pg}", flush=True)
        check(pg["open"] and pg["tab"] == "Moves" and pg["focus"] == "Stop",
              "it opens on Moves, the focus on Stop")
        floor = pg["floor"]
        check(pg["y"] == floor and not pg["held"] and floor < 400,
              f"the Mii stands on the floor, over the buttons ({pg['x']}, {pg['y']}, floor {floor})")
        # + and -: its size.
        big = keys("++")["size"]
        shot(wii, out, "big")
        small = keys("-" * 12)["size"]
        shot(wii, out, "small")
        sizes = [keys("+")["size"] for _ in range(6)]
        print(f"sizes, one + at a time: {sizes}", flush=True)
        size = sizes[-1]
        check((big, small, size) == (12, 4, 10), f"+ and - resize it, 0.4 to 1.4 ({big}, {small}, {size})")
        time.sleep(1)
        shot(wii, out, "playground")

        # 2. The menu, pressed through HBC's own queue (exact, one a frame).
        if anims:
            pg = keys("r")
            name = pg["focus"]
            check(f"body{name}" in anims, f"right: the first animation ({name})")
            keys("a")
            av = hbc.status(wii)["avatar"]
            check(av["anim"] == f"body{name}", f"A plays it ({av['anim']})")
            time.sleep(0.5)
            shot(wii, out, f"anim-{name}")
            if effects:   # Catnap_00: the Mii Channel's zzz over the napping Mii
                time.sleep(1)
                pg = status(wii)[0]
                check(pg["effects"] and pg["particles"] > 0, f"its effect plays ({pg['particles']} particles)")
                shot(wii, out, f"fx-{name}")
            pg = keys("r" * pg["per_page"])
            check(pg["page"] == 1 and pg["pages"] > 1,
                  f"right off the end of the page: the next ({pg['page']} of {pg['pages']}, {pg['focus']})")
            shot(wii, out, "page-2")
        else:
            print("no MII_REAL_BODIES: only Stop on the Moves tab", flush=True)
        # Up to the tabs; right to Poses, A; down to Stand, right 4: Look up.
        pg = keys("ura")
        check(pg["tab"] == "Poses", f"the Poses tab ({pg['tab']})")
        pg = keys("drrrra", 0.8)
        check(pg["focus"] == "Look up" and pg["pose"] == 4, f"Look up ({pg['focus']}, {pg['pose']})")
        shot(wii, out, "pose-look-up")
        pg = keys("uradrrrrra", 0.8)
        check(pg["tab"] == "Faces" and pg["focus"] == "Surprised" and pg["expression"] == 4,
              f"Faces: Surprised ({pg['tab']}, {pg['focus']}, {pg['expression']})")
        shot(wii, out, "face-surprised")
        # Back to its own face and pose, so the grab shows its own.
        pg = keys("llllla" + "ula" + "da")
        check(pg["expression"] == -1 and pg["pose"] == 0, f"its own face and Stand ({pg['expression']}, {pg['pose']})")

        # 3. A+B on the Mii: up it goes, and down.
        steps(f"stick 0 {CHEST}; wait 0.5; down Cross; wait 0.1; down Circle; wait 0.4")
        pg, av = status(wii)
        check(pg["held"], f"A+B on the Mii picks it up ({pg})")
        steps(f"stick 0 {UP}; wait 0.6")
        pg, av = status(wii)
        lifted = floor - pg["y"]
        check(pg["held"] and lifted > 40, f"it follows the pointer up ({lifted} up)")
        first = lifted
        if anims:
            check(av["anim"] == "bodyWallow", f"it squirms while held ({av['anim']})")
        shot(wii, out, "held")
        steps("up Circle; up Cross; wait 1")
        pg, av = status(wii)
        check(not pg["held"] and not pg["falling"] and pg["y"] == floor,
              f"let go, it falls to the floor ({pg['y']})")
        check(pg["landed"] == hard(lifted), f"and lands ({pg['landed']}, from {lifted} up)")
        # Higher: it trips.
        steps(f"stick 0 {CHEST}; wait 0.5; down Circle; wait 0.1; down Cross; wait 0.4")
        steps(f"stick 0 {TOP}; wait 0.6")
        pg, av = status(wii)
        lifted = floor - pg["y"]
        check(pg["held"] and lifted >= first, f"B then A picks it up too ({lifted} up)")
        shot(wii, out, "held-high")
        steps("up Cross; wait 0.15")
        pg, av = status(wii)
        check(pg["falling"] or pg["y"] == floor, f"letting go of A drops it ({pg['y']})")
        steps("up Circle; wait 1.5")
        pg, av = status(wii)
        check(pg["landed"] == hard(lifted) and pg["open"],
              f"it lands, or from higher trips; B let go after A+B does not leave ({pg['landed']}, {pg['open']})")
        shot(wii, out, "landed-high")
        # Down over the buttons: it stands where it is let go.
        steps(f"stick 0 {CHEST}; wait 0.5; down Cross; wait 0.1; down Circle; wait 0.3")
        steps(f"stick 0 {LOW}; wait 0.6; up Circle; up Cross; wait 1")
        pg, av = status(wii)
        check(not pg["held"] and pg["y"] == floor,
              f"it cannot go below the floor, over the buttons ({pg['y']})")
        shot(wii, out, "low")
        av = hbc.status(wii)["avatar"]
        check(16000 <= av["interval_us"] <= 17500, f"60 frames a second ({av['interval_us']} us)")

        # 4. B: back to the list.
        steps(f"stick 0 {OFF}; wait 0.3; press Circle; wait 1.5")
        pg, av = status(wii)
        check(not pg["open"] and av["state"] == "ready" and av["anim"] in ("", "bodyWait"),
              f"B goes back; the avatar beside the list ({av['anim']})")
        shot(wii, out, "back")
    except (OSError, hbc.HBCError, KeyError, TypeError) as exc:
        check(False, f"error: {exc!r}")
    finally:
        pad.close()
        faults = dolphin.faults()
        dolphin.stop()
    check(not faults, f"no faults in Dolphin's log ({faults[:3]})")
    print(f"pictures in {out}")
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
