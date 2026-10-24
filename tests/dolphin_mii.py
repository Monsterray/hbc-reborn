#!/usr/bin/env python3
"""sdk/mii end to end in Dolphin: the database on its NAND and an emulated
Wii Remote's Miis, with nobody at a Wii.

A fresh profile (Documents/mii-dolphin) gets an RFL_DB.dat with one Mii in
slot 0. tests/mii_app then runs once per step, its arguments in its SD
card's mii_test/args.txt (Dolphin boots a DOL without arguments), and
powers off at the end:

  check                 the database loads; the Wi-Fi MAC reads
  add "Dolphin" S   a new Mii in slot 1: only that slot and the CRC change
  hidden S              into the hidden database and out: the table and
                        slots end as they were
  remote-both ID        that Mii onto Wii Remote 1 and off again, each
                        written and read back (one boot: Dolphin keeps an
                        emulated Remote's memory only while it runs)
  remove ID             out of slot 1: the file is back as it started, but
                        for the hidden Mii's data (RFL leaves it too)

Build the app first: make -C tests/mii_app

usage: tests/dolphin_mii.py
"""

import binascii
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
from dolphin_mboard import test_mii  # noqa: E402

DOCS = pathlib.Path(os.path.expanduser("~")) / "Documents"
HOME = DOCS / "mii-dolphin"
PROFILE = HOME / "user"
SYNC = HOME / "sdsync"
DOLPHIN = r"C:\tools\Dolphin-x64\Dolphin.exe"
APP = root / "tests" / "mii_app" / "mii_app.dol"
DB = PROFILE / "Wii" / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat"
SIZE, CRC_AT, HDATA = 0xBE6C0, 0x1F1DE, 0x1F1E0
SETTINGS = ["Dolphin.Interface.ConfirmStop=False", "Dolphin.Interface.UsePanicHandlers=False",
            "Dolphin.Analytics.PermissionAsked=True", "Dolphin.Analytics.Enabled=False",
            "Dolphin.Core.WiiSDCard=True", "Dolphin.Core.WiiSDCardAllowWrites=True",
            "Dolphin.Core.WiiSDCardEnableFolderSync=True", f"Dolphin.General.WiiSDCardSyncFolder={SYNC}",
            "Dolphin.Core.WiimoteContinuousScanning=False"]


def crc_ok(f):
    return len(f) == SIZE and binascii.crc_hqx(f[:CRC_AT], 0) == int.from_bytes(f[CRC_AT:CRC_AT + 2], "big")


def first_db():
    f = bytearray(SIZE)
    f[0:4] = b"RNOD"
    f[4:4 + 74] = test_mii()
    f[0x1D00:0x1D04] = b"RNHD"
    struct.pack_into(">hh", f, 0x1D04, -1, -1)
    f[0x1D08:0x1D08 + 10000 * 12] = (bytes(8) + bytes.fromhex("7fff7fff")) * 10000   # free nodes, as RFL's format
    struct.pack_into(">H", f, CRC_AT, binascii.crc_hqx(bytes(f[:CRC_AT]), 0))
    return bytes(f)


def run(*args, seconds=120):
    (SYNC / "mii_test").mkdir(parents=True, exist_ok=True)
    (SYNC / "mii_test" / "args.txt").write_text("\n".join(args) + "\n")
    result = SYNC / "mii_test" / "result.txt"
    if result.exists():
        result.unlink()
    cmd = [DOLPHIN, "-b", "-u", str(PROFILE)]
    for s in SETTINGS:
        cmd += ["-C", s]
    proc = subprocess.Popen(cmd + ["-e", str(APP)])
    try:
        proc.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/PID", str(proc.pid)], capture_output=True)
        proc.wait(timeout=30)
        print("Dolphin did not power off by itself", flush=True)
    text = result.read_text(errors="replace") if result.exists() else "(no result.txt)"
    print(f"-- {' '.join(args)} --\n{text}", flush=True)
    return text


def main():
    if subprocess.run(["tasklist", "/FI", "IMAGENAME eq Dolphin.exe"], capture_output=True,
                      text=True).stdout.count("Dolphin.exe"):
        sys.exit("Dolphin is running: close it first (this test never touches a running Dolphin)")
    if not APP.exists():
        sys.exit("build tests/mii_app first")
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + what, flush=True)
        ok &= bool(cond)

    shutil.rmtree(HOME, ignore_errors=True)
    DB.parent.mkdir(parents=True)
    DB.write_bytes(first_db())
    start = DB.read_bytes()

    def good(text):
        return text.rstrip().endswith("done") and "failures 0" in text

    text = run("check")
    check(good(text) and "1 Miis, 0 hidden" in text, "check: the database loads")
    check("mii_wifi_mac: 0;" in text, "the Wi-Fi MAC reads")

    text = run("add", "Dolphin", "4242")
    m = re.search(r"new Mii ([0-9a-f]{16})", text)
    check(good(text) and m and "mii_db_add: 1" in text, "add: into slot 1")
    after = DB.read_bytes()
    check(crc_ok(after), "the CRC is right")
    diff = {i for i in range(SIZE) if after[i] != start[i]}
    check(diff and all(4 + 74 <= i < 4 + 148 or CRC_AT <= i < CRC_AT + 2 for i in diff),
          "only slot 1 and the CRC changed")
    check(after[4 + 74 + 2:4 + 74 + 22].decode("utf-16-be").rstrip("\0") == "Dolphin", "slot 1's name")
    mid = m.group(1) if m else "0" * 16

    before = DB.read_bytes()
    text = run("hidden", "777")
    check(good(text) and "mii_hidden_put: 0" in text, "hidden: in and out")
    after = DB.read_bytes()
    check(after[:HDATA] == before[:HDATA], "the table and slots are as before")
    check(any(after[HDATA:HDATA + 0x40]) and after[HDATA + 0x40:] == before[HDATA + 0x40:],
          "only hidden Mii 0's data stays behind")

    # Dolphin keeps an emulated Remote's memory only while it runs (it saves
    # it on a reset, not at a power-off), so both halves go in one boot.
    text = run("remote-both", mid)
    halves = text.split("read: copy")
    check(good(text) and text.count("mii_remote_write: 0") == 2 and
          text.count("the Remote holds what was written: 0") == 2, "remote-add and -remove: each written and read back")
    check(len(halves) == 3 and halves[1].startswith(" -1") and halves[2].startswith(" 0"),
          "a new Remote reads as empty; after the write, copy 0 is whole")
    text = run("remove", mid)
    check(good(text) and "gone" in text, "remove: out of slot 1")
    end = DB.read_bytes()
    check(end[:HDATA] == start[:HDATA], "the slots and table are back as they started")
    print("PASS" if ok else "FAIL", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
