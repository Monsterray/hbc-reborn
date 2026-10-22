#!/usr/bin/env python3
"""sdk/mboard end to end in Dolphin, with the real Wii Menu (4.3U).

1. mboard_app_dolphin.dol posts "Played Dolphin Test for 3 minutes." to the
   receive box and logs "Dolphin Test" (3 min) to today's play history.
2. The receive box holds the new message; every message already there is
   unchanged.
3. The Wii Menu starts (a scripted Wii Remote presses A past the Health and
   Safety screen) and moves the message onto the board: cdb.vff then holds
   it, and every file cdb.vff held before is still there.

Runs on its own copy of tools/msgboard's Wii Menu profile
(Documents/mboard-dolphin, made once from Documents/hbc-dolphin-sysmenu;
--fresh makes it again). Build the app first: make -C tests/mboard_app dolphin

With --ipl-symbols (config/43U/symbols.txt of the Wii Menu decompilation,
github.com/koopthekoopa/wii-ipl) Dolphin gets a symbol map of the Wii Menu, so
its OSReport() lines (what it does with each message) reach the log.

usage: tests/dolphin_mboard.py [--fresh] [--day YYYY-MM-DD] [--ipl-symbols FILE]
"""

import argparse
import calendar
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import vffread  # noqa: E402

DOCS = pathlib.Path(os.path.expanduser("~")) / "Documents"
BASE = DOCS / "hbc-dolphin-sysmenu" / "user"
PROFILE = DOCS / "mboard-dolphin" / "user"
DOLPHIN = r"C:\tools\Dolphin-x64\Dolphin.exe"
APP = root / "tests" / "mboard_app" / "mboard_app_dolphin.dol"
NANDPUT = root / "tests" / "nandput" / "nandput.dol"
NAND = PROFILE / "Wii"
CTL = NAND / "shared2/wc24/mbox/wc24recv.ctl"
MBX = NAND / "shared2/wc24/mbox/wc24recv.mbx"
CDB = NAND / "title/00000001/00000002/data/cdb.vff"
BODY = "Played Dolphin Test for 3 minutes.\nA second line."
SETTINGS = ["Dolphin.Interface.ConfirmStop=False", "Dolphin.Interface.UsePanicHandlers=False",
            "Dolphin.Analytics.PermissionAsked=True", "Dolphin.Analytics.Enabled=False",
            "Dolphin.Core.CPUThread=True", "Logger.Options.WriteToFile=True",
            "Logger.Options.Verbosity=4", "Logger.Logs.IOS=True", "Logger.Logs.IOS_WC24=True",
            "Logger.Logs.IOS_FS=True", "Logger.Logs.OSREPORT=True",
            "Dolphin.Input.BackgroundInput=True", "Dolphin.Core.WiiSDCard=True",
            "Dolphin.Core.WiiSDCardAllowWrites=True", "Dolphin.Core.EnableCustomRTC=True",
            "DualShockUDPClient.Server.Enabled=True",
            "DualShockUDPClient.Server.Entries=hbcpad:127.0.0.1:26761;"]


# Functions of the Wii Menu's mail import (ipl::nwc24::Manager::receive()):
# each is named OSReport in the map, so Dolphin's OSReport hook logs a line
# with its address every time it runs, and the trace shows the path taken.
TRACE = ("make_text__Q33ipl5nwc24", "createNewRecord__Q33ipl3cdb", "makeBuffer__Q33ipl3cdb",
         "createAtOnce__Q33ipl3cdb", "error_handling__Q33ipl3cdb", "error_handling__Q33ipl5nwc24",
         "error_handling_ignore_file__Q33ipl5nwc24", "CDBDatabasePrivateCreateRecordAtOnceEx",
         "CDBDatabaseCreateRecordAtOnceEx", "NWC24ReadMsgText", "NWC24ReadMsgAltName",
         "NWC24ReadMsgMBNoReply", "NWC24ReadMsgMBDelay", "NWC24ReadMsgMBRegDate",
         "NWC24ReadMsgMBUpdateSW", "NWC24ReadMsgMBOptOutFlag", "NWC24GetMsgNumAttached",
         "NWC24GetMsgDate", "NWC24DeleteMsg", "RFLiNWC24Msg2CharData")


