#!/usr/bin/env python2

import sys, os, os.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

wii.loadkeys()

tmdfile = sys.argv[1]
print "TMD file %s:"%tmdfile
tmd = wii.WiiTmd(open(tmdfile, "rb").read())
tmd.null_signature()
tmd.brute_sha()
tmd.update()
tmd.parse()
tmd.showinfo(" ")
f = open(tmdfile,"wb")
f.write(tmd.data)
f.close()
