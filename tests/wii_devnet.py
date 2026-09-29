#!/usr/bin/env python3
"""Run the channel DOL on a real Wii through its installed HBC and test it.

The DOL is sent with Wiiload, so nothing is written to NAND. The test checks
the version, status, an SD file round trip (under sd:/hbctest, removed
afterwards) and a network-log run of tests/netlog_app. That app's exit goes
through the installed channel's reload stub, so the Wii ends back in the
installed HBC, which the shared bench queue requires.

With --installed nothing is sent: the test checks the channel already
running (an installed WAD) and requires the app's exit to relaunch that
same channel through HBC's own reload stub.

usage: tests/wii_devnet.py [--installed] [--expect VERSION] [--log-port PORT] [WII-IP]
The address defaults to $HBC_WII or $WII_BENCH_IP. The log port must accept
inbound TCP through the PC firewall.
"""

import argparse
import os
import pathlib
import shutil
import socket
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402


def hbc_answers(wii):
    # A PING header is rejected at once; a bare connect would hold the loader.
    try:
        with socket.create_connection((wii, hbc.PORT), timeout=3) as conn:
            conn.sendall(b"PING" + bytes(12))
        return True
    except OSError:
        return False


class Target:
    def __init__(self, address):
        self.address = address


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wii", nargs="?")
    parser.add_argument("--log-port", type=int, default=hbc.LOG_PORT)
    parser.add_argument("--installed", action="store_true",
                        help="test the running installed channel; send nothing")
    parser.add_argument("--expect", help="version the Wii should run (default: this tree's)")
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    expected = opts.expect or dolphin_smoke.expected
    dolphin_smoke.expected = expected

    snap = None
    if opts.installed:
        try:
            running = hbc.version(wii)
        except (OSError, hbc.HBCError) as exc:
            raise SystemExit(f"FAIL: no HBCV reply; is the installed channel running? {exc}")
        if running != expected:
            raise SystemExit(f"FAIL: the Wii runs HBC {running}, this tree is {expected}")
        print(f"PASS: installed HBC {expected} is running on the Wii")
    else:
        # Snapshot the DOL so a rebuild during the run cannot change what boots.
        snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-wii-")) / "channelapp-channel.dol"
        shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)

        print(f"sending {snap.name} ({snap.stat().st_size} bytes) to {wii}")
        hbc.send(wii, str(snap), [])
        try:
            hbc.relaunch_wait(wii, expected)
        except hbc.HBCError as exc:
            raise SystemExit(f"FAIL: {exc}")
        print(f"PASS: HBC {expected} is running on the Wii")

    dolphin_smoke.check_devnet(Target(wii), log_port=opts.log_port)

    # netlog_app exited through the preserved reload stub.
    deadline = time.monotonic() + 60
    while not hbc_answers(wii):
        if time.monotonic() > deadline:
            raise SystemExit("FAIL: the Wii did not return to HBC after the app exited")
        time.sleep(2)
    try:
        now = hbc.version(wii)
    except (OSError, hbc.HBCError):
        now = "an HBC without HBCV"
    if opts.installed:
        # Only HBC's own stub relaunching the installed title answers HBCV here.
        if now != expected:
            raise SystemExit(f"FAIL: app exit returned to {now}, not the installed HBC")
        print(f"PASS: app exit relaunched the installed HBC {expected}")
    else:
        print(f"PASS: the Wii returned to {now}")
        shutil.rmtree(snap.parent, ignore_errors=True)


if __name__ == "__main__":
    main()
