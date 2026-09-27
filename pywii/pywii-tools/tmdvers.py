#!/usr/bin/env python3

import os
import re
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

wii.loadkeys()

args = sys.argv[1:]

def encode_version(version):
    """Pack SemVer major.minor.patch into TMD's 16-bit title_version.

    The wire layout is mmmmm nnnnnn ppppp (5/6/5 bits), preserving
    numeric ordering for supported versions.
    """
    match = re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version)
    if not match:
        raise ValueError("version must be major.minor.patch")
    major, minor, patch = map(int, match.groups())
    if major > 31 or minor > 63 or patch > 31:
        raise ValueError("TMD SemVer limits are major 0-31, minor 0-63, patch 0-31")
    return (major << 11) | (minor << 5) | patch


if len(args) != 2:
    raise SystemExit("usage: tmdvers.py TMD major.minor.patch")
tmdfile, version = args
try:
    newvers = encode_version(version)
except ValueError as error:
    raise SystemExit(str(error))

print("setting version of TMD file %s to 0x%04x" % (tmdfile, newvers))
tmd = wii.WiiTmd(open(tmdfile, "rb").read())
tmd.title_version = newvers
tmd.update()
tmd.null_signature()
tmd.brute_sha()
f = open(tmdfile,"wb")
f.write(tmd.data)
f.close()
