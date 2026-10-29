#!/usr/bin/env python3
"""Where the built channel keeps what the hardware reads (needs the ELF from
`make -C channel` and devkitPPC's nm; skipped without them).

The GX FIFO must be in MEM1. Taken from the heap it went to MEM2 once HBC's
.bss grew (1.10.21), and fed from MEM2 the GPU stalled at random on a real
Wii, never in Dolphin (REVIEW.md, 1.10.26).

The small-data base (r13, _SDA_BASE_) must be the start of .sdata. libogc's
ps_guMtxConcat loads its (0, 1) pair with a psq_l, whose displacement is 12
bits; with ld's default base (.sdata + 32 KiB) the -32768 offset of the
first .sdata object was truncated into the W bit and offset 0, and every
matrix concatenation added whatever lay at r13 (1.12.2: the Mii drawn as a
sliver while a particle coordinate sat there). The object it loads, Unit01,
must also stay within 2047 bytes of the base."""

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
OBJDUMP = NM and NM.replace("powerpc-eabi-nm", "powerpc-eabi-objdump")


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

    def test_small_data_base_is_the_section_start(self):
        nm = subprocess.run([NM, str(ELF)], capture_output=True, text=True, check=True).stdout
        base = {line.split()[2]: int(line.split()[0], 16) for line in nm.splitlines() if len(line.split()) == 3}
        self.assertIn("_SDA_BASE_", base)
        headers = subprocess.run([OBJDUMP, "-h", str(ELF)], capture_output=True, text=True, check=True).stdout
        sdata = [int(line.split()[3], 16) for line in headers.splitlines() if len(line.split()) > 3 and line.split()[1] == ".sdata"]
        self.assertEqual(len(sdata), 1, "no .sdata section")
        self.assertEqual(base["_SDA_BASE_"], sdata[0], "newrvl.ld: _SDA_BASE_ is not .sdata's start")
        # ps_guMtxConcat's psq_l of Unit01 through r13 must hold Unit01's real
        # offset: one past 2047 spills into the W bit (one float, not the pair)
        # or the I field (another GQR's scaling), and ld says nothing.
        unit = base["Unit01"] - base["_SDA_BASE_"]
        self.assertTrue(0 <= unit < 2048, f"Unit01 is {unit} bytes past _SDA_BASE_, out of psq_l's reach")
        dis = subprocess.run([OBJDUMP, "-d", str(ELF)], capture_output=True, text=True, check=True).stdout
        loads = [line.strip() for line in dis.splitlines() if "psq_" in line and "(r13)" in line]
        self.assertTrue(loads, "no psq_l through r13: ps_guMtxConcat not linked?")
        for line in loads:
            offset, w, i = line.split()[-1].replace("(r13)", "").split(",")[1:4]
            self.assertEqual((int(offset), int(w), int(i)), (unit, 0, 0), f"psq through r13 off: {line}")


if __name__ == "__main__":
    unittest.main()
