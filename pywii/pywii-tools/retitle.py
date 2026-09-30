#!/usr/bin/env python3
"""Give a ticket or TMD another title ID.

usage: retitle.py tik|tmd FILE TITLEID

TITLEID is 16 hex digits, or 4 characters taken as the low word of a
00010001 (channel) title, such as OHBC or LULZ.

A ticket's title key is encrypted with its title ID as the IV, so for a
ticket the key is decrypted under the old ID and encrypted again under the
new one; it is then fakesigned. A TMD only has its ID changed; sign it
afterwards (tmdupdatecr.py does). No key is ever printed.
"""

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Common")))
import pywii as wii

try:
    from Cryptodome.Cipher import AES
except ImportError:
    from Crypto.Cipher import AES


def title_id(text):
    if len(text) == 4:
        return bytes.fromhex("00010001") + text.encode("ascii")
    if len(text) == 16:
        return bytes.fromhex(text)
    raise SystemExit("title ID: 16 hex digits or 4 characters, not %r" % text)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("tik", "tmd"):
        raise SystemExit(__doc__)
    kind, path, new = sys.argv[1], sys.argv[2], title_id(sys.argv[3])
    data = open(path, "rb").read()
    if kind == "tmd":
        tmd = wii.WiiTmd(data)
        tmd.title_id = new
        tmd.update()
        out = tmd.data
    else:
        wii.loadkeys()
        tik = wii.WiiTik(data)
        if tik.title_key is None:
            raise SystemExit("retitle: no common key to decrypt the title key (see keys/)")
        if tik.title_id != new:
            key = wii.keys["korean-key" if tik.common_key_index == 1 else "common-key"]
            enc = AES.new(key, AES.MODE_CBC, new + b"\x00" * 8).encrypt(tik.title_key)
            # Into the body itself: brute_sha() re-parses the body, so an
            # attribute set on tik alone would be lost.
            tik.body = tik.body[:0x7f] + enc + tik.body[0x8f:0x9c] + new + tik.body[0xa4:]
            tik.parse()
        tik.null_signature()
        tik.brute_sha()
        out = tik.data
    open(path, "wb").write(out)
    print("%s %s: title %s-%s" % (kind, path, new[:4].hex(), new[4:].hex()))


if __name__ == "__main__":
    main()
