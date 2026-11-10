"""Build HBC's start-up call log (channel/channelapp/source/bootlog.c) on the
host and run tests/host/bootlog_host/test.c against it: a poll repeated before
the first frame takes one row with a repeat count, the calls kept from before
the first frame leave room for later ones, a thread id reused by a new thread
names each call by the thread that made it, and the status JSON ends whole.
What this cannot show: the IPC interrupt racing a thread on a real Wii."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")

DEVNET_H = """#include <gctypes.h>
u32 devnet_boot_ms(void);
void devnet_boot_mark(const char *mark);
void hlog(const char *fmt, ...);
"""


@unittest.skipUnless(CC, "no C compiler")
class BootlogTest(unittest.TestCase):
    def test_scenarios(self):
        host = root / "tests/host/bootlog_host"
        source = root / "channel/channelapp/source"
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp, "src")
            src.mkdir()
            for name in ("bootlog.c", "bootlog.h"):
                shutil.copy(source / name, src / name)
            (src / "devnet.h").write_bytes(DEVNET_H.encode())
            pathlib.Path(tmp, "config.h").write_bytes(b"")
            exe = pathlib.Path(tmp, "bootlog_test.exe" if os.name == "nt" else "bootlog_test")
            subprocess.run([CC, "-O1", "-Wall", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                            "-I", str(host / "include"), "-I", str(src),
                            str(src / "bootlog.c"), str(host / "test.c"), "-o", str(exe)],
                           check=True, env=dict(os.environ, TMP=tmp, TEMP=tmp, TMPDIR=tmp))
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.count("  PASS:"), 9, run.stdout)
        self.assertIn("all bootlog checks passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
