"""Exercise the transfer line's progress circle renderer with the host C compiler."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
SIZE = 24


@unittest.skipUnless(CC, "no host C compiler")
class TransferArcTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        src = pathlib.Path(cls.tmp.name, "arc.c")
        cls.exe = pathlib.Path(cls.tmp.name, "arc.exe" if os.name == "nt" else "arc")
        src.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "transfer_arc.h"

// The circle worked out pixel by pixel, with no table: what the renderer
// must give.
static void direct(unsigned char *pixels, unsigned filled, unsigned start) {
    const float tau = 6.28318530718f;
    for (unsigned y = 0; y < TRANSFER_ARC_SIZE; ++y)
        for (unsigned x = 0; x < TRANSFER_ARC_SIZE; ++x) {
            unsigned alpha = 0;
            for (unsigned sy = 0; sy < 2; ++sy)
                for (unsigned sx = 0; sx < 2; ++sx) {
                    float dx = x + 0.25f + 0.5f * sx - TRANSFER_ARC_SIZE / 2;
                    float dy = y + 0.25f + 0.5f * sy - TRANSFER_ARC_SIZE / 2;
                    float radius = sqrtf(dx * dx + dy * dy);
                    if (radius < 7.5f || radius > 10.0f)
                        continue;
                    float angle = atan2f(dx, -dy);
                    if (angle < 0)
                        angle += tau;
                    unsigned turn = (unsigned) (angle * TRANSFER_ARC_TURN / tau);
                    turn = (turn + TRANSFER_ARC_TURN - start) % TRANSFER_ARC_TURN;
                    alpha += turn < filled ? 255 : 48;
                }
            unsigned offset = ((y / 4) * (TRANSFER_ARC_SIZE / 4) + x / 4) * 64 +
                              ((y % 4) * 4 + x % 4) * 2;
            pixels[offset] = (alpha + 2) / 4;
            pixels[offset + 1] = 255;
            pixels[offset + 32] = 255;
            pixels[offset + 33] = 255;
        }
}

int main(int argc, char **argv) {
    unsigned char pixels[TRANSFER_ARC_SIZE * TRANSFER_ARC_SIZE * 4 + 2];
    unsigned char want[TRANSFER_ARC_SIZE * TRANSFER_ARC_SIZE * 4];
    unsigned char got[TRANSFER_ARC_SIZE * TRANSFER_ARC_SIZE * 4];
    assert(argc == 3);
    for (unsigned filled = 0; filled <= TRANSFER_ARC_TURN; filled += 5)
        for (unsigned start = 0; start < TRANSFER_ARC_TURN; start += 7) {
            direct(want, filled, start);
            transfer_arc_render(got, filled, start);
            assert(!memcmp(want, got, sizeof(got)));
        }
    assert(transfer_arc_amount(0, 0) == 0);
    assert(transfer_arc_amount(0, 100) == 0);
    assert(transfer_arc_amount(25, 100) == 64);
    assert(transfer_arc_amount(100, 100) == 256);
    assert(transfer_arc_amount(101, 100) == 256);
    assert(transfer_arc_amount(UINT32_MAX - 1, UINT32_MAX) == 255);
    pixels[0] = pixels[sizeof(pixels) - 1] = 123;
    transfer_arc_render(pixels + 1, strtoul(argv[1], NULL, 10), strtoul(argv[2], NULL, 10));
    assert(pixels[0] == 123 && pixels[sizeof(pixels) - 1] == 123);
    for (unsigned y = 0; y < TRANSFER_ARC_SIZE; ++y)
        for (unsigned x = 0; x < TRANSFER_ARC_SIZE; ++x) {
            unsigned offset = 1 + ((y / 4) * (TRANSFER_ARC_SIZE / 4) + x / 4) * 64 +
                                  ((y % 4) * 4 + x % 4) * 2;
            assert(pixels[offset + 1] == 255);
            assert(pixels[offset + 32] == 255 && pixels[offset + 33] == 255);
            putchar(pixels[offset]);
        }
    return 0;
}
''')
        subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "channel/channelapp/source"),
                        str(src), "-o", str(cls.exe)] + ([] if os.name == "nt" else ["-lm"]), check=True)

    def render(self, filled, start=0):
        pixels = subprocess.check_output([str(self.exe), str(filled), str(start)])
        self.assertEqual(len(pixels), SIZE * SIZE)
        return pixels

    def test_empty_track_and_full_circle(self):
        empty, full = self.render(0), self.render(256)
        self.assertEqual(max(empty), 48)
        self.assertEqual(max(full), 255)
        for pixels in (empty, full):
            self.assertEqual(pixels[12 * SIZE + 12], 0)  # hollow centre
            self.assertEqual(pixels[0], 0)
            self.assertTrue(any(0 < value < max(pixels) for value in pixels))
        self.assertEqual(self.render(999), full)

    def test_circle_has_equal_width_and_height(self):
        for fill in (0, 256):
            pixels = self.render(fill)
            for y in range(SIZE):
                for x in range(SIZE):
                    self.assertEqual(pixels[y * SIZE + x], pixels[x * SIZE + y])

    def test_clockwise_progress_from_the_top(self):
        quarter = self.render(64)
        self.assertEqual(quarter[3 * SIZE + 12], 255)
        self.assertEqual(quarter[11 * SIZE + 20], 255)
        self.assertEqual(quarter[20 * SIZE + 12], 48)
        self.assertEqual(quarter[12 * SIZE + 3], 48)
        frames = [self.render(fill) for fill in (0, 1, 64, 128, 192, 255, 256)]
        for before, after in zip(frames, frames[1:]):
            self.assertTrue(all(a <= b for a, b in zip(before, after)))

    def test_indeterminate_arc_rotates_and_wraps(self):
        rotated = self.render(64, 64)
        self.assertEqual(rotated[3 * SIZE + 12], 48)
        self.assertEqual(rotated[12 * SIZE + 20], 255)
        self.assertEqual(rotated[20 * SIZE + 12], 255)
        self.assertEqual(rotated, self.render(64, 320))
