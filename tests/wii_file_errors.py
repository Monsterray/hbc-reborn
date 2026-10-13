#!/usr/bin/env python3
"""File requests that must fail cleanly, against HBC and an app's agent.

On a Wii (this tree's channel DOL through the installed HBC; the SD card
must have less than 400 MB free) or with --dolphin (this tree's retail WAD,
a 64 MiB SD card), for HBC and then tests/agent_app, whose agent serves
the same requests:

  - `hbc.py ls` of a folder the card does not have: the error names it
  - `hbc.py put` of a local file that does not exist: the error says so
  - a 400 MB .img, more than the card has free: ENOSPC at once (before
    the data is sent, not after), and no .part file left behind

Ends in the installed HBC through tests/agent_app's exit.

usage: tests/wii_file_errors.py [WII-IP] | --dolphin
"""

import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402

SD_BYTES = 64 << 20


def cli(wii, *args):
    r = subprocess.run([sys.executable, str(root / "tools/hbc.py"), "--wii", wii, *args],
                       capture_output=True, text=True, timeout=120)
    return r.returncode, (r.stdout + r.stderr).strip()


def cases(wii, who, big):
    failed = []

    def check(ok, what):
        print(f"{'PASS' if ok else 'FAIL'} {who}: {what}", flush=True)
        if not ok:
            failed.append(what)

    code, out = cli(wii, "ls", "sd:/no-such-folder")
    check(code and "sd:/no-such-folder: Wii returned ENOENT" in out, f"ls a missing folder: {out}")
    code, out = cli(wii, "put", "no-such-local-file.bin", "sd:/x.bin")
    check(code and "no-such-local-file.bin: no such file or folder on this PC" in out,
          f"put a missing local file: {out}")
    sd = hbc.hardware(wii, ["Storage"])["Storage"].get("SD card", "")
    free = re.search(r"([\d.]+) GB free", sd)
    if free and float(free.group(1)) * 1e9 >= big.stat().st_size:
        print(f"SKIP {who}: the full-card case needs an SD card with under 400 MB free "
              f"(this one: {sd})", flush=True)
        return failed
    t = time.monotonic()
    code, out = cli(wii, "put", str(big), "sd:/big.img")
    took = time.monotonic() - t
    check(code and "ENOSPC" in out and took < 20,
          f"put 400 MB to a card without room: {out} ({took:.1f} s)")
    names = [n for _, _, n in hbc.list_dir(wii, "sd:/")[0]]
    check(not any(n.startswith("big.img") for n in names), f"nothing left behind: {names}")
    return failed


def main():
    dolphin = "--dolphin" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    big = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "hbc-big.img"
    with open(big, "wb") as f:   # 400 MB of data zlib cannot shrink
        for _ in range(400):
            f.write(os.urandom(1 << 20))
    d = dolphin_smoke.Dolphin(root / "channel/title/channel_retail.wad", sd=True,
                              extra=[f"Dolphin.Core.WiiSDCardFilesize={SD_BYTES}"])         if dolphin else None
    failed = []
    try:
        wii = d.address if d else hbc.wii_address(args[0] if args else None)
        if not d:
            shutil.copy(root / "channel/channelapp/channelapp-channel.dol",
                        big.with_name("hbc-errors.dol"))
            hbc.send(wii, str(big.with_name("hbc-errors.dol")), [])
            hbc.relaunch_wait(wii, dolphin_smoke.expected)
        print(f"HBC {hbc.hbc_wait(wii, 60)}", flush=True)
        time.sleep(3)
        failed += cases(wii, "HBC", big)
        hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["stay", "120"])
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                if hbc.is_agent(hbc.version(wii, hbc.POLL_TIMEOUT)):
                    break
            except (OSError, hbc.HBCError):
                pass
            time.sleep(0.5)
        time.sleep(2)
        failed += cases(wii, "agent", big)
    finally:
        if d:
            d.stop()
        else:
            hbc.exit_app(wii)
            print(f"back in HBC {hbc.hbc_wait(wii, 90)}", flush=True)
        big.unlink(missing_ok=True)
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
