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
the Wii, and where it lands depends on Priiloader: --reset 2, 5 or 13 (13:
stuck, no launch, AHBPROT read as closed, so STM's hot reset)
runs one, only with someone at the Wii, who brings it back to HBC.

    python C:/tools/wii-bench/wiibench.py add --wii dev --name "way out" \\
        --cwd <this tree> -- <python> tests/wii_way_out.py
"""

import argparse
import os
import pathlib
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402
from wii_agent_safety import APP  # noqa: E402

PRODUCTION = {"192.168.8.200", "192.168.8.243"}
WIICAP = pathlib.Path("C:/tools/wii-capture/wiicap.py")
CASES = {
    0: ("stub", ["IOS closed all 32 descriptors", "the reload stub"]),
    17: ("es_launch", ["no reply to the close of fd 3", "HBC through ES"]),
    21: ("stub_anyway", ["no reply to the close of fd 3", "launch refused (test)",
                         "nothing reset the Wii: the reload stub anyway"]),
}


def run_back(wii, bits, seconds=120):
    """Send agent_app wayout BITS; return HBC's crash and kept log once HBC
    (not the app) answers with a kept log from this run. The app lives a
    second, and an HBC before 1.10.14 cannot read the version 3 crash block,
    so the kept log, which every HBC serves, says the run is over."""
    hbc.request(wii, b"HBCC")
    hbc.send(wii, APP, ["wayout", str(bits)])
    mark = f"way out test, bits {bits},"
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        time.sleep(1)
        try:
            if hbc.is_agent(hbc.version(wii, 3)):
                continue
            kept = hbc.lastlog(wii)
        except (OSError, hbc.HBCError):
            continue
        if kept and mark in kept["text"]:
            return hbc.status(wii).get("crash"), kept
    raise AssertionError(f"bits {bits}: no kept log from this run within {seconds} s")


def check(wii, bits, way, steps):
    start = time.monotonic()
    crash, kept = run_back(wii, bits)
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


def hbc_answers(wii):
    """HBC itself answers: not the test app's agent, which answers too
    until the way out runs."""
    try:
        return not hbc.is_agent(hbc.version(wii, 3))
    except (OSError, hbc.HBCError):
        return False


def reset_run(wii, bits, seconds=240):
    """One reset: the TV recorded through the capture card (the dev Wii's),
    then wait for HBC to stop answering and answer again (Priiloader's
    autoboot, or someone at the Wii), and show what survived the reset."""
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    video = None
    if WIICAP.is_file():
        video = subprocess.Popen([sys.executable, str(WIICAP), "video", str(seconds),
                                  "--out", str(out / f"reset-{bits}.mp4")])
        time.sleep(5)   # the card's first frames are dark
    hbc.request(wii, b"HBCC")
    start = time.monotonic()
    hbc.send(wii, APP, ["wayout", str(bits)])
    # HBC goes quiet while it starts the app; then nothing answers once the
    # Wii resets; then HBC answers again, and keeps answering.
    gone = None
    while time.monotonic() - start < seconds:
        up = hbc_answers(wii)
        now = time.monotonic() - start
        if gone is None and not up and now > 2:
            gone = now
            print(f"bits {bits}: HBC stopped answering at {gone:.1f} s")
        if gone is not None and up and now > 15:
            time.sleep(5)
            if hbc_answers(wii):
                print(f"bits {bits}: HBC answered again at {now:.1f} s")
                break
        time.sleep(1)
    else:
        raise SystemExit(f"bits {bits}: HBC did not come back within {seconds} s")
    st = hbc.status(wii)
    print(f"HBC {st.get('version')} back; crash block: "
          f"{'kept' if st.get('crash') else 'none (lost, or an HBC before 1.10.14)'}")
    if st.get("crash"):
        print(hbc.crash_report(st["crash"]))
    kept = hbc.lastlog(wii)
    lines = [l for l in (kept or {}).get("text", "").splitlines() if l.startswith("agent: way out:")]
    print(f"kept log: {'kept, why ' + kept['why'] if kept else 'none'}")
    for line in lines:
        print("   ", line)
    if video:
        video.wait()
        print(f"TV recording: {out / f'reset-{bits}.mp4'}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wii")
    parser.add_argument("--reset", type=int, choices=(2, 5, 13),
                        help="one reset run; someone must be at the Wii")
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    if wii in PRODUCTION:
        raise SystemExit(f"{wii} is a production Wii: the way-out tests run on the bench Wii only")
    if not os.environ.get("WII_BENCH_JOB"):
        raise SystemExit("run this as a wii-bench queue job (wiibench.py add --wii dev ...)")
    print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}")
    if opts.reset:
        reset_run(wii, opts.reset)
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
