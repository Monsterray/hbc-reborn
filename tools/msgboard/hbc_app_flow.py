"""Dolphin check of HBC's play log with an app: boot this tree's retail WAD in
the Wii Menu profile on day DAY, send tests/agent_app over Wiiload, let it run,
exit it back to HBC, close Dolphin (HBC's power-off logs its own session), and
print the day's message.

    python tools/msgboard/hbc_app_flow.py DAY      (YYYY-MM-DD, a day with no message)"""
import os, pathlib, struct, subprocess, sys, time

root = pathlib.Path(__file__).resolve().parents[2]
here = pathlib.Path(__file__).parent
sys.path.insert(0, str(root / "tools"))
sys.path.insert(0, str(root / "tests"))
import hbc  # noqa: E402
import dolphin_smoke  # noqa: E402

BASE = pathlib.Path("C:/Users/Monty Perrotti/Documents/hbc-dolphin-sysmenu")
VFF = BASE / "user/Wii/title/00000001/00000002/data/cdb.vff"
day = sys.argv[1]
env = dict(os.environ, RTC=f"{day}T17:00", EXTRA="Dolphin.Core.EnableCheats=True")
runner = subprocess.Popen([sys.executable, str(here / "dolphin_sysmenu.py"), "install",
                           str(root / "channel/title/channel_retail.wad"), "170"], env=env)
wii = dolphin_smoke.host_address()
print("HBC:", hbc.hbc_wait(wii, 90), flush=True)
time.sleep(20)                                   # HBC's own first session
hbc.send(wii, str(root / "tests/agent_app/agent_app.dol"), ["stay", "120"])
deadline = time.monotonic() + 60
while time.monotonic() < deadline:
    try:
        if hbc.is_agent(hbc.version(wii, 2)):
            break
    except (OSError, hbc.HBCError):
        pass
    time.sleep(1)
print("app:", hbc.version(wii), flush=True)
time.sleep(40)                                   # the app's session
hbc.exit_app(wii)
print("back:", hbc.hbc_wait(wii, 90), flush=True)
time.sleep(15)                                   # HBC again, until Dolphin closes
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
            print("titles:", struct.unpack_from(">I", msg, 0x74)[0])
            print(msg[i:].decode("utf-16-be", "replace").split("\0\0")[0].replace("\0", " | "))
            lst = struct.unpack_from(">I", msg, 0x52c)[0] + 0x400
            for k in range(struct.unpack_from(">I", msg, 0x74)[0]):
                e = msg[lst + 8 + k * 0x88:lst + 8 + (k + 1) * 0x88]
                b, l = struct.unpack_from(">QQ", e, 8 + 0x58)
                print(f"  {e[8 + 0x68:8 + 0x6e]!r}: {(l - b) / 60750000:.0f} s")
            break
else:
    print("no message for", day)
