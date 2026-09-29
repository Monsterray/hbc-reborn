#!/usr/bin/env python3

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii

if len(sys.argv) != 6:
	print("Usage: %s -cetk|-tmd <infile> <outfile> <certs> <issuer>" % sys.argv[0])
	sys.exit(1)

pywii.loadkeys_dpki()

args = sys.argv[1:]
mode = args.pop(0)
infile = args.pop(0)
outfile = args.pop(0)
certfile = args.pop(0)
issuer = args.pop(0)

if sys.argv[1] == "-cetk":
	signed = pywii.WiiTik(open(infile, "rb").read())
elif sys.argv[1] == "-tmd":
	signed = pywii.WiiTmd(open(infile, "rb").read())
else:
	print("EYOUFAILIT")
	sys.exit(1)

certs, certlist = pywii.parse_certs(open(certfile, "rb").read())

signed.update_issuer(issuer)

if not signed.sign(certs):
	print("dpki signing failed")
	sys.exit(1)

with open(outfile, "wb") as output:
	output.write(signed.data)

print("successfully signed %s" % outfile)
sys.exit(0)
