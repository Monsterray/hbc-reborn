#!/usr/bin/env python3

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

wii.loadkeys()

args = sys.argv[1:]

tmdfile = args.pop(0)

if len(args) == 2:
	newvers = int(args.pop(0)) << 8 | int(args.pop(0))
else:
	newvers = int(args.pop(0), 16)

print("setting version of TMD file %s to 0x%04x" % (tmdfile, newvers))
tmd = wii.WiiTmd(open(tmdfile, "rb").read())
tmd.title_version = newvers
tmd.update()
tmd.null_signature()
tmd.brute_sha()
f = open(tmdfile,"wb")
f.write(tmd.data)
f.close()
