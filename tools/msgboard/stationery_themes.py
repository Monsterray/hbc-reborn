#!/usr/bin/env python3
"""Five example Message Board stationery themes, drawn and scored here (no
outside art or music), as board-data archives for sdk/mboard:

    stationery_themes.py OUTDIR      ->  OUTDIR/<theme>.arc, <theme>.png (a preview)

    ocean      waves and bubbles, a calm arpeggio
    sunset     a low sun and a dusk sky, silent
    forest     pines and leaves, silent
    nightsky   stars and a moon, "Twinkle Twinkle Little Star" (traditional)
    arcade     a pixel border for homebrew, a chiptune

Each letter keeps a light writing area: the Wii Menu writes the text in a dark
colour over the middle of the frame. Sound is mono DSP-ADPCM at 8 kHz, as
WiiLink's letters have it. See tools/msgboard/stationery.py for the format.
"""

import math
import pathlib
import random
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import stationery as st  # noqa: E402

W, H = st.LETTER_W, st.LETTER_H
RATE = 8000


# ---- Drawing helpers ---------------------------------------------------------

def vgrad(draw, box, top, bottom):
    x0, y0, x1, y1 = box
    for y in range(y0, y1):
        t = (y - y0) / max(1, y1 - y0 - 1)
        draw.line([(x0, y), (x1 - 1, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))


def card(colour_edge, paper=(252, 252, 248)):
    """A rounded letter with a coloured edge and a paper writing area."""
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, W - 1, H - 1), 18, fill=colour_edge + (255,))
    d.rounded_rectangle((10, 10, W - 11, H - 11), 12, fill=paper + (255,))
    return img, d


def rules(d, colour, x0=28, x1=W - 28, ys=range(186, 330, 44)):
    for y in ys:
        d.line([(x0, y), (x1, y)], fill=colour + (255,), width=1)


