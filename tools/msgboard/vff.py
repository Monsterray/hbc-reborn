"""Read the FAT16 volume inside a Wii cdb.vff: `tree FILE` lists it, `cat FILE
PATH` dumps one file (hex and UTF-16 text). Layout from the first experiment:
FAT copies at 0x20 and 0x14020, root directory at 0x28020 (0x1000 bytes),
512-byte clusters from 0x29020 (cluster 2)."""
import struct, sys

FAT, ROOT, ROOT_LEN, DATA, CL = 0x20, 0x28020, 0x1000, 0x29020, 0x200


def chain(img, c):
    out = []
    while 2 <= c < 0xfff8:
        out.append(c)
        c = struct.unpack_from("<H", img, FAT + 2 * c)[0]
    return out


def read_chain(img, c, size=None):
    data = b"".join(img[DATA + (n - 2) * CL:DATA + (n - 1) * CL] for n in chain(img, c))
    return data if size is None else data[:size]


def entries(raw):
    lfn = []
    for i in range(0, len(raw), 32):
        e = raw[i:i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xe5:
            lfn = []
            continue
        if e[11] == 0x0f:
            part = e[1:11] + e[14:26] + e[28:32]
            lfn.insert(0, part.decode("utf-16-le", "replace").split("\0")[0].rstrip("￿"))
            continue
        short = e[0:8].decode("latin-1").rstrip() + ("." + e[8:11].decode("latin-1").rstrip() if e[8:11].strip() else "")
        name = "".join(lfn) or short
        lfn = []
        attr, start, size = e[11], struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
        if name in (".", ".."):
            continue
        yield name, attr, start, size


def walk(img, raw, path=""):
    for name, attr, start, size in entries(raw):
        p = f"{path}/{name}"
        if attr & 0x10:
            print(f"{p}/")
            yield from walk(img, read_chain(img, start), p)
        else:
            print(f"{p}  ({size} bytes, cluster {start})")
            yield p, start, size


img = open(sys.argv[2], "rb").read()
files = list(walk(img, img[ROOT:ROOT + ROOT_LEN])) if sys.argv[1] == "tree" else None
if sys.argv[1] == "cat":
    import io, contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        files = list(walk(img, img[ROOT:ROOT + ROOT_LEN]))
    for p, start, size in files:
        if p == sys.argv[3]:
            data = read_chain(img, start, size)
            for off in range(0, len(data), 16):
                chunk = data[off:off + 16]
                print(f"{off:04x}  {chunk.hex(' '):47}  {''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)}")
            if len(sys.argv) > 4:
                open(sys.argv[4], "wb").write(data)
