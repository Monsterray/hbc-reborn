#!/usr/bin/env python3
"""The focus glow on an app in the list and the grid, in Dolphin.

The retail WAD in a throwaway NAND, four test apps. In each layout the D-pad
moves the focus and a picture is taken
  rest   two seconds after the move: the glow is all the way in
  mid    straight after the move: the fade has begun (the picture request
         costs a few frames, so how far it got varies)
  other  the row (cell) that lost the focus, a moment later
The glow follows the button's outline, so just outside a focused button's left
edge the picture is brighter than at the same place beside an unfocused one.
That, and the fade ending (two pictures of the rest, a second apart, equal)
are checked; the pictures go to OUTDIR (default: a temporary folder).

usage: tests/dolphin/dolphin_glow.py [OUTDIR] [WAD]
"""

import pathlib
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
from dolphin_meta import PAD_MAP, PAD_SETTINGS, Pad  # noqa: E402

APPS = ("1 First", "2 Second", "3 Third", "4 Fourth")
LEFT = 104        # the list's and the grid's left edge, in the 640 wide picture
ROW_Y = 64        # the first row's top; a row is 64 high


def meta_xml(name):
    return (f'<?xml version="1.0"?><app><name>{name}</name>'
            '<short_description>A test app</short_description></app>').encode()


def band(data, width, x0, x1, y0, y1):
    """Mean luma of a rectangle of a YUYV picture."""
    total = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            total += data[(y * width + x) * 2]
            n += 1
    return total / n


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="hbc-glow-"))
    wad = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else root / "channel/title/channel_retail.wad"
    out.mkdir(parents=True, exist_ok=True)
    dolphin = dolphin_smoke.Dolphin(wad, sd=True, extra=PAD_SETTINGS,
                                    files={"Config/WiimoteNew.ini": PAD_MAP})
    wii = dolphin.address
    pad = Pad()
    failed = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)
        return ok

    def picture(name):
        width, height, data = hbc.screen(wii)
        (out / f"{name}.png").write_bytes(hbc.yuyv_png(width, height, data))
        return width, data

    def halo(width, data, row, grid=False):
        """Luma just left of a button's edge, beside it: the band is 8 wide and
        most of the button high, so a bubble crossing it changes little."""
        y = ROW_Y + 64 * row
        return band(data, width, LEFT - 9, LEFT - 3, y + 12, y + 52)

    try:
        print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}", flush=True)
        deadline = time.monotonic() + 30
        while hbc.status(wii)["device"] != "sd":
            if time.monotonic() > deadline:
                raise SystemExit("the SD card did not mount")
            time.sleep(1)
        for folder in APPS:
            hbc.put_file(wii, f"sd:/apps/{folder}/boot.dol", b"\0" * 64)
            hbc.put_file(wii, f"sd:/apps/{folder}/meta.xml", meta_xml(folder))
        time.sleep(3)
        pad.send("stick 0 -1; wait 1")   # the pointer off the screen
        pad.done()

        for layout in ("list", "grid"):
            # The list: the first press focuses the first app, then down to
            # the second. The grid (four in a row here) goes right.
            step = "PadS" if layout == "list" else "PadE"
            # (The layout switch kept the list's focus on the second app.)
            pad.send("press PadS; wait 2" if layout == "list" else "press PadW; wait 2")
            pad.done()
            width, rest = picture(f"{layout}-rest")
            pad.send(f"press {step}")
            pad.done()
            picture(f"{layout}-mid")
            time.sleep(2)
            _, moved = picture(f"{layout}-moved")
            # First app focused in rest, the second in moved; the third is
            # never focused. Measured beside the left edge (list) or above
            # the top edge (grid) of the first, second and third.
            if layout == "list":
                def near(d, i):
                    y = ROW_Y + 64 * i
                    return band(d, width, 148, 157, y + 10, y + 54)
                first, second, third = 0, 1, 2
            else:
                def near(d, i):
                    x = 104 + 108 * i
                    return band(d, width, x + 12, x + 96, 56, 63)
                first, second, third = 0, 1, 2
            # The same place in both pictures, so the background's own
            # gradient cancels; the third is the control (bubbles drift).
            gone = near(rest, first) - near(moved, first)
            check(gone > 3, f"{layout}: the halo left the app that lost the focus ({gone:+.1f} luma)")
            came = near(moved, second) - near(rest, second)
            check(came > 3, f"{layout}: and came to the next ({came:+.1f})")
            still = near(moved, third) - near(rest, third)
            check(abs(still) < 4, f"{layout}: the third app's surroundings did not change ({still:+.1f})")
            pad.send("press Triangle; wait 2")   # the other layout
            pad.done()
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
