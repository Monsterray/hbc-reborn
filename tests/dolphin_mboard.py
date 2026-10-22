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

With --full, mboard_app_dolphin_full.dol (make -C tests/mboard_app
dolphin-full) posts the full message instead: a Mii face, a JPEG photo and
board data (a U8 archive holding a chjump.bin), with a Reply button, from
files this puts on Dolphin's SD card. The Wii Menu must file it with all of
them: the record holds the face, and the photo and board data as attachments.

usage: tests/dolphin_mboard.py [--fresh] [--full] [--day YYYY-MM-DD] [--ipl-symbols FILE]
"""

import argparse
import calendar
import io
import struct
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
APP_FULL = root / "tests" / "mboard_app" / "mboard_app_dolphin_full.dol"
BODY_FULL = "Played Dolphin Full Test for 4 minutes.\nWith a Mii, a photo and board data."


def test_mii():
    """A Mii's RFLCharData (74 bytes; RFLiCharData in the Wii Menu
    decompilation's RFLi_Types.h, big-endian bit fields from the top bit): a
    girl named "Tester", born 22 Oct, every face part inside the ranges
    RFLiCheckValidInfo() allows (an eyebrow's Y is at least 3), and a
    creation ID with its "normal Mii" bit (31) set."""
    def word(*fields):          # (value, bits), from the top bit down
        v, used = 0, 0
        for val, bits in fields:
            used += bits
            v |= val << (16 - used)
        return struct.pack(">H", v)
    out = word((0, 1), (1, 1), (10, 4), (22, 5), (4, 4), (0, 1))     # sex, birthday, colour
    out += "Tester".encode("utf-16-be").ljust(20, b"\0")
    out += bytes([0x40, 0x40]) + bytes.fromhex("89abcdef" "c0ffee00")  # height, build, ID
    out += word((0, 3), (0, 3), (0, 4), (0, 3), (0, 1), (0, 2))     # face
    out += word((33, 7), (1, 3), (0, 1))                            # hair
    out += word((6, 5), (6, 5))                                     # eyebrow type, rotation
    out += word((1, 3), (4, 4), (10, 5), (2, 4))                    # eyebrow colour, scale, Y, X
    out += word((2, 6), (4, 5), (12, 5))                            # eye type, rotation, Y
    out += word((0, 3), (4, 4), (2, 4))                             # eye colour, scale, X
    out += word((1, 4), (4, 4), (9, 5))                             # nose
    out += word((6, 5), (0, 2), (4, 4), (13, 5))                    # mouth
    out += word((0, 4), (0, 3), (4, 4), (10, 5))                    # glasses
    out += word((0, 2), (0, 2), (0, 3), (4, 4), (10, 5))            # mustache, beard
    out += word((0, 1), (4, 4), (20, 5), (2, 5))                    # mole
    out += "hbc-reborn".encode("utf-16-be").ljust(20, b"\0")        # creator
    assert len(out) == 74
    return out


def test_photo():
    """A 192x144 baseline JPEG: a gradient with a block, as a camera would."""
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (192, 144))
    for y in range(144):
        for x in range(192):
            img.putpixel((x, y), (x * 255 // 191, y * 255 // 143, 128))
    ImageDraw.Draw(img).rectangle((60, 40, 130, 100), fill=(250, 250, 250))
    out = io.BytesIO()
    img.save(out, "JPEG", quality=85, progressive=False)
    return out.getvalue()


def chjump_arc(title_hi, title_lo, arg=b""):
    """Board data as Nintendo's own messages carry it: a U8 archive holding
    chjump.bin ("ChJp", its size, 1, 0, the title to open, then its
    argument), laid out as the archive in a Wii Shop message on the bench."""
    arg = (arg + b"\0").ljust((len(arg) + 2) & ~1, b"\0") if arg else b""   # NUL, to 2 bytes
    chj = b"ChJp" + struct.pack(">IIIII", 0x20 + len(arg), 1, 0, title_hi, title_lo)
    chj += struct.pack(">II", 0x20, len(arg)) + arg
    names = b"\0chjump.bin\0"
    nodes = struct.pack(">BxHII", 1, 0, 0, 2) + struct.pack(">BxHII", 0, 1, 0, len(chj))
    meta = len(nodes) + len(names)
    data_at = (0x20 + meta + 31) & ~31
    nodes = nodes[:12] + struct.pack(">BxHII", 0, 1, data_at, len(chj))
    head = struct.pack(">IIII", 0x55AA382D, 0x20, meta, data_at) + bytes(16)
    arc = head + nodes + names
    return arc + bytes(data_at - len(arc)) + chj
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
    ap.add_argument("--full", action="store_true", help="the full message (mboard_post_msg)")
    ap.add_argument("--post-only", action="store_true", help="stop before the Wii Menu runs")
    ap.add_argument("--restore", action="store_true",
                    help="then put the files back with tests/nandput and check they match")
    opts = ap.parse_args()
    global APP, BODY
    if opts.full:
        APP, BODY = APP_FULL, BODY_FULL
    if not APP.exists():
        sys.exit(f"build {APP} first: make -C tests/mboard_app " + ("dolphin-full" if opts.full else "dolphin"))
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
    app_extra = ()
    if opts.full:
        # The parts, on Dolphin's SD card (its folder sync).
        sync = PROFILE.parent / "sdsync"
        shutil.rmtree(sync, ignore_errors=True)
        (sync / "mboard_test").mkdir(parents=True)
        parts = {"face.bin": test_mii(), "photo.jpg": test_photo(),
                 "board.arc": chjump_arc(0x00010001, 0x4F484243)}   # opens HBC (OHBC)
        for name, data in parts.items():
            (sync / "mboard_test" / name).write_bytes(data)
        print("parts: " + ", ".join(f"{k} {len(v)} bytes" for k, v in parts.items()))
        app_extra = ("Dolphin.Core.WiiSDCardEnableFolderSync=True",
                     f"Dolphin.General.WiiSDCardSyncFolder={sync}")
    log = dolphin(["-e", str(APP)], f"{opts.day}T12:00", 40, extra=app_extra)
    if opts.full:
        result = PROFILE.parent / "sdsync" / "mboard_test" / "result.txt"
        print(result.read_text() if result.exists() else "no result.txt")
    print("-- app's KD and FS calls --")
    for line in log.splitlines():
        if "KD" in line or "wc24recv" in line or "cdb.vff" in line or "OSREPORT" in line:
            print("  " + line[:180])
    after = snapshot()
    new = {k: v for k, v in after["box"].items() if k not in before["box"]}
    check(after["ctl"]["num"] == before["ctl"]["num"] + 1, f"receive box: {before['ctl']['num']} -> {after['ctl']['num']} messages")
    check(len(new) == 1 and utf16(BODY) in next(iter(new.values()), b""), f"the new message file {list(new)}")
    check(all(after["box"].get(k) == v for k, v in before["box"].items()), "the box's other messages unchanged")
    if opts.full:
        msg = next(iter(new.values()), b"")
        for what in (b"X-WiiFace:", b"image/jpeg", b"application/x-wii-msgboard"):
            check(what in msg, f"the message has {what.decode()}")
        check(b"X-Wii-MB-NoReply" not in msg, "the message has a Reply button")
    else:
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
        if opts.full:
            # CDB's 0x400-byte file header, then the board record
            # (ipl::cdb RIPL_BoardRecord: "RI_5", offsets of the title,
            # text and face at 0x11C/0x120/0x124, two attachments at 0x128).
            rec = text[0x400:]
            print(f"  record magic {rec[:4]!r}, folder type {k.split('/')[-2]}")
            check(rec[:4] == b"RI_5", "a board record")
            face_at = struct.unpack_from(">I", rec, 0x124)[0]
            check(face_at and rec[face_at:face_at + 0x18] == test_mii()[:0x18],
                  f"the record holds the Mii (at 0x{face_at:x})")
            atts = [struct.unpack_from(">III", rec, 0x128 + 12 * j) for j in range(2)]
            print(f"  attachments (type, offset, size): {atts}")
            arc = chjump_arc(0x00010001, 0x4F484243)
            check(arc in rec, "the record holds the board data")
            check(k.split("/")[-2] == "odh" and any(a[2] for a in atts),
                  "the photo was kept (an odh record with an attachment)")

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
