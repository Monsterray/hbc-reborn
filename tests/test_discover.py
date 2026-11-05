"""Compile discover_is_query (channel/channelapp/source/discover.h) on the
host: HBC and the agent answer only a 16-byte HBCU query with byte 4 zero,
and never a datagram from port 4299, which is another console's reply. Two
Wiis that answered replies kept answering each other (1.14 audit)."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class DiscoverTest(unittest.TestCase):
    def test_only_queries_are_answered(self):
        source = r'''
#include <assert.h>
#include <string.h>
#include "discover.h"

int main(void) {
    u8 q[96] = "HBCU";
    u8 reply[96] = "HBCU\x01\x00\x10";
    assert(discover_is_query(q, 16, 53000));
    assert(!discover_is_query(q, 16, 4299));     // a console's own port
    assert(!discover_is_query(q, 12, 53000));    // short
    assert(!discover_is_query(q, 17, 53000));    // long
    assert(!discover_is_query(reply, 16, 53000)); // a reply: version 1
    assert(!discover_is_query(reply, 28, 4299));
    q[3] = 'V';
    assert(!discover_is_query(q, 16, 53000));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "gctypes.h").write_text(
                "#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n"
                "typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;\n"
                "typedef int32_t s32;\n")
            src = tmp / "check.c"
            exe = tmp / "check"
            src.write_text(source)
            subprocess.run([CC, "-std=c99", "-Wall", "-Werror", "-I", str(tmp),
                            "-I", str(ROOT / "channel/channelapp/source"), "-o", str(exe), str(src)],
                           check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
