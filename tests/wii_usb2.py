#!/usr/bin/env python3
"""Both USB drives through HBC (usbmsd.c): "usb" and "usb2".

Runs this tree's channel DOL through the installed HBC, then with file
requests only (no controller input): lists each drive's root, writes a 1 MB
file to each, reads it back and compares, deletes it, and prints the Storage
page of `hbc.py hw`. HBC mounts a drive other than its app list's on demand.
Ends in the installed HBC through tests/agent_app's exit.

usage: tests/wii_usb2.py [WII-IP]
"""

import os
import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402


def main():
    wii = hbc.wii_address(sys.argv[1] if len(sys.argv) > 1 else None)
    work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-usb2-"))
    snap = work / "channelapp-channel.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    hbc.send(wii, str(snap), [])
    hbc.relaunch_wait(wii, dolphin_smoke.expected)
    time.sleep(3)
    st = hbc.status(wii)
    print(f"HBC {st['version']}: device {st['device']}, inserted {st['inserted']}", flush=True)
    failed = []
    for dev in ("usb", "usb2"):
        try:
            t = time.monotonic()
            entries, _ = hbc.list_dir(wii, f"{dev}:/")
            print(f"{dev}: {len(entries)} entries at the root ({time.monotonic() - t:.1f} s, "
                  f"mount included): {[n for _, _, n in entries][:6]}", flush=True)
            blob = os.urandom(1 << 20)
            path = f"{dev}:/hbctest-usb2.bin"
            t = time.monotonic()
            hbc.put_file(wii, path, blob)
            up = time.monotonic() - t
            t = time.monotonic()
            back = hbc.get_file(wii, path)
            down = time.monotonic() - t
            hbc.file_request(wii, "D", path)
            assert back == blob, f"{dev}: what came back differs"
            print(f"{dev}: 1 MiB written in {up:.1f} s, read back in {down:.1f} s, the same",
                  flush=True)
        except (OSError, hbc.HBCError, AssertionError) as exc:
            print(f"FAIL {dev}: {exc}", flush=True)
            failed.append(dev)
    st = hbc.status(wii)
    print(f"inserted now {st['inserted']}; drives {st.get('usb_drives')}; "
          f"no file system: {st.get('unmountable')}", flush=True)
    for label, value in hbc.hardware(wii, ["Storage"])["Storage"].items():
        print(f"  {label:16} {value}", flush=True)
    hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
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
    if failed:
        raise SystemExit(f"failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
