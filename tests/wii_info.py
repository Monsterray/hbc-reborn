#!/usr/bin/env python3
"""DEV > Info's pages and `hbc.py hw` on a real Wii (sdk/hbc_agent/info.c).

Runs this tree's channel DOL through the installed HBC, prints what `hw`
reports, then runs agent_app: the same `hw` from an agent, then its HOME overlay's
DEV > Info pages, a picture of each to OUT (the focus is checked before
every A press), and back to the installed HBC through agent_app's exit.

usage: tests/wii_info.py [--out DIR] [WII-IP]
"""

import argparse
import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import agent_checks  # noqa: E402
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402

PAGES = ["System", "Video", "Storage", "USB", "Network"]


def shot(wii, path):
    w, h, data = hbc.screen(wii)
    path.write_bytes(hbc.yuyv_png(w, h, data))


def keys(wii, seq, wait):
    hbc.send_keys(wii, seq)
    time.sleep(wait + 0.25 * len(seq))


# ov_ui.c's ids: the bar's DEV, DEV's Info tab, the first Info page button.
BAR_DEV, BAR_EXIT, TAB_INFO, INFO_PAGE = 100, 102, 121, 133


def where(wii):
    return hbc.status(wii).get("overlay_ui")


def press(wii, moves, focus):
    """Move, check the focus is where it should be, and only then press A:
    a guessed A on a HOME menu can launch or switch off something."""
    if moves:
        keys(wii, moves, 1)
    ui = where(wii)
    assert ui and ui["focus"] == focus, f"focus {ui} before A, wanted {focus}"
    keys(wii, "a", 2)


def overlay_pages(wii, out):
    """In agent_app's overlay (its bar has nothing that leaves the app): DEV,
    Info, then each page in turn, a picture of each."""
    keys(wii, "h", 2)
    ui = where(wii)
    assert ui and ui["menu"] == 0 and ui["focus"] == BAR_EXIT, ui
    press(wii, "ll", BAR_DEV)
    press(wii, "r", TAB_INFO)
    assert where(wii)["tab"] == 1
    shot(wii, out / "info.png")
    keys(wii, "d" + "llll", 1)
    for i, name in enumerate(PAGES):
        press(wii, "r" if i else "", INFO_PAGE + i)
        time.sleep(3)   # the pages are gathered in a thread
        assert where(wii)["info_page"] == i
        shot(wii, out / f"info-{i}-{name.lower()}.png")
        keys(wii, "b", 1)
    keys(wii, "h", 2)
    print(f"pictures in {out}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wii", nargs="?")
    parser.add_argument("--out")
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    out = pathlib.Path(opts.out or tempfile.mkdtemp(prefix="hbc-info-"))
    out.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-info-"))

    snap = work / "channelapp-channel.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    hbc.send(wii, str(snap), [])
    hbc.relaunch_wait(wii, dolphin_smoke.expected)
    print(f"HBC {hbc.version(wii)}")
    t = time.monotonic()
    pages = hbc.hardware(wii)
    print(f"hw from HBC, {time.monotonic() - t:.1f} s:")
    for page, rows in pages.items():
        print(f"  {page}")
        for label, value in rows.items():
            print(f"    {label:16} {value}")
    assert set(pages) == set(PAGES), pages
    assert pages["System"].get("IOS", "").startswith("IOS"), pages["System"]

    # An agent app answers the same.
    hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["stay", "60"])
    agent_checks.wait_agent(wii)
    time.sleep(2)
    app_pages = hbc.hardware(wii)
    assert app_pages["System"] == pages["System"], (app_pages["System"], pages["System"])
    print("hw from agent_app: the same System page")
    try:
        overlay_pages(wii, out)
    finally:
        if (hbc.status(wii).get("overlay_ui")):
            keys(wii, "h", 2)
    hbc.exit_app(wii)    # through the installed channel's stub
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            if hbc.version(wii, hbc.POLL_TIMEOUT) != dolphin_smoke.expected:
                break
        except (OSError, hbc.HBCError):
            break
        time.sleep(0.5)
    print(f"back in HBC {hbc.hbc_wait(wii, 90)}")
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
