#!/usr/bin/env python2

import sys, os, os.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

args = sys.argv[1:]

if len(args) < 1 or (args[0] == "-dpki" and len(args) < 2):
	print("Usage: %s [-dpki] <wad>" % sys.argv[0])
	sys.exit(1)

if args[0] == "-dpki":
	wii.loadkeys_dpki()
	args.pop(0)
else:
	wii.loadkeys()

wadfile = args.pop(0)
wad = wii.WiiWad(wadfile)
wad.showinfo()
