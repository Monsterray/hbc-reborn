#!/usr/bin/python2

import sys, os, os.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

wii.loadkeys()

if len(sys.argv) != 4:
	print("Usage:")
	print(" python %s <encrypted ISO> <partition number> <dol output>"%sys.argv[0])
	sys.exit(1)

iso_name, partno, dol_name = sys.argv[1:4]
partno = int(partno)

disc = wii.WiiDisc(iso_name)
disc.showinfo()
part = wii.WiiCachedPartition(disc, partno, cachesize=32, debug=False)
partdata = wii.WiiPartitionData(part)

dolf = open(dol_name, "wb")
dolf.write(partdata.dol)
dolf.close()


