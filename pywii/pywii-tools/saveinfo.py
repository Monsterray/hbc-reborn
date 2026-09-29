#!/usr/bin/env python2

import sys, os, os.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

if len(sys.argv) < 2:
	print("Usage: %s <save file>" % sys.argv[0])
	sys.exit(1)

if not hasattr(wii, "WiiSave"):
	print("Error: this PyWii has no WiiSave support")
	sys.exit(1)

wii.loadkeys()

savefile = sys.argv[1]
save = wii.WiiSave(savefile)
save.showcerts()
