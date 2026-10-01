"""Dolphin check of the play log's name for a Wiiload upload without the
agent: boot this tree's retail WAD in the Wii Menu profile on day DAY, send
tests/rtc_shift as MyTool/boot.dol (hbc.py names it "MyTool"; rtc_shift
exits at once), close Dolphin, and print the day's message.

    python tools/msgboard/hbc_named_flow.py DAY      (YYYY-MM-DD, a day with no message)"""
import os, pathlib, shutil, struct, subprocess, sys, tempfile, time

root = pathlib.Path(__file__).resolve().parents[2]
here = pathlib.Path(__file__).parent
sys.path.insert(0, str(root / "tools"))
sys.path.insert(0, str(root / "tests"))
import hbc  # noqa: E402
import dolphin_smoke  # noqa: E402

BASE = pathlib.Path("C:/Users/Monty Perrotti/Documents/hbc-dolphin-sysmenu")
VFF = BASE / "user/Wii/title/00000001/00000002/data/cdb.vff"
day = sys.argv[1]
# Another Dolphin (WiiStation's, say) on this PC answers on the same address:
# talking to it would drive someone else's emulator. Wait for a free port.
import socket
with socket.socket() as probe:
    probe.settimeout(1)
    if probe.connect_ex((dolphin_smoke.host_address(), 4299)) == 0:
        raise SystemExit("TCP 4299 is in use by another Dolphin on this PC; try again when it is free")
env = dict(os.environ, RTC=f"{day}T17:00", EXTRA="Dolphin.Core.EnableCheats=True")
runner = subprocess.Popen([sys.executable, str(here / "dolphin_sysmenu.py"), "install",
                           str(root / "channel/title/channel_retail.wad"), "110"], env=env)
wii = dolphin_smoke.host_address()
print("HBC:", hbc.hbc_wait(wii, 90), flush=True)
time.sleep(20)
tmp = pathlib.Path(tempfile.mkdtemp()) / "MyTool"
tmp.mkdir()
shutil.copy(root / "tests/rtc_shift/rtc_shift.dol", tmp / "boot.dol")
hbc.send(wii, str(tmp / "boot.dol"), ["0"])
time.sleep(5)
print("back:", hbc.hbc_wait(wii, 90), flush=True)
runner.wait()

img = VFF.read_bytes()
y, m, d = (int(x) for x in day.split("-"))
for c in range(2, 40000):
    off = 0x29020 + (c - 2) * 0x200
    h = img[off:off + 0x80]
    if h[:8] == b"CDBFILE\x02" and h[0x14:0x20] == b"playtimelog\0":
        t = time.gmtime(struct.unpack_from(">I", h, 0x7c)[0] + 946684800)
        if (t.tm_year, t.tm_mon, t.tm_mday) == (y, m, d):
            msg = img[off:off + 0x2000]
            i = msg.find("Today".encode("utf-16-be"))
            print(msg[i:].decode("utf-16-be", "replace").split("\0\0")[0].replace("\0", " | "))
            break
else:
    print("no message for", day)
