#!/usr/bin/env python2

import sys
import os
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

if len(sys.argv) != 4:
	print("Usage: %s keyfile.priv infile outfile"%sys.argv[0])
	sys.exit(1)

if sys.argv[1] == "-":
	k = sys.stdin.buffer.read()
else:
	k = open(sys.argv[1],"rb").read()

if len(k) != 30:
	print("Failed to read private key")
	sys.exit(2)

indata = open(sys.argv[2],"rb").read()
sha = wii.SHA.new(indata).digest()

print("SHA1: %s"%sha.hex())
print()
print("Signature:")
r,s = wii.ec.generate_ecdsa(k,sha)
print("R =",r[:15].hex())
print("   ",r[15:].hex())
print("S =",s[:15].hex())
print("   ",s[15:].hex())

outdata = b"SIG0" + r + s + indata

fd = open(sys.argv[3],"wb")
fd.write(outdata)
fd.close()
