#!/usr/bin/env python3
"""Study a Wii's Mii face resource, RFL_Res.dat (docs/research/mii-rendering.md).

  mii_res.py info FILE           the archives: parts, sizes, texture formats
  mii_res.py dump FILE DIR       every shape as OBJ, every texture as PNG

The file is Nintendo's: read it from your own Wii (tests/wii_mii_res.py puts
a copy in the bench job's run folder) and keep it and its dumps out of the
repository. Big-endian layout, from the RFL decompilations:

  file:     u16 archive count (18), u16 version (>= 0x39D), u32 archive offsets
  archive:  u16 file count N, u16 largest size, u32 x (N+1) file offsets
            from the end of that table (the last is the end)
  shape:    4-byte tag; faceline only: 3 x (3 floats) anchors (nose, beard,
            hair); u16 count, s16 x/y/z positions (8 fraction bits); u16
            count, s16 normals (14); except nose, forehead, hair and beard:
            u16 count, s16 s/t (13); u8 primitive count, each u8 vertex
            count, u8 GX primitive, then u8 indexes per vertex
  texture:  a BTI-like 0x20 header (format, size, wrap, image offset at 0x1C)
"""

import pathlib
import struct
import sys
import zlib

ARCHIVES = ["beard", "eye", "eyebrow", "faceline", "facetex", "forehead", "glass", "glasstex",
            "hair", "mask", "mole", "mouth", "mustache", "nose", "nline", "nlinetex", "cap", "captex"]
SHAPES = {"beard": b"berd", "faceline": b"face", "forehead": b"frhd", "glass": b"glas",
          "hair": b"hair", "mask": b"mask", "nose": b"nose", "nline": b"nsln", "cap": b"cap_"}
NO_TEXCOORDS = {"nose", "forehead", "hair", "beard"}
FORMATS = {0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565", 5: "RGB5A3", 6: "RGBA8",
           8: "C4", 9: "C8", 10: "C14X2", 14: "CMPR"}
# GX primitives: quads, triangles, triangle strip, triangle fan (others unused).
PRIMS = {0x80: "quads", 0x90: "triangles", 0x98: "strip", 0xA0: "fan", 0xA8: "lines",
         0xB0: "linestrip", 0xB8: "points"}


class ResError(ValueError):
    pass


def archives(data):
    """{name: [file bytes, ...]} for each of the 18 archives."""
    try:
        return _archives(data)
    except struct.error as exc:
        raise ResError(f"cut short: {exc}") from None


def _archives(data):
    if len(data) < 4 + 4 * len(ARCHIVES):
        raise ResError("too short for RFL_Res.dat")
    count, version = struct.unpack_from(">HH", data)
    if count != len(ARCHIVES):
        raise ResError(f"{count} archives, not {len(ARCHIVES)}: not RFL_Res.dat")
    if version < 0x39D:
        raise ResError(f"version {version:#x} is older than RFL accepts (0x39D)")
    out = {}
    for i, name in enumerate(ARCHIVES):
        at = struct.unpack_from(">I", data, 4 + 4 * i)[0]
        n, _largest = struct.unpack_from(">HH", data, at)
        offs = struct.unpack_from(f">{n + 1}I", data, at + 4)
        base = at + 4 + 4 * (n + 1)
        if base + offs[-1] > len(data) or list(offs) != sorted(offs):
            raise ResError(f"archive {name}: offsets out of order or past the end")
        out[name] = [data[base + offs[j]:base + offs[j + 1]] for j in range(n)]
    return out, version


def shape(name, f):
    """(anchors, positions, normals, texcoords, primitives) of one shape file;
    positions etc. are float tuples, primitives (kind, [(p, n, t), ...])."""
    tag = SHAPES[name]
    if f[:4] != tag:
        raise ResError(f"{name}: tag {f[:4]!r}, not {tag!r}")
    at, anchors = 4, None
    if name == "faceline":
        anchors = [struct.unpack_from(">3f", f, at + 12 * k) for k in range(3)]
        at += 36
    if struct.unpack_from(">H", f, at)[0] == 0:
        return anchors, [], [], [], []

    def table(at, fields, frac):
        n = struct.unpack_from(">H", f, at)[0]
        raw = struct.unpack_from(f">{n * fields}h", f, at + 2)
        rows = [tuple(v / (1 << frac) for v in raw[k * fields:(k + 1) * fields]) for k in range(n)]
        return rows, at + 2 + 2 * n * fields

    pos, at = table(at, 3, 8)
    nrm, at = table(at, 3, 14)
    txc = []
    if name not in NO_TEXCOORDS:
        txc, at = table(at, 2, 13)
    per = 2 if name in NO_TEXCOORDS else 3
    prims = []
    count = f[at]
    at += 1
    for _ in range(count):
        n, kind = f[at], f[at + 1]
        at += 2
        verts = [tuple(f[at + per * v:at + per * v + per]) for v in range(n)]
        at += per * n
        prims.append((PRIMS.get(kind, f"{kind:#x}"), verts))
    return anchors, pos, nrm, txc, prims


def triangles(kind, verts):
    if kind == "triangles":
        return [verts[k:k + 3] for k in range(0, len(verts) - 2, 3)]
    if kind == "quads":
        return [t for k in range(0, len(verts) - 3, 4)
                for t in ([verts[k], verts[k + 1], verts[k + 2]], [verts[k], verts[k + 2], verts[k + 3]])]
    if kind == "strip":
        return [[verts[k], verts[k + 1], verts[k + 2]] if k % 2 == 0 else [verts[k + 1], verts[k], verts[k + 2]]
                for k in range(len(verts) - 2)]
    if kind == "fan":
        return [[verts[0], verts[k], verts[k + 1]] for k in range(1, len(verts) - 1)]
    return []


