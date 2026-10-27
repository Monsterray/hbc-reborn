#!/usr/bin/env python3
"""The Mii library (sdk/mii) on the bench Wii, as queue jobs.

  backup    copy /shared2/menu/FaceLib (RFL_DB.dat) to the job's run folder,
            through tests/nandget (read-only)
  check     read-only: tests/mii_app lists the database's Miis and the Miis on
            Wii Remote 1 (IDs and checks; only this test's names)
  add       a random Mii named "HBC Mii" (--name), created by "hbc-reborn",
            into the first free slot (mii_db_add, mii_db_save). The database
            is copied before and after: only that slot and the CRC may differ,
            and the CRC must be right. The new Mii's ID goes in mii_id.txt.
  remove ID take that Mii out again (only one this test made); before and
            after as for add
  edit ID FIELD=N...
            change a Mii this test made: any field of mii.h's struct
            (height=, build=, favorite=, fav_color=, ...), name=TEXT,
            birthday=M/D, mingle=0|1; only that slot and the CRC may change
  hidden    a random Mii into the hidden database and out again: afterwards
            the table and slots are as before, and only that Mii's 0x40 bytes
            of data may differ (RFL leaves them too)
  draw [SLOT] sdk/mii's drawing: the Mii's icons (96 and 256 px, normal and
            smiling) and a full-body capture, checked by mii_app and saved
            as PNGs in the run folder (needs sd:/mii/RFL_Res.dat)
  crowd COUNTS [SECONDS] [MASK]
            sdk/mii's cost: COUNTS (e.g. 20,40,60) random Miis walking with the
            Mii Channel's bodies, SECONDS each, faces MASK px (64, 128): memory a
            Mii, CPU and GPU time a frame, frames a second; TV pictures from
            the capture card every 8 s
  walk COUNTS|max [SECONDS]
            levels of detail: COUNTS random Miis walking in from the horizon to
            the bottom of the screen, then away into it, SECONDS each way;
            "max" builds until memory runs out; TV pictures every 3 s
  remote-add ID / remote-remove ID
            copy the database's Mii with that ID to Wii Remote 1, or take it
            off; written and read back
  remote-rewrite
            write Wii Remote 1's own Miis back unchanged (both copies), read
            back and compared; its memory is saved first, to the SD card and
            the run folder (remote_backup.bin)
  restore BACKUP_DIR
            put a backup's RFL_DB.dat back (tests/nandput: same size only,
            read back and compared), then compare a new copy with it

usage: tests/wii_mii.py backup | check | draw [SLOT] | add [--name NAME] | remove ID | edit ID FIELD=N... | hidden | remote-add ID | remote-remove ID | remote-rewrite | restore BACKUP_DIR
queue: wiibench.py add --wii dev --name mii-STEP --cwd REPO --timeout 1200 -- C:/Python312/python.exe tests/wii_mii.py STEP
"""

import binascii
import os
import pathlib
import random
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import wii_mboard as wm  # noqa: E402  (bench_wii, tool, launch, snapshot, check)
sys.path.insert(0, str(root / "tools"))
import mii_res  # noqa: E402  (png)

APP = root / "tests" / "mii_app" / "mii_app.dol"
NANDPUT = root / "tests" / "nandput" / "nandput.dol"
FACELIB = wm.FACELIB
DB = "RFL_DB.dat"
SIZE, CRC_AT, HDATA = 0xBE6C0, 0x1F1DE, 0x1F1E0


def crc_ok(f):
    return len(f) == SIZE and binascii.crc_hqx(f[:CRC_AT], 0) == int.from_bytes(f[CRC_AT:CRC_AT + 2], "big")


def changed(a, b):
    """The byte ranges where a and b differ, as (start, end)."""
    out, i, n = [], 0, min(len(a), len(b))
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def run_app(out, *args, label, wait_s=300):
    wm.launch(APP, *args, wait_s=wait_s)
    wm.tool("get", "sd:/mii_test/result.txt", str(out / f"{label}_result.txt"))
    report = (out / f"{label}_result.txt").read_text(errors="replace")
    print(f"-- {label}_result.txt --\n{report}", flush=True)
    return report


def app_ok(report):
    return report.rstrip().endswith("done") and "failures 0" in report


def db_copy(out, label):
    ok = wm.snapshot(out / label, [FACELIB])
    f = (out / label / FACELIB[1] / DB).read_bytes()
    return ok, f


def within(ranges, lo, hi):
    return all(lo <= s and e <= hi for s, e in ranges)


