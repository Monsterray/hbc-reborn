#!/usr/bin/env python3
"""Launch an app and come back to HBC, over and over, on a Wii: a queue job
for the random crashes (HBC's Guru Meditation, the network going quiet, the
SD card gone) seen with HBC built on libogc 3.2.1.

Each round sends tests/agent_app/agent_app.dol with "exit", waits for HBC to
answer again, then lists sd:/ and reads HBC's status. It stops at the first
round that fails and says which step.

    python tests/wii_launch_stress.py [--rounds N] [--hbc DOL]

--hbc sends that HBC DOL before every round: an app's exit goes back to the
installed channel, so each launch is then made by the sent build.
"""

import argparse
import os
import pathlib
import sys
import time

root = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=25)
    ap.add_argument("--hbc")
    a = ap.parse_args()
    wii = os.environ.get("WII_BENCH_IP") or os.environ["HBC_WII"]
    app = str(root / "tests/agent_app/agent_app.dol")

    print(f"start: HBC {hbc.version(wii)}", flush=True)

    for n in range(1, a.rounds + 1):
        t = time.monotonic()
        step = "send hbc"
        try:
            if a.hbc:
                # every round: the app's exit went back to the installed one
                try:
                    hbc.send(wii, a.hbc, [])
                except (OSError, hbc.HBCError):
                    pass  # HBC itself: the send times out but loads
                time.sleep(3)
                hbc.hbc_wait(wii, 90)
            step = "send"
            hbc.send(wii, app, ["exit"])
            time.sleep(3)  # HBC answers for a moment before the app starts
            step = "back"
            v = hbc.hbc_wait(wii, 90)
            step = "sd"
            sd = len(hbc.list_dir(wii, "sd:/")[0])
            step = "status"
            s = hbc.status(wii)
        except (OSError, hbc.HBCError) as exc:
            print(f"FAIL round {n} at {step}: {exc!r}", flush=True)
            return 1
        print(f"round {n}: HBC {v}, sd:/ {sd} entries, inserted {s.get('inserted')}, "
              f"heap {s.get('heap_free')}, {time.monotonic() - t:.1f} s", flush=True)
    print("PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
