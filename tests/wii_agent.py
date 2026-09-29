#!/usr/bin/env python3
"""Test sdk/hbc_agent on a real Wii, through tests/agent_checks.py.

The Wii must be in HBC. Without --installed this sends the tree's channel DOL
first, and sends it again whenever an app exits back to an older installed
channel, so crash reports (HBC 1.5.0 and later) are always read by this
tree's HBC. That also shows the crash block surviving the real title launch.
With --installed the running installed channel must already be this tree's.

The run also compares transfer speed inside the app (two buffer slots)
with HBC itself (four), and ends with the Wii in HBC.

usage: tests/wii_agent.py [--installed] [--log-port PORT] [WII-IP]
"""

import argparse
import os
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


def speed(wii, label, blobs):
    """Framed put and get of each blob; returns {name: (up MB/s, down MB/s)}."""
    out = {}
    for name, blob in blobs.items():
        path = f"sd:/hbcbench/agent-{name}.bin"
        start = time.monotonic()
        hbc.put_file(wii, path, blob)
        up = time.monotonic() - start
        start = time.monotonic()
        assert hbc.get_file(wii, path) == blob, (label, name)
        down = time.monotonic() - start
        hbc.file_request(wii, "D", path)
        out[name] = (len(blob) / up / 1e6, len(blob) / down / 1e6)
        print(f"  {label:5} {name:7} {len(blob):8} bytes: up {out[name][0]:.2f} MB/s, "
              f"down {out[name][1]:.2f} MB/s")
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wii", nargs="?")
    parser.add_argument("--log-port", type=int, default=hbc.LOG_PORT)
    parser.add_argument("--installed", action="store_true",
                        help="the installed channel is this tree's; send no DOL")
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    expected = dolphin_smoke.expected

    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-wii-agent-"))
    dol = snap / "channelapp-channel.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", dol)
    shutil.copy(agent_checks.APP, snap / "agent_app.dol")
    agent_checks.APP = snap / "agent_app.dol"
    sends = 0

    def back_in_hbc(w):
        nonlocal sends
        running = hbc.hbc_wait(w, 90)
        if running == expected:
            return
        if opts.installed:
            raise AssertionError(f"returned to HBC {running}, not the installed {expected}")
        hbc.send(w, str(dol), [])
        hbc.relaunch_wait(w, expected)
        sends += 1

    try:
        back_in_hbc(wii)
        print(f"PASS: HBC {expected} is running on the Wii")

        blobs = {"random": os.urandom(2 * 1024 * 1024),
                 "elf": (root / "channel/channelapp/channelapp_nopax.elf").read_bytes()}
        hbc.file_request(wii, "M", "sd:/hbcbench")
        print("transfer speed, HBC:")
        in_hbc = speed(wii, "HBC", blobs)

        crash = agent_checks.check_agent(wii, opts.log_port, "crash", back_in_hbc)
        assert crash, "HBC reported no crash"

        # Speed inside the app, then leave it.
        hbc.send(wii, str(agent_checks.APP), ["stay", "120"])
        agent_checks.wait_agent(wii)
        print("transfer speed, agent:")
        in_app = speed(wii, "agent", blobs)
        st = hbc.status(wii)
        print(f"  agent stack {st['agent_stack_used']} of {st['agent_stack_size']}, "
              f"heap {st['heap_free']} free")
        hbc.exit_app(wii)
        back_in_hbc(wii)
        hbc.file_request(wii, "D", "sd:/hbcbench")
        for name in blobs:
            ratio_up = in_app[name][0] / in_hbc[name][0]
            ratio_down = in_app[name][1] / in_hbc[name][1]
            print(f"  {name}: agent/HBC up {ratio_up:.2f}, down {ratio_down:.2f}")
        print(f"PASS: agent on the Wii ({sends} DOL sends to reach HBC {expected})")
    except (AssertionError, OSError, hbc.HBCError) as exc:
        raise SystemExit(f"FAIL: {exc!r}")
    finally:
        shutil.rmtree(snap, ignore_errors=True)


if __name__ == "__main__":
    main()
