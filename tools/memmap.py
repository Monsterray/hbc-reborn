#!/usr/bin/env python3
"""Where everything in an ELF lives: one CSV row per symbol, with its address,
size, output section and the object file it came from.

The linker map (-Wl,-Map) gives each object's input sections and the
linker's own symbols (__bss_end, __Arena1Lo, ...); nm gives every symbol's
address and size. A row's object is the input section its address falls in.

  tools/memmap.py channelapp.map channelapp_nopax.elf > memmap.csv

(`make -C channel/channelapp memmap` does this for HBC.)

Sort or filter the CSV in a spreadsheet; `--sections` prints each output
section's range and size instead. NM (default: devkitPPC's powerpc-eabi-nm)
names the nm to use.
"""

import bisect
import csv
import os
import re
import subprocess
import sys

OUTPUT = re.compile(r"^(\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
INPUT = re.compile(r"^ (\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (.+)$")
NAME_ONLY = re.compile(r"^ (\S+)$")


def short(obj):
    """build/avatar.o, libogc.a(gx.o): no toolchain folders."""
    obj = obj.strip().replace("\\", "/")
    return re.sub(r"^.*/(?=[^/]+\.a\()", "", obj) if ".a(" in obj else re.sub(r"^.*/lib/gcc/.*/", "", obj)


def read_map(path):
    """The output sections [(name, start, size)] and the input sections
    [(start, size, section, object)], sorted by start."""
    outs, ins, pending = [], [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("Linker script and memory map", 1)[-1].splitlines()
    for line in lines:
        m = OUTPUT.match(line)
        if m and not line.startswith(" "):
            outs.append((m[1], int(m[2], 16), int(m[3], 16)))
            continue
        m = NAME_ONLY.match(line)
        if m:   # a long input section name: its numbers are on the next line
            pending = m[1]
            continue
        m = INPUT.match(line)
        if m and (m[1] or pending) and not m[4].startswith("*"):
            start, size = int(m[2], 16), int(m[3], 16)
            if size and start:
                ins.append((start, size, m[1] or pending, short(m[4])))
        pending = None
    ins.sort()
    return outs, ins


def read_nm(elf):
    nm = os.environ.get("NM") or os.path.join(os.environ.get("DEVKITPPC", "/opt/devkitpro/devkitPPC"),
                                              "bin", "powerpc-eabi-nm")
    out = subprocess.run([nm, "-n", "-S", elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            yield int(p[0], 16), int(p[1], 16), p[2], p[3]
        elif len(p) == 3:
            yield int(p[0], 16), None, p[1], p[2]


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    outs, ins = read_map(argv[1])
    if "--sections" in argv:
        for name, start, size in outs:
            if size:
                print(f"{name:20} {start:08x}-{start + size:08x} {size:9d}")
        return
    starts = [i[0] for i in ins]
    out_starts = sorted((s, n, z) for n, s, z in outs if z)
    w = csv.writer(sys.stdout, lineterminator="\n")
    w.writerow(["address", "size", "end", "section", "type", "symbol", "object"])
    for addr, size, kind, name in read_nm(argv[2]):
        k = bisect.bisect_right(starts, addr) - 1
        obj = ins[k][3] if k >= 0 and addr < ins[k][0] + ins[k][1] else ""
        j = bisect.bisect_right([s for s, _, _ in out_starts], addr) - 1
        sec = out_starts[j][1] if j >= 0 and addr < out_starts[j][0] + out_starts[j][2] else ""
        w.writerow([f"{addr:08x}", "" if size is None else size,
                    "" if size is None else f"{addr + size:08x}", sec, kind, name, obj])


if __name__ == "__main__":
    main(sys.argv)
