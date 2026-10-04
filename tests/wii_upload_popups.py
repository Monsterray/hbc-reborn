#!/usr/bin/env python3
"""Check that popups an upload raises close themselves and report back.

Runs this tree's channel DOL through the installed HBC (or, with
--installed, the channel already running), then sends:

  1. a file that is not a Wii app: `send` must fail at once with HBC's
     reason, and HBC's error popup must close itself;
  2. a ZIP app with nobody to answer: the question must time out as No
     ("declined") about 10 s after the first popup closed;
  3. the same ZIP with --yes: installed without a question, then removed.

Screens of the first popup go to OUT (default a temporary folder). The Wii
ends in the installed HBC: tests/agent_app's exit goes through its stub.

usage: tests/wii_upload_popups.py [--installed] [--out DIR] [WII-IP]
"""

import argparse
import io
import pathlib
import shutil
import sys
import tempfile
import time
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402

APP = "hbcpopup"


def outcome_of(wii, path, yes=False):
    before = hbc.upload_seq(wii)
    if before is None:
        raise SystemExit("FAIL: HBC does not report upload results (protocol 6)")
    sent = time.monotonic()
    hbc.send(wii, str(path), [], yes=yes)
    up = hbc.upload_outcome(wii, before, 60)
    return up, time.monotonic() - sent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wii", nargs="?")
    parser.add_argument("--installed", action="store_true")
    parser.add_argument("--out")
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    out = pathlib.Path(opts.out or tempfile.mkdtemp(prefix="hbc-popups-"))
    out.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-popups-"))

    if not opts.installed:
        snap = work / "channelapp-channel.dol"
        shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
        hbc.send(wii, str(snap), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
    print(f"HBC {hbc.version(wii)} is running")

    bogus = work / "notanapp.dol"
    bogus.write_bytes(b"not a Wii program\n" * 64)
    up, took = outcome_of(wii, bogus)
    if up.get("result") != "error" or up.get("error") != "not_wii_app":
        raise SystemExit(f"FAIL: a file that is no app gave {up}")
    print(f"PASS: send reported '{up['text']}' after {took:.1f} s")
    shown = time.monotonic()
    time.sleep(2)
    for name in ("popup-2s", "popup-6s"):
        w, h, data = hbc.screen(wii)
        (out / f"{name}.png").write_bytes(hbc.yuyv_png(w, h, data))
        time.sleep(4)

    zbuf = io.BytesIO()
    with zipfile.ZipFile(zbuf, "w") as z:
        z.write(root / "tests/agent_app/agent_app.dol", f"{APP}/boot.dol")
        z.writestr(f"{APP}/meta.xml",
                   "<?xml version=\"1.0\"?>\n<app version=\"1\"><name>Popup test</name></app>\n")
    zpath = work / f"{APP}.zip"
    zpath.write_bytes(zbuf.getvalue())

    # The first popup still holds HBC's menu: this upload waits for it.
    up, took = outcome_of(wii, zpath)
    since = time.monotonic() - shown
    if up.get("result") != "declined":
        raise SystemExit(f"FAIL: an unanswered ZIP question gave {up}")
    if since < 17:
        raise SystemExit(f"FAIL: declined {since:.1f} s after the first popup; "
                         "both popups should have waited about 10 s")
    print(f"PASS: the ZIP question timed out as No, {since:.1f} s after the first popup")
    w, h, data = hbc.screen(wii)
    (out / "after-declined.png").write_bytes(hbc.yuyv_png(w, h, data))

    up, took = outcome_of(wii, zpath, yes=True)
    if up.get("result") != "installed" or up.get("text") != APP:
        raise SystemExit(f"FAIL: --yes gave {up}")
    print(f"PASS: --yes installed {APP} in {took:.1f} s without a question")
    hbc.remove_tree(wii, f"sd:/apps/{APP}", out=lambda *a: None)
    print(f"screens in {out}")

    if not opts.installed:
        # Back to the installed HBC through its reload stub.
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:   # until this DOL stops answering
            try:
                hbc.version(wii, hbc.POLL_TIMEOUT)
            except (OSError, hbc.HBCError):
                break
            time.sleep(0.2)
        print(f"back in HBC {hbc.hbc_wait(wii, 90)}")
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
