#!/usr/bin/env python3
"""Make Message Board stationery: the board-data part (application/x-wii-msgboard,
sdk/mboard's mboard_msg.board) that gives a letter its own look on a Wii.

    stationery.py LETTER.png THUMB.png [--sound WAV] [--chjump TITLE-ID] -o board.arc

The archive is laid out as the WiiLink letters the Wii Menu files carry
(docs/messageboard-messages.md, "custom stationery and a board icon"):

    letter_LZ.bin     LZ77-compressed U8: img/my_Letter_a.tpl .. _i.tpl, the
                      letter cut into a 9-slice frame (a b c / d e f / g h i:
                      64 | 384 | 64 wide, 144 / 168 / 64 high: 512 x 376 in all)
    thumbnail_LZ.bin  LZ77 U8: img/my_LetterS_b.tpl, the board icon, 144 x 96
    sound.bns         optional: mono Nintendo DSP-ADPCM, as WiiLink's (8 kHz there)
    chjump.bin        optional: a button that opens a channel (as a Wii Shop
                      message's)

Images are any size PIL reads; they are scaled to fit and kept with their
alpha (RGB5A3 TPLs). The whole archive must stay within the board data's
0x1E000 bytes (sdk/mboard's MBOARD_BOARD_MAX); a long sound is what overflows.
Pure Python plus Pillow; tests/test_stationery.py checks every encoder against
a decoder of its own.
"""

import argparse
import struct
import sys
import wave

BOARD_MAX = 0x1E000
LETTER_W, LETTER_H = 512, 376
THUMB_W, THUMB_H = 144, 96
COLS = (64, 384, 64)
ROWS = (144, 168, 64)
SLICES = "abcdefghi"
# WiiLink's letter archive lists the corners first, then the rest.
SLICE_ORDER = "acgibdefh"


# ---- Textures: TPL, RGB5A3 ---------------------------------------------------

