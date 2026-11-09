#!/usr/bin/env python3
"""Unplug and replug the emulated SD card while HBC runs, in Dolphin.

Dolphin takes the SD card in and out while it runs: its "Toggle SD Card"
hotkey (Source/Core/DolphinQt/HotkeyScheduler.cpp) flips Core.WiiSDCard, and
the SDIO device (IOS/SDIO/SDIOSlot0.cpp) reads that setting on every status
request and raises a card event. Nothing else can change it from outside the
process, so this test binds the hotkey to a button of the scripted DSU pad
(tools/dsu_pad.py, as tests/dolphin/dolphin_meta.py drives) in the profile's
Hotkeys.ini, with background input on. The emulated USB storage is not
hot-pluggable in Dolphin, so USB is covered only by tests/wii/wii_replug.py.

Steps, each checked with tests/lib/replug_checks.py:
  1  boot, the SD card mounts, the app list shows a test app, a file makes
     the round trip (put, ls, get, delete) under sd:/replug_test/
  2  SD out: status stops listing it, ls and get fail, the app list empties
     (not stale), HBC still answers
  3  SD in: the device mounts again, the app list shows the test app again,
     ls, put and get work
  4  SD out and in three times in quick succession: no hang, no crash, the
     card and the list come back
  5  SD out and left out for 15 s: still gone, HBC still answers; then in
Finally no fault lines in Dolphin's log.

usage: tests/dolphin/dolphin_replug.py [DOL-or-WAD]   (default: this tree's channel DOL)
One Dolphin at a time on this PC (port 4299): check `tasklist | grep -i dolphin`.
"""

import pathlib
import sys
import time

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import dolphin_meta  # noqa: E402
import dolphin_smoke as smoke  # noqa: E402
import replug_checks as rc  # noqa: E402
hbc = smoke.hbc

HOTKEYS = """[Hotkeys]
Device = DSUClient/0/hbcpad
Wii/Toggle SD Card = Triangle
"""
APP = "sd-replug-test-app"


def main():
    image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                         root / "channel/channelapp/channelapp-channel.dol").resolve()
    files = {"Config/Hotkeys.ini": HOTKEYS,
             f"Load/WiiSDSync/apps/{APP}/boot.dol": bytes(256),
             f"Load/WiiSDSync/apps/{APP}/meta.xml":
                 '<?xml version="1.0" encoding="UTF-8"?><app version="1"><name>Replug test</name></app>'}
    # Hotkeys obey window focus unless told not to (Background input is only
    # for the controllers), and the Dolphin window may not have it.
    extra = dolphin_meta.PAD_SETTINGS + ["Dolphin.General.HotkeysRequireFocus=False",
                                         "Logger.Logs.IOS_SD=True"]
    run = smoke.Dolphin(image, sd=True, extra=extra, files=files)
    pad = dolphin_meta.Pad()
    try:
        wii = run.address
        smoke.wait_version(run, 90)
        st = rc.wait_for(wii, lambda s: s.get("device") == "sd" and s.get("apps", 0) >= 1, 60,
                         "the SD card to mount and the app list to load")
        apps = st["apps"]
        time.sleep(3)  # start-up settles
        rc.check_present(wii, "sd", "step1")
        print(f"1 boot: sd mounted, {apps} app(s), file round trip ok", flush=True)

        def out():
            """Toggle the card. dsu_pad.py says after each step how many DSU
            clients it has; a press with none reached no Dolphin, so it is
            safe to repeat (a press that arrived must not be, it toggles)."""
            for _ in range(20):
                pad.send("press Triangle 0.3")
                clients = None
                for line in pad.proc.stdout:
                    if line.strip() == "ok":
                        break
                    if " clients " in line:
                        clients = int(line.rsplit(None, 1)[1])
                if clients:
                    return
                time.sleep(1)
            raise AssertionError("Dolphin never connected to the DSU pad")

        out()  # the card leaves
        try:
            rc.wait_for(wii, lambda s: rc.is_gone(s, "sd"), 15, "sd to show as gone")
        except AssertionError as exc:
            raise AssertionError("the card did not leave (does the hotkey reach Dolphin?): "
                                 f"{exc}")
        rc.check_gone(wii, "sd")
        st = rc.wait_for(wii, lambda s: s.get("apps") == 0, 15, "the app list to empty")
        assert st.get("device") is None, st.get("device")
        rc.alive(wii, smoke.expected)
        print("2 SD out: gone in status, file requests fail, app list empty, HBC answers",
              flush=True)

        out()  # the card returns
        st = rc.wait_for(wii, lambda s: s.get("device") == "sd" and s.get("apps") == apps, 60,
                         "the SD card to mount again and the app list to reload")
        rc.check_present(wii, "sd", "step3")
        print(f"3 SD in: mounted again, {st['apps']} app(s) reloaded, ls/put/get ok", flush=True)

        for i in range(3):
            out()
            time.sleep(0.4 + 0.5 * i)
            out()
            time.sleep(0.4)
        rc.alive(wii, smoke.expected)
        rc.wait_for(wii, lambda s: s.get("device") == "sd" and s.get("apps") == apps, 90,
                    "the SD card and list after quick toggling")
        rc.check_present(wii, "sd", "step4")
        print("4 three quick out/in cycles: no hang, card and list back", flush=True)

        out()
        rc.check_gone(wii, "sd")
        time.sleep(15)
        rc.check_gone(wii, "sd")
        rc.alive(wii, smoke.expected)
        out()
        rc.wait_for(wii, lambda s: s.get("device") == "sd" and s.get("apps") == apps, 60,
                    "the SD card after a long absence")
        rc.check_present(wii, "sd", "step5")
        print("5 SD out for 15 s: stayed gone, HBC answered; back in: all works", flush=True)

        assert run.running(), "Dolphin exited"
        assert not run.faults(), run.faults()[:10]
        print("PASS: HBC recovers from the SD card being removed and inserted", flush=True)
    finally:
        try:
            pad.close()
        except Exception:
            pass
        run.close()


if __name__ == "__main__":
    main()
