"""The agent's Sideways remote (sdk/hbc_agent/sideways.h) on synthetic
accelerometer reports: when a remote counts as sideways, and its D-pad turned."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")

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
    struct { int valid; } ir;
    struct { float x, y, z; } gforce;
} WPADData;
"""

CHECK = """
#include <assert.h>
#include "sideways.h"

// A held pose: enough scans in a row for any change to take.
#define UPD(s, d) do { for (int k_ = 0; k_ < SIDEWAYS_LEAVE_SCANS; ++k_) sideways_update(s, d); } while (0)

// A sideways remote stays sideways through brief upright readings (a twist,
// a tilt, a glimpse of the sensor bar) and turns only when they last.
static void flicker(void) {
    sideways_remote s = {0};
    WPADData d = { .data_present = WPAD_DATA_ACCEL, .gforce = {-1, 0, 0} };
    int i;
    for (i = 0; i < SIDEWAYS_ENTER_SCANS - 1; ++i) sideways_update(&s, &d);
    assert(!s.sideways); // not yet
    sideways_update(&s, &d);
    assert(s.sideways);
    d.gforce.x = 0; d.gforce.y = 1; // twisted along its length
    for (i = 0; i < SIDEWAYS_LEAVE_SCANS - 1; ++i) sideways_update(&s, &d);
    assert(s.sideways);
    d.gforce.x = -1; d.gforce.y = 0; // back: the streak starts again
    sideways_update(&s, &d);
    d.gforce.x = 0; d.gforce.y = 1;
    for (i = 0; i < SIDEWAYS_LEAVE_SCANS - 1; ++i) sideways_update(&s, &d);
    assert(s.sideways);
    sideways_update(&s, &d);
    assert(!s.sideways); // held long enough
    d.gforce.x = -1; d.gforce.y = 0; d.ir.valid = 1;
    for (i = 0; i < 2 * SIDEWAYS_LEAVE_SCANS; ++i) sideways_update(&s, &d);
    assert(!s.sideways); // pointing at the bar stays upright
}

int main(void) {
    flicker();
    sideways_remote a = {0}, b = {0};
    WPADData d = { .data_present = WPAD_DATA_ACCEL, .gforce = {0, 1, 0} };
    u32 dirs[] = {WPAD_BUTTON_UP, WPAD_BUTTON_RIGHT, WPAD_BUTTON_DOWN, WPAD_BUTTON_LEFT};
    unsigned i, mask;
    UPD(&a, &d);
    assert(!a.sideways);
    d.gforce.x = -1; d.gforce.y = 0; // D-pad at the left, buttons towards the player
    UPD(&a, &d);
    assert(a.sideways);
    for (mask = 0; mask < 16; ++mask) {
        u32 in = 0x12340008, out = in; // other buttons pass through
        for (i = 0; i < 4; ++i) {
            if (mask & (1u << i)) { in |= dirs[i]; out |= dirs[(i + 3) % 4]; }
        }
        assert(sideways_buttons(&a, in) == out);
        assert(sideways_buttons(&b, in) == in); // each remote its own
    }
    d.gforce.x = -.5f; d.gforce.y = .5f; d.gforce.z = .707f;
    UPD(&a, &d);
    UPD(&b, &d);
    assert(a.sideways && !b.sideways); // near the diagonal nothing changes
    d.gforce.x = d.gforce.y = 0; d.gforce.z = 1;
    UPD(&a, &d);
    assert(a.sideways); // face up says nothing: the last one stays
    d.gforce.x = 2; d.gforce.z = 0;
    UPD(&a, &d);
    assert(a.sideways); // a shake
    d.data_present = 0; d.gforce.x = 1;
    UPD(&a, &d);
    assert(a.sideways); // no accelerometer report
    d.data_present = WPAD_DATA_ACCEL; d.gforce.x = -1;
    d.btns_d = d.btns_h = WPAD_BUTTON_UP;
    UPD(&a, &d);
    assert(sideways_buttons(&a, d.btns_d) == WPAD_BUTTON_LEFT);
    d.btns_d = 0; d.gforce.x = 0; d.gforce.y = 1;
    UPD(&a, &d);
    assert(sideways_buttons(&a, d.btns_h) == WPAD_BUTTON_LEFT); // held: kept
    d.btns_h = 0; d.btns_u = WPAD_BUTTON_UP;
    UPD(&a, &d);
    assert(sideways_buttons(&a, d.btns_u) == WPAD_BUTTON_LEFT); // and its release
    d.btns_u = 0;
    UPD(&a, &d);
    assert(!a.sideways); // upright once let go
    d.gforce.x = -1; d.gforce.y = 0;
    UPD(&a, &d);
    d.gforce.x = 1;
    UPD(&a, &d);
    assert(!a.sideways); // D-pad at the right is not this
    d.gforce.x = -.42f; d.gforce.y = 0; d.gforce.z = .91f; // pointing, rolled 25 degrees
    UPD(&b, &d);
    assert(!b.sideways);
    d.gforce.x = -1; d.gforce.z = 0; d.ir.valid = 1; // sees the sensor bar: pointing
    UPD(&b, &d);
    assert(!b.sideways);
    d.ir.valid = 0;
    UPD(&a, &d);
    assert(a.sideways);
    d.btns_d = d.btns_h = WPAD_BUTTON_UP;
    UPD(&a, &d);
    d.btns_d = 0; d.ir.valid = 1; // sees a light mid-press
    UPD(&a, &d);
    assert(sideways_buttons(&a, d.btns_h) == WPAD_BUTTON_LEFT);
    d.btns_h = 0; d.btns_u = WPAD_BUTTON_UP;
    UPD(&a, &d);
    assert(sideways_buttons(&a, d.btns_u) == WPAD_BUTTON_LEFT);
    d.btns_u = 0;
    UPD(&a, &d);
    assert(!a.sideways);
    d.ir.valid = 0;
    UPD(&a, &d);
    d.gforce.x = -1; d.gforce.y = 0; d.gforce.z = 0;
    UPD(&a, &d);
    assert(a.sideways);
    d.err = -1;
    UPD(&a, &d);
    assert(a.sideways && !a.dpad_held); // no report: kept
    UPD(&a, 0);
    assert(!a.sideways && !a.streak); // gone: forgotten
    return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler")
class SidewaysRemoteTest(unittest.TestCase):
    def test_sideways(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "wiiuse").mkdir()
            (tmp / "wiiuse/wpad.h").write_bytes(WPAD.encode())
            (tmp / "check.c").write_bytes(CHECK.encode())
            exe = tmp / ("check.exe" if os.name == "nt" else "check")
            subprocess.run([CC, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "sdk/hbc_agent"),
                            str(tmp / "check.c"), "-o", str(exe)], check=True,
                           env=dict(os.environ, TMP=str(tmp), TEMP=str(tmp), TMPDIR=str(tmp)))
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
