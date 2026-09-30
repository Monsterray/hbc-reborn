# Developer network protocol

HBC listens on TCP port 4299 while its menu is shown. Besides Wiiload
uploads, it answers a few developer requests that make testing on a real
Wii faster. `tools/hbc.py` implements the client side; this page is the wire
format for other tools.

## Access

HBC answers only hosts on its own `/16` network, as it always has for
Wiiload. Wiiload already runs any code a LAN host sends, so these requests
add no new privilege; do not expose port 4299 beyond your LAN. The same goes
for `HBCN`: any host on the `/16` can point the next app's log output at
itself, just as it could send that app in the first place.

HBC waits at most 2 s for a request's 16-byte header after accepting a
connection, and ends a request as soon as IOS reports the connection closed.
IOS usually does so at once, but sometimes reports a closed socket only as
"readable" with no data until that limit, so a client that connects and
closes without sending anything costs between nothing and 2 s.

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

Error numbers are newlib's, not the host's: for example `EBADMSG` is 77 and
`ENOSYS` is 88. `tools/hbc.py` has the table.

## Requests

| Magic | Header bytes 4-15 | Payload | Reply |
| --- | --- | --- | --- |
| `HAXX` | Wiiload 0.5 upload | data, then arguments | none |
| `HBCV` | zero | none | NUL-terminated version string (no reply header) |
| `HBCS` | zero | none | JSON status |
| `HBCF` | op (1), flags (1), path length (2), size (4), 0 (4) | path, then the data for a put | see below |
| `HBCN` | log port (2), zero | none | empty; sets the app log target |
| `HBCC` | zero | none | empty; forgets the reported crash (protocol 3) |
| `HBCX` | zero | none | empty; an [agent](#in-app-agent) app then exits to HBC (protocol 3) |

### Status

```json
{"version":"1.4.0","proto":2,"ios":58,"ios_revision":6175,"ahbprot":true,
 "mem1_free":4080,"mem2_free":50599776,"heap_free":116032,
 "tcp_stack_used":2824,"tcp_stack_size":8192,"init_ms":968,"scan_ms":300,
 "ip":"192.168.8.213","apps":14,"device":"sd","inserted":["sd"],
 "log":"192.168.8.147:4405",
 "last":{"op":"p","bytes":5328444,"wire":2663251,"ms":3674,
         "net_ms":1881,"disk_ms":3292,"cpu_ms":164}}
```

`proto` is 2 when the framed ops below exist, and 3 when `HBCC`, the
`crash` field, and the in-app agent's `HBCX` exist. `device` is the mounted device
that file requests can use; `inserted` also lists devices that were present
at the last device poll. `last` describes the most recent file transfer:
bytes on the network, and time the Wii spent on Wi-Fi, SD, and zlib/CRC.
Network and SD overlap in framed transfers, so their sum can exceed `ms`.
`heap_free` is newlib's free heap (its "used" figure would count the gap
between MEM1 and MEM2), `tcp_stack_used` is the loader thread's stack
high-water mark, `init_ms` is the time from HBC's `main()` to its menu, and
`scan_ms` is the last full app scan. `zlib_mem` says where the zlib arena
landed (see Performance), and `tcp_last_failure` records what IOS returned
during the last receive that failed: `r<n>` per `net_read` result,
`p<events>/<result>` per poll, then the reason. `crash` is `null`, or the
crash an agent app reported before it returned to this HBC (see
[Crash reports](#crash-reports)).

### Files

Paths are `<device>:/<path>` with device `sd`, `usb`, `carda` or `cardb`. HBC
rejects paths containing `..`, `//`, a backslash, a second colon, or a control
character, and paths of 256 bytes or more.

| Op | Action | Reply payload |
| --- | --- | --- |
| `p` | framed upload of `size` bytes (at most 512 MiB) to `path.part`, then rename to `path`; parent directories are created | none |
| `g` | framed download; flags bit 0 allows zlib frames | reply header with the file size, then frames |
| `P` | protocol 1 upload of `size` raw bytes, as `p` without checks | none |
| `G` | protocol 1 download | the file |
| `L` | list a directory | lines of `d <name>` or `f <size> <name>`; a listing that reached 256 KiB ends with `! truncated` |
| `C` | checksum a file | u32 size, u32 CRC-32 |
| `D` | delete a file or an empty directory | none |
| `M` | create a directory and its parents | none |

A put to a path ending in `/` is refused with `EISDIR`. After a put or delete
under `<device>:/apps/<name>/` on the mounted device, HBC reloads that app's
menu entry, so an uploaded app appears (and a deleted one disappears) at once.

If an app is launched from the Wii while a transfer runs, HBC stops the
transfer first (an upload's partial file is deleted), and only then unmounts
the card.

### Framed transfers

Ops `p` and `g` move data in frames of at most 64 KiB:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | raw length, 1 to 65536 |
| 4 | 4 | wire length; equal to the raw length for stored data, smaller for one zlib stream |
| 8 | 4 | CRC-32 of the raw bytes |
| 12 | wire length | data |

An upload sends frames until their raw lengths add up to `size`. HBC checks
every CRC and answers `EBADMSG` if any frame fails, deleting the partial file.
A download ends with a terminator frame whose raw and wire lengths are 0 and
whose CRC field holds the read status (0, or a negative errno). The client
must check every CRC and the total length.

On the Wii, the loader thread handles the network while a worker thread
handles SD and zlib, passing four MEM2 buffer pairs through message queues.
IOS then services Wi-Fi and SDIO concurrently, and Broadway compresses while
the radio is busy. HBC compresses downloads with zlib level 1 and skips
compression for 8 frames after one that did not shrink. Transfers run above
the UI thread's priority; the thread mostly waits on IOS, so the menu keeps
drawing.

### Performance

Measured on the bench Wii (802.11g, IOS58) with `tests/wii_netbench.py`:

| Data | Protocol 1 up / down | Framed up / down |
| --- | --- | --- |
| 4 MiB random | 0.93 / 0.57 MB/s | 0.90 / 0.60 MB/s |
| 4 MiB zeros | 0.95 / 0.56 MB/s | 4.19 / 4.86 MB/s |
| 5.3 MB ELF | 0.87 / 0.57 MB/s | 1.45 / 0.91 MB/s |

A back-to-back A/B on the bench Wii (three interleaved rounds) found no
difference between 1.3.0 and 1.4.0: ELF downloads 0.89 against 0.88 MB/s,
uploads 1.44 against 1.46. Single runs vary by 30% or more with Wi-Fi
conditions, so compare interleaved runs.

zlib's state lives in a 320 KiB MEM1 arena reserved at startup, because by
the time a transfer runs the menu has filled MEM1 and the heap returns MEM2.
`tests/membench` measured, on the Wii:

| | MEM1 | MEM2 | Locked cache, DMA from MEM2 |
| --- | --- | --- | --- |
| memcpy | 213 MB/s | 70 MB/s | |
| CRC-32 | 173 MB/s | 100 MB/s | 244 MB/s |
| inflate (state there) | 44.5 MB/s | 35.5 MB/s | |
| deflate level 1 / 3 / 6 (state there) | 7.9 / 6.3 / 3.3 MB/s | 7.0 / 5.7 / 3.1 MB/s | |

With the arena, a 5.3 MB ELF download spent 653 ms of Wii CPU instead of
729 ms. The frame buffers stay in MEM2: IOS DMAs them and the CPU streams
through them once. HBC does not use the locked cache: it would save about
30 ms of CRC per 5 MB, while taking half the L1 data cache from the menu
thread for the whole transfer.

Incompressible data is bound by the IOS network stack: about 1.35 MB/s
received and 0.63 MB/s sent. Larger IOS blocks helped (4 KiB to 16 KiB,
libogc's heap limit, raised uploads from 0.67 to 0.94 MB/s); socket buffer
sizes, blocking receives, and zlib level 6 did not. Before 1.2.3 the loader
slept 20 ms whenever IOS had no data ready and uploads ran at 0.62 MB/s.

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

IOS clears low memory (`0x0` to `0x3fff`) whenever it boots a title, so the
block is gone once an app exits and the reload stub starts the installed
channel again (Dolphin's `IOS.cpp` `SetupMemory` models the same clear). HBC
therefore writes a copy of the block to MEM2 at `0x91800000`, reads it first
thing in `main()`, and restores the target from it. An app that happens to
use that memory costs only the copy, which its check word then rejects.

## In-app agent

An app linked with [`sdk/hbc_agent`](../sdk/hbc_agent.h) answers on port
4299 while it runs, with the same framing:

| Request | Agent's answer |
| --- | --- |
| `HBCV` | `<version> agent`, so clients can tell the app from HBC |
| `HBCS` | JSON status: `"agent":true`, `app`, `app_version`, `uptime_ms`, IOS, memory, `agent_stack_used`/`agent_stack_size`, `ip`, mounted devices (`inserted`, `device`), `log`, `last`, `exit_requested` |
| `HBCF` | every file op above, on the devices the app mounted |
| `HBCN` | sets the log target for the apps after this one |
| `HBCX` | replies, then the app exits to HBC |
| `HBCK` | header bytes 4-5: a count (at most 64), then that many of `udlrabh12`; queued as controller presses (`h` is HOME, `1` and `2` the remote's buttons, which apps check with `hbc_agent_home_pending()`) |
| `HBCP` | reply header, then u32 width, u32 height, and the YUYV framebuffer the VI is showing |
| `HAXX` | closes the connection without reading, then exits to HBC: the upload fails once, and the retry reaches HBC |

The agent is HBC's own `devfile.c`, `devstream.c` and `tcp.c` built into a
library, with two transfer slots instead of four and a 16 KiB worker stack.
Its thread runs at priority 40 (libogc's main thread runs at 64) and at 48
during a transfer, and it frees its transfer buffers after each request.

`HBCS` from an app that links the overlay adds `wpad_handles`: whether the
agent found and checked wiiuse's per-remote handles (below). Every agent
reports its own cost: `agent_idle_wakes` and `agent_idle_us` (wake-ups to
check for a connection and their total time), `agent_request_ms` (time
spent answering), and `heap_arena` (newlib's heap size). After the overlay
was open, the agent and HBC report `overlay`: frames drawn, `avg_us` and
`max_us` per drawn frame, `bytes` borrowed, and `buffers` (`own`, `lent`,
or `app's` when it drew over the app's framebuffer).

### HOME overlay

HBC 1.7.0's own HOME menu is the overlay (`channel/channelapp/source/home.c`,
agent config `no_network`), so HBC also answers `HBCK` and `HBCP`: its
`devnet.c` hands them to `hbc_agent_handle()`. While the overlay is open,
HBC's main loop is paused, and the overlay's per-frame hook
(`loader_signal_threads`) keeps the loader thread accepting connections.

`hbc_agent_home()` (`sdk/hbc_agent/overlay.c`) copies the frame the VI is
showing, then draws its own frames in two framebuffers: the copy, dimmed,
with the strip and menus drawn by `ov_ui.c` and `ov_draw.c` in software
(YUYV, HBC's Droid fonts pre-rendered by `mkfont.py`). It never touches the
app's GX state, and hands the VI the app's framebuffer back when it closes.
Those two files are portable C, so `tests/overlay_preview` renders every
page on the PC and `tests/test_overlay_ui.py` walks them in CI.

LEDs, IR sensitivity and the sensor bar position need wiiuse's per-remote
handle, which libogc keeps in a static array. The agent finds it from
`WPAD_Rumble`'s code, which loads the array with one small-data load before
indexing it (`lwz rX,d(r13); slwi r3,r3,2; lwzx r3,rX,r3`), and uses it only
if every handle names its own channel; otherwise those settings are greyed
out. libogc re-applies the Wii's own sensor bar and IR settings whenever a
remote connects, so a small thread re-applies the session's, and holds
rumble off for remotes whose rumble is switched off, while any such setting
is in force. The overlay redraws only frames that differ from the last one drawn (a
signature of the laid-out items, the pointers and the remotes' state), and
draws opaque runs as 32-bit stores with no divides; on the bench Wii that
took a full frame from 19.1 ms to 7.9 ms on average. While it is open, IR is
on for every connected remote, mapped to the framebuffer; each remote gets
back its previous data format on close (left on if the handles are missing
and the format is unknown).

The overlay drives the remote's speaker itself, after Nintendo's SDK
(REVIEW.md 1.7.6):
- It puts the remote's link in sniff mode (5 ms) and runs the SDK's start-up
  sequence.
- It encodes each 20-byte report just before sending it, every 6.67 ms:
  4-bit ADPCM at 6 kHz (rate `0x07d0`), or 8-bit PCM at 3 kHz (rate `0x0fa0`).
- It skips a block unencoded when the Bluetooth controller already holds more
  than 3 packets.

Find plays only a chime. Changing a remote's volume in More plays a chirp at
the new level; volume 10 is `0x40`, and PCM gets twice the byte. The Test page
plays one tune in each format: press 1 for ADPCM or 2 for PCM, or point at a
button and press A.

In `status`, each remote's `spk` is `[sent, skipped, wrapped, worst gap in us,
fewest free ACL buffers, most, sniff]`. `wrapped` counts ticks where lwbt's
buffer count had gone below zero.

### Crash reports

The agent installs a libogc panic handler. On a fatal exception it writes a
120-byte block to MEM2 at `0x91800020`, then lets libogc show its crash
screen for 3 s and return through the reload stub. HBC takes the block in
`main()`, clears it, and reports it in `HBCS` until `HBCC`:

```json
"crash":{"app":"agent_app","exception":3,"name":"DSI","pc":"80004cac",
         "lr":"800047a8","msr":"00009032","cr":"20002494","ctr":"00000000",
         "dar":"00000010","dsisr":"42000000","sp":"8008b160",
         "uptime_ms":1002,"frames":["80004004"]}
```

`frames` are return addresses found by walking the stack's back chain; a
leaf function's caller is in `lr`. The block layout is `hbc_crash_block` in
`sdk/hbc_agent.h`: magic `HBCC`, version 1, the exception number
(`PPC_EXCPT_*`), `pc`, `msr`, `lr`, `cr`, `ctr`, `dar`, `dsisr`, `sp`,
`uptime_ms`, twelve frames, the app name (20 bytes), and a rotate-and-XOR
check word over the rest. `hbc.py crash --elf app.elf` adds function names
and lines with `powerpc-eabi-addr2line`.

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
