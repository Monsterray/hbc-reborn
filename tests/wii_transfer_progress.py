#!/usr/bin/env python3
"""HBC's transfer line and Options > Progress on a Wii, as a queue job: this
tree's HBC (channel DOL, through the installed HBC). Nobody holds the Wii
Remote, so HBC's menu is pressed from the PC (`hbc.py buttons`), and every A
waits until HBCS says where the focus is:

  upload     24 MB into sd:/apps/hbctest-progress/: the line reads
             "Receiving hbctest-progress/data.bin 42%" (PR 4's app folder)
  1 u u a    Options, up past Grid edges to Progress: Circle, A
  d d l a    down to Back, left to Ok, A: Options closes with the circle on
  upload     the same again, now beside the progress circle

The capture card pictures the TV through both uploads (tv_percent_*,
tv_circle_*) and Options (tv_options_*). A channel DOL boot saves no
settings, so the installed HBC keeps its own; the folder is removed after.
Ends in the installed HBC.

queue: wiibench.py add --wii dev --name transfer-progress --cwd REPO --timeout 900 -- <python> tests/wii_transfer_progress.py
"""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
import wii_mboard as wm  # noqa: E402

FOLDER = "sd:/apps/hbctest-progress"
UPLOAD = 24 * 1024 * 1024


def options(wii):
    return hbc.status(wii)["options"]


def until(what, seconds, step=0.3):
    deadline = time.monotonic() + seconds
    while True:
        got = what()
        if got or time.monotonic() > deadline:
            return got
        time.sleep(step)


def watch(out, label, seconds):
    """The capture card, a picture a second (it takes a few to warm up)."""
    return subprocess.Popen([wm.PY, wm.CAPTURE, "watch", str(seconds), "--every", "1",
                             "--out", str(out), "--label", label],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    wii = wm.bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-progress-")) / "hbc-progress.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    data = os.urandom(UPLOAD)
    failed = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    def upload(label):
        cam = watch(out, label, 20)
        time.sleep(4)
        t0 = time.monotonic()
        hbc.put_file(wii, f"{FOLDER}/data.bin", data, level=0)
        print(f"{label}: {UPLOAD >> 20} MB in {time.monotonic() - t0:.1f} s", flush=True)
        cam.wait(60)

    def press(key, want_focus):
        """One button, then the focus HBCS reports; False when it is not the one wanted."""
        hbc.send_buttons(wii, key)
        got = until(lambda: (lambda o: o["focus"] == want_focus and o)(options(wii)), 5)
        check(bool(got), f"{key}: the focus on {want_focus} ({options(wii)['focus']})")
        return bool(got)

    def accept(want_focus):
        """A, only while Options is open with the focus where it should be."""
        o = options(wii)
        if not (o["open"] and o["focus"] == want_focus):
            check(False, f"no A: Options is not on {want_focus} ({o})")
            return False
        hbc.send_buttons(wii, "a")
        return True

    try:
        hbc.send(wii, str(snap), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
        o = options(wii)
        check(not o["open"] and o["progress"] == "percent", f"Options closed, the percentage in use ({o})")

        upload("tv_percent")

        hbc.send_buttons(wii, "1")
        o = until(lambda: (lambda o: o["open"] and o)(options(wii)), 10)
        check(bool(o) and o["focus"] == "back", f"1 opens Options on Back ({options(wii)})")
        if o and o["focus"] == "back" and press("u", "page") and press("u", "circle") and \
                accept("circle"):
            o = until(lambda: (lambda o: o["choice"] == "circle" and o)(options(wii)), 5)
            check(bool(o), f"A picks Circle ({options(wii)})")
            cam = watch(out, "tv_options", 6)
            cam.wait(30)
            if press("d", "page") and press("d", "back") and press("l", "ok") and accept("ok"):
                # Options reads closed a moment before main.c takes its choice.
                o = until(lambda: (lambda o: not o["open"] and o["progress"] == "circle" and o)(
                    options(wii)), 10)
                check(bool(o), f"Ok closes Options, the circle in use ({options(wii)})")
        o = options(wii)
        if o["open"]:   # never leave it open: B only
            hbc.send_buttons(wii, "b")

        if options(wii)["progress"] == "circle":
            upload("tv_circle")
        hbc.remove_tree(wii, FOLDER, force=True)
        check(True, "the test folder removed")
    except (OSError, hbc.HBCError, KeyError) as exc:
        check(False, f"error: {exc!r}")
        try:
            wm.still(out, "tv_error")
        except Exception as still_exc:   # the capture card is a picture, not the test
            print(f"tv still: {still_exc}", flush=True)
    finally:
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
        time.sleep(8)
        print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
        shutil.rmtree(snap.parent, ignore_errors=True)
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
