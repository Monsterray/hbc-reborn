#!/usr/bin/env python2

import sys, os, os.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

args = sys.argv[1:]

if len(args) < 2 or (args[0] == "-dpki" and len(args) < 3):
	print("Usage: %s [-dpki] <cert file> <certs>" % sys.argv[0])
	sys.exit(1)

if args[0] == "-dpki":
	wii.loadkeys_dpki()
	args.pop(0)
else:
	wii.loadkeys()

certfile = args.pop(0)

certs, certlist = wii.parse_certs(open(args.pop(0), "rb").read())

print("Certification file %s: " % certfile)
cert = wii.WiiCert(open(certfile, "rb").read())
cert.showinfo(" ")
cert.showsig(certs," ")

print("Certificates:")
for cert in certlist:
	cert.showinfo(" - ")
	cert.showsig(certs,"    ")
