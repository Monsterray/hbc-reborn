"""Build sdk/hbc_agent/way_out.c on the host and run it against a model of
the Wii's IPC registers (tests/way_out_host/test.c): a healthy IOS leaves
through the reload stub; a close that never replies launches HBC through ES;
an IOS that acks nothing, or a launch ES refuses, resets the Wii through
HW_RESETS with AHBPROT open, else through STM while IOS still acks, else
falls back on the stub. What this cannot show: how a real IOS wedges, and
what a reset does on a real Wii (see docs/devnet.md, "The way out")."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


@unittest.skipUnless(CC, "no C compiler")
class WayOutTest(unittest.TestCase):
    def test_scenarios(self):
        host = root / "tests/way_out_host"
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp, "way_out_test.exe" if os.name == "nt" else "way_out_test")
            subprocess.run([CC, "-O1", "-Wall", "-Werror", "-Wno-unused-function",
                            "-D_CRT_SECURE_NO_WARNINGS", "-I", str(host / "include"),
                            str(host / "test.c"), "-o", str(exe)],
                           check=True, env=dict(os.environ, TMP=tmp, TEMP=tmp, TMPDIR=tmp))
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.count("  PASS:"), 17, run.stdout)
        self.assertIn("all way-out scenarios passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
