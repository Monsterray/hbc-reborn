#!/usr/bin/env python3
"""Options > Grid edges in Dolphin (PR 7): the retail WAD in a throwaway NAND.

  1 u a      Options opens on Back; up is Turn page (the nearest button in
             the row above); A picks it
  d l a      down to Back, left to Ok, A: Options closes, Turn page in use
  1 ... b    Options again shows Turn page bright; B leaves it

HBCS says where the focus is before every A. Pictures of the page go to
OUTDIR (default: a temporary folder). Needs the retail WAD (make -C channel).

usage: tests/dolphin_options_edges.py [OUTDIR]
"""

import pathlib
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
from dolphin_meta import PAD_MAP, PAD_SETTINGS, Pad, shot  # noqa: E402


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="hbc-options-"))
    out.mkdir(parents=True, exist_ok=True)
    dolphin = dolphin_smoke.Dolphin(root / "channel/title/channel_retail.wad", sd=True,
                                    extra=PAD_SETTINGS, files={"Config/WiimoteNew.ini": PAD_MAP})
    wii = dolphin.address
    pad = Pad()
    failed = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    def options():
        return hbc.status(wii)["options"]

    def step(keys, want, what):
        pad.send(keys)
        pad.done()
        o = options()
        check(all(o.get(k) == v for k, v in want.items()), f"{what} ({o})")
        return all(o.get(k) == v for k, v in want.items())

    try:
        print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}", flush=True)
        time.sleep(3)
        pad.send("stick 0 -1; wait 1")   # the pointer off the screen
        pad.done()
        check(options()["edges"] == "wrap", "Wrap rows is the default")
        hbc.send_buttons(wii, "1")
        time.sleep(3)
        shot(wii, out, "options")
        check(options()["focus"] == "back", f"Options opens on Back ({options()})")
        if step("press PadN; wait 2", {"focus": "page"}, "up from Back: Turn page") and \
                step("press Cross; wait 2", {"edge_choice": "page"}, "A picks Turn page"):
            shot(wii, out, "options-page")
            if step("press PadS; wait 2; press PadW; wait 2", {"focus": "ok"}, "down, left: Ok"):
                step("press Cross; wait 3", {"open": False, "edges": "page"},
                     "Ok closes Options, Turn page in use")
        if options()["open"]:
            pad.send("press Circle; wait 2")   # B: never leave it open
            pad.done()
        hbc.send_buttons(wii, "1")
        time.sleep(3)
        check(options()["edge_choice"] == "page", f"Options again shows Turn page ({options()})")
        shot(wii, out, "options-again")
        pad.send("press Circle; wait 2")
        pad.done()
        check(not options()["open"], "B closes Options")
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
