"""Page boundaries and held-key timing for the app picker."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("cc")

CHECK = r"""
#include <assert.h>
#include "browser_nav.h"

int main(void) {
    browser_repeat r = {0};
    int columns, count, selected;
    assert(browser_repeat_step(&r, BN_RIGHT, BN_RIGHT, 0) == BN_RIGHT);
    assert(!browser_repeat_step(&r, 0, BN_RIGHT, 399));
    assert(browser_repeat_step(&r, 0, BN_RIGHT, 400) == BN_RIGHT);
    assert(!browser_repeat_step(&r, 0, BN_RIGHT, 519));
    assert(browser_repeat_step(&r, 0, BN_RIGHT, 520) == BN_RIGHT);
    // A long animation yields one step, without accumulating repeats.
    assert(browser_repeat_step(&r, 0, BN_RIGHT, 2000) == BN_RIGHT);
    assert(!browser_repeat_step(&r, 0, BN_RIGHT, 2001));
    // Changing direction and tapping again both respond immediately.
    assert(browser_repeat_step(&r, BN_LEFT, BN_LEFT, 2002) == BN_LEFT);
    assert(!browser_repeat_step(&r, 0, 0, 2003));
    assert(browser_repeat_step(&r, BN_LEFT, BN_LEFT, 2004) == BN_LEFT);
    // Ambiguous chords do not move, and cannot keep a repeat running.
    assert(!browser_repeat_step(&r, BN_LEFT | BN_RIGHT, BN_LEFT | BN_RIGHT, 3000));
    assert(!r.held);
    assert(browser_repeat_step(&r, 0, BN_DOWN, 3001) == BN_DOWN);

    // 3- and 4-column grids: crossing an edge keeps the row, and enters
    // at the opposite edge, rather than wrapping to the adjacent row.
    assert(browser_edge_step(3, 40, 3, 5, BN_LEFT) == 3);
    assert(browser_edge_step(18, 40, 3, 5, BN_LEFT) == 5);
    assert(browser_edge_step(5, 40, 3, 5, BN_RIGHT) == 18);
    assert(browser_edge_step(7, 45, 4, 5, BN_RIGHT) == 24);
    assert(browser_edge_step(24, 45, 4, 5, BN_LEFT) == 7);
    // Missing rows on the last page choose the nearest available app.
    assert(browser_edge_step(19, 22, 4, 5, BN_RIGHT) == 21);
    assert(browser_edge_step(21, 22, 4, 5, BN_RIGHT) == 21);
    assert(browser_edge_step(4, 12, 1, 5, BN_RIGHT) == 9);
    assert(browser_edge_step(9, 12, 1, 5, BN_RIGHT) == 11);
    for (columns = 1; columns <= 4; ++columns) {
        for (count = 1; count <= 65; ++count) {
            for (selected = 0; selected < count; ++selected) {
                int left = browser_edge_step(selected, count, columns, 5, BN_LEFT);
                int right = browser_edge_step(selected, count, columns, 5, BN_RIGHT);
                assert(left >= 0 && left < count);
                assert(right >= 0 && right < count);
                assert(left <= selected && right >= selected);
                if (selected % columns) assert(left == selected - 1);
                if (selected % columns < columns - 1 && selected + 1 < count)
                    assert(right == selected + 1);
                assert(browser_edge_step(selected, count, columns, 5, BN_UP) == selected);
            }
        }
    }
    return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler")
class BrowserNavigationTest(unittest.TestCase):
    def test_edges_and_repeat(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "check.c").write_text(CHECK)
            exe = tmp / ("check.exe" if os.name == "nt" else "check")
            subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "channel/channelapp/source"),
                            str(tmp / "check.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
