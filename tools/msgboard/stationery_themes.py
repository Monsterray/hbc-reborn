#!/usr/bin/env python3
"""Five example Message Board stationery themes, drawn and scored here (no
outside art or music), as board-data archives for sdk/mboard:

    stationery_themes.py OUTDIR      ->  OUTDIR/<theme>.arc, <theme>.png (a preview)

    ocean      waves and bubbles, a calm arpeggio
    sunset     a low sun and a dusk sky, silent
    forest     misty hills, three rows of pines, light rays, mushrooms, silent
    nightsky   a spacy night: nebulae, gold stars, a ringed planet, a crescent
               moon, and an original dreamy tune with an echo
    arcade     a pixel border for homebrew, a chiptune

Each letter keeps a light writing area, where the Wii Menu writes the text in
a dark colour, and a darker band across the top, where it writes the sender's
name in a light one (as WiiLink's letters do). Sound is mono DSP-ADPCM at 8 kHz, as
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


def tune(notes, bpm, wave="sine", volume=0.55, tail=0.4, echo=0.0, echo_s=0.28):
    """notes: [(name or None, beats)]. A soft attack and decay per note;
    echo: how much of the sound comes back echo_s later (0 for none)."""
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
    if echo:
        d = int(echo_s * RATE)
        out += [0.0] * (2 * d)
        for i in range(d, len(out)):
            out[i] += echo * out[i - d]
    peak = max(1e-9, max(abs(v) for v in out))
    return [int(v / peak * volume * 32767) for v in out]



# ---- Drawing at twice the size, and clipping to the paper --------------------

S = 2                       # supersampling: drawn at S x, scaled down smooth


def big_card(edge, paper=(252, 252, 248)):
    """A letter drawn at S x: the edge, the paper, and a mask of the paper."""
    img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, W * S - 1, H * S - 1), 18 * S, fill=edge + (255,))
    d.rounded_rectangle((10 * S, 10 * S, (W - 11) * S, (H - 11) * S), 12 * S, fill=paper + (255,))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((10 * S, 10 * S, (W - 11) * S, (H - 11) * S), 12 * S, fill=255)
    return img, mask


def on_paper(img, mask, layer):
    """layer drawn over the paper only: nothing reaches the edge."""
    clipped = Image.new("RGBA", img.size, (0, 0, 0, 0))
    clipped.paste(layer, (0, 0), Image.composite(layer.getchannel("A"), Image.new("L", img.size, 0), mask))
    img.alpha_composite(clipped)


def layer():
    return Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))


def band(lay, y0, y1):
    """lay with everything outside rows y0-y1 (letter pixels) cut away."""
    cut = Image.new("L", lay.size, 0)
    ImageDraw.Draw(cut).rectangle((0, y0 * S, lay.width, y1 * S - 1), fill=255)
    lay.putalpha(Image.composite(lay.getchannel("A"), Image.new("L", lay.size, 0), cut))
    return lay


def small(img):
    return img.resize((W, H), Image.LANCZOS)


def vgrad_layer(top, bottom, y0, y1):
    lay = layer()
    d = ImageDraw.Draw(lay)
    for y in range(y0 * S, y1 * S):
        t = (y - y0 * S) / max(1, (y1 - y0) * S - 1)
        d.line([(0, y), (W * S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    return lay


def star(d, cx, cy, r, colour, points=5):
    """A five-point star, as Mario's are drawn."""
    pts = []
    for k in range(points * 2):
        a = -math.pi / 2 + k * math.pi / points
        rr = r if k % 2 == 0 else r * 0.45
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d.polygon(pts, fill=colour)


