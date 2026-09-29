#!/usr/bin/env python3

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

wii.loadkeys()

if len(sys.argv) < 2:
	print("Usage: %s <ticket>" % sys.argv[0])
	sys.exit(1)

tikfile = sys.argv[1]
print("fixing Tik file %s " % tikfile)
tik = wii.WiiTik(open(tikfile, "rb").read())
tik.null_signature()
tik.brute_sha()
tik.update()
f = open(tikfile,"wb")
f.write(tik.data)
f.close()
