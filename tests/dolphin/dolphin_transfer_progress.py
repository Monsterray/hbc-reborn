#!/usr/bin/env python3
"""HBC's transfer line in Dolphin, with Options > Progress set to Circle: the
retail WAD in a throwaway NAND and an emulated SD card.

  1. Options (1) shows the Progress row; the pad goes up to it, picks
     Circle (up from Back through Grid edges, the buttons nearest Back), then
     down to Back and left to Ok.
  2. An upload of a hundred megabytes into sd:/apps/ridgerac/ is on screen
     as "Receiving ridgerac/data.bin" beside a progress circle (pictures
     during it), and the line is gone when it ends.
  3. The choice outlives HBC: an app runs and exits (settings.xml is saved
     as HBC starts it), and settings.xml in the NAND says progress_arc 1.

Pictures go to OUTDIR (default: a temporary folder). Needs the retail WAD
(make -C channel) and tests/apps/agent_app/agent_app.dol.

usage: tests/dolphin/dolphin_transfer_progress.py [OUTDIR]
"""

import os
import pathlib
import shutil
import sys
import tempfile
import threading
import time

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
from dolphin_meta import PAD_MAP, PAD_SETTINGS, Pad, shot  # noqa: E402

# Dolphin, drawing every frame to a file, runs slowly: the pad's steps wait
# longer than they would. Dolphin writes every frame it draws to its profile (hbc.py screen waits
# behind the upload, so it cannot picture one in progress).
DUMP = ["Dolphin.Movie.DumpFrames=True", "Graphics.Settings.DumpFramesAsImages=True",
        "Graphics.Settings.PNGCompressionLevel=1"]
UPLOAD = 160 * 1024 * 1024   # a dozen seconds in Dolphin, long enough to picture


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="hbc-progress-"))
    out.mkdir(parents=True, exist_ok=True)
    dolphin = dolphin_smoke.Dolphin(root / "channel/title/channel_retail.wad", sd=True,
                                    extra=PAD_SETTINGS + DUMP, files={"Config/WiimoteNew.ini": PAD_MAP})
    wii = dolphin.address
    failed = []
    pad = Pad()

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    try:
        print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}", flush=True)
        time.sleep(3)

        # 1. Options: up from Back past App badges and Grid edges to the
        # Progress row (Circle, nearest Back), A, down to Back, left to Ok, A.
        pad.send("stick 0 -1; wait 1")   # the pointer off the screen
        pad.done()
        hbc.send_buttons(wii, "1")
        time.sleep(3)
        shot(wii, out, "options-percent")
        pad.send("press PadN; wait 2; press PadN; wait 2; press PadN; wait 2; press Cross; wait 2")
        pad.done()
        shot(wii, out, "options-circle")
        pad.send("press PadS; wait 2; press PadS; wait 2; press PadS; wait 2; press PadW; wait 2; "
                 "press Cross; wait 3")
        pad.done()
        shot(wii, out, "menu")

        # 2. An upload, on screen while it runs.
        data = os.urandom(UPLOAD)
        result = {}

        def upload():
            try:
                hbc.put_file(wii, "sd:/apps/ridgerac/data.bin", data, level=0)
                result["ok"] = True
            except (OSError, hbc.HBCError) as exc:
                result["error"] = str(exc)

        thread = threading.Thread(target=upload)
        began = time.time()
        thread.start()
        thread.join(300)
        ended = time.time()
        check(result.get("ok"), f"the upload finished ({result}, {ended - began:.1f} s)")
        time.sleep(1.5)
        # Six frames spread over the upload, and one after it.
        frames = sorted((f for f in (dolphin.profile / "Dump/Frames").rglob("*.png")
                         if began < f.stat().st_mtime <= ended + 2), key=lambda f: f.stat().st_mtime)
        check(len(frames) >= 12, f"Dolphin dumped frames during the upload ({len(frames)})")
        for i in range(6 if frames else 0):
            shutil.copy(frames[len(frames) * (i + 1) // 8], out / f"upload-{i}.png")
        shot(wii, out, "after-upload")

        # 3. Through an app's launch and back: the choice is in settings.xml.
        before = hbc.lastlog(wii)
        hbc.send(wii, str(root / "tests/apps/agent_app/agent_app.dol"), [], "progress")
        time.sleep(10)
        hbc.exit_app(wii, 240)
        hbc.hbc_wait(wii, 240)
        check(hbc.lastlog(wii) != before, "an app ran in between")
        found = list((dolphin.profile / "Wii").rglob("settings.xml"))
        text = found[0].read_text() if found else ""
        check("<progress_arc>1</progress_arc>" in text, f"settings.xml keeps the circle ({text.strip()[:120]})")
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
