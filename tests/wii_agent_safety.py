#!/usr/bin/env python3
"""The agent's safety tools on a real Wii (sdk/hbc_agent/safety.c).

Runs tests/agent_app (built with this tree's agent) once per tool and reads
back what HBC and the agent report:

  assert, abort  crash kind and reason, the thread list in the kept log
  deadlock       a hang, with both threads waiting on a mutex in the list
  mallocfail     HBCS alloc_failures (agent_app links with --wrap=malloc)
  flip           HBCS frames: 60 and 30 frames a second, single buffer
  optin          the reload stub guard and memory low points: a change past
                 HBC's stub is seen, put back at exit, and the app still
                 returns to HBC
  reset          the Reset callback the agent installed exits to HBC
  overflow       the stack guard's breakpoint stops a runaway recursion

An HBC older than this tree names the new crash kinds "exception" and the
kept log's "abort" and "stack" "unknown"; the reasons and texts still say
which. Leaves the Wii in HBC.

usage: tests/wii_agent_safety.py [ONLY ...] [--wii WII-IP]
"""

import argparse
import pathlib
import re
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import agent_checks  # noqa: E402
import hbc  # noqa: E402

APP = str(root / "tests/agent_app/agent_app.dol")
THREADS = "-- threads when the app stopped"


def run_until_back(wii, args, seconds=60):
    """Send agent_app with args and wait until it has run and HBC is back;
    return the crash and the kept log HBC has. A quick crash can be over
    before the agent is ever seen, so HBC's report also counts."""
    hbc.request(wii, b"HBCC")
    hbc.send(wii, APP, args)
    deadline, seen = time.monotonic() + seconds, False
    while time.monotonic() < deadline:
        try:
            text = hbc.version(wii, hbc.POLL_TIMEOUT)
        except (OSError, hbc.HBCError):
            time.sleep(0.5)
            continue
        if hbc.is_agent(text):
            seen = True
        else:
            st = hbc.status(wii)
            if seen or st.get("crash"):
                time.sleep(1)
                return hbc.status(wii).get("crash"), hbc.lastlog(wii)
        time.sleep(0.5)
    w, h, data = hbc.screen(wii)
    shot = pathlib.Path(tempfile.gettempdir()) / f"agent-safety-{args[0]}.png"
    shot.write_bytes(hbc.yuyv_png(w, h, data))
    raise AssertionError(f"{args[0]}: the app did not run and return within {seconds} s "
                         f"(screen: {shot})")


def kind_ok(crash, kind):
    return crash and crash["kind"] in (kind, "exception")


def check_assert(wii):
    crash, kept = run_until_back(wii, ["assert"])
    assert kind_ok(crash, "assert"), crash
    assert re.fullmatch(r"main\.c:\d+: argc == 99", crash["reason"]), crash
    assert "assertion \"argc == 99\" failed" in kept["text"], kept["text"][-400:]
    assert THREADS in kept["text"], kept["text"][-400:]
    print(f"PASS assert: {crash['reason']}; kept log ends with the thread list")
    print("   ", "\n    ".join(kept["text"].rstrip().splitlines()[-4:]))


def check_abort(wii):
    crash, kept = run_until_back(wii, ["abort"])
    assert kind_ok(crash, "abort") and crash["reason"] == "abort() called", crash
    assert "agent_app: abort()" in kept["text"] and THREADS in kept["text"], kept["text"][-400:]
    print(f"PASS abort: {crash['reason']}")


def check_deadlock(wii):
    crash, kept = run_until_back(wii, ["deadlock"], 90)
    assert crash and crash["kind"] == "hang", crash
    lines = kept["text"][kept["text"].index(THREADS):].splitlines()
    waiting = [l for l in lines if "on a mutex" in l]
    assert len(waiting) >= 2, lines
    print(f"PASS deadlock: a hang, {len(waiting)} threads on a mutex:")
    print("   ", "\n    ".join(lines[1:]))


def agent_status(wii, args, settle=4):
    hbc.send(wii, APP, args)
    agent_checks.wait_agent(wii)
    time.sleep(settle)
    return hbc.status(wii)


def check_mallocfail(wii):
    st = agent_status(wii, ["mallocfail"])
    hbc.exit_app(wii)
    fails = st["safety"]["alloc_failures"]
    assert fails and fails["count"] >= 1 and fails["last_size"] == 1536 << 20, fails
    print(f"PASS mallocfail: {fails}")


def check_flip(wii):
    rows = {}
    for n in (1, 2, 0):
        st = agent_status(wii, ["flip", str(n)], 5)
        rows[n] = st["safety"]["frames"]
        hbc.exit_app(wii)
    assert 58 <= rows[1]["fps"] <= 61, rows
    assert 29 <= rows[2]["fps"] <= 31, rows
    assert rows[0]["single_buffer"] and rows[0]["fps"] == 0, rows
    print(f"PASS frames: flip 1 {rows[1]}, flip 2 {rows[2]}, never {rows[0]}")


def check_optin(wii):
    st = agent_status(wii, ["stubsmash", "optin"], 4)
    sf = st["safety"]
    assert sf["stub"] and sf["stub"]["changed_at_ms"] > 0, sf
    assert sf["mem_low"] and sf["mem_low"]["mem2"] > 0, sf
    hbc.exit_app(wii)
    kept = hbc.lastlog(wii)
    assert "putting back the reload stub" in kept["text"], kept["text"][-400:]
    print(f"PASS optin: stub change seen at {sf['stub']['changed_at_ms']} ms and put back, "
          f"lowest free {sf['mem_low']}; cost {sf['cost']}")


def check_reset(wii):
    crash, kept = run_until_back(wii, ["reset"])
    assert crash is None, crash
    assert kept["why"] == "exit" and "Reset pressed" in kept["text"], kept
    print("PASS reset: the agent's Reset callback exited to HBC")


def check_overflow(wii):
    crash, kept = run_until_back(wii, ["overflow"])
    assert kind_ok(crash, "stack"), crash
    assert crash["reason"].startswith("main thread stack overflow"), crash
    via = "breakpoint" if int(crash["dsisr"], 16) & 0x00400000 else "marker"
    assert THREADS in kept["text"], kept["text"][-400:]
    print(f"PASS overflow: {crash['reason']} (by the {via}), pc {crash['pc']}, "
          f"dar {crash['dar']}")


CHECKS = {"assert": check_assert, "abort": check_abort, "deadlock": check_deadlock,
          "mallocfail": check_mallocfail, "flip": check_flip, "optin": check_optin,
          "reset": check_reset, "overflow": check_overflow}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("only", nargs="*", help=" ".join(CHECKS))
    parser.add_argument("--wii")
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    unknown = set(opts.only) - set(CHECKS)
    if unknown:
        parser.error(f"unknown checks: {', '.join(sorted(unknown))}")
    print(f"HBC {hbc.hbc_wait(wii, 90)} on {wii}")
    failed = []
    for name in opts.only or list(CHECKS):
        try:
            CHECKS[name](wii)
        except (AssertionError, hbc.HBCError, OSError) as exc:
            print(f"FAIL {name}: {exc!r}")
            failed.append(name)
            hbc.exit_app(wii)
            hbc.hbc_wait(wii, 90)
    if failed:
        raise SystemExit(f"failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
