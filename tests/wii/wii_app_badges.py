#!/usr/bin/env python3
"""Options > App badges on the bench Wii: the amber Updated pill and light.

Runs this tree's channel DOL, turns App badges on (1 u a d l a, reading HBCS
"options" before each key), installs tests/apps/agent_app as sd:/apps/!AAA
verify (it sorts second) and counts amber pixels in that row's button only
(the avatar is orange; the full screen would count it):

  new            no amber in its row
  run once       hbc.py send of the identical boot.dol counts as a run: no amber
  update         one byte appended to boot.dol: amber (pill and light)
  identical send the changed file sent again: amber gone
  sort_id        a meta.xml with only a <sort_id> added: no amber

Another app on the list with a real Updated pill is a positive reference when
there is one. Every send of the app ends in the installed HBC, which forgets
the badge setting, so this test sends this tree's HBC again and turns badges
on after each. The port can stop answering for minutes: calls retry for 5 min
and say when. Pictures go to $WII_BENCH_OUT (default: current folder). At the
end the test app, its apps_seen.txt line and the badge setting are gone.

usage (a bench job): tests/wii/wii_app_badges.py [WII-IP]
"""

import json
import os
import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import wii_checks  # noqa: E402
import hbc  # noqa: E402

APP = "!AAA verify"
PATH = f"sd:/apps/{APP}"
SEEN = "sd:/hbc/apps_seen.txt"
DOL = (root / "tests/apps/agent_app/agent_app.dol").read_bytes()
CHANNEL = str(root / "channel/channelapp/channelapp-channel.dol")
OUT = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
ROWS = {0: (68, 124), 1: (133, 188)}   # list layout, y range of a button; x 160-480
failed = []
outages = []
t0 = time.monotonic()
wii = None


def say(msg):
    print(f"{time.monotonic() - t0:6.1f} {msg}", flush=True)


