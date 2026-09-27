#!/usr/bin/env python3
"""Check that the channel and retail WAD use the same release SemVer."""

import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
config = (root / "channel/channelapp/config.h").read_text()
title_makefile = (root / "channel/title/Makefile").read_text()

channel = re.search(r'^#define CHANNEL_VERSION_STR "([^"]+)"$', config, re.M)
wad = re.search(r"^CHANNEL_VERSION\s*=\s*(\S+)$", title_makefile, re.M)
assert channel and wad, "channel or WAD release version is missing"
assert re.fullmatch(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)", channel[1])
assert channel[1] == wad[1], f"channel {channel[1]} != WAD {wad[1]}"

date = re.search(r"^#define CHANNEL_VERSION_DATE (\d+)llu$", config, re.M)
assert date and re.fullmatch(r"\d{12}", date[1]), "channel update date must be YYYYMMDDHHMM"
print(f"release version {channel[1]}, update timestamp {date[1]}")
