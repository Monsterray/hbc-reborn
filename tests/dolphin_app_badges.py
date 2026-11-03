#!/usr/bin/env python3
"""Options > App badges in Dolphin: the retail WAD in a throwaway NAND.

  1 u a d l a   Options opens on Back; up is App badges: On; A picks it;
                down to Back, left to Ok, A
  apps          three test apps: one with an old-style run marker, one never
                run, one changed after its marker was made
  list, grid    a red badge (never run) and a gold one (changed) in each
  launch        the changed app from the list; it runs and HBC comes back:
                its gold badge is gone, the red one stays

HBCS says where the focus is before every A. Pictures go to OUTDIR (default:
a temporary folder). Needs the retail WAD (make -C channel) and
tests/agent_app/agent_app.dol.

usage: tests/dolphin_app_badges.py [OUTDIR]
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
from dolphin_meta import PAD_MAP, PAD_SETTINGS, Pad, launch, shot  # noqa: E402

APPS = ("1 Already run", "2 Never run", "3 Updated app")


def meta_xml(name, description):
    return (f'<?xml version="1.0"?><app><name>{name}</name>'
            f'<short_description>{description}</short_description>'
            '<arguments><arg>meta</arg></arguments></app>').encode()


def color_counts(data):
    """Red and gold pixels in a YUYV screen."""
    red = gold = 0
    for i in range(0, len(data), 4):
        y0, cb, y1, cr = data[i:i + 4]
        d, e = cb - 128, cr - 128
        for y in (y0, y1):
            c = 298 * (y - 16)
            r = (c + 409 * e + 128) >> 8
            g = (c - 100 * d - 208 * e + 128) >> 8
            b = (c + 516 * d + 128) >> 8
            red += r > 180 and g < 100 and b < 110
            gold += r > 180 and g > 130 and b < 100
    return red, gold


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="hbc-badges-"))
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
        return ok

    def options():
        return hbc.status(wii)["options"]

    def step(keys, want, what):
        pad.send(keys)
        pad.done()
        o = options()
        return check(all(o.get(k) == v for k, v in want.items()), f"{what} ({o})")

    def picture(name):
        width, height, data = hbc.screen(wii)
        (out / f"{name}.png").write_bytes(hbc.yuyv_png(width, height, data))
        return color_counts(data)

    try:
        print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}", flush=True)
        deadline = time.monotonic() + 30
        while hbc.status(wii)["device"] != "sd":
            if time.monotonic() > deadline:
                raise SystemExit("the SD card did not mount")
            time.sleep(1)
        time.sleep(2)
        pad.send("stick 0 -1; wait 1")   # the pointer off the screen
        pad.done()
        check(options()["badges"] == "off", "App badges are off by default")

        hbc.send_buttons(wii, "1")
        time.sleep(3)
        check(options()["focus"] == "back", f"Options opens on Back ({options()})")
        if step("press PadN; wait 2", {"focus": "badges_on"}, "up from Back: App badges On") and \
                step("press Cross; wait 2", {"badge_choice": "on"}, "A picks On"):
            shot(wii, out, "options-badges-on")
            if step("press PadS; wait 2; press PadW; wait 2", {"focus": "ok"}, "down, left: Ok"):
                step("press Cross; wait 4", {"open": False, "badges": "on"},
                     "Ok closes Options, App badges on")
        if options()["open"]:
            pad.send("press Circle; wait 2")   # B: never leave it open
            pad.done()

        dol = (root / "tests/agent_app/agent_app.dol").read_bytes()
        for folder in APPS:
            path = f"sd:/apps/{folder}"
            hbc.put_file(wii, f"{path}/boot.dol", dol)
            hbc.put_file(wii, f"{path}/meta.xml", meta_xml(folder, "Before update"))
            if folder != "2 Never run":
                # The marker HBC Reborn wrote before it kept file stamps.
                hbc.put_file(wii, f"{path}/.hbc-ran", b"HBC ran\n")
        time.sleep(3)
        marker = "sd:/apps/3 Updated app/.hbc-ran"
        before = hbc.get_file(wii, marker)
        check(before.startswith(b"HBC ran 2\n"), "an old marker takes today's file stamps")
        hbc.put_file(wii, "sd:/apps/3 Updated app/meta.xml", meta_xml("3 Updated app", "After update"))
        time.sleep(3)

        for layout in ("list", "grid"):
            red, gold = picture(f"badges-{layout}")
            check(red >= 8 and gold >= 8, f"{layout}: a red and a gold badge ({red} red, {gold} gold)")
            pad.send("press Triangle; wait 2")   # 2: the other layout
            pad.done()

        ran = launch(wii, pad, out, "3 Updated app", 3, False)
        check(ran, "the changed app ran and HBC came back")
        check(hbc.get_file(wii, marker) != before, "its marker took the new file stamps")
        pad.send("stick 0 -1; wait 2")
        pad.done()
        red, gold = picture("badges-after-launch")
        check(red >= 8 and gold == 0, f"after the launch: red stays, gold gone ({red} red, {gold} gold)")
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
