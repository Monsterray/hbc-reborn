"""Build sdk/hbc_agent/way_out.c on the host and run it against a model of
the Wii's IPC registers (tests/way_out_host/test.c), after Dolphin's
WiiIPC: a healthy IOS leaves through the reload stub (late replies drained
first); a close that never replies launches HBC through ES (the stub's
title, then OHBC, then LULZ); a launch that does not go ahead resets the Wii
through STM while IOS still acks, then through HW_RESETS with AHBPROT open
(never on a vWii); an IOS that acks nothing resets through HW_RESETS; and
with a reset in the last 10 minutes, or nothing working, it falls back on
the stub. The model checks each ES ioctlv's vectors, raises the IPC
interrupt on a Y write as Dolphin does, and fails on any write to a register
it does not know. What this cannot show: how a real IOS wedges, and what a
reset does on a real Wii (see docs/devnet.md, "The way out")."""

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
                            "-Wno-initializer-overrides",   # WANT(): defaults, then the scenario's
                            "-D_CRT_SECURE_NO_WARNINGS", "-I", str(host / "include"),
                            str(host / "test.c"), "-o", str(exe)],
                           check=True, env=dict(os.environ, TMP=tmp, TEMP=tmp, TMPDIR=tmp))
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.count("  PASS:"), 29, run.stdout)
        self.assertIn("all way-out scenarios passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
