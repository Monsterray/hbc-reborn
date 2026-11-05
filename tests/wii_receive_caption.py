#!/usr/bin/env python3
"""The Wiiload receive dialog's heading (PR 8), on a Wii, as a queue job:
this tree's HBC (channel DOL, through the installed HBC) is sent the agent
app as hbctest-recv/boot.dol, padded with 8 MiB so the dialog stays up. The
capture card pictures it (tv_receive_*): "Receiving hbctest-recv/boot.dol"
above the bar, the PC's address lower left. The app must load (hbc.py send
fails with HBC's reason otherwise); it then exits, back to the installed HBC.

queue: wiibench.py add --wii devwii --name receive-caption --cwd REPO --timeout 600 -- <python> tests/wii_receive_caption.py
"""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
import wii_mboard as wm  # noqa: E402

PAD = 8 << 20


def main():
    wii = wm.bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="hbc-receive-"))
    snap = tmp / "hbc-receive.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    app = tmp / "hbctest-recv" / "boot.dol"
    app.parent.mkdir()
    app.write_bytes((root / "tests/agent_app/agent_app.dol").read_bytes() + os.urandom(PAD))
    failed = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    try:
        hbc.send(wii, str(snap), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
        check(hbc.upload_file(str(app)) == "hbctest-recv/boot.dol", "the dialog is sent hbctest-recv/boot.dol")
        cam = subprocess.Popen([wm.PY, wm.CAPTURE, "watch", "40", "--every", "1",
                                "--out", str(out), "--label", "tv_receive"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(4)
        hbc.send(wii, str(app), ["exit"])
        cam.wait(90)
        check(True, "the padded boot.dol loaded (send fails with HBC's reason otherwise)")
    except (OSError, hbc.HBCError) as exc:
        check(False, f"error: {exc!r}")
    finally:
        time.sleep(8)
        try:
            print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
        except (OSError, hbc.HBCError) as exc:
            check(False, f"not back in HBC: {exc!r}")
        shutil.rmtree(tmp, ignore_errors=True)
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