def write_map(symbols):
    """The decompilation's symbols.txt as a Dolphin map, for OSReport's hook.
    Returns {address: name} of the traced functions."""
    out, traced = [".text section layout"], {}
    for line in pathlib.Path(symbols).read_text(errors="replace").splitlines():
        m = re.match(r"(\S+) = \.text:0x([0-9A-Fa-f]{8}); // type:function size:0x([0-9A-Fa-f]+)", line)
        if m:
            name = m[1]
            if name.startswith(TRACE):
                traced[int(m[2], 16)] = name.split("__")[0]
                name = "OSReport"
            out.append(f"{m[2].lower()} {int(m[3], 16):08x} {m[2].lower()} 4 {name}")
    maps = PROFILE / "Maps"
    maps.mkdir(exist_ok=True)
    (maps / "00000001_00000002.map").write_text("\n".join(out) + "\n")
    return traced


def dolphin(boot, rtc, seconds, pad=None, extra=()):
    """Run Dolphin on the profile; a normal close after `seconds` (so it
    saves its NAND), killed only if that hangs. Returns the log's new part."""
    log = PROFILE / "Logs" / "dolphin.log"
    start = log.stat().st_size if log.exists() else 0
    args = [DOLPHIN, "-b", "-u", str(PROFILE)]
    for s in SETTINGS + list(extra) + [f"Dolphin.Core.CustomRTCValue={calendar.timegm(time.strptime(rtc, '%Y-%m-%dT%H:%M'))}"]:
        args += ["-C", s]
    args += boot
    proc = subprocess.Popen(args)
    helper = subprocess.Popen([sys.executable, str(root / "tools/msgboard/dsu_pad.py"), "26761", pad]) if pad else None
    deadline = time.monotonic() + seconds
    while proc.poll() is None and time.monotonic() < deadline:
        time.sleep(0.5)
    if helper:
        helper.kill()
    if proc.poll() is None:
        subprocess.run(["taskkill", "/PID", str(proc.pid)], capture_output=True)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    data = log.read_bytes()[start:].decode(errors="replace") if log.exists() else ""
    return data


def snapshot():
    box = vffread.Vff(MBX.read_bytes())
    return {"ctl": vffread.box_header(CTL.read_bytes()), "box": box.files(),
            "cdb": vffread.Vff(CDB.read_bytes()).files()}


