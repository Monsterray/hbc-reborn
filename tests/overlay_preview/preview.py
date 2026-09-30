#!/usr/bin/env python3
"""Build and run the overlay preview, then write PNG files and a contact sheet.

usage: python3 tests/overlay_preview/preview.py [OUTDIR]   (needs a C compiler and Pillow)
"""

import os
import pathlib
import shutil
import subprocess
import sys

from PIL import Image

root = pathlib.Path(__file__).resolve().parents[2]
W, H = 640, 480


def yuyv_to_image(data):
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        row = data[y * W * 2:(y + 1) * W * 2]
        for x in range(0, W, 2):
            y0, cb, y1, cr = row[x * 2:x * 2 + 4]
            for i, yy in enumerate((y0, y1)):
                c, d, e = yy - 16, cb - 128, cr - 128
                r = (298 * c + 409 * e + 128) >> 8
                g = (298 * c - 100 * d - 208 * e + 128) >> 8
                b = (298 * c + 516 * d + 128) >> 8
                px[x + i, y] = (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)))
    return img


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else root / "tests/overlay_preview/out")
    out.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    exe = out / ("preview.exe" if os.name == "nt" else "preview")
    agent = root / "sdk/hbc_agent"
    subprocess.run([cc, "-O2", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(agent),
                    str(root / "tests/overlay_preview/preview.c"), str(agent / "ov_ui.c"),
                    str(agent / "ov_draw.c"), "-o", str(exe)] + ([] if os.name == "nt" else ["-lm"]), check=True,
                   env=dict(os.environ, TMP=str(out), TEMP=str(out), TMPDIR=str(out)))
    subprocess.run([str(exe), str(out)], check=True)
    shots = sorted(out.glob("*.yuyv"))
    images = []
    for shot in shots:
        img = yuyv_to_image(shot.read_bytes())
        img.save(shot.with_suffix(".png"))
        shot.unlink()
        images.append(img)
    cols = 3
    sheet = Image.new("RGB", (cols * W // 2, -(-len(images) // cols) * H // 2), "white")
    for i, img in enumerate(images):
        sheet.paste(img.resize((W // 2, H // 2)), ((i % cols) * W // 2, (i // cols) * H // 2))
    sheet.save(out / "sheet.png")
    print(f"{len(images)} frames in {out}")


if __name__ == "__main__":
    main()
