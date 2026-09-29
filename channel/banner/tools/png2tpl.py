#!/usr/bin/env python3
"""Convert an 8-bit PNG to a single-texture GX TPL file.

usage: png2tpl.py input.png output.tpl wrap_s wrap_t

Grey becomes I8, grey+alpha IA8, RGB RGB565, and RGBA RGBA8. The output
matches the original libpng-based png2tpl byte for byte.
"""

import struct
import sys
import zlib

GRAY, RGB, GRAY_ALPHA, RGBA = 0, 2, 4, 6
CHANNELS = {GRAY: 1, RGB: 3, GRAY_ALPHA: 2, RGBA: 4}


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG file")
    pos, idat, header = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    width, height, depth, color, _, _, interlace = header
    if depth != 8 or color not in CHANNELS or interlace:
        raise SystemExit(f"{path}: only non-interlaced 8-bit grey, RGB and "
                         "alpha PNGs are supported")

    bpp = CHANNELS[color]
    stride = width * bpp
    raw = zlib.decompress(b"".join(idat))
    rows, prev = [], bytearray(stride)
    for y in range(height):
        line = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        ftype, row = line[0], bytearray(line[1:])
        for x in range(stride):
            a = row[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ftype == 1:
                row[x] = (row[x] + a) & 0xff
            elif ftype == 2:
                row[x] = (row[x] + b) & 0xff
            elif ftype == 3:
                row[x] = (row[x] + ((a + b) >> 1)) & 0xff
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
                row[x] = (row[x] + pred) & 0xff
        rows.append(row)
        prev = row
    return width, height, color, rows


def tile(width, height, bw, bh, texel, pixel):
    """Lay pixels out in bw x bh blocks of `texel` bytes each."""
    wo = (width + bw - 1) & ~(bw - 1)
    ho = (height + bh - 1) & ~(bh - 1)
    out = bytearray(wo * ho * texel)
    blocks_per_row = wo // bw
    for y in range(height):
        for x in range(width):
            block = (y // bh) * blocks_per_row + x // bw
            offset = (block * bw * bh + (y % bh) * bw + x % bw) * texel
            pixel(out, offset, x, y)
    return out


def encode(width, height, color, rows):
    if color == GRAY:
        def px(out, o, x, y):
            out[o] = rows[y][x]
        return 1, tile(width, height, 8, 4, 1, px)
    if color == GRAY_ALPHA:
        def px(out, o, x, y):
            g, a = rows[y][2 * x:2 * x + 2]
            out[o:o + 2] = bytes((a, g))
        return 3, tile(width, height, 4, 4, 2, px)
    if color == RGB:
        def px(out, o, x, y):
            r, g, b = rows[y][3 * x:3 * x + 3]
            out[o:o + 2] = struct.pack(">H", (r << 8 & 0xf800) | (g << 3 & 0x07e0) | (b >> 3))
        return 4, tile(width, height, 4, 4, 2, px)

    # RGBA8 blocks hold 16 AR pairs followed by 16 GB pairs.
    wo, ho = (width + 3) & ~3, (height + 3) & ~3
    out = bytearray(wo * ho * 4)
    for y in range(height):
        for x in range(width):
            r, g, b, a = rows[y][4 * x:4 * x + 4]
            block = (y // 4) * (wo // 4) + x // 4
            o = block * 64 + ((y % 4) * 4 + x % 4) * 2
            out[o:o + 2] = bytes((a, r))
            out[o + 32:o + 34] = bytes((g, b))
    return 6, out


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__.strip().splitlines()[2])
    width, height, color, rows = read_png(sys.argv[1])
    print(f"Texture: {width} x {height}")
    fmt, texels = encode(width, height, color, rows)
    header = struct.pack(">III", 0x0020af30, 1, 12) + struct.pack(">II", 20, 0)
    texhdr = struct.pack(">HHIIIIIIfBBBB", height, width, fmt, 64,
                         int(sys.argv[3]), int(sys.argv[4]), 1, 1, 0.0, 0, 0, 0, 0)
    blob = header + texhdr
    with open(sys.argv[2], "wb") as out:
        out.write(blob + bytes(64 - len(blob)) + texels)


if __name__ == "__main__":
    main()