def big_rules(d, colour, ys=range(186, 330, 44)):
    for y in ys:
        d.line([(28 * S, y * S), ((W - 28) * S, y * S)], fill=colour + (255,), width=S)


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
                  ("D4", .5), ("G4", 1.5)], 132, volume=0.28)
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
    img, mask = big_card((52, 110, 58))
    sky = vgrad_layer((44, 104, 150), (196, 228, 228), 10, 150)
    d = ImageDraw.Draw(sky)
    # Light coming through the trees.
    rays = layer()
    dr = ImageDraw.Draw(rays)
    for k, x in enumerate((120, 230, 360)):
        dr.polygon([(x * S, 10 * S), ((x + 26) * S, 10 * S), ((x + 90) * S, 150 * S), ((x + 30) * S, 150 * S)],
                   fill=(255, 250, 220, 70))
    sky.alpha_composite(rays)
    # Misty hills, then three rows of pines, far to near.
    for y, colour in ((112, (170, 205, 180)), (124, (140, 185, 150))):
        d.ellipse((-60 * S, y * S, 300 * S, (y + 80) * S), fill=colour + (255,))
        d.ellipse((220 * S, (y - 6) * S, 600 * S, (y + 80) * S), fill=colour + (255,))
    rnd = random.Random(5)
    for row, (base, height, colour, n) in enumerate(((136, 46, (88, 140, 96), 13), (146, 62, (62, 120, 72), 10),
                                                      (156, 80, (38, 96, 52), 7))):
        for i in range(n):
            x = (i + 0.5) * W / n + rnd.uniform(-12, 12)
            h = height * rnd.uniform(0.8, 1.15)
            w = h * 0.55
            for tier in range(3):                         # a tiered pine
                top = base - h + tier * h * 0.28
                bottom = base - h * 0.42 + tier * h * 0.3
                half = w * (0.45 + tier * 0.2)
                d.polygon([(x * S, top * S), ((x + half) * S, bottom * S), ((x - half) * S, bottom * S)],
                          fill=tuple(c - tier * 6 for c in colour) + (255,))
            d.rectangle(((x - 3) * S, (base - 6) * S, (x + 3) * S, (base + 2) * S), fill=(96, 70, 44, 255))
    # A soft meadow edge under the trees, then nothing below the scene.
    d.rectangle((0, 156 * S, W * S, 162 * S), fill=(120, 170, 100, 255))
    on_paper(img, mask, band(sky, 10, 162))
    # The forest floor: grass, ferns, mushrooms, and a stream of fallen leaves.
    floor = vgrad_layer((120, 170, 90), (78, 130, 66), H - 64, H - 10)
    fd = ImageDraw.Draw(floor)
    for i in range(70):
        x = rnd.uniform(12, W - 12)
        y = rnd.uniform(H - 62, H - 14)
        fd.line([(x * S, y * S), ((x + rnd.uniform(-3, 3)) * S, (y - 7) * S)], fill=(150, 200, 110, 255), width=S)
    for x in (70, 210, 330, 450):                         # mushrooms
        y = H - 30 + rnd.uniform(-6, 6)
        fd.rectangle(((x - 3) * S, (y - 2) * S, (x + 3) * S, (y + 9) * S), fill=(245, 235, 215, 255))
        fd.chord(((x - 11) * S, (y - 10) * S, (x + 11) * S, (y + 6) * S), 180, 360, fill=(210, 60, 50, 255))
        for dx, dy in ((-5, -5), (3, -7), (6, -3)):
            fd.ellipse(((x + dx - 2) * S, (y + dy - 2) * S, (x + dx + 2) * S, (y + dy + 2) * S), fill=(255, 250, 240, 255))
    for i in range(26):                                   # leaves
        x, y = rnd.uniform(16, W - 26), rnd.uniform(H - 58, H - 18)
        colour = rnd.choice(((214, 120, 40), (190, 80, 40), (230, 170, 60), (120, 160, 60)))
        fd.ellipse((x * S, y * S, (x + 9) * S, (y + 5) * S), fill=colour + (255,))
    # Keep the floor below the writing area.
    cut = Image.new("L", floor.size, 0)
    ImageDraw.Draw(cut).rectangle((0, (H - 64) * S, W * S, H * S), fill=255)
    floor.putalpha(Image.composite(floor.getchannel("A"), Image.new("L", floor.size, 0), cut))
    on_paper(img, mask, floor)
    big_rules(ImageDraw.Draw(img), (190, 215, 180))
    img = small(img)
    thumb = envelope((52, 110, 58), (120, 180, 100),
                     mark=lambda dd: dd.polygon([(64, 74), (72, 58), (80, 74)], fill=(52, 110, 58, 255)))
    return img, thumb, None


