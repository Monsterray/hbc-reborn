"""Dolphin check of HBC's C writer: build tests/cdblog_host, log sessions into
the Dolphin profile's cdb.vff with it, have the Wii Menu merge a play_rec.dat
record into the same day, and print the day's message.

    python tools/msgboard/c_then_menu.py DAY   (DAY: YYYY-MM-DD, a day with no message yet)"""
import os, pathlib, shutil, struct, subprocess, sys, tempfile

root = pathlib.Path(__file__).resolve().parents[2]
here = pathlib.Path(__file__).parent
sys.path.insert(0, str(here))
import cdb_log  # noqa: E402

BASE = pathlib.Path("C:/Users/Monty Perrotti/Documents/hbc-dolphin-sysmenu")
VFF = BASE / "user/Wii/title/00000001/00000002/data/cdb.vff"
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
day = sys.argv[1]
py = sys.executable

with tempfile.TemporaryDirectory() as tmp:
    exe = pathlib.Path(tmp) / "cdblog_host"
    subprocess.run([CC, "-O2", "-Wall", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I",
                    str(root / "channel/channelapp/source"), str(root / "tests/cdblog_host/main.c"),
                    str(root / "channel/channelapp/source/cdblog.c"), "-o", str(exe)], check=True)
    for args in (["Homebrew Channel", "OHBC", f"{day}T19:00:00", f"{day}T19:20:00", f"{day}T19:20:01"],
                 ["Wii64 (from C)", "HBWII6", f"{day}T19:20:05", f"{day}T19:55:00", f"{day}T19:55:01"],
                 ["Homebrew Channel", "OHBC", f"{day}T19:55:02", f"{day}T19:58:00", f"{day}T19:58:01"]):
        r = subprocess.run([str(exe), str(VFF)] + args, capture_output=True, text=True)
        print("C writer:", args[0], "->", r.stdout.strip(), r.stderr.strip())

subprocess.run([py, str(here / "playrec_re.py"), "rec_at", "HBC Menu Romeo", "HROM01",
                f"{day}T20:00", f"{day}T20:10"], check=True)
env = dict(os.environ, RTC=f"{day}T20:15",
           PAD="wait 20; press Cross; wait 2; press Cross; wait 2; press Cross; wait 8")
subprocess.run([py, str(here / "dolphin_sysmenu.py"), "menu", "50"], env=env, capture_output=True)
img = VFF.read_bytes()
y, m, d = (int(x) for x in day.split("-"))
v = cdb_log.Vff(bytearray(img))
found = None
for c in range(2, v.nclusters):
    off = cdb_log.DATA + (c - 2) * cdb_log.CL
    h = img[off:off + 0x80]
    if h[:8] == b"CDBFILE\x02" and h[0x14:0x20] == b"playtimelog\0":
        secs = struct.unpack_from(">I", h, 0x7c)[0]
        import time
        t = time.gmtime(secs + 946684800)
        if (t.tm_year, t.tm_mon, t.tm_mday) == (y, m, d):
            found = img[off:off + 0x2000]
if not found:
    raise SystemExit("no message for that day")
i = found.find("Today".encode("utf-16-be"))
print("titles:", struct.unpack_from(">I", found, 0x74)[0])
print(found[i:].decode("utf-16-be", "replace").split("\0\0")[0].replace("\0", " | "))
print("play_rec.dat left:", (VFF.parent / "play_rec.dat").exists())
