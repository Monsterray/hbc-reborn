"""Reference implementation of HBC's play log writer (docs/messageboard.md):
add one record to the Message Board's play log in a cdb.vff image, creating
the day's message when there is none. HBC's C version follows this.

    python cdb_log.py CDB.VFF NAME TITLEID START END [NOW]

Times are local, YYYY-MM-DDTHH:MM[:SS]; NOW (the clock at writing, default
END) goes into the FAT and message timestamps."""
import calendar, struct, sys, time, zlib

FAT1, FAT2, ROOT, ROOT_LEN, DATA, CL = 0x20, 0x14020, 0x28020, 0x1000, 0x29020, 0x200
TICKS = 60750000
TEXT_AT, LIST_SIZE, TAIL, MAX_TITLES = 0x548, 0x668, 0x18, 12


class Vff:
    def __init__(self, img):
        self.img = img
        self.nclusters = (FAT2 - FAT1) // 2

    def fat(self, c):
        return struct.unpack_from("<H", self.img, FAT1 + 2 * c)[0]

    def set_fat(self, c, v):
        struct.pack_into("<H", self.img, FAT1 + 2 * c, v)
        struct.pack_into("<H", self.img, FAT2 + 2 * c, v)

    def chain(self, c):
        out = []
        while 2 <= c < 0xfff8:
            out.append(c)
            c = self.fat(c)
        return out

    def at(self, c):
        return DATA + (c - 2) * CL

    def alloc(self, after=None):
        c = next(n for n in range(2, self.nclusters) if self.fat(n) == 0)
        self.set_fat(c, 0xffff)
        if after:
            self.set_fat(after, c)
        self.img[self.at(c):self.at(c) + CL] = bytes(CL)
        return c

    # Directories: the root is a fixed area; others are cluster chains.
    def slots(self, start):
        if start == 0:
            return [ROOT + i for i in range(0, ROOT_LEN, 32)]
        return [self.at(c) + i for c in self.chain(start) for i in range(0, CL, 32)]

    def find(self, start, short):
        for off in self.slots(start):
            e = self.img[off:off + 32]
            if e[0] == 0:
                return None
            if e[0] != 0xe5 and e[11] != 0x0f and e[:11] == short:
                return off
        return None

    def free_slots(self, start, n):
        """Offsets of n consecutive free entries, growing the directory if needed."""
        run = []
        for off in self.slots(start):
            if self.img[off] in (0, 0xe5):
                run.append(off)
                if len(run) == n:
                    return run
            else:
                run = []
        assert start != 0, "root directory full"
        self.alloc(self.chain(start)[-1])
        return self.free_slots(start, n)

    def entry(self, off, short, attr, cluster, size, stamp):
        t, d = stamp
        e = bytearray(32)
        e[0:11] = short
        e[11] = attr
        e[13] = 1
        struct.pack_into("<HHH", e, 14, t, d, d)
        struct.pack_into("<HH", e, 22, t, d)
        struct.pack_into("<HI", e, 26, cluster, size)
        self.img[off:off + 32] = e

    def mkdir(self, parent, short, stamp, lfn=None):
        c = self.alloc()
        n = 2 if lfn else 1
        slots = self.free_slots(parent, n)
        if lfn:
            s = 0
            for ch in short:
                s = (((s & 1) << 7) | (s >> 1)) + ch & 0xff
            name = lfn.encode("utf-16-le") + b"\0\0"
            name += b"\xff" * (26 - len(name))
            e = bytearray(32)
            e[0], e[11], e[13] = 0x41, 0x0f, s
            e[1:11], e[14:26], e[28:32] = name[0:10], name[10:22], name[22:26]
            self.img[slots[0]:slots[0] + 32] = e
        self.entry(slots[-1], short, 0x10, c, 0, stamp)
        base = self.at(c)
        self.entry(base, b".          ", 0x10, c, 0, stamp)
        self.entry(base + 32, b"..         ", 0x10, parent, 0, stamp)
        return c

    def subdir(self, parent, short, stamp, lfn=None):
        off = self.find(parent, short)
        if off is not None:
            return struct.unpack_from("<H", self.img, off + 26)[0]
        return self.mkdir(parent, short, stamp, lfn)

    def read_file(self, off):
        start, size = struct.unpack_from("<HI", self.img, off + 26)
        data = b"".join(self.img[self.at(c):self.at(c) + CL] for c in self.chain(start))
        return bytearray(data[:size])

    def write_file(self, off, data, stamp):
        start = struct.unpack_from("<H", self.img, off + 26)[0]
        cl = self.chain(start) if start else []
        need = max(1, (len(data) + CL - 1) // CL)
        while len(cl) < need:
            cl.append(self.alloc(cl[-1] if cl else None))
        if not start:
            struct.pack_into("<H", self.img, off + 26, cl[0])
        for c in cl[need:]:  # a shorter file gives clusters back
            self.set_fat(c, 0)
        self.set_fat(cl[need - 1], 0xffff)
        padded = bytes(data) + bytes(need * CL - len(data))
        for i, c in enumerate(cl[:need]):
            self.img[self.at(c):self.at(c) + CL] = padded[i * CL:(i + 1) * CL]
        t, d = stamp
        struct.pack_into("<HH", self.img, off + 22, t, d)
        struct.pack_into("<I", self.img, off + 28, len(data))


def parse(t):
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M"):
        try:
            return time.strptime(t, fmt)
        except ValueError:
            pass
    raise ValueError(t)


def secs2000(tm):
    return calendar.timegm(tm) - 946684800


def fat_stamp(tm):
    return ((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec // 2),
            ((tm.tm_year - 1980) << 9) | (tm.tm_mon << 5) | tm.tm_mday)


def short(name):
    base, _, ext = name.partition(".")
    return base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def record(name, tid, start, end):
    body = bytearray(128)
    nm = name.encode("utf-16-be")[:80]
    body[4:4 + len(nm)] = nm
    struct.pack_into(">QQ", body, 0x58, start, end)
    body[0x68:0x68 + min(len(tid), 6)] = tid.encode("ascii")[:6]
    struct.pack_into(">I", body, 0, sum(struct.unpack(">31I", bytes(body[4:]))) & 0xffffffff)
    return bytes(body)


def hhmm(ticks):
    m = ticks // TICKS // 60
    return f"{m // 60:02d}:{m % 60:02d}"


def build(header, entries, now_ticks):
    lines, total = [], 0
    for e in entries:
        nm = e[12:12 + 80].decode("utf-16-be").split("\0")[0]
        b, l = struct.unpack_from(">QQ", e, 8 + 0x58)
        lines.append(f"{nm}\n     {hhmm(l - b)}")
        total += l - b
    body = "Today's Play History\n\n" + "\n\n".join(lines) + f"\n\nTotal Play Time\n     {hhmm(total)}"
    text = "Today's Accomplishments".encode("utf-16-be") + b"\0\0" + body.encode("utf-16-be") + b"\0\0"
    list_at = (TEXT_AT + len(text) + 0x1f) & ~0x1f
    lst = bytearray(LIST_SIZE)
    lst[:4] = b"03_0"
    for i, e in enumerate(entries):
        lst[8 + i * 0x88:8 + (i + 1) * 0x88] = e
    out = bytearray(header[:TEXT_AT]) + text
    out += bytes(list_at - len(out)) + lst + bytes(TAIL)
    newest = max(struct.unpack_from(">Q", e, 8 + 0x60)[0] for e in entries)
    struct.pack_into(">I", out, 0x74, len(entries))
    struct.pack_into(">I", out, 0x7c, newest // TICKS)
    struct.pack_into(">Q", out, 0x410, now_ticks)
    struct.pack_into(">I", out, 0x52c, list_at - 0x400)
    struct.pack_into(">I", out, 0x540, zlib.crc32(bytes(out[0x400:0x540])) & 0xffffffff)
    return out


def new_header(wiiid, number):
    h = bytearray(TEXT_AT)
    h[0:8] = b"CDBFILE\x02"
    h[8:16] = wiiid
    struct.pack_into("<I", h, 0x10, 12)  # little-endian, unlike the rest
    h[0x14:0x20] = b"playtimelog\0"
    struct.pack_into(">I", h, 0x70, number)
    h[0x400:0x404] = b"RI_5"
    struct.pack_into(">I", h, 0x40c, 2)
    struct.pack_into(">IIII", h, 0x518, 1, 0x148, 0x178, 0)
    struct.pack_into(">IIII", h, 0x528, 3, 0, LIST_SIZE, 0)
    return h


def log(img, name, tid, start_tm, end_tm, now_tm, wiiid=None):
    v = Vff(img)
    rec = struct.pack("<II", 1, 0) + record(name, tid, secs2000(start_tm) * TICKS, secs2000(end_tm) * TICKS)
    stamp = fat_stamp(now_tm)
    now_ticks = secs2000(now_tm) * TICKS
    # The day's message: under /YYYY/MM-1/DD/*/*/HAEA_#1/LOG, type playtimelog.
    day = [b"%-11s" % f"{end_tm.tm_year:04d}".encode(), b"%-11s" % f"{end_tm.tm_mon - 1:02d}".encode(),
           b"%-11s" % f"{end_tm.tm_mday:02d}".encode()]
    found = None
    d = 0
    for s in day:
        off = v.find(d, s)
        if off is None:
            break
        d = struct.unpack_from("<H", img, off + 26)[0]
    else:
        for hoff in v.slots(d):
            if img[hoff] == 0:
                break
            if img[hoff] in (0xe5, 0x2e) or img[hoff + 11] != 0x10:
                continue
            for moff in v.slots(struct.unpack_from("<H", img, hoff + 26)[0]):
                if img[moff] == 0:
                    break
                if img[moff] in (0xe5, 0x2e) or img[moff + 11] != 0x10:
                    continue
                cur = struct.unpack_from("<H", img, moff + 26)[0]
                for s in (b"HAEA_#1    ", b"LOG        "):
                    o = v.find(cur, s)
                    cur = struct.unpack_from("<H", img, o + 26)[0] if o is not None else None
                    if cur is None:
                        break
                if cur is None:
                    continue
                for foff in v.slots(cur):
                    if img[foff] == 0:
                        break
                    if img[foff] == 0xe5 or img[foff + 11] & 0x18:
                        continue
                    data = v.read_file(foff)
                    if data[:8] == b"CDBFILE\x02" and data[0x14:0x20] == b"playtimelog\0":
                        found = (foff, data)
    if found:
        foff, data = found
        count = struct.unpack_from(">I", data, 0x74)[0]
        lst = struct.unpack_from(">I", data, 0x52c)[0] + 0x400
        entries = [bytes(data[lst + 8 + i * 0x88:lst + 8 + (i + 1) * 0x88]) for i in range(count)]
        # The Wii Menu lists every record; HBC keeps one line per title a day,
        # so a title already there gets longer instead (its start stays).
        for i, e in enumerate(entries):
            if e[8 + 4:8 + 0x58] == rec[8 + 4:8 + 0x58] and e[8 + 0x68:8 + 0x6e] == rec[8 + 0x68:8 + 0x6e]:
                b, l = struct.unpack_from(">QQ", e, 8 + 0x58)
                rb, rl = struct.unpack_from(">QQ", rec, 8 + 0x58)
                nm = e[8 + 4:8 + 0x58].decode("utf-16-be").split("\0")[0]
                entries[i] = struct.pack("<II", 1, 0) + record(nm, e[8 + 0x68:8 + 0x6e].split(b"\0")[0].decode(),
                                                               b, b + (l - b) + (rl - rb))
                v.write_file(foff, build(data, entries, now_ticks), stamp)
                return "combined with the title's line"
        if len(entries) >= MAX_TITLES:
            raise SystemExit("the day's play log is full")
        out = build(data, entries + [rec], now_ticks)
        v.write_file(foff, out, stamp)
        return "added to the day's message"
    # A new message: the next number after cdb.conf's, a folder per field of END.
    conf = v.find(0, b"CDB~1   CON")
    number = struct.unpack(">I", bytes(v.read_file(conf)[:4]))[0] + 1
    if wiiid is None:
        raise SystemExit("no message to take the console ID from; pass it")
    cur = 0
    for s in (f"{end_tm.tm_year:04d}", f"{end_tm.tm_mon - 1:02d}", f"{end_tm.tm_mday:02d}",
              f"{end_tm.tm_hour:02d}", f"{end_tm.tm_min:02d}", "HAEA_#1"):
        cur = v.subdir(cur, short(s), stamp)
    cur = v.subdir(cur, short("LOG"), stamp, lfn="log")
    fname = f"{secs2000(end_tm):08X}.000"
    foff = v.free_slots(cur, 1)[0]
    v.entry(foff, short(fname), 0x20, 0, 0, stamp)
    v.write_file(foff, build(new_header(wiiid, number), [rec], now_ticks), stamp)
    v.write_file(conf, struct.pack(">I", number), stamp)
    return f"created message {number}: {fname}"


def wiiid_of(img):
    """The console ID from any message already in the image."""
    for c in range(2, (FAT2 - FAT1) // 2):
        off = DATA + (c - 2) * CL
        if img[off:off + 8] == b"CDBFILE\x02":
            return bytes(img[off + 8:off + 16])
    return None


if __name__ == "__main__":
    path = sys.argv[1]
    img = bytearray(open(path, "rb").read())
    start, end = parse(sys.argv[4]), parse(sys.argv[5])
    now = parse(sys.argv[6]) if len(sys.argv) > 6 else end
    print(log(img, sys.argv[2], sys.argv[3], start, end, now, wiiid_of(img)))
    open(path, "wb").write(img)