def nightsky():
    """A spacy night, as a Mario night level: deep space, nebulae, gold
    stars, a ringed planet, a shooting star and a crescent moon."""
    img, mask = big_card((34, 24, 78))
    sky = vgrad_layer((14, 10, 44), (58, 36, 120), 10, 152)
    rnd = random.Random(11)
    # Nebulae: soft clouds of colour, blurred.
    from PIL import ImageFilter
    neb = layer()
    nd = ImageDraw.Draw(neb)
    for cx, cy, r, colour in ((120, 60, 70, (170, 70, 200, 90)), (360, 110, 80, (60, 120, 230, 80)),
                              (260, 40, 50, (230, 90, 160, 70))):
        nd.ellipse(((cx - r) * S, (cy - r * 0.6) * S, (cx + r) * S, (cy + r * 0.6) * S), fill=colour)
    sky.alpha_composite(neb.filter(ImageFilter.GaussianBlur(18 * S)))
    d = ImageDraw.Draw(sky)
    for _ in range(90):                                   # far stars
        x, y, r = rnd.uniform(14, W - 14), rnd.uniform(14, 148), rnd.choice((0.6, 0.8, 1.0, 1.4))
        d.ellipse(((x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S), fill=(255, 250, 230, rnd.randint(150, 255)))
    for x, y, r in ((60, 40, 9), (200, 100, 7), (300, 32, 6), (445, 128, 8), (150, 132, 5)):   # gold stars
        star(d, x * S, y * S, r * S, (255, 214, 60, 255))
        star(d, x * S, y * S, r * 0.5 * S, (255, 245, 170, 255))
    # A ringed planet.
    px, py, pr = 92, 102, 22
    ring = layer()
    rd = ImageDraw.Draw(ring)
    rd.ellipse(((px - 40) * S, (py - 9) * S, (px + 40) * S, (py + 9) * S), outline=(255, 200, 120, 230), width=3 * S)
    planet = layer()
    pd = ImageDraw.Draw(planet)
    pd.ellipse(((px - pr) * S, (py - pr) * S, (px + pr) * S, (py + pr) * S), fill=(236, 120, 90, 255))
    pd.chord(((px - pr) * S, (py - pr) * S, (px + pr) * S, (py + pr) * S), 200, 340, fill=(250, 170, 110, 255))
    sky.alpha_composite(ring)
    sky.alpha_composite(planet)
    front = layer()                                       # the ring's near half over the planet
    ImageDraw.Draw(front).arc(((px - 40) * S, (py - 9) * S, (px + 40) * S, (py + 9) * S), 0, 180,
                              fill=(255, 210, 140, 255), width=3 * S)
    sky.alpha_composite(front)
    # A shooting star.
    d.line([(300 * S, 70 * S), (240 * S, 92 * S)], fill=(255, 250, 220, 160), width=2 * S)
    star(d, 300 * S, 70 * S, 6 * S, (255, 250, 220, 255))
    # The moon: a crescent cut out of a disc (no dark circle), with a glow.
    mx, my, mr = W - 92, 54, 30
    moon = layer()
    md = ImageDraw.Draw(moon)
    md.ellipse(((mx - mr) * S, (my - mr) * S, (mx + mr) * S, (my + mr) * S), fill=(255, 244, 196, 255))
    md.ellipse(((mx - mr + 18) * S, (my - mr - 8) * S, (mx + mr + 18) * S, (my + mr - 8) * S), fill=(0, 0, 0, 0))
    glow = moon.filter(ImageFilter.GaussianBlur(8 * S))
    sky.alpha_composite(glow)
    sky.alpha_composite(moon)
    on_paper(img, mask, band(sky, 10, 152))
    ground = vgrad_layer((58, 36, 120), (24, 16, 64), H - 54, H - 10)
    gd = ImageDraw.Draw(ground)
    for x, y, r in ((40, H - 32, 5), (130, H - 22, 4), (250, H - 34, 6), (380, H - 24, 4), (470, H - 36, 5)):
        star(gd, x * S, y * S, r * S, (255, 214, 60, 255))
    cut = Image.new("L", ground.size, 0)
    ImageDraw.Draw(cut).rectangle((0, (H - 54) * S, W * S, H * S), fill=255)
    ground.putalpha(Image.composite(ground.getchannel("A"), Image.new("L", ground.size, 0), cut))
    on_paper(img, mask, ground)
    big_rules(ImageDraw.Draw(img), (205, 200, 235))
    img = small(img)
    thumb = envelope((34, 24, 78), (120, 90, 200),
                     mark=lambda dd: star(dd, 72, 70, 9, (255, 214, 60, 255)))
    # An original dreamy tune (not Nintendo's): slow bells over an echo.
    music = tune([("E5", .5), ("B4", .5), ("G5", .5), ("F#5", .5), ("E5", 1), (None, .5),
                  ("D5", .5), ("A4", .5), ("F#5", .5), ("E5", .5), ("D5", 1), (None, .5),
                  ("C5", .5), ("G4", .5), ("E5", .5), ("D5", .5), ("B4", .75), ("C5", .25), ("D5", .5),
                  ("B4", 2)], 116, wave="sine", volume=0.28, tail=0.6, echo=0.45, echo_s=0.26)
    return img, thumb, music


def arcade():
    img, mask = big_card((30, 30, 30), paper=(250, 250, 240))
    colours = [(240, 70, 70), (250, 200, 60), (80, 200, 90), (70, 150, 240), (190, 90, 220)]
    deco = layer()
    d = ImageDraw.Draw(deco)
    # A solid banner across the top: the Wii Menu writes the sender's name
    # there, as on WiiLink's letters, so it must be dark and plain.
    d.rectangle((0, 10 * S, W * S, 70 * S), fill=(36, 34, 72, 255))
    # A pixel strip under it and at the bottom, kept inside the paper.
    for i, x in enumerate(range(10, W - 10, 16)):
        for j, y in enumerate((70, H - 42, H - 26)):
            if (i + j) % 2 == 0:
                d.rectangle((x * S, y * S, (x + 16) * S - 1, (y + 16) * S - 1), fill=colours[(i + j) % 5] + (255,))
    for k, x in enumerate((60, 140, 220, 300, 380)):     # little invaders
        c = colours[k]
        for (px, py) in [(2, 0), (3, 0), (1, 1), (2, 1), (3, 1), (4, 1), (0, 2), (1, 2), (3, 2), (5, 2),
                         (0, 3), (1, 3), (2, 3), (3, 3), (4, 3), (5, 3), (1, 4), (4, 4), (0, 5), (5, 5)]:
            d.rectangle(((x + px * 6) * S, (96 + py * 6) * S, (x + px * 6 + 6) * S - 1, (102 + py * 6) * S - 1),
                        fill=c + (255,))
    on_paper(img, mask, deco)
    big_rules(ImageDraw.Draw(img), (210, 210, 200))
    img = img.resize((W, H), Image.NEAREST)               # keep the pixels sharp
    ImageDraw.Draw(img).text((W // 2 - 42, 140), "HOMEBREW  MAIL", fill=(30, 30, 30, 255))
    thumb = envelope((30, 30, 30), (240, 70, 70),
                     mark=lambda dd: [dd.rectangle((60 + i * 8, 64, 66 + i * 8, 70), fill=colours[i] + (255,))
                                      for i in range(3)])
    music = tune([("E5", .25), ("G5", .25), ("E6", .25), ("C6", .25), ("D6", .25), ("G6", .5), (None, .25),
                  ("C5", .25), ("E5", .25), ("G5", .25), ("C6", .5), ("B5", .25), ("G5", .25), ("C6", 1)],
                 140, wave="square", volume=0.18, tail=0.1)
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