def slot_change(ok, before, after):
    """Only one slot and the CRC changed, and the CRC is right."""
    diff = changed(before, after)
    print(f"changed byte ranges: {[(hex(s), hex(e)) for s, e in diff]}", flush=True)
    ok = wm.check(ok, crc_ok(after), "the database's CRC is right")
    slots = {(s - 4) // 74 for s, e in diff if s < 4 + 100 * 74}
    rest = [(s, e) for s, e in diff if not (4 <= s and e <= 4 + 100 * 74) and not within([(s, e)], CRC_AT, CRC_AT + 2)]
    ok = wm.check(ok, len(slots) == 1 and not rest, "only one slot and the CRC changed")
    return ok


def main():
    step = sys.argv[1] if len(sys.argv) > 1 else ""
    wm.bench_wii()
    need = [APP, wm.NANDGET] + ([NANDPUT] if step == "restore" else [])
    missing = [str(p.relative_to(root)) for p in need if not p.exists()]
    if missing:
        sys.exit(f"build {', '.join(missing)} first (make -C its folder): nothing was run")
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    ok = True
    if step == "backup":
        ok, f = db_copy(out, "mii_bak")
        ok = wm.check(ok, crc_ok(f), "the backup is a whole database with a right CRC")
    elif step == "check":
        ok = app_ok(run_app(out, "check", label="check"))
    elif step == "add":
        name = sys.argv[sys.argv.index("--name") + 1] if "--name" in sys.argv else "HBC Mii"
        ok, before = db_copy(out, "before")
        report = run_app(out, "add", name, str(random.randrange(1, 1 << 31)), label="add")
        ok = wm.check(ok, app_ok(report), "mii_app add")
        m = re.search(r"new Mii ([0-9a-f]{16})", report)
        if m:
            (out / "mii_id.txt").write_text(m.group(1) + "\n")
            print(f"new Mii ID {m.group(1)} (in mii_id.txt)", flush=True)
        ok2, after = db_copy(out, "after")
        ok = slot_change(ok and ok2, before, after)
    elif step == "remove" and len(sys.argv) > 2:
        ok, before = db_copy(out, "before")
        ok = wm.check(ok, app_ok(run_app(out, "remove", sys.argv[2], label="remove")), "mii_app remove")
        ok2, after = db_copy(out, "after")
        ok = slot_change(ok and ok2, before, after)
    elif step == "hidden":
        ok, before = db_copy(out, "before")
        report = run_app(out, "hidden", str(random.randrange(1, 1 << 31)), label="hidden")
        ok = wm.check(ok, app_ok(report), "mii_app hidden")
        ok2, after = db_copy(out, "after")
        diff = changed(before, after)
        print(f"changed byte ranges: {[(hex(s), hex(e)) for s, e in diff]}", flush=True)
        ok = wm.check(ok and ok2, after[:HDATA] == before[:HDATA], "the table and slots are as before")
        ok = wm.check(ok, len({(s - HDATA) // 0x40 for s, _ in diff}) <= 1 and within(diff, HDATA, SIZE),
                      "only one hidden Mii's data differs")
    elif step in ("remote-add", "remote-remove") and len(sys.argv) > 2:
        ok = app_ok(run_app(out, step, sys.argv[2], label=step))
    elif step == "edit" and len(sys.argv) > 3:
        ok, before = db_copy(out, "before")
        ok = wm.check(ok, app_ok(run_app(out, "edit", *sys.argv[2:], label="edit")), "mii_app edit")
        ok2, after = db_copy(out, "after")
        ok = slot_change(ok and ok2, before, after)
    elif step == "draw":
        # sdk/mii's drawing on the Wii: icons (96 and 256 px) and a capture,
        # from sd:/mii/RFL_Res.dat (Save Mii faces), as PNGs in the run folder.
        ok = app_ok(run_app(out, "draw", *sys.argv[2:3], label="draw"))
        for name in ("icon_normal", "icon_smile", "icon_large", "body", *(f"body_lod{k}" for k in range(4)),
                     *(f"head_lod{k}" for k in range(4)), "face_cpu", "face_gpu"):
            raw = out / f"{name}.rgba"
            if wm.tool("get", f"sd:/mii_test/{name}.rgba", str(raw), check=False).returncode:
                ok = False
                continue
            head, body = raw.read_bytes().split(b"\n", 1)
            w, h = map(int, head.split()[:2])
            rows = [[tuple(body[4 * (y * w + x):4 * (y * w + x) + 4]) for x in range(w)] for y in range(h)]
            (out / f"{name}.png").write_bytes(mii_res.png(w, h, rows))
            print(f"{name}.png: {w} x {h}", flush=True)
    elif step == "gpuprobe" and len(sys.argv) > 2:
        ok = app_ok(run_app(out, step, *sys.argv[2:4], label=step, wait_s=900))
    elif step in ("crowd", "walk") and len(sys.argv) > 2:
        import threading
        done = threading.Event()

        def pictures():   # the capture card, while the crowd walks
            k = 0
            while not done.wait(8 if step == "crowd" else 3):
                wm.still(out, f"{step}_{k:03d}")
                k += 1
        threading.Thread(target=pictures, daemon=True).start()
        try:
            ok = app_ok(run_app(out, step, *sys.argv[2:5], label=step, wait_s=3600))
        finally:
            done.set()
    elif step == "remote-rewrite":
        ok = app_ok(run_app(out, step, label=step))
        wm.tool("get", "sd:/mii_test/remote_backup.bin", str(out / "remote_backup.bin"), check=False)
    elif step == "restore" and len(sys.argv) > 2:
        backup = pathlib.Path(sys.argv[2]) / FACELIB[1] / DB
        if not backup.exists() or not crc_ok(backup.read_bytes()):
            sys.exit(f"{backup}: missing or not a whole database")
        wm.tool("put", str(backup), f"sd:/miirestore/{DB}")
        wm.launch(NANDPUT, "sd:/miirestore", FACELIB[0], DB)
        wm.tool("get", "sd:/miirestore/put_result.txt", str(out / "put_result.txt"))
        report = (out / "put_result.txt").read_text(errors="replace")
        print(report, flush=True)
        ok = wm.check(ok, report.rstrip().endswith("done") and "restored, verified" in report, "nandput")
        ok2, after = db_copy(out, "after_restore")
        ok = wm.check(ok and ok2, after == backup.read_bytes(), "RFL_DB.dat matches the backup")
    else:
        sys.exit(__doc__)
    print("PASS" if ok else "FAIL", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
