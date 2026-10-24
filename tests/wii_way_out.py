#!/usr/bin/env python3
"""The agent's way out (sdk/hbc_agent/way_out.c) on a real Wii, from a bench
queue job. tests/agent_app's wayout mode calls hbc_agent_fatal_now() with
the agent's test bits; the app prints each step on the TV, and the kept log
(any HBC) and the crash block (HBC 1.10.14 on) say which way it left.

  0    IOS closes all 32 descriptors: the reload stub              -> stub
  17   fd 3's close gets no reply: HBC launched through ES; if the
       launch fails, the stub (16: never a reset)                  -> es_launch
  21   that, and the launch fails: the stub anyway, no reset       -> stub_anyway

All three come back to HBC by themselves. The resets (bits 2: IOS acks
nothing, then HW_RESETS; 5: the launch fails, then HW_RESETS or STM) reboot
the Wii, and where it lands depends on Priiloader: --reset 2 or --reset 5
runs one, only with someone at the Wii, who brings it back to HBC.

    python C:/tools/wii-bench/wiibench.py add --wii dev --name "way out" \\
        --cwd <this tree> -- <python> tests/wii_way_out.py
"""

import argparse
import os
import pathlib
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402
from wii_agent_safety import APP, run_until_back  # noqa: E402

PRODUCTION = {"192.168.8.200", "192.168.8.243"}
CASES = {
    0: ("stub", ["IOS closed all 32 descriptors", "the reload stub"]),
    17: ("es_launch", ["no reply to the close of fd 3", "HBC through ES"]),
    21: ("stub_anyway", ["no reply to the close of fd 3", "launch refused (test)",
                         "nothing reset the Wii: the reload stub anyway"]),
}


def check(wii, bits, way, steps):
    start = time.monotonic()
    crash, kept = run_until_back(wii, ["wayout", str(bits)], 120)
    back = time.monotonic() - start
    text = kept["text"] if kept else ""
    lines = [l for l in text.splitlines() if l.startswith("agent: way out:")]
    print(f"bits {bits}: back in HBC after {back:.1f} s")
    for line in lines:
        print("   ", line)
    if crash:
        print(hbc.crash_report(crash))
        assert crash["reason"] == f"way out test, bits {bits}", crash
        if "way_out" in crash:   # an installed HBC 1.10.14 or later
            assert crash["way_out"] == way, (crash["way_out"], way)
    at = 0
    for step in steps:
        found = [i for i, l in enumerate(lines) if step in l and i >= at]
        assert found, f"bits {bits}: no {step!r} in order in {lines}"
        at = found[0]
    print(f"PASS way out, bits {bits}: {way}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wii")
    parser.add_argument("--reset", type=int, choices=(2, 5),
                        help="one reset run; someone must be at the Wii")
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    if wii in PRODUCTION:
        raise SystemExit(f"{wii} is a production Wii: the way-out tests run on the bench Wii only")
    if not os.environ.get("WII_BENCH_JOB"):
        raise SystemExit("run this as a wii-bench queue job (wiibench.py add --wii dev ...)")
    print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}")
    if opts.reset:
        hbc.send(wii, APP, ["wayout", str(opts.reset)])
        print(f"bits {opts.reset}: sent; the Wii should reset within about 5 s. Bring it back "
              "to HBC; then `hbc.py lastlog` and `hbc.py crash` show what survived.")
        return
    failed = []
    for bits, (way, steps) in CASES.items():
        try:
            check(wii, bits, way, steps)
        except (AssertionError, hbc.HBCError, OSError) as exc:
            print(f"FAIL bits {bits}: {exc!r}")
            failed.append(bits)
            hbc.hbc_wait(wii, 90)
    if failed:
        raise SystemExit(f"failed: {failed}")


if __name__ == "__main__":
    main()
