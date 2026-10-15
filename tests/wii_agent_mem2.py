#!/usr/bin/env python3
"""The agent and the app's MEM2, on a Wii.

Runs tests/agent_app's mem2 mode (built with this tree's agent): the app
fills HBC's records in MEM2 (0x91800000, the netlog copy, crash block and
kept log) as an app's heap there would; meanwhile this sets a log target
(HBCN, what `hbc.py log` sends), and checks HBCS agent_mem: the agent's
lasting memory came from the top of the MEM2 arena. The app then reports
whether anything wrote into its records (nothing may), and exits; back in
HBC, the log target must be there (the agent copies it to MEM2 as the app
stops) and the app's kept log too. The log target is cleared at the end.

usage: tests/wii_agent_mem2.py [WII-IP] [--dol AGENT_APP.dol]
"""

import pathlib
import struct
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

PORT = 4406   # a target nobody listens on: only its record matters


def main():
    args = sys.argv[1:]
    dol = root / "tests/agent_app/agent_app.dol"
    if "--dol" in args:
        dol = pathlib.Path(args.pop(args.index("--dol") + 1))
        args.remove("--dol")
    wii = hbc.wii_address(args[0] if args else None)
    failed = []

    def check(ok, what):
        print(f"{'PASS' if ok else 'FAIL'} {what}", flush=True)
        if not ok:
            failed.append(what)

    hbc.request(wii, b"HBCN" + struct.pack(">H", 0))
    hbc.send(wii, str(dol), ["mem2", "20"])
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            if hbc.is_agent(hbc.version(wii, hbc.POLL_TIMEOUT)):
                break
        except (OSError, hbc.HBCError):
            pass
        time.sleep(0.5)
    try:
        st = hbc.status(wii)
        mem = st.get("agent_mem", {})
        check(mem.get("from") == "arena2_top" and int(mem.get("addr", "0"), 16) >= 0x93000000,
              f"agent's lasting memory at the top of MEM2: {mem}")
        check("arena2_free" in st and "mem2_free" not in st,
              f"status names the SYS arena: arena2_free {st.get('arena2_free')}")
        hbc.request(wii, b"HBCN" + struct.pack(">H", PORT))
        print(f"log target set to this PC:{PORT} while the app runs", flush=True)
        while time.monotonic() < deadline + 60:
            try:
                if not hbc.is_agent(hbc.version(wii, hbc.POLL_TIMEOUT)):
                    break
            except (OSError, hbc.HBCError):
                pass
            time.sleep(0.5)
        time.sleep(2)
        st = hbc.status(wii)
        log = (hbc.lastlog(wii) or {}).get("text", "")
        print("  " + "\n  ".join(l for l in log.splitlines() if "records" in l), flush=True)
        check("agent_app: records untouched" in log, "nothing wrote into the records while it ran")
        check((st.get("log") or "").endswith(f":{PORT}"),
              f"HBC has the log target from the app's stop: {st.get('log')}")
    finally:
        hbc.request(wii, b"HBCN" + struct.pack(">H", 0))
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
