"""Build tests/host/panic_host/test.c on the host: the crash screen's plain logic
(channel/channelapp/source/panic_core.h: where a stack walk may read, the
backtrace, the cause text, the crash block `hbc.py crash` reads) and the
stub's IPC wait (channel/channelapp/stub/ipc_wait.h: a limit, late replies
taken). What this cannot show: the screen itself and a real IOS (docs/history/1.15.md,
1.15.0)."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


@unittest.skipUnless(CC, "no C compiler")
class PanicHostTest(unittest.TestCase):
    def test_logic(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp, "panic_test.exe" if os.name == "nt" else "panic_test")
            subprocess.run([CC, "-O1", "-Wall", "-Werror", "-Wno-unused-function",
                            "-D_CRT_SECURE_NO_WARNINGS", "-I", str(root / "tests/host/way_out_host/include"),
                            str(root / "tests/host/panic_host/test.c"), "-o", str(exe)],
                           check=True, env=dict(os.environ, TMP=tmp, TEMP=tmp, TMPDIR=tmp))
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("all panic scenarios passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
