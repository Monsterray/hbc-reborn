#!/usr/bin/env python3
"""File times and attributes from HBC and from an app's agent (libogc 3 and
libogc2 builds of tests/agent_app), on a Wii.

Runs this tree's channel DOL through the installed HBC, then for it and each
agent_app build: uploads a file, and checks that its listed and stat'ed time
(op L, S) is when it was written by the Wii's clock (HBCS "time"), give or
take FAT's 2 s steps; then syncs a folder with a 50 MB file twice and checks
the second sync takes the file as the same from its date, without the Wii
reading it (no checksum request); and sets and clears every FAT attribute
(agent_checks.check_attrs), each libogc clearing them its own way. Ends in the installed HBC.

usage: tests/wii_file_meta.py [WII-IP]
"""

import os
import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import agent_checks  # noqa: E402
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402

DIR = "sd:/hbctest/meta"


def check_times(wii, who):
    """A fresh file's time against the Wii's clock; returns failures."""
    failed = []
    wii_now = hbc.status(wii)["time"]
    hbc.put_file(wii, f"{DIR}/fresh.txt", b"written now\n")
    e = hbc.stat_remote(wii, f"{DIR}/fresh.txt")
    listed = {x["name"]: x for x in hbc.list_entries(wii, DIR)[0]}
    skew = e["mtime"] - wii_now
    print(f"  {who}: stat {e}; Wii clock {hbc.wii_time_text(wii_now)}, "
          f"file {hbc.wii_time_text(e['mtime'])} ({skew:+d} s)", flush=True)
    if not -3 <= skew <= 30:
        failed.append(f"{who}: file time {skew:+d} s from the Wii's clock")
    if listed.get("fresh.txt", {}).get("mtime") != e["mtime"]:
        failed.append(f"{who}: listing and stat disagree")
    if e["readonly"]:
        failed.append(f"{who}: a new file listed read-only")
    return failed


def check_attributes(wii, who):
    """FAT attributes (op A): every one set and cleared, read-only kept by
    writes and deletes. Clearing goes another way on each libogc (devfile.c)."""
    try:
        agent_checks.check_attrs(wii, f"{DIR}/attrs")
        return []
    except (AssertionError, OSError, hbc.HBCError) as exc:
        return [f"{who}: attributes: {exc!r}"]


def check_sync(wii, who, work):
    hbc.sync(wii, str(work), f"{DIR}/sync", out=lambda _: None)
    time.sleep(3)
    t = time.monotonic()
    res = hbc.sync(wii, str(work), f"{DIR}/sync", out=lambda _: None)
    took = time.monotonic() - t
    print(f"  {who}: second sync {res} in {took:.1f} s", flush=True)
    return [] if res == (0, 2, 0, 0) and took < 5 else [f"{who}: second sync {res}, {took:.1f} s"]


def run_app(wii, dol):
    hbc.send(wii, str(dol), ["stay", "200"])
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            if hbc.is_agent(hbc.version(wii, hbc.POLL_TIMEOUT)):
                time.sleep(2)
                return
        except (OSError, hbc.HBCError):
            pass
        time.sleep(0.5)
    raise hbc.HBCError(f"{dol.name} did not answer")


def main():
    wii = hbc.wii_address(sys.argv[1] if len(sys.argv) > 1 else None)
    work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-meta-"))
    (work / "big.img").write_bytes(os.urandom(50 << 20))
    (work / "small.txt").write_bytes(b"small\n")
    old = time.time() - 600
    for f in work.iterdir():
        os.utime(f, (old, old))
    snap = work.parent / "hbc-meta.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    failed = []
    try:
        hbc.send(wii, str(snap), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
        time.sleep(3)
        print("HBC:", flush=True)
        failed += check_times(wii, "HBC") + check_sync(wii, "HBC", work) + check_attributes(wii, "HBC")
        for dol in (root / "tests/agent_app/agent_app.dol",
                    root / "tests/agent_app/agent_app-libogc2.dol"):
            run_app(wii, dol)
            print(f"{dol.name}:", flush=True)
            failed += check_times(wii, dol.name) + check_attributes(wii, dol.name)
            hbc.exit_app(wii)
            hbc.hbc_wait(wii, 90)
    finally:
        try:
            hbc.remove_tree(wii, DIR, out=lambda _: None)
        except (OSError, hbc.HBCError):
            pass
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["exit"])
        time.sleep(8)
        print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
        shutil.rmtree(work, ignore_errors=True)
        snap.unlink(missing_ok=True)
    for f in failed:
        print("FAIL", f)
    if failed:
        raise SystemExit(1)
    print("PASS")


if __name__ == "__main__":
    main()