def check(ok, what):
    say(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failed.append(what)
    return ok


def patient(fn, *args, **kw):
    """fn, retried while the port does not answer (up to 5 min); logs outages."""
    start = time.monotonic()
    while True:
        try:
            r = fn(*args, **kw)
        except (OSError, hbc.HBCError) as exc:
            if time.monotonic() - start > 300:
                raise
            if not outages or outages[-1][0] != start:
                outages.append([start, 0])
                say(f"4299 not answering ({exc!r}); retrying")
            time.sleep(5)
            continue
        if outages and outages[-1][0] == start and not outages[-1][1]:
            outages[-1][1] = time.monotonic() - start
            say(f"4299 answered again after {outages[-1][1]:.0f} s")
        return r


def status():
    # strict=False: a debug field of a work-in-progress HBC carried raw newlines
    return json.loads(hbc.request(wii, b"HBCS"), strict=False)


def options():
    return patient(status)["options"]


def key(k, want):
    patient(hbc.send_buttons, wii, k)
    deadline = time.monotonic() + 8
    o = {}
    while time.monotonic() < deadline:
        time.sleep(0.8)
        o = options()
        if all(o.get(a) == b for a, b in want.items()):
            return o
    raise SystemExit(f"FAIL: after {k!r} options are {o}, wanted {want}")


def set_badges(on):
    want = "on" if on else "off"
    o = options()
    if o["badges"] == want:
        return
    assert not o["open"], o
    patient(hbc.send_buttons, wii, "1")
    deadline = time.monotonic() + 8
    while not options()["open"] and time.monotonic() < deadline:
        time.sleep(0.4)
    o = options()
    assert o["open"] and o["focus"] == "back", o
    o = key("u", {})
    if on:
        assert o["focus"] == "badges_on", o
    elif o["focus"] == "badges_on":
        o = key("l", {"focus": "badges_off"})
    key("a", {"badge_choice": want})
    key("d", {"focus": "back"})
    key("l", {"focus": "ok"})
    key("a", {"open": False, "badges": want})


def amber(data, x0, y0, x1, y1, width=640):
    """Amber (Updated) pixels in a rectangle of a YUYV screen."""
    n = 0
    for y in range(y0, y1):
        for x in range(x0 & ~1, x1, 2):
            i = (y * width + x) * 2
            y0_, cb, y1_, cr = data[i:i + 4]
            d, e = cb - 128, cr - 128
            for lum in (y0_, y1_):
                c = 298 * (lum - 16)
                r = (c + 409 * e + 128) >> 8
                g = (c - 100 * d - 208 * e + 128) >> 8
                b = (c + 516 * d + 128) >> 8
                n += r > 200 and 150 < g < 200 and b < 110
    return n


def picture(name):
    """Save the screen; return (amber in the test app's row, amber in the first row)."""
    w, h, data = patient(hbc.screen, wii)
    (OUT / f"{name}.png").write_bytes(hbc.yuyv_png(w, h, data))
    mine = amber(data, 160, *ROWS[1][:1], 480, ROWS[1][1], w)
    first = amber(data, 160, ROWS[0][0], 480, ROWS[0][1], w)
    say(f"picture {name}: amber in the app's row {mine}, in the first row {first}")
    return mine, first


def dev_hbc():
    say("HBC: " + patient(wii_checks.ensure_hbc, wii, CHANNEL, say=say))
    deadline = time.monotonic() + 40
    while patient(status)["device"] != "sd" and time.monotonic() < deadline:
        time.sleep(0.5)
    time.sleep(2)


def send_identical(data):
    """hbc.py send of the very file on the card counts as a run; then this HBC again."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="hbc-badge-send-")) / APP
    tmp.mkdir(parents=True)
    (tmp / "boot.dol").write_bytes(data)
    patient(hbc.send, wii, str(tmp / "boot.dol"), ["exit"])
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        try:
            hbc.version(wii, hbc.POLL_TIMEOUT)
        except (OSError, hbc.HBCError):
            break
        time.sleep(0.3)
    say(f"back in HBC {patient(hbc.hbc_wait, wii, 120)}")
    shutil.rmtree(tmp.parent, ignore_errors=True)
    dev_hbc()
    set_badges(True)
    time.sleep(2)


def meta(sort_id=None):
    extra = f"<sort_id>{sort_id}</sort_id>" if sort_id is not None else ""
    return (f'<?xml version="1.0"?><app><name>{APP}</name>'
            f'<short_description>badge verify</short_description>{extra}</app>').encode()


def main():
    global wii
    wii = hbc.wii_address(sys.argv[1] if len(sys.argv) > 1 else None)
    wii_checks.refuse_production(wii)
    dev_hbc()
    say(f"version {patient(hbc.version, wii)}")
    orig_seen = None
    try:
        orig_seen = patient(hbc.get_file, wii, SEEN)
        (OUT / "apps_seen.orig.txt").write_bytes(orig_seen)
    except (OSError, hbc.HBCError) as exc:
        say(f"no apps_seen.txt before: {exc!r}")
    try:
        if orig_seen is not None and APP.encode() + b"\t" in orig_seen:
            # a line a killed run left: the app would not be new
            orig_seen = b"".join(ln for ln in orig_seen.splitlines(True)
                                 if not ln.startswith(APP.encode() + b"\t"))
            patient(hbc.put_file, wii, SEEN, orig_seen)
            say("removed a stale apps_seen.txt line of the test app")
        set_badges(True)
        cur = DOL
        patient(hbc.put_file, wii, f"{PATH}/boot.dol", cur)
        patient(hbc.put_file, wii, f"{PATH}/meta.xml", meta())
        time.sleep(4)
        order = patient(status)["app_list"]["order"]
        check(order.index(APP) == 1 if APP in order else False, f"the test app is the second row ({order[:3]})")
        mine, first = picture("00-new")
        say(f"reference: first row amber {first} (a real Updated pill when > 0)")
        check(mine == 0, f"new app: no amber in its row ({mine})")

        send_identical(cur)
        mine, _ = picture("01-after-run")
        check(mine == 0, f"after one run: no amber ({mine})")

        cur = DOL + b"\0"
        patient(hbc.put_file, wii, f"{PATH}/boot.dol", cur)
        time.sleep(4)
        mine, first = picture("02-updated-first-focus")
        check(mine > 0, f"updated: amber in its row ({mine})")
        patient(hbc.send_buttons, wii, "d")
        time.sleep(2)
        mine2, _ = picture("03-updated-focus-moved")
        check(mine2 > 0, f"updated, focus moved: amber ({mine2})")
        patient(hbc.send_buttons, wii, "d")
        time.sleep(2)
        mine3, _ = picture("04-updated-focus-moved-again")
        check(mine3 > 0, f"updated, focus elsewhere: amber ({mine3})")

        send_identical(cur)
        mine, _ = picture("05-after-identical-send")
        check(mine == 0, f"identical send clears Updated ({mine})")

        patient(hbc.put_file, wii, f"{PATH}/meta.xml", meta(10))
        time.sleep(4)
        mine, _ = picture("06-sort-id-only")
        check(mine == 0, f"a sort_id-only meta.xml change: no amber ({mine})")
    finally:
        try:
            patient(hbc.remove_tree, wii, PATH, out=lambda *a: None)
        except (OSError, hbc.HBCError) as exc:
            say(f"cleanup of the test app: {exc!r}")
        try:
            text = patient(hbc.get_file, wii, SEEN).decode()
            keep = "".join(ln + "\n" for ln in text.splitlines() if not ln.startswith(APP + "\t"))
            if orig_seen is not None and sorted(keep.splitlines()) == sorted(orig_seen.decode().splitlines()):
                keep = orig_seen.decode()
                say("apps_seen.txt equals the original again")
            else:
                say("apps_seen.txt: only the test app's line removed (the rest differs from the original)")
            patient(hbc.put_file, wii, SEEN, keep.encode())
        except (OSError, hbc.HBCError) as exc:
            say(f"apps_seen cleanup: {exc!r}")
        try:
            set_badges(False)
        except BaseException as exc:  # noqa: BLE001
            say(f"badges off failed: {exc!r}")
        say(f"4299 outages: {[(round(a - t0), round(b)) for a, b in outages]}")
    if failed:
        raise SystemExit(f"failed: {failed}")
    say("PASS")


if __name__ == "__main__":
    main()
