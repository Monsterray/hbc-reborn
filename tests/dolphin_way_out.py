#!/usr/bin/env python3
"""The agent's way out (sdk/hbc_agent/way_out.c) in Dolphin, from the retail
WAD installed in a throwaway profile, with tests/agent_app's wayout mode:
hbc_agent_fatal_now() with the agent's test bits, then the way HBC reports
and the kept log's steps.

    make -C channel && make -C channel/title      # this tree's HBC as a WAD
    make -C tests/agent_app
    python tests/dolphin_way_out.py [channel/title/channel_retail.wad]

  bits 0   IOS closes all 32 descriptors: the reload stub          -> stub
  bits 1   fd 3's close gets no reply: HBC launched through ES      -> es_launch
  bits 5   that, and the launch fails: STM (IOS still acks), then
           HW_RESETS, then the stub anyway (Dolphin ignores both)   -> stub_anyway
  bits 2   IOS acks nothing: HW_RESETS (AHBPROT always reads open
           in Dolphin), then the stub anyway                         -> stub_anyway
  bits 21  stuck, the launch fails, no reset (16): the stub anyway   -> stub_anyway

What Dolphin cannot show: a real IOS wedge (it runs no cIOS, and its IPC
always answers, so the failures here are the test bits), and what a reset
does: Dolphin models neither HW_RESETS bit 0 nor STM's hot reset, so on a
Wii bits 5 and 2 reboot the console instead of falling back on the stub.
"""

import pathlib
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import dolphin_smoke  # noqa: E402
from dolphin_smoke import hbc  # noqa: E402

APP = root / "tests/agent_app/agent_app.dol"

CASES = [
    (0, "stub", ["IOS closed all 32 descriptors", "the reload stub"]),
    (1, "es_launch", ["no reply to the close of fd 3", "HBC through ES"]),
    (5, "stub_anyway", ["no reply to the close of fd 3", "launch refused (test)",
                        "STM hot reset", "system reset (HW_RESETS)",
                        "nothing reset the Wii: the reload stub anyway"]),
    (2, "stub_anyway", ["IOS stopped answering after 0 closes", "system reset (HW_RESETS)",
                        "nothing reset the Wii: the reload stub anyway"]),
    (21, "stub_anyway", ["no reply to the close of fd 3", "launch refused (test)",
                         "no reset (test): the reload stub anyway"]),
]


def run_case(wii, bits, way, steps):
    hbc.request(wii, b"HBCC")
    start = time.monotonic()
    hbc.send(wii, str(APP), ["wayout", str(bits)])
    # The app lives a second, then the way out: back in HBC within a minute.
    time.sleep(3)
    hbc.hbc_wait(wii, 90)
    back = time.monotonic() - start
    crash, kept = hbc.status(wii).get("crash"), hbc.lastlog(wii)
    print(hbc.crash_report(crash) if crash else "no crash report")
    text = kept["text"] if kept else ""
    lines = [l for l in text.splitlines() if l.startswith("agent: way out:")]
    for line in lines:
        print("   ", line)
    assert crash and crash["kind"] == "fatal" and crash["code"] == 0x82, crash
    assert crash["reason"] == f"way out test, bits {bits}", crash
    assert crash.get("way_out") == way, (crash.get("way_out"), way, text[-600:])
    at = 0
    for step in steps:
        found = [i for i, l in enumerate(lines) if step in l and i >= at]
        assert found, f"bits {bits}: no {step!r} in order in {lines}"
        at = found[0]
    print(f"PASS way out, bits {bits}: {way} in {back:.1f} s")


def main():
    image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                         root / "channel/title/channel_retail.wad").resolve()
    if not image.is_file() or not APP.is_file():
        raise SystemExit(f"build {image} and {APP} first")
    run = dolphin_smoke.Dolphin(image, sd=True)
    result = 1
    try:
        dolphin_smoke.wait_version(run, 90)
        for bits, way, steps in CASES:
            run_case(run.address, bits, way, steps)
        result = 0
    except (AssertionError, OSError, hbc.HBCError) as exc:
        print(f"FAIL: {exc!r}")
    finally:
        run.stop()
    if result:
        print(f"kept profile for inspection: {run.profile}")
    sys.exit(result)


if __name__ == "__main__":
    main()
