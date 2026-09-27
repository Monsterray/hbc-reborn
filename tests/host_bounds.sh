#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

make -C "$root/channel/wiiload"
make -C "$root/wiipax/client"

case "$(uname -s)" in
	Darwin) gc=-Wl,-dead_strip ;;
	*) gc=-Wl,--gc-sections ;;
esac
${CC:-cc} -std=gnu11 -ffunction-sections -fdata-sections "$gc" \
	-o "$tmp/wiipax-section-check" "$root/tests/wiipax_section_bounds.c"

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
