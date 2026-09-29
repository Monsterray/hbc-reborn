#!/usr/bin/env python2

import sys, os, os.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

if len(sys.argv) < 2:
	print("Usage: %s <encrypted ISO> [partition number]" % sys.argv[0])
	sys.exit(1)

wii.loadkeys()

isofile = sys.argv[1]
disc = wii.WiiDisc(isofile,readonly=True)
disc.showinfo()

partitions = disc.read_partitions()

parts = range(len(partitions))

try:
	pnum = int(sys.argv[2])
	partitions[pnum]
	parts = [pnum]
except:
	pass

for partno in parts:
	part = wii.WiiCachedPartition(disc,partno)
	part.showinfo()
	pdat = wii.WiiPartitionData(part)
	pdat.showinfo()

