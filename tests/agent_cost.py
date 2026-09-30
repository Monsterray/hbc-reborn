#!/usr/bin/env python3
"""Measure what sdk/hbc_agent costs, on a real Wii and in the build.

Runs tests/agent_app (the agent in a small app) and reads back:
  start-up    heap and arena hbc_agent_init() takes (the app prints it)
  idle        the agent thread's wake-ups and time awake, over 20 s
  transfers   heap growth and agent time for framed uploads and downloads
  overlay     the HOME overlay's frame time and borrowed memory, in the app
              and in this tree's HBC (sent over Wiiload if the Wii runs
              another version)
and prints them with the agent's code and static data sizes from the build.
Leaves the Wii in HBC.

usage: tests/agent_cost.py [--log-port PORT] [WII-IP]
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import agent_checks  # noqa: E402
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402


def sizes():
    """text, data, bss of each object in libhbcagent.a, and what HBC links."""
    tool = hbc.devkit_tool("powerpc-eabi-size")
    lib = root / "sdk/hbc_agent/libhbcagent.a"
    out = subprocess.run([tool, str(lib)], capture_output=True, text=True).stdout
    objs = {}
    for line in out.splitlines()[1:]:
        f = line.split()
        if len(f) >= 6:
            objs[f[5].split("(")[0]] = tuple(int(x) for x in f[:3])
    return objs


def run_overlay(wii, keys_seq):
    for keys, wait in keys_seq:
        hbc.send_keys(wii, keys)
        time.sleep(wait + 0.25 * len(keys))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wii", nargs="?")
    parser.add_argument("--log-port", type=int, default=hbc.LOG_PORT)
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    expected = dolphin_smoke.expected
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="hbc-cost-"))
    dol = tmp / "channelapp-channel.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", dol)
    shutil.copy(agent_checks.APP, tmp / "agent_app.dol")
    report = {"objects": sizes()}

    hbc.hbc_wait(wii, 90)
    log = agent_checks.LogCollector(opts.log_port)
    try:
        log.server.register(wii)
        hbc.send(wii, str(tmp / "agent_app.dol"), ["stay", "240"])
        agent_checks.wait_agent(wii)
        log.wait_for("agent_app: network 0", 30)
        m = re.search(r"init cost heap (-?\d+) bytes \(in use\), arena1 (-?\d+), arena2 (-?\d+)",
                      log.text)
        report["init"] = {"heap": int(m[1]), "arena1": int(m[2]), "arena2": int(m[3])}

        # Idle: nothing talks to the agent for 20 s.
        s0 = hbc.status(wii)
        time.sleep(20)
        s1 = hbc.status(wii)
        span_us = (s1["uptime_ms"] - s0["uptime_ms"]) * 1000
        wakes = s1["agent_idle_wakes"] - s0["agent_idle_wakes"]
        awake = s1["agent_idle_us"] - s0["agent_idle_us"]
        report["idle"] = {"seconds": span_us / 1e6, "wakes": wakes, "awake_us": awake,
                          "cpu_percent": 100 * awake / span_us,
                          "stack_used": s1["agent_stack_used"],
                          "stack_size": s1["agent_stack_size"]}

        # Transfers: 2 MiB random and a 5.3 MB ELF, up then down.
        blobs = {"random": os.urandom(2 * 1024 * 1024),
                 "elf": (root / "channel/channelapp/channelapp_nopax.elf").read_bytes()}
        hbc.file_request(wii, "M", "sd:/hbcbench")
        before = hbc.status(wii)
        rows = []
        for name, blob in blobs.items():
            path = f"sd:/hbcbench/cost-{name}.bin"
            t = time.monotonic()
            hbc.put_file(wii, path, blob)
            up = hbc.status(wii)
            t_up = time.monotonic() - t
            t = time.monotonic()
            assert hbc.get_file(wii, path) == blob, name
            t_down = time.monotonic() - t
            down = hbc.status(wii)
            hbc.file_request(wii, "D", path)
            rows.append({"name": name, "bytes": len(blob),
                         "up_mb_s": len(blob) / t_up / 1e6, "down_mb_s": len(blob) / t_down / 1e6,
                         "up_cpu_ms": up["last"]["cpu_ms"], "down_cpu_ms": down["last"]["cpu_ms"]})
        after = hbc.status(wii)
        hbc.file_request(wii, "D", "sd:/hbcbench")
        report["transfers"] = {"rows": rows,
                               "heap_arena_growth": after["heap_arena"] - before["heap_arena"],
                               "heap_free_after": after["heap_free"],
                               "stack_used": after["agent_stack_used"]}

        # The overlay in the app: open, DEV, WiiMote, close.
        run_overlay(wii, [("h", 2), ("llla", 2), ("b", 1), ("rrrra", 2), ("h", 3)])
        report["overlay_app"] = hbc.status(wii).get("overlay")
        hbc.exit_app(wii)

        # The overlay as HBC's HOME menu, in this tree's HBC.
        if hbc.hbc_wait(wii, 90) != expected:
            hbc.send(wii, str(dol), [])
            hbc.relaunch_wait(wii, expected)
        time.sleep(10)  # HBC takes HOME only once its app list is up
        run_overlay(wii, [("h", 2), ("la", 2), ("b", 1), ("h", 3)])
        report["overlay_hbc"] = hbc.status(wii).get("overlay")
    finally:
        hbc.LogServer.unregister(wii)
        log.close()
        shutil.rmtree(tmp, ignore_errors=True)

    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
