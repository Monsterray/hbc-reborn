#!/bin/sh
set -eu
# WiiPAX and wiiload refuse malformed input: a section name outside .shstrtab,
# and arguments that leave no room for the terminator. CI runs this on Linux.

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

make -C "$root/channel/wiiload"
make -C "$root/wiipax/client"

# Link the real WiiPAX objects so the check does not rely on dead stripping.
client="$root/wiipax/client"
${CC:-cc} -std=gnu11 -o "$tmp/wiipax-section-check" \
	"$root/tests/host/wiipax_section_bounds.c" \
	"$client"/LzFind.o "$client"/LzmaEnc.o "$client"/LzmaDec.o \
	"$client"/lzma.o "$client"/stub_*.o

if "$tmp/wiipax-section-check" >"$tmp/wiipax.out" 2>&1; then
	echo "WiiPAX accepted a section name outside .shstrtab" >&2
	exit 1
fi
grep -q 'Section #0 name out of .shstrtab bounds' "$tmp/wiipax.out"

dd if=/dev/zero of="$tmp/input.dol" bs=64 count=1 2>/dev/null
if WIILOAD=tcp:127.0.0.1 "$root/channel/wiiload/wiiload" \
	"$tmp/input.dol" "$(awk 'BEGIN { for (i = 0; i < 1021; ++i) printf "x" }')" \
	>"$tmp/wiiload.out" 2>&1; then
	echo "wiiload accepted arguments that leave no terminator space" >&2
	exit 1
fi
grep -q 'argument string too long' "$tmp/wiiload.out"

echo "host bounds checks passed"
