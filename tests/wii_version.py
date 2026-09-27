#!/usr/bin/env python3
"""Read the running HBC version from its local Wiiload service."""

import pathlib
import re
import socket
import sys
import time


if len(sys.argv) != 2:
    raise SystemExit(f"usage: {sys.argv[0]} <Wii IPv4 address>")

config = pathlib.Path(__file__).resolve().parents[1] / "channel/channelapp/config.h"
match = re.search(r'^#define CHANNEL_VERSION_STR "(\d+\.\d+\.\d+)"$', config.read_text(), re.M)
if not match:
    raise SystemExit("channel version is not major.minor.patch")
expected = match.group(1)
deadline = time.monotonic() + 90
last = "no reply"

while time.monotonic() < deadline:
    try:
        with socket.create_connection((sys.argv[1], 4299), timeout=2) as conn:
            conn.settimeout(2)
            conn.sendall(b"HBCV" + bytes(12))
            reply = bytearray()
            while len(reply) < 64 and b"\0" not in reply:
                chunk = conn.recv(64 - len(reply))
                if not chunk:
                    break
                reply.extend(chunk)
        actual = bytes(reply).split(b"\0", 1)[0].decode("ascii")
        if actual == expected:
            print(f"HBC {actual} is running at {sys.argv[1]}")
            break
        last = f"HBC {actual}" if actual else "no version reply"
    except (OSError, UnicodeError) as exc:
        last = str(exc)
    time.sleep(1)
else:
    raise SystemExit(f"Expected HBC {expected}; last result: {last}")
