"""The shake a widget gives when the D-pad pushes focus against an edge."""

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
#include <math.h>
#include "edge_bump.h"

int main(void) {
    unsigned ms;
    float peak = 0;
    assert(edge_bump_offset(0) == 0);
    // It moves toward the pushed edge first, then back past rest.
    assert(edge_bump_offset(EDGE_BUMP_PERIOD_MS / 4) > 3);
    assert(edge_bump_offset(EDGE_BUMP_PERIOD_MS * 3 / 4) < -1);
    for (ms = 0; ms < EDGE_BUMP_MS; ++ms) {
        float d = edge_bump_offset(ms);
        assert(fabsf(d) <= EDGE_BUMP_PX);
        if (fabsf(d) > peak)
            peak = fabsf(d);
    }
    assert(peak > EDGE_BUMP_PX / 2);
    // It settles: small near the end, and exactly still after.
    assert(fabsf(edge_bump_offset(EDGE_BUMP_MS - 10)) < 0.1f);
    assert(edge_bump_offset(EDGE_BUMP_MS) == 0);
    assert(edge_bump_offset(100000) == 0);
    return 0;
}
"""


@unittest.skipUnless(CC, "no host C compiler")
class EdgeBumpTest(unittest.TestCase):
    def test_curve(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp, "bump.c")
            exe = pathlib.Path(tmp, "bump.exe" if os.name == "nt" else "bump")
            src.write_text(CHECK)
            subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "channel/channelapp/source"),
                            str(src), "-o", str(exe)] + ([] if os.name == "nt" else ["-lm"]),
                           check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
