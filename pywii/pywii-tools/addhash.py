#!/usr/bin/env python2
import sys
import re
import os
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

if len(sys.argv) < 4:
    print("Usage: %s <template in> <file to hash> <output>" % sys.argv[0])
    sys.exit(1)

hash = wii.SHA.new(open(sys.argv[2], "rb").read()).digest().hex()
f = open(sys.argv[1], "r", newline="")
data = f.read()
f.close()
data = re.sub('@SHA1SUM@', hash, data)
open(sys.argv[3], "w", newline="").write(data)
