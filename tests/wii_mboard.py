#!/usr/bin/env python3
"""The Message Board library (sdk/mboard) on the bench Wii, as queue jobs.

  backup   copy the Wii Menu's data folder (cdb.vff), /shared2/wc24 and its
           mbox to the job's run folder, through tests/nandget (read-only;
           --unlock lifts IOS's NAND permission check for the copy only)
  post     the receive box and cdb.vff before; tests/mboard_app posts
           "Played Final Fantasy VII for 42 minutes." and logs 42 minutes of
           "Final Fantasy VII" to today's play history; the same files after.
           Checks the new message byte for byte and that every message and
           cdb.vff file already there is unchanged.
  board    for a person at the Wii: waits (up to --minutes) while they open
           the Wii Menu, look at the Message Board and start HBC again, with
           a picture from the capture card every 15 s; then checks that the
           Wii Menu filed the message in cdb.vff and kept every file it had.
           Run after `post`; give it the post job's run folder.

usage: tests/wii_mboard.py backup | post | board POST_RUN_DIR [--minutes N]
queue: wiibench.py add --name mboard-STEP --cwd REPO --timeout 2400 -- C:/Python312/python.exe tests/wii_mboard.py STEP
"""

import os
import pathlib
import shutil
import struct
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import vffread  # noqa: E402

PROTECTED = ("192.168.8.200", "192.168.8.243")  # real data: never written by this script
PY = sys.executable
HBC = str(root / "tools" / "hbc.py")
NANDGET = root / "tests" / "nandget" / "nandget.dol"
APP = root / "tests" / "mboard_app" / "mboard_app.dol"
CAPTURE = "C:/tools/wii-capture/wiicap.py"
SD_BAK = "sd:/mbbak_"  # + the folder name: nandget makes one folder level only
WC24 = ("/shared2/wc24", "wc24", False)
MBOX = ("/shared2/wc24/mbox", "mbox", False)
MENU = ("/title/00000001/00000002/data", "menu", True)
TITLE = "WiiStation"
BODY = "Played Final Fantasy VII\nfor 42 minutes."
GAME, GAME_ID, GAME_SECONDS = "Final Fantasy VII", "WSFF7", 42 * 60


def bench_wii():
    wii = os.environ.get("WII_BENCH_IP")
    if not wii or not os.environ.get("WII_BENCH_JOB_START"):
        sys.exit("run this as a wii-bench queue job (it writes the Wii's NAND or SD card)")
    if wii.strip() in PROTECTED:
        sys.exit(f"refusing {wii}: a production Wii")
    return wii


def tool(*args, check=True):
    print("$ hbc.py " + " ".join(args), flush=True)
    return subprocess.run([PY, HBC, *args], check=check)


def answers():
    """HBC (or anything) answers HBCV on 4299 within a few seconds."""
    return subprocess.run([PY, HBC, "--timeout", "3", "version"], capture_output=True).returncode == 0


def still(out, label):
    """A picture of the TV, through the capture card (only meaningful while
    this job holds the Wii)."""
    subprocess.run([PY, CAPTURE, "still", "--preset", "wii", "--out", str(out / f"{label}.png")],
                   capture_output=True)


def launch(dol, *args, start_s=30, wait_s=300):
    """Send a DOL, see HBC go away while it runs, then wait for HBC to come
    back. Needs no net log: the app's own report goes to the SD card."""
    t0 = time.monotonic()
    # An app that is done within 10 s has HBC back before `send` can tell
    # it launched, and `send` then fails though the app ran (REVIEW.md):
    # the app's own report says what happened.
    tool("send", str(dol), *args, check=False)
    deadline = time.monotonic() + start_s
    while answers() and time.monotonic() < deadline:
        time.sleep(0.5)
    print(f"[{time.monotonic() - t0:.1f} s] " + ("the app is running" if time.monotonic() < deadline
          else "HBC answered throughout: the app was quick, or did not start"), flush=True)
    tool("wait", str(wait_s))
    print(f"[{time.monotonic() - t0:.1f} s] HBC back", flush=True)


def snapshot(dest, folders):
    """Copy NAND folders to dest/<name>; True when every copy finished."""
    for nand, sub, unlock in folders:
        launch(NANDGET, *(["--unlock"] if unlock else []), nand, SD_BAK + sub)
    ok = True
    for _, sub, _ in folders:
        shutil.rmtree(dest / sub, ignore_errors=True)
        tool("get", "-r", SD_BAK + sub, str(dest / sub))
        report = (dest / sub / "result.txt").read_text(errors="replace")
        print(f"-- {sub}/result.txt --\n{report}", flush=True)
        ok &= report.rstrip().endswith("done") and " of " in report and "MISMATCH" not in report
    return ok


def backup(out):
    ok = snapshot(out / "mboard_bak", [WC24, MBOX, MENU])
    vff = out / "mboard_bak" / "menu" / "cdb.vff"
    if not vff.exists() or vff.stat().st_size != 0x1400000 or vff.read_bytes()[:4] != b"VFF ":
        print("cdb.vff missing or not a 20 MiB VFF", flush=True)
        ok = False
    return ok


def box(folder):
    ctl = (folder / "wc24recv.ctl").read_bytes()
    return vffread.box_header(ctl), vffread.Vff((folder / "wc24recv.mbx").read_bytes()), ctl


def check(ok, cond, what):
    print(("ok    " if cond else "FAIL  ") + what, flush=True)
    return ok and bool(cond)


