#!/usr/bin/env python3
"""HBC's HOME > Exit > System Menu in Dolphin: the installed WAD, a throwaway
NAND with no System Menu. Each A is pressed only after HBCS's overlay_ui
focus says it lands where meant. Passes when HBC leaves with no exception in
Dolphin's log (Dolphin then has no menu to start, and stops or idles).

With MII_FILES (a folder holding RFL_DB.dat, RFL_Res.dat and the two
bodies, all the user's own) the avatar stands first, so its shutdown runs.

usage: tests/dolphin_exit_menu.py [WAD]"""
import os
import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402

BAR_EXIT, EX_SYS = 102, 111


def focus(wii):
    ui = hbc.status(wii).get("overlay_ui") or {}
    return ui.get("focus")


def main():
    image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else root / "channel/title/channel_retail.wad").resolve()
    mii = os.environ.get("MII_FILES")
    if mii:   # the profile made here first, with the Miis on its NAND and SD card
        made = pathlib.Path(tempfile.mkdtemp(prefix="hbc-dolphin-"))
        facelib = made / "Wii/shared2/menu/FaceLib"
        sd = made / "Load/WiiSDSync/mii"
        facelib.mkdir(parents=True)
        sd.mkdir(parents=True)
        shutil.copy(pathlib.Path(mii) / "RFL_DB.dat", facelib)
        for name in ("RFL_Res.dat", "maleBody.brres", "femaleBody.brres"):
            shutil.copy(pathlib.Path(mii) / name, sd)
        dolphin_smoke.tempfile.mkdtemp = lambda prefix="": str(made)
    strict = ["Dolphin.Core.AccurateCPUCache=True"]   # a missing cache flush shows, as on a Wii
    run = dolphin_smoke.Dolphin(image, sd=True, mmu=True, extra=strict)
    result = 1
    try:
        dolphin_smoke.wait_version(run, 90)
        wii = run.address
        print(f"HBC {dolphin_smoke.expected} up; letting the avatar start", flush=True)
        time.sleep(15)
        print("avatar:", hbc.status(wii).get("avatar"), flush=True)
        if os.environ.get("SD_WRITES"):   # as before the bench Wii's crash: WADs onto the card
            blob = (image.read_bytes() * 2)[:2_055_040]
            for name in ("a.wad", "b.wad", "c.wad"):
                hbc.put_file(wii, "sd:/wad/" + name, blob)
            hbc.file_request(wii, "D", "sd:/wad/a.wad")
            print("wrote three WADs to sd:/wad and removed one", flush=True)
        hbc.send_keys(wii, "h")
        time.sleep(2)
        assert focus(wii) == BAR_EXIT, f"HOME bar focus {focus(wii)}, wanted Exit"
        hbc.send_keys(wii, "a")
        time.sleep(1.5)
        f = focus(wii)
        print("after Exit, focus", f, flush=True)
        if f != EX_SYS:
            hbc.send_keys(wii, "d")
            time.sleep(1)
            f = focus(wii)
        assert f == EX_SYS, f"Exit row focus {f}, wanted System Menu ({EX_SYS})"
        hbc.send_keys(wii, "a")
        print("pressed System Menu", flush=True)
        left = False
        for _ in range(30):   # HBC stops answering once it has left
            time.sleep(1)
            try:
                st = hbc.status(wii)
                print("still up:", (st.get("overlay_ui") or {}), flush=True)
            except (OSError, hbc.HBCError):
                left = True
                break
        assert left, "HBC never left"
        time.sleep(10)
        faults = run.faults()
        for line in faults[:30]:
            print("log:", line)
        result = 1 if faults else 0
        print("PASS" if not faults else "FAIL: an exception on the way out")
    except (AssertionError, OSError, hbc.HBCError) as exc:
        print(f"FAIL: {exc!r}")
    finally:
        run.stop()
    print(f"profile: {run.profile}")
    sys.exit(result)


if __name__ == "__main__":
    main()
