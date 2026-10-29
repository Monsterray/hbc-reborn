#!/usr/bin/env python3
"""HBC's Mii picker in Dolphin, driven as a person would: the retail WAD in a
throwaway NAND whose Mii Channel database holds 20 Miis (19 in slots 0-18,
one in slot 40), and a synthetic RFL_Res.dat on the SD card (no Nintendo
data: its heads draw nothing visible, so the coins look plain).

  1. The avatar stands with the first Mii (no favourites).
  2. A on the avatar opens the picker (`hbc.py buttons m`: the scripted
     pointer moves only up and down, and the avatar is at the left); it
     opens on the avatar's page, 2 pages of 15 coins, and draws all 20
     heads.
  3. B closes it; the avatar is as it was.
  4. The pad (tools/msgboard/dsu_pad.py, as tests/dolphin_meta.py drives
     it): + to page 2, D-pad right, A: the avatar is that Mii (slot 16).
  5. The pointer: the picker again, - back to page 1; a second on the
     middle coin of the second row shows its name; A chooses it.
  6. The choice outlives HBC: an app runs and exits (settings.xml is saved
     as HBC starts it), and HBC comes back with the same Mii.

With MII_REAL_BODIES (your own maleBody.brres and femaleBody.brres) the
Miis stand on the Mii Channel's bodies. With MII_REAL_EFFECTS, a folder with your own effect_nigaoe.breff and
.breft, step 4's new Mii also appears in a poof of the Mii Channel's dust.

Pictures of the picker go to OUTDIR (default: a temporary folder). Needs
the retail WAD (make -C channel) and tests/agent_app/agent_app.dol.

usage: tests/dolphin_mii_picker.py [OUTDIR]
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

SLOTS = list(range(19)) + [40]
NAMES = {slot: f"Mii {i + 1:02d}" for i, slot in enumerate(SLOTS)}
# The pointer's height on the picker's second row of coins (tests/dolphin_meta.py
# maps the stick's Y to it); X stays at the screen's middle, on the middle
# column.
ROW2 = 0.17
OFF = -1


def database():
    f = test_mii.empty_db()
    for i, slot in enumerate(SLOTS):
        raw = test_mii.encode(test_mii.mii_with(slot), name=NAMES[slot], hair_color=i % 8,
                              fav_color=i % 12, face_color=i % 6, hair_type=(i * 5) % 72)
        f[4 + slot * 74:4 + slot * 74 + 74] = raw
    struct.pack_into(">H", f, test_mii.DB_CRC, test_mii.crc(f[:test_mii.DB_CRC]))
    return bytes(f)


def avatar(wii, seconds=60):
    deadline = time.monotonic() + seconds
    while True:
        av = hbc.status(wii)["avatar"]
        if av["state"] != "loading" or time.monotonic() > deadline:
            return av
        time.sleep(0.5)


def picker(wii):
    return hbc.status(wii)["mii_picker"]


def wait_picker(wii, want_open, seconds=10):
    """Open with its coins up (it takes no presses before), or closed."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        p = picker(wii)
        if p["open"] == want_open and (not want_open or p["pages"]):
            return p
        time.sleep(0.3)
    return picker(wii)


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="hbc-miipick-"))
    out.mkdir(parents=True, exist_ok=True)
    files = {"Config/WiimoteNew.ini": PAD_MAP,
             "Wii/shared2/menu/FaceLib/RFL_DB.dat": database(),
             "Load/WiiSDSync/mii/RFL_Res.dat": test_miidraw.resource()}
    bodies = os.environ.get("MII_REAL_BODIES")
    if bodies:   # your own Mii Channel bodies (deleted with Dolphin)
        for name in ("maleBody.brres", "femaleBody.brres"):
            files[f"Load/WiiSDSync/mii/{name}"] = (pathlib.Path(bodies) / name).read_bytes()
    effects = os.environ.get("MII_REAL_EFFECTS")
    if effects:   # your own Mii Channel effects, for the poof (deleted with Dolphin)
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

    try:
        print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}", flush=True)
        pad.send(f"stick 0 {OFF}; wait 1")
        pad.done()
        av = avatar(wii)
        print(f"avatar: {av}", flush=True)
        check(av["state"] == "ready" and av["slot"] == 0 and av["mii"] == NAMES[0],
              "the avatar stands with the first Mii")

        # 2. A on the avatar: the picker, on page 1, every head drawn.
        hbc.send_buttons(wii, "m")
        p = wait_picker(wii, True)
        check(p["open"] and p["miis"] == 20 and p["pages"] == 2 and p["page"] == 1,
              f"the picker opens on page 1 of 2 with 20 Miis ({p})")
        deadline = time.monotonic() + 30
        while picker(wii)["heads"] < 20 and time.monotonic() < deadline:
            time.sleep(0.5)
        p = picker(wii)
        check(p["heads"] == 20, f"all 20 heads drawn ({p['heads']})")
        check(p["tip"] == 0, f"a second on the focused coin shows its name ({p['tip']})")
        shot(wii, out, "picker-page1")

        # 3. B: closed, nothing changed.
        pad.send("press Circle; wait 1.5")
        pad.done()
        p = wait_picker(wii, False)
        av = avatar(wii)
        check(not p["open"] and av["slot"] == 0, f"B closes it, the avatar as it was ({av['mii']})")

        # 4. The pad: + to page 2 (the focus stays on the first cell), right, A.
        hbc.send_buttons(wii, "m")
        p = wait_picker(wii, True)
        check(p["focus"] == 0, f"it opens on the avatar's coin ({p['focus']})")
        pad.send("wait 1; press R1; wait 1")
        pad.done()
        p = picker(wii)
        check(p["page"] == 2 and p["focus"] == 15,
              f"+ turns to page 2, the focus in the same cell ({p['page']}, {p['focus']})")
        shot(wii, out, "picker-page2")
        pad.send("press PadE; wait 0.5")
        pad.done()
        p = picker(wii)
        check(p["focus"] == 16, f"right: the next coin ({p['focus']})")
        pad.send("press Cross; wait 0.1")
        pad.done()
        if effects:   # the new Mii appears in a poof of dust
            most, deadline = 0, time.monotonic() + 5
            k = 0
            while time.monotonic() < deadline:
                most = max(most, hbc.status(wii)["playground"]["particles"])
                if k % 6 == 0:
                    shot(wii, out, f"poof-{k // 6}")
                k += 1
                time.sleep(0.05)
            check(most >= 10, f"the new Mii poofs in ({most} particles at most)")
        time.sleep(1.4)
        check(not wait_picker(wii, False)["open"], "A on a Mii closes the picker")
        av = avatar(wii)
        print(f"avatar: {av}", flush=True)
        check(av["state"] == "ready" and av["slot"] == 16 and av["mii"] == NAMES[16],
              f"the avatar is the Mii chosen with the pad (slot 16, {av['mii']})")
        shot(wii, out, "avatar-pad")

        # 5. The pointer: it opens on page 2 (slot 16's); - back to page 1,
        # and the pointer on the middle coin of the second row: index 7.
        hbc.send_buttons(wii, "m")
        p = wait_picker(wii, True)
        check(p["page"] == 2, f"the picker opens on the avatar's page ({p['page']})")
        pad.send(f"press L1; wait 0.7; stick 0 {ROW2}; wait 0.5")
        pad.done()
        tip = picker(wii)["tip"]
        time.sleep(1.2)
        p = picker(wii)
        check(tip == -1 and p["tip"] == 7, f"its name after a second on it, not before ({tip}, {p['tip']})")
        shot(wii, out, "picker-pointer")
        pad.send("press Cross; wait 1.5")
        pad.done()
        av = avatar(wii)
        print(f"avatar: {av}", flush=True)
        check(not picker(wii)["open"] and av["slot"] == 7 and av["mii"] == NAMES[7],
              f"the pointer chose the middle coin of row 2 (slot 7, {av['mii']})")
        pad.send(f"stick 0 {OFF}; wait 1")
        pad.done()

        # 6. Through an app's launch and back: the same Mii.
        before = hbc.lastlog(wii)
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), [], "miipick")
        time.sleep(5)
        hbc.exit_app(wii, 90)
        hbc.hbc_wait(wii, 90)
        av = avatar(wii)
        print(f"after the app: {av}", flush=True)
        check(av["state"] == "ready" and av["slot"] == 7, "the choice outlives HBC (settings.xml)")
        check(hbc.lastlog(wii) != before, "an app ran in between")
    except (OSError, hbc.HBCError, KeyError) as exc:
        check(False, f"error: {exc}")
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
