#!/usr/bin/env python3
"""Measure developer-network throughput on a real Wii (or Dolphin).

Sends the channel DOL through the installed HBC with Wiiload, then times
status round trips and verified uploads and downloads of random and zero
data under sd:/hbcbench, which it removes afterwards.

usage: tests/wii_netbench.py [--no-send] [--mib N] [WII-IP]
"""

import argparse
import os
import pathlib
import shutil
import statistics
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402


def timed(fn, *args):
    start = time.perf_counter()
    result = fn(*args)
    return time.perf_counter() - start, result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wii", nargs="?")
    parser.add_argument("--no-send", action="store_true", help="use the running HBC")
    parser.add_argument("--mib", type=int, default=4)
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    expected = dolphin_smoke.expected

    if not opts.no_send:
        snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-wii-")) / "channelapp-channel.dol"
        shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
        elapsed, _ = timed(hbc.send, wii, str(snap), [])
        size = snap.stat().st_size
        print(f"wiiload send: {size} bytes in {elapsed:.2f} s")
        shutil.rmtree(snap.parent, ignore_errors=True)
        start = time.monotonic()
        hbc.relaunch_wait(wii, expected)
        print(f"HBC {expected} answered {time.monotonic() - start:.1f} s after the send")

    while hbc.status(wii)["device"] != "sd":
        time.sleep(1)

    latencies = [timed(hbc.status, wii)[0] * 1000 for _ in range(20)]
    print(f"status round trip: median {statistics.median(latencies):.1f} ms, "
          f"max {max(latencies):.1f} ms")

    n = opts.mib << 20
    elf = (root / "channel/channelapp/channelapp_nopax.elf").read_bytes()
    samples = (("random", os.urandom(n)), ("zeros", bytes(n)), ("elf", elf))
    methods = (("raw", lambda p, d: hbc.file_request(wii, "P", p, len(d), d),
                lambda p: hbc.file_request(wii, "G", p)),
               ("framed", lambda p, d: hbc.put_file(wii, p, d),
                lambda p: hbc.get_file(wii, p)))
    failed = False
    for name, data in samples:
        path = f"sd:/hbcbench/{name}.bin"
        for method, put, get in methods:
            t_put, _ = timed(put, path, data)
            up = hbc.status(wii).get("last", {})
            t_get, got = timed(get, path)
            down = hbc.status(wii).get("last", {})
            ok = got == data
            failed |= not ok
            mb = len(data) / 1e6
            print(f"{name:6} {method:6} {mb:5.2f} MB: put {mb / t_put:5.2f} MB/s "
                  f"(wire {up.get('wire', 0) / 1e6:.2f} MB, net {up.get('net_ms')} "
                  f"sd {up.get('disk_ms')} cpu {up.get('cpu_ms')} ms), "
                  f"get {mb / t_get:5.2f} MB/s (wire {down.get('wire', 0) / 1e6:.2f} MB, "
                  f"net {down.get('net_ms')} sd {down.get('disk_ms')} cpu {down.get('cpu_ms')} ms) "
                  f"verified {ok}")
            hbc.file_request(wii, "D", path)
    hbc.file_request(wii, "D", "sd:/hbcbench")
    if failed:
        raise SystemExit("FAIL: a download did not match its upload")

if __name__ == "__main__":
    main()
