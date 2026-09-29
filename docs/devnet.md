# Developer network protocol

HBC listens on TCP port 4299 while its menu is shown. Besides Wiiload
uploads, it answers a few developer requests that make testing on a real
Wii faster. `tools/hbc.py` implements the client side; this page is the wire
format for other tools.

## Access

HBC answers only hosts on its own `/16` network, as it always has for
Wiiload. Wiiload already runs any code a LAN host sends, so these requests
add no new privilege; do not expose port 4299 beyond your LAN.

## Framing

Every request starts with a 16-byte header whose first four bytes select the
request. Integers are big-endian. One request per connection; HBC closes the
connection after its reply. A probe must send a header: a bare
connect-and-close holds the loader for its 10 s receive timeout.

Replies to `HBCS`, `HBCF` and `HBCN` start with an 8-byte header:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | status: 0, or a negative newlib `errno` |
| 4 | 4 | payload length |

## Requests

| Magic | Header bytes 4-15 | Payload | Reply |
| --- | --- | --- | --- |
| `HAXX` | Wiiload 0.5 upload | data, then arguments | none |
| `HBCV` | zero | none | NUL-terminated version string (no reply header) |
| `HBCS` | zero | none | JSON status |
| `HBCF` | op (1), 0 (1), path length (2), size (4), 0 (4) | path, then `size` bytes for a put | see below |
| `HBCN` | log port (2), zero | none | empty; sets the app log target |

### Status

```json
{"version":"1.2.0","ios":58,"ios_revision":6176,"ahbprot":true,
 "mem1_free":4080,"mem2_free":50546528,"ip":"192.168.8.213","apps":12,
 "device":"sd","inserted":["sd"],"log":"192.168.8.147:4405"}
```

`device` is the mounted device that file requests can use; `inserted` also
lists devices that were present at the last device poll.

### Files

Paths are `<device>:/<path>` with device `sd`, `usb`, `carda` or `cardb`. HBC
rejects paths containing `..`, `//`, a backslash, a second colon, or a control
character, and paths of 256 bytes or more.

| Op | Action | Reply payload |
| --- | --- | --- |
| `P` | write `size` bytes (at most 512 MiB) to `path.part`, then rename to `path`; parent directories are created | none |
| `G` | read a file | the file |
| `L` | list a directory | lines of `d <name>` or `f <size> <name>` |
| `D` | delete a file or an empty directory | none |
| `M` | create a directory and its parents | none |

HBC rescans the app list every 30 frames, so an app put under `sd:/apps/`
appears in the menu without a restart.

### Network log

`HBCN` records the client's address and the given port in a 32-byte block at
`0x80002f20`, just past the reload stub's return-title words:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `HBCN` |
| 4 | 4 | version, 1 |
| 8 | 4 | PC IPv4 address |
| 12 | 2 | port |
| 14 | 2 | flags, 0 |
| 16 | 4 | magic ^ version ^ address ^ port |
| 20 | 12 | reserved, 0 |

Port 0 clears the block. The block survives the jump to the next app. An app
that includes [`sdk/hbc_netlog.h`](../sdk/hbc_netlog.h) and calls
`hbc_netlog_init()` connects to that address and copies `stdout` and `stderr`
to it, still writing to any console it set up first.

## Workflow

```sh
export HBC_WII=192.168.1.50
python3 tools/hbc.py status
python3 tools/hbc.py run myapp.dol arg1 arg2   # prints the app's output
python3 tools/hbc.py put build/data.bin sd:/apps/myapp/data.bin
python3 tools/hbc.py ls sd:/apps/myapp
```

`run` needs inbound TCP on the log port (4405 by default, `--log-port`) to be
allowed through the PC's firewall.
