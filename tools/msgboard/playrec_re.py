"""Reverse-engineering helpers for the Message Board's play log, on the Dolphin
profile only. `snap NAME` copies cdb.vff to snaps/NAME.vff; `rec NAME TITLEID
START_MIN_AGO END_MIN_AGO` writes a valid play_rec.dat; `diff A B` lists the
changed byte ranges between two snapshots, with a hex dump of each."""
import pathlib, shutil, struct, sys, time

BASE = pathlib.Path("C:/Users/Monty Perrotti/Documents/hbc-dolphin-sysmenu")
DATA = BASE / "user/Wii/title/00000001/00000002/data"
SNAPS = BASE / "snaps"
TICKS = 60750000
TO_2000 = 946684800


def wii_ticks(unix):
    # Wii time is local time: the RTC holds local seconds since 2000.
    return (int(unix) - time.timezone + (3600 if time.localtime(unix).tm_isdst else 0) - TO_2000) * TICKS


def record(name, title_id, start_ago_min, end_ago_min):
    now = time.time()
    body = bytearray(128)
    nm = name.encode("utf-16-be")[:80]
    body[4:4 + len(nm)] = nm
    struct.pack_into(">QQ", body, 0x58, wii_ticks(now - start_ago_min * 60), wii_ticks(now - end_ago_min * 60))
    tid = title_id.encode("ascii")[:6]
    body[0x68:0x68 + len(tid)] = tid
    words = struct.unpack(">31I", bytes(body[4:]))
    struct.pack_into(">I", body, 0, sum(words) & 0xffffffff)
    (DATA / "play_rec.dat").write_bytes(bytes(body).ljust(128, b"\0"))
    print("wrote play_rec.dat:", name, title_id, f"{start_ago_min - end_ago_min} min")


def diff(a, b):
    x, y = (SNAPS / f"{a}.vff").read_bytes(), (SNAPS / f"{b}.vff").read_bytes()
    ranges, i, n = [], 0, min(len(x), len(y))
    while i < n:
        if x[i] != y[i]:
            j = i
            while j < n and (x[j] != y[j] or (j + 16 < n and x[j:j + 16] != y[j:j + 16])):
                j += 1
            ranges.append((i, j))
            i = j
        i += 1
    print(f"{len(ranges)} changed ranges")
    for s, e in ranges[:40]:
        print(f"-- 0x{s:08x}..0x{e:08x} ({e - s} bytes)")
        lo, hi = s & ~15, min((e + 15) & ~15, s + 512)
        for off in range(lo, hi, 16):
            chunk = y[off:off + 16]
            text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            print(f"   {off:08x}  {chunk.hex(' ')}  {text}")


cmd = sys.argv[1]
if cmd == "snap":
    SNAPS.mkdir(parents=True, exist_ok=True)
    shutil.copy(DATA / "cdb.vff", SNAPS / f"{sys.argv[2]}.vff")
    print("snap", sys.argv[2], "play_rec.dat present:", (DATA / "play_rec.dat").exists())
elif cmd == "rec_at":
    # Absolute local times, YYYY-MM-DDTHH:MM, for the custom-RTC runs.
    import calendar
    loc = lambda t: (calendar.timegm(time.strptime(t, "%Y-%m-%dT%H:%M")) - TO_2000) * TICKS
    now = time.time()
    body = bytearray(128)
    nm = sys.argv[2].encode("utf-16-be")[:80]
    body[4:4 + len(nm)] = nm
    struct.pack_into(">QQ", body, 0x58, loc(sys.argv[4]), loc(sys.argv[5]))
    body[0x68:0x68 + len(sys.argv[3])] = sys.argv[3].encode("ascii")[:6]
    struct.pack_into(">I", body, 0, sum(struct.unpack(">31I", bytes(body[4:]))) & 0xffffffff)
    (DATA / "play_rec.dat").write_bytes(bytes(body))
    print("wrote play_rec.dat:", sys.argv[2], sys.argv[4], "->", sys.argv[5])
elif cmd == "rec":
    record(sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5]))
elif cmd == "diff":
    diff(sys.argv[2], sys.argv[3])
