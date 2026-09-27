#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stub="$root/channel/channelapp/stub.elf"
bin="$root/channel/channelapp/stub.bin"
nm=${DEVKITPPC:-/opt/devkitpro/devkitPPC}/bin/powerpc-eabi-nm

start=$($nm "$stub" | awk '$3 == "_start" { print $1 }')
sbss=$($nm "$stub" | awk '$3 == "__sbss_start" { print $1 }')
size=$(wc -c < "$bin")

[ "$start" = 80001800 ] || { echo "stub entry linked at $start, expected 80001800" >&2; exit 1; }
[ "$((0x$sbss))" -ge $((0x80005000)) ] || { echo "stub BSS overlaps Wii low-memory state" >&2; exit 1; }
[ "$size" -le $((0x1800)) ] || { echo "stub image overlaps return-title configuration" >&2; exit 1; }
echo "stub entry and return-title memory layout: PASS"
