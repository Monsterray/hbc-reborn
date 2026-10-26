#!/usr/bin/env python3
"""Where the built channel keeps what the hardware reads (needs the ELF from
`make -C channel` and devkitPPC's nm; skipped without them).

The GX FIFO must be in MEM1. Taken from the heap it went to MEM2 once HBC's
.bss grew (1.10.21), and fed from MEM2 the GPU stalled at random on a real
Wii, never in Dolphin (REVIEW.md, 1.10.26)."""

import os
import pathlib
import shutil
import subprocess
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
ELF = root / "channel/channelapp/channelapp_nopax.elf"
FIFO_SIZE = 256 * 1024
MEM1 = (0x80000000, 0x81800000)


def find_nm():
    for base in (os.environ.get("DEVKITPPC"), "/opt/devkitpro/devkitPPC", "C:/devkitPro/devkitPPC"):
        if base:
            nm = pathlib.Path(base) / "bin" / ("powerpc-eabi-nm.exe" if os.name == "nt" else "powerpc-eabi-nm")
            if nm.exists():
                return str(nm)
    return shutil.which("powerpc-eabi-nm")


NM = find_nm()


@unittest.skipUnless(ELF.exists() and NM, "build the channel (make -C channel) with devkitPPC first")
class MemoryLayoutTest(unittest.TestCase):
    def symbols(self):
        out = subprocess.run([NM, "-S", str(ELF)], capture_output=True, text=True, check=True).stdout
        table = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 4:
                table[parts[3]] = (int(parts[0], 16), int(parts[1], 16), parts[2])
        return table

    def test_gx_fifo_is_static_in_mem1(self):
        sym = self.symbols().get("gp_fifo")
        self.assertIsNotNone(sym, "gp_fifo is no longer a static array (a heap FIFO can land in MEM2)")
        addr, size, kind = sym
        self.assertIn(kind.lower(), ("b", "d"), f"gp_fifo is {kind}, not data")
        self.assertEqual(size, FIFO_SIZE)
        self.assertEqual(addr % 32, 0, f"gp_fifo at {addr:#x} is not 32-byte aligned")
        self.assertTrue(MEM1[0] <= addr and addr + size <= MEM1[1], f"gp_fifo at {addr:#x} is not in MEM1")


if __name__ == "__main__":
    unittest.main()