def envelope(edge, flap, mark=None):
    """A 144 x 96 board icon: an envelope in the theme's colours."""
    img = Image.new("RGBA", (st.THUMB_W, st.THUMB_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((4, 6, 139, 91), 10, fill=edge + (255,))
    d.rounded_rectangle((9, 11, 134, 86), 7, fill=(252, 252, 248, 255))
    d.line([(9, 13), (72, 52), (134, 13)], fill=flap + (255,), width=5)
    if mark:
        mark(d)
    return img


# ---- Music: square and sine voices, mixed to 16-bit ---------------------------

NOTE = {n: i for i, n in enumerate(["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"])}


def freq(name):
    """'A4' -> 440 Hz."""
    n, octave = name[:-1], int(name[-1])
    return 440.0 * 2 ** ((NOTE[n] + 12 * (octave - 4) - 9) / 12)


def tune(notes, bpm, wave="sine", volume=0.55, tail=0.4):
    """notes: [(name or None, beats)]. A soft attack and decay per note."""
    out = []
    beat = 60.0 / bpm
    for name, beats in notes:
        n = int(beats * beat * RATE)
        f = freq(name) if name else 0
        for i in range(n):
            t = i / RATE
            env = min(1.0, i / 80) * max(0.0, 1 - i / (n + RATE * tail)) if f else 0
            if wave == "square":
                v = 1.0 if (t * f) % 1 < 0.5 else -1.0
                v *= 0.5
            elif wave == "tri":
                v = 4 * abs((t * f) % 1 - 0.5) - 1
            else:
                v = math.sin(2 * math.pi * f * t) + 0.3 * math.sin(4 * math.pi * f * t)
            out.append(v * env)
    peak = max(1e-9, max(abs(v) for v in out))
    return [int(v / peak * volume * 32767) for v in out]


# ---- The themes -------------------------------------------------------------

def ocean():
    img, d = card((40, 120, 190))
    vgrad(d, (10, 10, W - 10, 130), (70, 170, 230), (150, 215, 245))
    for k in range(4):                                   # waves
        y = 112 + k * 6
        pts = [(x, y + 6 * math.sin(x / 22 + k)) for x in range(10, W - 10, 4)]
        d.line(pts, fill=(255, 255, 255, 200 - k * 40), width=3)
    rnd = random.Random(1)
    for _ in range(14):                                  # bubbles
        x, y, r = rnd.randint(30, W - 40), rnd.randint(20, 95), rnd.randint(4, 11)
        d.ellipse((x - r, y - r, x + r, y + r), outline=(255, 255, 255, 230), width=2)
    vgrad(d, (10, H - 54, W - 10, H - 10), (150, 215, 245), (60, 150, 210))
    rules(d, (170, 205, 230))
    thumb = envelope((40, 120, 190), (90, 180, 235))
    music = tune([("C4", .5), ("E4", .5), ("G4", .5), ("C5", .5), ("G4", .5), ("E4", .5),
                  ("A3", .5), ("C4", .5), ("E4", .5), ("A4", .5), ("E4", .5), ("C4", .5),
                  ("F3", .5), ("A3", .5), ("C4", .5), ("F4", .5), ("G3", .5), ("B3", .5),
                  ("D4", .5), ("G4", 1.5)], 132)
    return img, thumb, music


def sunset():
    img, d = card((215, 110, 70))
    vgrad(d, (10, 10, W - 10, 140), (250, 140, 90), (255, 210, 150))
    d.ellipse((W // 2 - 60, 70, W // 2 + 60, 190), fill=(255, 235, 170, 255))
    d.rectangle((10, 130, W - 10, 140), fill=(252, 252, 248, 255))
    for k, y in enumerate((40, 58)):                     # clouds
        d.rounded_rectangle((60 + 200 * k, y, 160 + 200 * k, y + 16), 8, fill=(255, 245, 235, 220))
    vgrad(d, (10, H - 50, W - 10, H - 10), (255, 210, 150), (230, 130, 90))
    rules(d, (240, 200, 180))
    thumb = envelope((215, 110, 70), (250, 160, 100))
    return img, thumb, None


def forest():
    img, d = card((60, 130, 70))
    vgrad(d, (10, 10, W - 10, 135), (180, 225, 170), (225, 245, 215))
    for k in range(9):                                   # pines
        x = 30 + k * 58
        h = 60 + (k * 37) % 40
        d.polygon([(x, 130), (x + 26, 130 - h), (x + 52, 130)], fill=(50 + k * 6, 120 + k * 4, 60, 255))
        d.rectangle((x + 22, 130, x + 30, 138), fill=(110, 80, 50, 255))
    rnd = random.Random(3)
    for _ in range(18):                                  # falling leaves
        x, y = rnd.randint(20, W - 30), rnd.randint(H - 52, H - 18)
        d.ellipse((x, y, x + 10, y + 6), fill=(90 + rnd.randint(0, 80), 150, 60, 255))
    d.line([(10, H - 56), (W - 10, H - 56)], fill=(60, 130, 70, 255), width=3)
    rules(d, (190, 220, 185))
    thumb = envelope((60, 130, 70), (120, 190, 110))
    return img, thumb, None


def nightsky():
    img, d = card((25, 35, 80))
    vgrad(d, (10, 10, W - 10, 140), (20, 30, 75), (60, 70, 140))
    rnd = random.Random(7)
    for _ in range(60):                                  # stars
        x, y = rnd.randint(16, W - 16), rnd.randint(14, 130)
        r = rnd.choice((1, 1, 1, 2))
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 220, 255))
    d.ellipse((W - 120, 26, W - 60, 86), fill=(255, 245, 200, 255))      # the moon
    d.ellipse((W - 104, 20, W - 50, 74), fill=(40, 52, 110, 255))
    vgrad(d, (10, H - 50, W - 10, H - 10), (60, 70, 140), (25, 35, 80))
    rules(d, (200, 205, 230))
    thumb = envelope((25, 35, 80), (90, 100, 170))
    # "Twinkle Twinkle Little Star": traditional, in the public domain.
    music = tune([("C4", 1), ("C4", 1), ("G4", 1), ("G4", 1), ("A4", 1), ("A4", 1), ("G4", 2),
                  ("F4", 1), ("F4", 1), ("E4", 1), ("E4", 1), ("D4", 1), ("D4", 1), ("C4", 2)],
                 150, wave="tri", volume=0.6)
    return img, thumb, music


def arcade():
    img, d = card((30, 30, 30), paper=(250, 250, 240))
    colours = [(240, 70, 70), (250, 200, 60), (80, 200, 90), (70, 150, 240), (190, 90, 220)]
    for i, x in enumerate(range(10, W - 10, 16)):        # a pixel border, top and bottom
        for j, y in enumerate((10, 26, H - 42, H - 26)):
            if (i + j) % 2 == 0:
                d.rectangle((x, y, x + 15, y + 15), fill=colours[(i + j) % 5] + (255,))
    for k, x in enumerate((60, 140, 220, 300, 380)):     # little invaders
        c = colours[k]
        for (px, py) in [(2, 0), (3, 0), (1, 1), (2, 1), (3, 1), (4, 1), (0, 2), (1, 2), (3, 2), (5, 2),
                         (0, 3), (1, 3), (2, 3), (3, 3), (4, 3), (5, 3), (1, 4), (4, 4), (0, 5), (5, 5)]:
            d.rectangle((x + px * 6, 60 + py * 6, x + px * 6 + 5, 65 + py * 6), fill=c + (255,))
    d.text((W // 2 - 70, 112), "HOMEBREW  MAIL", fill=(30, 30, 30, 255))
    rules(d, (210, 210, 200))
    thumb = envelope((30, 30, 30), (240, 70, 70),
                     mark=lambda dd: [dd.rectangle((60 + i * 8, 64, 66 + i * 8, 70), fill=colours[i] + (255,))
                                      for i in range(3)])
    music = tune([("E5", .25), ("G5", .25), ("E6", .25), ("C6", .25), ("D6", .25), ("G6", .5), (None, .25),
                  ("C5", .25), ("E5", .25), ("G5", .25), ("C6", .5), ("B5", .25), ("G5", .25), ("C6", 1)],
                 140, wave="square", volume=0.45, tail=0.1)
    return img, thumb, music


THEMES = {"ocean": ocean, "sunset": sunset, "forest": forest, "nightsky": nightsky, "arcade": arcade}


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "stationery")
    out.mkdir(parents=True, exist_ok=True)
    for name, make in THEMES.items():
        letter, thumb, music = make()
        arc = st.board(letter, thumb, music, RATE)
        if len(arc) > st.BOARD_MAX:
            sys.exit(f"{name}: {len(arc)} bytes, over {st.BOARD_MAX}")
        (out / f"{name}.arc").write_bytes(arc)
        prev = Image.new("RGBA", (W + 20 + st.THUMB_W, H), (60, 60, 60, 255))
        prev.alpha_composite(letter, (0, 0))
        prev.alpha_composite(thumb, (W + 20, 0))
        prev.save(out / f"{name}.png")
        secs = f", {len(music) / RATE:.1f} s of music" if music else ""
        print(f"{name}: {len(arc)} bytes{secs}")


if __name__ == "__main__":
    main()
