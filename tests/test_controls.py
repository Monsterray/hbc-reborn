"""Exercise the shared Wii Remote menu mapping with synthetic sensor reports."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("cc")

WPAD = """
#include <stdint.h>
typedef uint32_t u32;
#define WPAD_BUTTON_UP 0x0800
#define WPAD_BUTTON_DOWN 0x0400
#define WPAD_BUTTON_LEFT 0x0100
#define WPAD_BUTTON_RIGHT 0x0200
#define WPAD_ERR_NONE 0
#define WPAD_DATA_ACCEL 2
typedef struct {
    int err;
    u32 data_present, btns_d, btns_h, btns_u;
    struct { float x, y, z; } gforce;
} WPADData;
"""

CHECK = """
#include <assert.h>
#include "wpad_menu.h"

int main(void) {
    hbc_wpad_menu a = {0}, b = {0};
    WPADData d = { .data_present = WPAD_DATA_ACCEL, .gforce = {0, 1, 0} };
    u32 dirs[] = {WPAD_BUTTON_UP, WPAD_BUTTON_RIGHT, WPAD_BUTTON_DOWN, WPAD_BUTTON_LEFT};
    unsigned i, mask;
    hbc_wpad_menu_update(&a, &d);
    assert(!a.sideways);
    d.gforce.x = -1; d.gforce.y = 0; // D-pad left, face towards player.
    hbc_wpad_menu_update(&a, &d);
    assert(a.sideways);
    for (mask = 0; mask < 16; ++mask) {
        u32 in = 0x12340008, out = in; // Extension buttons and A survive.
        for (i = 0; i < 4; ++i) {
            if (mask & (1u << i)) { in |= dirs[i]; out |= dirs[(i + 3) % 4]; }
        }
        assert(hbc_wpad_menu_buttons(&a, in) == out);
        assert(hbc_wpad_menu_buttons(&b, in) == in); // Independent remotes.
    }
    d.gforce.x = -.5f; d.gforce.y = .5f; d.gforce.z = .707f;
    hbc_wpad_menu_update(&a, &d);
    hbc_wpad_menu_update(&b, &d);
    assert(a.sideways && !b.sideways); // Hysteresis near the diagonal.
    d.gforce.x = d.gforce.y = 0; d.gforce.z = 1;
    hbc_wpad_menu_update(&a, &d);
    assert(a.sideways); // Face up is ambiguous: keep last orientation.
    d.gforce.x = 2; d.gforce.z = 0;
    hbc_wpad_menu_update(&a, &d);
    assert(a.sideways); // Ignore a shake.
    d.data_present = 0; d.gforce.x = 1;
    hbc_wpad_menu_update(&a, &d);
    assert(a.sideways); // No accelerometer report.
    d.data_present = WPAD_DATA_ACCEL; d.gforce.x = -1;
    d.btns_d = d.btns_h = WPAD_BUTTON_UP;
    hbc_wpad_menu_update(&a, &d);
    assert(hbc_wpad_menu_buttons(&a, d.btns_d) == WPAD_BUTTON_LEFT);
    d.btns_d = 0; d.gforce.x = 0; d.gforce.y = 1;
    hbc_wpad_menu_update(&a, &d);
    assert(hbc_wpad_menu_buttons(&a, d.btns_h) == WPAD_BUTTON_LEFT);
    d.btns_h = 0; d.btns_u = WPAD_BUTTON_UP;
    hbc_wpad_menu_update(&a, &d);
    assert(hbc_wpad_menu_buttons(&a, d.btns_u) == WPAD_BUTTON_LEFT);
    d.btns_u = 0;
    hbc_wpad_menu_update(&a, &d);
    assert(!a.sideways); // Upright again after releasing.
    d.gforce.x = -1; d.gforce.y = 0;
    hbc_wpad_menu_update(&a, &d);
    d.gforce.x = 1;
    hbc_wpad_menu_update(&a, &d);
    assert(!a.sideways); // D-pad to the right does not rotate.
    d.gforce.x = -1;
    hbc_wpad_menu_update(&a, &d);
    d.err = -1;
    hbc_wpad_menu_update(&a, &d);
    assert(!a.sideways && !a.dpad_held);
    hbc_wpad_menu_update(&a, 0);
    assert(!a.sideways);
    return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler")
class ControlsTest(unittest.TestCase):
    def test_sideways_menu(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "wiiuse").mkdir()
            (tmp / "wiiuse/wpad.h").write_text(WPAD)
            (tmp / "check.c").write_text(CHECK)
            exe = tmp / ("check.exe" if os.name == "nt" else "check")
            subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "sdk"),
                            str(tmp / "check.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
