#!/usr/bin/env python2

import sys, os, os.path, struct
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

if len(sys.argv) < 3:
	print("Usage: %s <arc file> <input dir>" % sys.argv[0])
	sys.exit(1)

fstb = wii.WiiFSTBuilder(0x20)

fstb.addfrom(sys.argv[2])

arc = open(sys.argv[1],"wb")
# dummy generate to get length
fstlen = len(fstb.fst.generate())
dataoff = wii.align(0x20+fstlen,0x20)
fst = fstb.fst.generate(dataoff)

hdr = struct.pack(">IIII16x",0x55AA382d,0x20,fstlen,dataoff)
arc.write(hdr)

arc.write(fst)
wii.falign(arc,0x20)
for f in fstb.files:
	data = open(f, "rb").read()
	arc.write(data)
	wii.falign(arc,0x20)

arc.close()
