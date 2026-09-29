#!/usr/bin/env python3

import hashlib
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

args = sys.argv[1:]

if len(args) < 2 or (args[0] == "-dpki" and len(args) < 3):
	print("Usage: %s [-dpki] <wad> <indir> [contentdir]" % sys.argv[0])
	sys.exit(1)

if args[0] == "-dpki":
	wii.loadkeys_dpki()
	args.pop(0)
else:
	wii.loadkeys()

wadfile = args.pop(0)
indir = args.pop(0)
# Contents and footer may live outside the ticket/TMD directory.
contentdir = args.pop(0) if args else indir

tmd = wii.WiiTmd(open(os.path.join(indir, "tmd"), "rb").read())
tik = wii.WiiTik(open(os.path.join(indir, "cetk"), "rb").read())

certs, certlist = wii.parse_certs(open(os.path.join(indir, "certs"), "rb").read())

footer = open(os.path.join(contentdir, "footer"), "rb").read()

contents = []
for ct in tmd.get_content_records():
	data = open(os.path.join(contentdir, "%08X" % ct.cid), "rb").read()
	# Refuse contents the TMD does not describe, such as an unresolved symlink.
	if len(data) != ct.size or hashlib.sha1(data).digest() != ct.sha:
		sys.exit("content %08X does not match the TMD size and SHA-1" % ct.cid)
	contents.append((data, ct.cid))

wad = wii.WiiWadMaker(wadfile, tmd, tik, certlist, footer)

for data, cid in contents:
	wad.adddata(data, cid)

wad.finish()

if not wad.tik.signcheck(wad.certs):
	wad.tik.null_signature()
	wad.tik.brute_sha()
	wad.updatetik()

if not wad.tmd.signcheck(wad.certs):
	wad.tmd.null_signature()
	wad.tmd.brute_sha()
	wad.updatetmd()
