#!/usr/bin/env python2

import sys, os, os.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

def hexdump(s):
	return ' '.join("%02x"%x for x in s)

if len(sys.argv) < 3:
	print("Usage: %s <encrypted ISO> <partition number>" % sys.argv[0])
	sys.exit(1)

isofile = sys.argv[1]
disc = wii.WiiDisc(isofile)
disc.showinfo()
part = wii.WiiPartition(disc,int(sys.argv[2]))
part.showinfo()

part.tmd.update_signature(open("signthree.bin", "rb").read())
part.tmd.brute_sha()
part.updatetmd()

part.showinfo()