def rgb5a3(r, g, b, a):
    if a >= 0xE0:
        return 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3
    return (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4


def tpl(img):
    """One RGB5A3 image as a TPL, with the header fields WiiLink's have."""
    img = img.convert("RGBA")
    w, h = img.size
    px = img.load()
    data = bytearray()
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            for k in range(16):
                x, y = tx + k % 4, ty + k // 4
                v = rgb5a3(*px[x, y]) if x < w and y < h else 0
                data += struct.pack(">H", v)
    head = struct.pack(">III", 0x0020AF30, 1, 0x0C) + struct.pack(">II", 0x14, 0)
    image = struct.pack(">HHIIIIII", h, w, 5, 0x40, 0, 0, 1, 1).ljust(0x40 - 0x14, b"\0")
    return head + image + bytes(data)


# ---- Archives: U8 -----------------------------------------------------------

def u8(files):
    """files: [(path, bytes)] in the order wanted, with at most one folder
    level ("img/x.tpl"). Data aligned to 32 bytes, as Nintendo's tools do."""
    tree = []          # (name, None) for a folder, then its files
    folders = {}
    for path, data in files:
        if "/" in path:
            d, name = path.split("/", 1)
            if d not in folders:
                folders[d] = len(tree)
                tree.append([d, []])
            tree[folders[d]][1].append((name, data))
        else:
            tree.append([path, data])
    nodes, names = [], bytearray(b"\0")
    payload = []

    def add_name(n):
        off = len(names)
        names.extend(n.encode() + b"\0")
        return off

    count = 1 + sum(1 + len(e[1]) if isinstance(e[1], list) else 1 for e in tree)
    nodes.append([1, 0, 0, count])               # the root folder
    for name, entry in tree:
        if isinstance(entry, list):
            first = len(nodes)
            # Nintendo's packer puts 1 in a first-level folder's parent field.
            nodes.append([1, add_name(name), 1, first + 1 + len(entry)])
            for fname, data in entry:
                nodes.append([0, add_name(fname), None, len(data)])
                payload.append((len(nodes) - 1, data))
        else:
            nodes.append([0, add_name(name), None, len(entry)])
            payload.append((len(nodes) - 1, entry))
    meta = len(nodes) * 12 + len(names)
    at = (0x20 + meta + 31) & ~31
    data_start = at
    blob = bytearray()
    for idx, data in payload:
        nodes[idx][2] = at
        blob += data
        pad = (-len(blob)) % 32
        blob += bytes(pad)
        at += len(data) + pad
    # The header's size runs to the data, padding included (as Nintendo's).
    out = bytearray(struct.pack(">IIII", 0x55AA382D, 0x20, data_start - 0x20, data_start) + bytes(16))
    for t, n, d, s in nodes:
        out += struct.pack(">I", t << 24 | n) + struct.pack(">II", d, s)
    out += names
    out += bytes(data_start - len(out))
    out += blob
    return bytes(out)


def u8_files(a):
    """[(path, bytes)] of a U8 archive (one folder level), for checking."""
    magic, root, size, data = struct.unpack_from(">IIII", a, 0)
    n = struct.unpack_from(">I", a, root + 8)[0]
    strs = root + n * 12
    path, ends, files = [], [], []
    for i in range(n):
        t = a[root + i * 12]
        no = int.from_bytes(a[root + i * 12 + 1:root + i * 12 + 4], "big")
        d, s = struct.unpack_from(">II", a, root + i * 12 + 4)
        name = a[strs + no:a.index(b"\0", strs + no)].decode()
        while ends and i >= ends[-1]:
            ends.pop()
            path.pop()
        if t == 1:
            if i:
                path.append(name)
                ends.append(s)
        else:
            files.append(("/".join(path + [name]), a[d:d + s]))
    return files


# ---- Compression: LZ77, type 0x10 -------------------------------------------

def lz10(data):
    """Nintendo's LZ77 (0x10): greedy, 4 KiB window, 3-18 byte matches."""
    out = bytearray([0x10]) + len(data).to_bytes(3, "little")
    heads = {}
    i, n = 0, len(data)
    while i < n:
        flag_at = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len, best_disp = 0, 0
            key = data[i:i + 3]
            if len(key) == 3:
                for j in reversed(heads.get(key, ())):
                    if i - j > 0x1000:
                        break
                    ln = 3
                    while ln < 18 and i + ln < n and data[j + ln] == data[i + ln]:
                        ln += 1
                    if ln > best_len:
                        best_len, best_disp = ln, i - j
                        if ln == 18:
                            break
            step = best_len if best_len >= 3 else 1
            if best_len >= 3:
                flags |= 0x80 >> bit
                v = (best_len - 3) << 12 | (best_disp - 1)
                out += bytes([v >> 8, v & 0xFF])
            else:
                out.append(data[i])
            for k in range(i, min(i + step, n - 2)):
                lst = heads.setdefault(data[k:k + 3], [])
                lst.append(k)
                if len(lst) > 64:
                    del lst[:32]
            i += step
        out[flag_at] = flags
    out += bytes((-len(out)) % 4)
    return bytes(out)


def unlz10(d):
    size = int.from_bytes(d[1:4], "little")
    o, i = bytearray(), 4
    while len(o) < size:
        f = d[i]
        i += 1
        for b in range(8):
            if len(o) >= size:
                break
            if f & (0x80 >> b):
                x = d[i] << 8 | d[i + 1]
                i += 2
                for _ in range((x >> 12) + 3):
                    o.append(o[-((x & 0xFFF) + 1)])
            else:
                o.append(d[i])
                i += 1
    return bytes(o)


# ---- Sound: DSP-ADPCM in a BNS file -------------------------------------------

# Eight predictor pairs (x/2048) that cover smooth to sharp signals; each frame
# picks the one, and the scale, that codes it best.
COEFS = [(0, 0), (2048, 0), (4096, -2048), (3584, -1536), (3072, -1024), (4608, -2560),
         (1024, 0), (3840, -1920)]


def clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def adpcm_encode(samples):
    """Mono 16-bit samples to DSP-ADPCM frames (8 bytes: a header byte, then
    14 4-bit nibbles), keeping the decoder's history exact."""
    out = bytearray()
    h1 = h2 = 0
    for f in range(0, len(samples), 14):
        frame = list(samples[f:f + 14]) + [0] * (14 - len(samples[f:f + 14]))
        best = None
        for p, (c1, c2) in enumerate(COEFS):
            for scale in range(12):
                a, b, err, nib = h1, h2, 0, []
                for s in frame:
                    pred = (c1 * a + c2 * b + 1024) >> 11
                    d = s - pred
                    q = int(round(d / float(1 << scale)))
                    q = -8 if q < -8 else 7 if q > 7 else q
                    v = clamp16(pred + (q << scale))
                    err += (v - s) * (v - s)
                    nib.append(q & 15)
                    a, b = v, a
                    if best and err >= best[0]:
                        break
                else:
                    if not best or err < best[0]:
                        best = (err, p, scale, nib, a, b)
        _, p, scale, nib, h1, h2 = best
        out.append(p << 4 | scale)
        for k in range(0, 14, 2):
            out.append(nib[k] << 4 | nib[k + 1])
    return bytes(out)


def adpcm_decode(data, count):
    out, h1, h2 = [], 0, 0
    for f in range(0, len(data), 8):
        p, scale = data[f] >> 4, data[f] & 15
        c1, c2 = COEFS[p]
        for k in range(14):
            nib = data[f + 1 + k // 2] >> (4 if k % 2 == 0 else 0) & 15
            q = nib - 16 if nib >= 8 else nib
            v = clamp16(((c1 * h1 + c2 * h2 + 1024) >> 11) + (q << scale))
            out.append(v)
            h2, h1 = h1, v
            if len(out) == count:
                return out
    return out


def bns(samples, rate):
    """A mono BNS laid out as WiiLink's sound.bns: header, INFO, DATA."""
    adpcm = adpcm_encode(samples)
    # Frames follow each other unpadded, as in WiiLink's.
    coefs = b"".join(struct.pack(">hh", a, b) for a, b in COEFS)
    # Offsets count from the INFO chunk's data (its start + 8): the channel
    # list at 0x18 holds channel 0's info at 0x1C: its data at 0 in DATA, its
    # ADPCM info (coefficients, then gain, scale and history, all 0) at 0x28.
    info = (struct.pack(">BBBBHHII", 0, 0, 1, 0, rate, 0, 0, len(samples)) +
            struct.pack(">III", 0x18, 0, 0x1C) + struct.pack(">III", 0, 0x28, 0) +
            coefs + bytes(16))
    info_chunk = b"INFO" + struct.pack(">I", 8 + len(info)) + info
    data_chunk = b"DATA" + struct.pack(">I", 8 + len(adpcm)) + adpcm
    total = 0x20 + len(info_chunk) + len(data_chunk)
    head = (b"BNS " + struct.pack(">HHIHH", 0xFEFF, 0x0100, total, 0x20, 2) +
            struct.pack(">IIII", 0x20, len(info_chunk), 0x20 + len(info_chunk), len(data_chunk)))
    return head + info_chunk + data_chunk


def bns_info(b):
    """(rate, samples, adpcm bytes) of a mono BNS, for checking."""
    info = struct.unpack_from(">I", b, 0x10)[0]
    rate = struct.unpack_from(">H", b, info + 0x0C)[0]
    count = struct.unpack_from(">I", b, info + 0x14)[0]
    data = struct.unpack_from(">I", b, 0x18)[0]
    size = struct.unpack_from(">I", b, data + 4)[0]
    return rate, count, b[data + 8:data + size]


def read_wav(path, rate):
    """Mono 16-bit samples at `rate` from a WAV (mixed down, resampled)."""
    with wave.open(path, "rb") as w:
        ch, width, src = w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(w.getnframes())
    if width != 2:
        sys.exit(f"{path}: 16-bit PCM WAV only")
    vals = struct.unpack(f"<{len(raw) // 2}h", raw)
    mono = [sum(vals[i:i + ch]) // ch for i in range(0, len(vals), ch)]
    if src == rate:
        return mono
    n = int(len(mono) * rate / src)
    return [mono[min(len(mono) - 1, int(i * src / rate))] for i in range(n)]


# ---- The board data ---------------------------------------------------------

def chjump(title_hi, title_lo, arg=b""):
    arg = (arg + b"\0").ljust((len(arg) + 2) & ~1, b"\0") if arg else b""
    return (b"ChJp" + struct.pack(">IIIII", 0x20 + len(arg), 1, 0, title_hi, title_lo) +
            struct.pack(">II", 0x20, len(arg)) + arg)


def fit(img, size):
    from PIL import Image
    img = img.convert("RGBA")
    if img.size != size:
        img = img.resize(size, Image.LANCZOS)
    return img


def board(letter, thumb, sound=None, rate=8000, jump=None):
    """letter: a 512 x 376 image (scaled if not), thumb: 144 x 96; sound:
    mono 16-bit samples at `rate`; jump: (title_hi, title_lo[, arg]).
    Returns the archive's bytes."""
    letter = fit(letter, (LETTER_W, LETTER_H))
    pieces = {}
    y = 0
    for r, h in enumerate(ROWS):
        x = 0
        for c, w in enumerate(COLS):
            pieces[SLICES[r * 3 + c]] = letter.crop((x, y, x + w, y + h))
            x += w
        y += h
    letter_u8 = u8([(f"img/my_Letter_{k}.tpl", tpl(pieces[k])) for k in SLICE_ORDER])
    thumb_u8 = u8([("img/my_LetterS_b.tpl", tpl(fit(thumb, (THUMB_W, THUMB_H))))])
    files = [("letter_LZ.bin", lz10(letter_u8)), ("thumbnail_LZ.bin", lz10(thumb_u8))]
    if sound:
        files.append(("sound.bns", bns(sound, rate)))
    if jump:
        files.append(("chjump.bin", chjump(*jump)))
    return u8(files)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("letter")
    ap.add_argument("thumb")
    ap.add_argument("--sound", help="a 16-bit PCM WAV, mixed to mono")
    ap.add_argument("--rate", type=int, default=8000, help="the sound's rate (default 8000, as WiiLink's)")
    ap.add_argument("--chjump", help="a title to open, e.g. 000100014F484243 (HBC)")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    from PIL import Image
    sound = read_wav(a.sound, a.rate) if a.sound else None
    jump = (int(a.chjump[:8], 16), int(a.chjump[8:], 16)) if a.chjump else None
    arc = board(Image.open(a.letter), Image.open(a.thumb), sound, a.rate, jump)
    if len(arc) > BOARD_MAX:
        sys.exit(f"{len(arc)} bytes: over the board data's {BOARD_MAX} (shorten the sound)")
    open(a.out, "wb").write(arc)
    print(f"{a.out}: {len(arc)} bytes")


if __name__ == "__main__":
    main()