def obj(name, parsed):
    anchors, pos, nrm, txc, prims = parsed
    lines = [f"# {name}"]
    if anchors:
        lines += [f"# anchor {label} {x:.3f} {y:.3f} {z:.3f}"
                  for label, (x, y, z) in zip(("nose", "beard", "hair"), anchors)]
    lines += [f"v {x:.4f} {y:.4f} {z:.4f}" for x, y, z in pos]
    lines += [f"vn {x:.5f} {y:.5f} {z:.5f}" for x, y, z in nrm]
    lines += [f"vt {s:.5f} {1 - t:.5f}" for s, t in txc]
    for kind, verts in prims:
        for tri in triangles(kind, verts):
            if txc:
                lines.append("f " + " ".join(f"{p + 1}/{t + 1}/{n + 1}" for p, n, t in tri))
            else:
                lines.append("f " + " ".join(f"{p + 1}//{n + 1}" for p, n in tri))
    return "\n".join(lines) + "\n"


def texture(f):
    """(format name, width, height, RGBA rows) of one texture file."""
    fmt, _alpha, w, h = struct.unpack_from(">BBHH", f)
    image = struct.unpack_from(">I", f, 0x1C)[0]
    px = decode(fmt, f[image:], w, h)
    return FORMATS.get(fmt, str(fmt)), w, h, px


# Bit expansion as GX does it: the high bits repeated into the low ones.
def x5(v):
    return v << 3 | v >> 2


def x6(v):
    return v << 2 | v >> 4


def x3(v):
    return v << 5 | v << 2 | v >> 1


def rgb5a3(v):
    if v & 0x8000:
        return (x5(v >> 10 & 31), x5(v >> 5 & 31), x5(v & 31), 255)
    return ((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, x3(v >> 12 & 7))


def decode(fmt, d, w, h):
    """GX tiled texture to rows of RGBA tuples (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8)."""
    bw, bh, bits = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16),
                    5: (4, 4, 16), 6: (4, 4, 32)}.get(fmt, (0, 0, 0))
    if not bits:
        raise ResError(f"texture format {FORMATS.get(fmt, fmt)} not decoded")
    out = [[(0, 0, 0, 0)] * w for _ in range(h)]
    at = 0
    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            if fmt == 6:   # RGBA8: AR then GB halves of 32 bytes
                for k in range(16):
                    x, y = bx + k % 4, by + k // 4
                    a, r = d[at + 2 * k], d[at + 2 * k + 1]
                    g, b = d[at + 32 + 2 * k], d[at + 33 + 2 * k]
                    if x < w and y < h:
                        out[y][x] = (r, g, b, a)
                at += 64
                continue
            for k in range(bw * bh):
                x, y = bx + k % bw, by + k // bw
                if fmt == 0:
                    v = d[at + k // 2] >> (4 if k % 2 == 0 else 0) & 15
                    p = (v * 17,) * 3 + (v * 17,)
                elif fmt == 1:
                    v = d[at + k]
                    p = (v, v, v, v)
                elif fmt == 2:
                    v = d[at + k]
                    p = ((v & 15) * 17,) * 3 + ((v >> 4) * 17,)
                elif fmt == 3:
                    a, i = d[at + 2 * k], d[at + 2 * k + 1]
                    p = (i, i, i, a)
                else:
                    v = d[at + 2 * k] << 8 | d[at + 2 * k + 1]
                    p = rgb5a3(v) if fmt == 5 else \
                        (x5(v >> 11), x6(v >> 5 & 63), x5(v & 31), 255)
                if x < w and y < h:
                    out[y][x] = p
            at += bw * bh * bits // 8
    return out


def png(w, h, rows):
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def info(data):
    arcs, version = archives(data)
    print(f"RFL_Res.dat version {version:#x}, {len(data)} bytes")
    for name, files in arcs.items():
        sizes = [len(f) for f in files]
        line = f"  {name:9} {len(files):3} files, {sum(sizes):7} bytes"
        if name in SHAPES:
            counts = [len(shape(name, f)[1]) for f in files]
            line += f", positions {min(counts)}..{max(counts)}, {counts.count(0)} empty"
        else:
            kinds = sorted({f"{texture(f)[0]} {texture(f)[1]}x{texture(f)[2]}" for f in files if f})
            line += ", " + ", ".join(kinds)
        print(line)


def dump(data, out):
    arcs, _ = archives(data)
    out.mkdir(parents=True, exist_ok=True)
    for name, files in arcs.items():
        for i, f in enumerate(files):
            if name in SHAPES:
                parsed = shape(name, f)
                if parsed[1]:
                    (out / f"{name}_{i:03}.obj").write_text(obj(f"{name} {i}", parsed), newline="\n")
            elif f:
                _, w, h, px = texture(f)
                (out / f"{name}_{i:03}.png").write_bytes(png(w, h, px))
    print(f"dumped to {out}")


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("info", "dump") or (sys.argv[1] == "dump") != (len(sys.argv) == 4):
        raise SystemExit(__doc__)
    data = pathlib.Path(sys.argv[2]).read_bytes()
    try:
        if sys.argv[1] == "info":
            info(data)
        else:
            dump(data, pathlib.Path(sys.argv[3]))
    except (ResError, struct.error) as exc:
        raise SystemExit(f"{sys.argv[2]}: {exc}")


if __name__ == "__main__":
    main()
