#!/usr/bin/env python3
"""Options > App badges in Dolphin: the retail WAD in a throwaway NAND.

  1 u a d l a   Options opens on Back; up is App badges: On; A picks it;
                down to Back, left to Ok, A
  apps          three test apps, all New. Two are launched from the list (HBC
                comes back each time) and sd:/hbc/apps_seen.txt then has a
                line for each; the third stays New
  update        one launched app's meta.xml changes: Updated
  list, grid    a blue New pill and an amber Updated pill in each
  launch        the changed app again: its Updated pill is gone, New stays,
                and apps_seen.txt has its new stamp
  sort_id       its meta.xml gets only a <sort_id> (hbc.py order): still no
                Updated pill

HBCS says where the focus is before every A. Pictures go to OUTDIR (default:
a temporary folder). Needs the retail WAD (make -C channel) and
tests/apps/agent_app/agent_app.dol.

usage: tests/dolphin/dolphin_app_badges.py [OUTDIR]
"""

import pathlib
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
from waits import wait_until  # noqa: E402
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
from dolphin_meta import PAD_MAP, PAD_SETTINGS, Pad, launch, shot  # noqa: E402

APPS = ("1 Already run", "2 Never run", "3 Updated app")


def meta_xml(name, description, *more):
    args = "".join(f"<arg>{a}</arg>" for a in ("meta",) + more)
    return (f'<?xml version="1.0"?><app><name>{name}</name>'
            f'<short_description>{description}</short_description>'
            f'<arguments>{args}</arguments></app>').encode()


TAB = chr(9)


def color_counts(data):
    """Blue (New) and amber (Updated) pill pixels in a YUYV screen."""
    blue = amber = 0
    for i in range(0, len(data), 4):
        y0, cb, y1, cr = data[i:i + 4]
        d, e = cb - 128, cr - 128
        for y in (y0, y1):
            c = 298 * (y - 16)
            r = (c + 409 * e + 128) >> 8
            g = (c - 100 * d - 208 * e + 128) >> 8
            b = (c + 516 * d + 128) >> 8
            blue += r < 120 and 130 < g < 200 and b > 190
            amber += r > 200 and 150 < g < 200 and b < 110
    return blue, amber


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
            time.sleep(0.5)
        time.sleep(1)
        pad.send("stick 0 -1; wait 1")   # the pointer off the screen
        pad.done()
        check(options()["badges"] == "off", "App badges are off by default")

        hbc.send_buttons(wii, "1")
        wait_until(lambda: options()["open"], 10)
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

        dol = (root / "tests/apps/agent_app/agent_app.dol").read_bytes()
        for folder in APPS:
            path = f"sd:/apps/{folder}"
            hbc.put_file(wii, f"{path}/boot.dol", dol)
            hbc.put_file(wii, f"{path}/meta.xml", meta_xml(folder, "Before update"))
        time.sleep(3)
        seen = "sd:/hbc/apps_seen.txt"
        pad.send("stick 0 -1; wait 2")
        pad.done()
        blue, amber = picture("badges-all-new")
        check(blue >= 8 and amber == 0, f"all three New ({blue} blue, {amber} amber)")

        for row, folder in ((1, APPS[0]), (3, APPS[2])):
            check(launch(wii, pad, out, folder, row, False), f"{folder} ran and HBC came back")
        pad.send("stick 0 -1; wait 2")
        pad.done()
        lines = [l for l in hbc.get_file(wii, seen).decode().splitlines() if not l.startswith("#")]
        check(sorted(l.split(TAB)[0] for l in lines) == [APPS[0], APPS[2]],
              f"apps_seen.txt has the two launched apps ({lines})")
        blue, amber = picture("badges-after-two-launches")
        check(blue >= 8 and amber == 0, f"New stays on the third only ({blue} blue, {amber} amber)")
        try:
            hbc.get_file(wii, "sd:/apps/1 Already run/.hbc-ran")
            check(False, "no marker file in the app folder")
        except Exception:
            check(True, "no marker file in the app folder")

        before = hbc.get_file(wii, seen)
        hbc.put_file(wii, "sd:/apps/3 Updated app/meta.xml", meta_xml("3 Updated app", "After update", "updated"))
        time.sleep(3)
        for layout in ("list", "grid"):
            blue, amber = picture(f"badges-{layout}")
            check(blue >= 8 and amber >= 8,
                  f"{layout}: a New and an Updated pill ({blue} blue, {amber} amber)")
            # The light is part of the picture without the focus too.
            way = "0 1" if layout == "list" else "-1 0"
            back = "0 -1" if layout == "list" else "1 0"
            pad.send(f"stick {way}; wait 2")
            pad.done()
            blue, amber = picture(f"badges-{layout}-unfocused")
            check(blue >= 8 and amber >= 8,
                  f"{layout}, focus elsewhere: a New and an Updated pill ({blue} blue, {amber} amber)")
            pad.send(f"stick {back}; wait 2")
            pad.done()
            pad.send("press Triangle; wait 2")   # 2: the other layout
            pad.done()

        # The new argument makes this run's kept log differ from the first
        # run's, so launch() sees it at once instead of waiting out 60 s.
        check(launch(wii, pad, out, "3 Updated app", 3, False), "the updated app ran and HBC came back")
        check(hbc.get_file(wii, seen) != before, "apps_seen.txt took the new file stamps")
        pad.send("stick 0 -1; wait 2")
        pad.done()
        blue, amber = picture("badges-after-launch")
        check(blue >= 8 and amber == 0, f"after the launch: New stays, Updated gone ({blue} blue, {amber} amber)")

        # hbc.py order rewrites meta.xml with a <sort_id>: not an update.
        meta = meta_xml("3 Updated app", "After update", "updated")
        hbc.put_file(wii, "sd:/apps/3 Updated app/meta.xml",
                     meta.replace(b"</app>", b"<sort_id>10</sort_id></app>"))
        time.sleep(3)
        pad.send("stick 0 -1; wait 2")
        pad.done()
        blue, amber = picture("badges-after-sort-id")
        check(blue >= 8 and amber == 0, f"a new <sort_id> alone: no Updated pill ({blue} blue, {amber} amber)")
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
