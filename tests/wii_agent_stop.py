#!/usr/bin/env python3
"""hbc_agent_stop() and hbc_agent_listen() on a real Wii.

Runs tests/agent_app (built with this tree's agent) in four modes:

  stop       stop (twice), unmount, WPAD off, IOS_ReloadIOS(58), 12 s past
             the 5 s hang watchdog, then exit(0): no hang or crash reported,
             back in HBC, and what the app wrote (sd:/hbctest/agent_stop.txt):
             both stops 0, every call after it inert, the network statuses
             it saw after the reload
  restart    the same, then hbc_agent_init() again: the agent answers
  listen     hbc_agent_listen(false): the app's own server answers on 4299;
             hbc_agent_listen(true): the agent answers again
  stopcrash  stop, then a crash: libogc's own screen, nothing for HBC

Leaves the Wii in HBC.

usage: tests/wii_agent_stop.py [ONLY ...] [--wii WII-IP] [--dol AGENT_APP.dol]
"""

import argparse
import pathlib
import socket
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

RESULT = "sd:/hbctest/agent_stop.txt"


def wait_for(fn, seconds, what):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            value = fn()
            if value:
                return value
        except (OSError, hbc.HBCError):
            pass
        time.sleep(0.5)
    raise hbc.HBCError(f"no {what} within {seconds} s")


def agent_up(wii):
    return hbc.is_agent(hbc.version(wii, hbc.POLL_TIMEOUT))


def hbc_up(wii):
    return not hbc.is_agent(hbc.version(wii, hbc.POLL_TIMEOUT))


def result_file(wii):
    lines = hbc.get_file(wii, RESULT).decode().splitlines()
    hbc.file_request(wii, "D", RESULT)
    return lines


def check_stopped(lines):
    """What agent_app wrote after its stop and IOS reload."""
    print("  " + "\n  ".join(lines), flush=True)
    want = {"stop 0", "again 0", "exit_requested 0", "home_pending 0", "home -19", "listen 0"}
    missing = want - set(lines)
    assert not missing, f"agent_stop.txt lacks {sorted(missing)}"
    reload = next(l for l in lines if l.startswith("reload "))
    assert not reload.startswith("reload -"), "IOS_ReloadIOS(58) failed"
    # net_get_status() each second after the reload: 0 (up) or -16 (-EBUSY,
    # starting) would mean something started the network again.
    statuses = reload.split("net", 1)[1].split()
    assert not {"0", "-16"} & set(statuses), f"the network started after the stop: {statuses}"


def run_stop(wii, dol):
    hbc.request(wii, b"HBCC")
    hbc.send(wii, dol, ["stop"])
    time.sleep(5)
    wait_for(lambda: hbc_up(wii), 60, "HBC")
    time.sleep(2)
    crash = hbc.status(wii).get("crash")
    assert not crash, f"a report after the stop: {crash}"
    check_stopped(result_file(wii))


def run_restart(wii, dol):
    hbc.send(wii, dol, ["restart"])
    time.sleep(16)   # the stop, the reload and its 12 s
    wait_for(lambda: agent_up(wii) and hbc.status(wii)["uptime_ms"] < 30000, 60,
             "agent after hbc_agent_init() again")
    st = hbc.status(wii)
    print(f"  agent again: uptime {st['uptime_ms']} ms, IOS {st['ios']}", flush=True)
    hbc.exit_app(wii)
    check_stopped(result_file(wii))


def ask_app(wii):
    with socket.create_connection((wii, 4299), timeout=3) as s:
        s.sendall(b"PING" + bytes(12))
        return s.recv(8)


def run_listen(wii, dol):
    hbc.send(wii, dol, ["listen"])
    wait_for(lambda: agent_up(wii), 60, "agent")
    reply = wait_for(lambda: ask_app(wii) == b"MINE" and b"MINE", 30, "the app's own server")
    print(f"  the app's server on 4299 answered {reply!r}", flush=True)
    wait_for(lambda: agent_up(wii), 15, "agent after hbc_agent_listen(true)")
    log = (hbc.lastlog(wii) or {}).get("text", "")
    print("  " + "\n  ".join(l for l in log.splitlines() if "listen" in l), flush=True)
    assert "listen(false) 0, bind 0" in log, "listen(false) or the app's bind failed"
    hbc.exit_app(wii)


def run_stopcrash(wii, dol):
    hbc.request(wii, b"HBCC")
    hbc.send(wii, dol, ["stopcrash"])
    time.sleep(5)
    wait_for(lambda: hbc_up(wii), 60, "HBC after libogc's crash screen")
    time.sleep(2)
    crash = hbc.status(wii).get("crash")
    assert not crash, f"the agent reported a crash after its stop: {crash}"
    print("  libogc's crash screen, then HBC; no report", flush=True)


TESTS = {"stop": run_stop, "restart": run_restart, "listen": run_listen,
         "stopcrash": run_stopcrash}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("only", nargs="*", help=" ".join(TESTS))
    ap.add_argument("--wii")
    ap.add_argument("--dol", default=str(root / "tests/agent_app/agent_app.dol"))
    args = ap.parse_args()
    wii = hbc.wii_address(args.wii)
    failed = []
    unknown = set(args.only) - set(TESTS)
    if unknown:
        raise SystemExit(f"no test {sorted(unknown)}; there are {', '.join(TESTS)}")
    for name in args.only or list(TESTS):
        print(f"{name}:", flush=True)
        try:
            TESTS[name](wii, args.dol)
            print(f"PASS {name}", flush=True)
        except (OSError, hbc.HBCError, AssertionError, KeyError) as exc:
            print(f"FAIL {name}: {exc}", flush=True)
            failed.append(name)
            wait_for(lambda: hbc_up(wii) or hbc.exit_app(wii), 120, "HBC")
    print(f"back in HBC {hbc.hbc_wait(wii, 90)}")
    if failed:
        raise SystemExit(f"failed: {', '.join(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
