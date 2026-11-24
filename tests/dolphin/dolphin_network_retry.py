#!/usr/bin/env python3
"""A failed network start is cleaned up, then retried without a reload.

Requires an HBC_TEST_INIT_FAILURE build (DOL or WAD), which fails the first
start that would have succeeded. Never run that build on a Wii. With no
button pressed, HBC must clean up the failed start and come back on the
network through the TCP thread's own retry, without the stuck-cleanup
countdown or a relaunch: Dolphin's log shows no IOS start after HBC's own,
and HBC counts one successful network start.
Usage: python3 tests/dolphin/dolphin_network_retry.py TEST-IMAGE
"""
import pathlib
import shutil
import subprocess
import sys
import time

import dolphin_smoke as smoke

hbc = smoke.hbc
IOS_START = "Starting IOS "
NET_OPEN = "Opening /dev/net/ip/top"


def count(run, text):
    log = run.profile / "Logs/dolphin.log"
    return log.read_text(errors="replace").count(text) if log.exists() else 0


def main():
    image = pathlib.Path(sys.argv[1]).resolve()
    run = smoke.Dolphin(image)
    started = time.monotonic()
    passed = False
    try:
        # The injected failure, its cleanup and the 2 s retry all come first.
        smoke.wait_version(run, 90)
        online = time.monotonic() - started
        ios_starts = count(run, IOS_START)
        # The failed start was cleaned up (net_deinit closes the IP device)
        # and started again: a clean boot opens it once.
        opens = count(run, NET_OPEN)
        assert opens >= 2, f"the IP device opened {opens} time(s): no failed start retried"
        assert hbc.status(run.address)["net_starts"] == 1
        # Long enough for the 5 s watchdog and the 10 s countdown, had either run.
        time.sleep(20)
        assert count(run, IOS_START) == ios_starts, "IOS reloaded: HBC relaunched"
        status = hbc.status(run.address)
        assert status["net_starts"] == 1, status["net_starts"]
        assert status["version"] == smoke.expected
        assert not run.faults(), run.faults()[:10]
        print(f"PASS: failed start retried with no input and no reload; online "
              f"{online:.0f} s after launch", flush=True)
        passed = True
    finally:
        if run.dolphin.endswith(".app"):
            subprocess.run(["pkill", "-KILL", "-f", str(run.profile)], check=False)
        run.stop()
        if passed:
            shutil.rmtree(run.profile, ignore_errors=True)
        else:
            print(f"Retained profile: {run.profile}", flush=True)


if __name__ == "__main__":
    main()