def utf16(s):
    return s.encode("utf-16-be")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--day", default=time.strftime("%Y-%m-%d"))
    ap.add_argument("--ipl-symbols")
    ap.add_argument("--post-only", action="store_true", help="stop before the Wii Menu runs")
    ap.add_argument("--restore", action="store_true",
                    help="then put the files back with tests/nandput and check they match")
    opts = ap.parse_args()
    if not APP.exists():
        sys.exit(f"build {APP} first: make -C tests/mboard_app dolphin")
    if subprocess.run(["tasklist", "/FI", "IMAGENAME eq Dolphin.exe"], capture_output=True,
                      text=True).stdout.count("Dolphin.exe"):
        print("note: another Dolphin is running; this one uses its own profile")
    if opts.fresh and PROFILE.exists():
        shutil.rmtree(PROFILE)
    if not PROFILE.exists():
        print(f"copying {BASE} -> {PROFILE}")
        shutil.copytree(BASE, PROFILE, ignore=shutil.ignore_patterns("Logs", "Cache", "ScreenShots"))
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    before = snapshot()
    originals = {f: f.read_bytes() for f in (CTL, MBX, CDB)}
    log = dolphin(["-e", str(APP)], f"{opts.day}T12:00", 40)
    print("-- app's KD and FS calls --")
    for line in log.splitlines():
        if "KD" in line or "wc24recv" in line or "cdb.vff" in line or "OSREPORT" in line:
            print("  " + line[:180])
    after = snapshot()
    new = {k: v for k, v in after["box"].items() if k not in before["box"]}
    check(after["ctl"]["num"] == before["ctl"]["num"] + 1, f"receive box: {before['ctl']['num']} -> {after['ctl']['num']} messages")
    check(len(new) == 1 and utf16(BODY) in next(iter(new.values()), b""), f"the new message file {list(new)}")
    check(all(after["box"].get(k) == v for k, v in before["box"].items()), "the box's other messages unchanged")
    check(any(utf16("Dolphin Test") in v for v in after["cdb"].values()), "play history has \"Dolphin Test\"")
    check(all(k in after["cdb"] for k in before["cdb"]), "cdb.vff kept every file")

    if opts.post_only:
        print("PASS" if ok else "FAIL")
        sys.exit(0 if ok else 1)
    extra, traced = (), {}
    if opts.ipl_symbols:
        traced = write_map(opts.ipl_symbols)
        print(f"Wii Menu map written, tracing {len(traced)} functions")
        extra = ("Dolphin.Interface.DebugModeEnabled=True", "Logger.Logs.OSREPORT_HLE=True")
    menu_log = dolphin(["-n", "0000000100000002"], f"{opts.day}T12:05", 60,
                       pad="wait 20; press Cross; wait 2; press Cross; wait 2; press Cross; wait 10",
                       extra=extra)
    if opts.ipl_symbols:
        print("-- the Wii Menu's mail import: its reports and the traced calls --")
        on = False
        for line in menu_log.splitlines():
            if "OSREPORT_HLE" not in line:
                continue
            text = line.split("]: ", 1)[-1]
            on = on or "mail work heap" in text
            m = re.match(r"([0-9a-f]{8})->([0-9a-f]{8})\|", text)
            if m and int(m[2], 16) in traced:
                print(f"  call {traced[int(m[2], 16)]} (from {m[1]})")
            elif on and not text.startswith("8152e718"):
                print("  " + text[:160])
            if "START reset_run" in text:
                break
    final = snapshot()
    board = [k for k, v in final["cdb"].items() if k not in after["cdb"] and utf16(BODY.split("\n")[0]) in v]
    check(board, f"the Wii Menu moved the message onto the board: {board}")
    check(all(k in final["cdb"] for k in before["cdb"]), "cdb.vff still has every file it had before")
    changed = [k for k, v in before["cdb"].items() if final["cdb"].get(k) != v]
    print(f"files of cdb.vff the Wii Menu rewrote (its play history, cdb.conf): {changed}")
    print(f"receive box after the Wii Menu: {final['ctl']['num']} messages (ids {final['ctl']['ids']})")
    for k in board:
        text = final["cdb"][k]
        i = text.find(utf16("Played"))
        print(f"{k}: ...{text[i:i + 160].decode('utf-16-be', 'replace')!r}")

    if opts.restore:
        # The restore path: tests/nandput puts the files back as they were
        # before the app ran, from an SD card (Dolphin's synced SD folder).
        sync = PROFILE.parent / "sdsync"
        shutil.rmtree(sync, ignore_errors=True)
        (sync / "mbrestore").mkdir(parents=True)
        for f, data in originals.items():
            (sync / "mbrestore" / f.name).write_bytes(data)
        sd = ["Dolphin.Core.WiiSDCardEnableFolderSync=True", f"Dolphin.General.WiiSDCardSyncFolder={sync}"]
        for args in (["sd:/mbrestore", "/shared2/wc24/mbox", "wc24recv.ctl", "wc24recv.mbx"],
                     ["--unlock", "sd:/mbrestore", "/title/00000001/00000002/data", "cdb.vff"]):
            (sync / "nandput_args.txt").write_text("\n".join(args) + "\n")
            dolphin(["-e", str(NANDPUT)], f"{opts.day}T12:10", 60, extra=sd)
            result = sync / "mbrestore" / "put_result.txt"
            print(result.read_text() if result.exists() else "no put_result.txt")
        for f, data in originals.items():
            check(f.read_bytes() == data, f"restored {f.name} to its bytes from before the test")
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
