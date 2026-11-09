"""Prototype: add one title to today's play log message in a cdb.vff, as HBC
would, to check the Wii Menu accepts what we write.

    python cdb_add.py CDB.VFF NAME TITLEID START_MIN_AGO END_MIN_AGO

Finds the newest playtimelog message, appends a list entry (a play_rec.dat
image), regenerates the text, updates the count, the times and the header CRC,
and grows the file's cluster chain if it needs to. Both FAT copies are kept equal."""
import struct, sys, time, zlib

FAT1, FAT2, ROOT, ROOT_LEN, DATA, CL = 0x20, 0x14020, 0x28020, 0x1000, 0x29020, 0x200
TICKS, TO_2000 = 60750000, 946684800
TEXT_AT, LIST_SIZE, TAIL = 0x548, 0x668, 0x18

img = bytearray(open(sys.argv[1], "rb").read())


def fat(c):
    return struct.unpack_from("<H", img, FAT1 + 2 * c)[0]


def set_fat(c, v):
    struct.pack_into("<H", img, FAT1 + 2 * c, v)
    struct.pack_into("<H", img, FAT2 + 2 * c, v)


def chain(c):
    out = []
    while 2 <= c < 0xfff8:
        out.append(c)
        c = fat(c)
    return out


def clusters_total():
    return (FAT2 - FAT1) // 2


def walk(raw_off_list, path=""):
    """Yield (path, dir entry offset in img, attr, start, size) for every entry."""
    for base, length in raw_off_list:
        for i in range(base, base + length, 32):
            e = img[i:i + 32]
            if e[0] == 0:
                return
            if e[0] == 0xe5 or e[11] == 0x0f:
                continue
            name = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            if name in (".", ".."):
                continue
            full = f"{path}/{name}" + (f".{ext}" if ext else "")
            attr, start, size = e[11], struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
            yield full, i, attr, start, size
            if attr & 0x10:
                yield from walk([(DATA + (c - 2) * CL, CL) for c in chain(start)], full)


def local_ticks(unix):
    return (int(unix) - time.timezone + (3600 if time.localtime(unix).tm_isdst else 0) - TO_2000) * TICKS


def record(name, tid, start, end):
    body = bytearray(128)
    nm = name.encode("utf-16-be")[:80]
    body[4:4 + len(nm)] = nm
    struct.pack_into(">QQ", body, 0x58, start, end)
    body[0x68:0x68 + len(tid)] = tid.encode("ascii")[:6]
    struct.pack_into(">I", body, 0, sum(struct.unpack(">31I", bytes(body[4:]))) & 0xffffffff)
    return bytes(body)


def hhmm(ticks):
    m = ticks // TICKS // 60
    return f"{m // 60:02d}:{m % 60:02d}"


# The newest playtimelog message.
logs = [(p, off, start, size) for p, off, attr, start, size in walk([(ROOT, ROOT_LEN)])
        if not attr & 0x10 and p.endswith(".000")]
logs = [l for l in logs if b"playtimelog" in bytes(img[DATA + (l[2] - 2) * CL:DATA + (l[2] - 2) * CL + 0x40])]
path, dirent, start, size = sorted(logs)[-1]
cl = chain(start)
msg = bytearray(b"".join(img[DATA + (c - 2) * CL:DATA + (c - 1) * CL] for c in cl)[:size])
print("message", path, size, "bytes,", len(cl), "clusters")

count = struct.unpack_from(">I", msg, 0x74)[0]
list_at = struct.unpack_from(">I", msg, 0x52c)[0] + 0x400
assert msg[list_at:list_at + 4] == b"03_0", msg[list_at:list_at + 8]
entries = [msg[list_at + 8 + i * 0x88:list_at + 8 + (i + 1) * 0x88] for i in range(count)]

now = time.time()
s_ago, e_ago = float(sys.argv[4]), float(sys.argv[5])
new = struct.pack("<II", 1, 0) + record(sys.argv[2], sys.argv[3], local_ticks(now - s_ago * 60), local_ticks(now - e_ago * 60))
entries.append(new)
assert len(entries) <= 12

# Text: the subject, then the play history, from the list.
lines, total = [], 0
for e in entries:
    nm = e[12:12 + 80].decode("utf-16-be").split("\0")[0]
    b, l = struct.unpack_from(">QQ", e, 8 + 0x58)
    lines.append(f"{nm}\n     {hhmm(l - b)}")
    total += l - b
body = "Today's Play History\n\n" + "\n\n".join(lines) + f"\n\nTotal Play Time\n     {hhmm(total)}"
text = "Today's Accomplishments".encode("utf-16-be") + b"\0\0" + body.encode("utf-16-be") + b"\0\0"
new_list_at = (TEXT_AT + len(text) + 0x1f) & ~0x1f
lst = bytearray(LIST_SIZE)
lst[:8] = msg[list_at:list_at + 8]
for i, e in enumerate(entries):
    lst[8 + i * 0x88:8 + (i + 1) * 0x88] = e
out = bytearray(msg[:TEXT_AT]) + text
out += bytes(new_list_at - len(out)) + lst + bytes(TAIL)

struct.pack_into(">I", out, 0x74, len(entries))
struct.pack_into(">I", out, 0x7c, local_ticks(now) // TICKS)
struct.pack_into(">Q", out, 0x410, local_ticks(now))
struct.pack_into(">I", out, 0x52c, new_list_at - 0x400)
struct.pack_into(">I", out, 0x540, zlib.crc32(bytes(out[0x400:0x540])) & 0xffffffff)

# Grow the chain if needed, then write clusters and the new size.
need = (len(out) + CL - 1) // CL
while len(cl) < need:
    free = next(c for c in range(2, clusters_total()) if fat(c) == 0)
    set_fat(cl[-1], free)
    set_fat(free, 0xffff)
    cl.append(free)
    print("allocated cluster", free)
padded = out + bytes(need * CL - len(out))
for i, c in enumerate(cl[:need]):
    img[DATA + (c - 2) * CL:DATA + (c - 1) * CL] = padded[i * CL:(i + 1) * CL]
struct.pack_into("<I", img, dirent + 28, len(out))
open(sys.argv[1], "wb").write(img)
print(f"added {sys.argv[2]}: {len(entries)} titles, file {size} -> {len(out)} bytes")
print(body)
