#!/usr/bin/env python2

import sys, os, os.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

wii.loadkeys()

if len(sys.argv) < 2:
	print("Usage: %s <tmd>" % sys.argv[0])
	sys.exit(1)

tmdfile = sys.argv[1]
print("TMD file %s:"%tmdfile)
tmd = wii.WiiTmd(open(tmdfile, "rb").read())
tmd.null_signature()
tmd.brute_sha()
tmd.update()
tmd.parse()
tmd.showinfo(" ")
f = open(tmdfile,"wb")
f.write(tmd.data)
f.close()