def post(out):
    before, after = out / "before", out / "after"
    ok = snapshot(before, [MBOX, MENU])
    still(out, "0-before")
    launch(APP, "post", TITLE, BODY.replace("\n", "\\n"))
    still(out, "1-after-post")
    tool("get", "sd:/mboard_test/result.txt", str(out / "post_result.txt"))
    launch(APP, "play", GAME, GAME_ID, str(GAME_SECONDS))
    tool("get", "sd:/mboard_test/result.txt", str(out / "play_result.txt"))
    ok &= snapshot(after, [MBOX, MENU])
    for name in ("post_result.txt", "play_result.txt"):
        print(f"-- {name} --\n" + (out / name).read_text(errors="replace"), flush=True)
    ok = check(ok, "mboard_post: 0 (posted)" in (out / "post_result.txt").read_text(errors="replace"),
               "mboard_post returned MBOARD_OK")
    ok = check(ok, "mboard_log_play: " in (out / "play_result.txt").read_text(errors="replace") and
               "mboard_log_play: -" not in (out / "play_result.txt").read_text(errors="replace"),
               "mboard_log_play succeeded")

    h0, v0, ctl0 = box(before / "mbox")
    h1, v1, ctl1 = box(after / "mbox")
    f0, f1 = v0.files(), v1.files()
    new = {k: v for k, v in f1.items() if k not in f0}
    ok = check(ok, h1["num"] == h0["num"] + 1, f"receive box: {h0['num']} -> {h1['num']} messages")
    ok = check(ok, len(new) == 1, f"one new message file: {list(new)}")
    text = BODY.encode("utf-16-be")
    ok = check(ok, all(text in v for v in new.values()), "it holds the text, as UTF-16")
    ok = check(ok, all(len(v) % 32 == 0 for v in new.values()), "the file is padded to 32 bytes")
    ok = check(ok, all(f1.get(k) == v for k, v in f0.items()), "every message already there is unchanged")
    ok = check(ok, v1.fats_match(), "the two FATs match")
    entries_before = {struct.unpack_from(">I", ctl0, o)[0]: ctl0[o:o + 0x80] for o in range(0x80, 0x8000, 0x80)}
    ok = check(ok, all(ctl1[o:o + 0x80] == entries_before.get(struct.unpack_from(">I", ctl1, o)[0], b"")
                       for o in range(0x80, 0x8000, 0x80)
                       if struct.unpack_from(">I", ctl1, o)[0] in h0["ids"]),
               "every control entry already there is unchanged")
    ok = check(ok, h1["free_space"] == (v1.free() - 1) * 512, "free space recorded as the VF library counts it")

    c0 = vffread.Vff((before / "menu" / "cdb.vff").read_bytes()).files()
    c1 = vffread.Vff((after / "menu" / "cdb.vff").read_bytes()).files()
    ok = check(ok, all(k in c1 for k in c0), f"cdb.vff kept all {len(c0)} files")
    changed = [k for k, v in c0.items() if c1.get(k) != v]
    ok = check(ok, all("/HAEA_#1/log/" in k or k == "/cdb.conf" for k in changed),
               f"only play history (and cdb.conf) changed: {changed}")
    ok = check(ok, any(GAME.encode("utf-16-be") in v for v in c1.values()),
               f'today\'s play history lists "{GAME}"')
    return ok


def board(out, post_dir, minutes):
    """The person at the Wii does the Wii Menu part; this job only waits,
    takes pictures, and reads the result once HBC is back."""
    print(f"Waiting up to {minutes} min: go to the Wii Menu, open the Message Board, then start HBC.",
          flush=True)
    deadline = time.monotonic() + minutes * 60
    gone = False
    n = 0
    while time.monotonic() < deadline:
        up = answers()
        gone = gone or not up
        if gone and up:
            break
        still(out, f"board-{n:03d}")
        n += 1
        time.sleep(15)
    if not (gone and answers()):
        print("HBC did not leave and come back in time", flush=True)
        return False
    time.sleep(10)
    ok = snapshot(out / "after_board", [MBOX, MENU])
    c0 = vffread.Vff((post_dir / "after" / "menu" / "cdb.vff").read_bytes()).files()
    c1 = vffread.Vff((out / "after_board" / "menu" / "cdb.vff").read_bytes()).files()
    filed = {k: v for k, v in c1.items() if k not in c0 and BODY.split("\n")[0].encode("utf-16-be") in v}
    ok = check(ok, filed, f"the Wii Menu filed the message: {list(filed)}")
    ok = check(ok, all(k in c1 for k in c0), f"cdb.vff kept all {len(c0)} files it had")
    h, v, _ = box(out / "after_board" / "mbox")
    print(f"receive box now: {h['num']} messages, ids {h['ids']}; new cdb.vff files: "
          f"{[k for k in c1 if k not in c0]}", flush=True)
    return ok


def main():
    step = sys.argv[1] if len(sys.argv) > 1 else ""
    bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    if step == "backup":
        ok = backup(out)
    elif step == "post":
        ok = post(out)
    elif step == "board" and len(sys.argv) > 2:
        minutes = int(sys.argv[sys.argv.index("--minutes") + 1]) if "--minutes" in sys.argv else 20
        ok = board(out, pathlib.Path(sys.argv[2]), minutes)
    else:
        sys.exit(__doc__)
    print("PASS" if ok else "FAIL", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
