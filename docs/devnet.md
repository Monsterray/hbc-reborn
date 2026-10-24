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
| `HBCA` | name length (u16 at 4), flags (u16 at 6) | the name, UTF-8, up to 63 bytes | empty; the play log's name for the next Wiiload upload, kept 2 minutes (protocol 5). Flag 1 installs a ZIP upload without asking (protocol 6); the length may then be 0 |
| `HBCL` | zero | none | the kept log: `HBCL 1 <why> <uptime_ms> <app>\n` then the text; `ENOENT` when there is none (protocol 4) |
| `HBCX` | zero | none | empty; an [agent](#in-app-agent) app then exits to HBC (protocol 3) |
| `HBCH` | pages (u16 at 4: bit 0 System, 1 Video, 2 Storage, 3 USB, 4 Network; 0 for all) | none | JSON: the hardware and settings DEV > Info shows, `{"System":{"Console":"...",...},"Video":{...},"Storage":{...},"USB":{...},"Network":{...}}` (HBC 1.9.6 and its agent). In an app that never started libogc's USB, the USB page starts it and leaves it running. |

### Status

```json
{"version":"1.4.0","proto":2,"ios":58,"ios_revision":6175,"ahbprot":true,
 "arena1_free":4080,"arena2_free":50599776,"heap_free":116032,
 "tcp_stack_used":2824,"tcp_stack_size":8192,"init_ms":968,"scan_ms":300,
 "ip":"192.168.8.213","apps":14,"device":"sd","inserted":["sd"],
 "log":"192.168.8.147:4405",
 "last":{"op":"p","bytes":5328444,"wire":2663251,"ms":3674,
         "net_ms":1881,"disk_ms":3292,"cpu_ms":164}}
```

`proto` is 6 when `HBCA` takes flags and `upload` exists, 5 when `HBCA` exists, and 4 when `HBCL` and the crash fields
`kind`, `code` and `reason` exist. It is 2 when the framed ops below exist, and 3 when `HBCC`, the
`crash` field, and the in-app agent's `HBCX` exist. `device` is the mounted device
that file requests can use; `inserted` also lists devices that were present
at the last device poll. `last` describes the most recent file transfer:
bytes on the network, and time the Wii spent on Wi-Fi, SD, and zlib/CRC.
Network and SD overlap in framed transfers, so their sum can exceed `ms`.
`arena1_free` and `arena2_free` are the MEM1 and MEM2 that libogc's arena
still has unclaimed (`SYS_GetArena1Size()`, `SYS_GetArena2Size()`), not
free memory in an app's own heap: an app that built its heap from the
arena shows little here with most of its heap free. They were
`mem1_free` and `mem2_free` before 1.10.7. `heap_free` is newlib's free heap (its "used" figure would count the gap
between MEM1 and MEM2), `tcp_stack_used` is the loader thread's stack
high-water mark, `init_ms` is the time from HBC's `main()` to its menu, and
`scan_ms` is the last full app scan. `net_starts` counts the times the network came up since HBC started
(HOME > HBC > Restart network adds one). `transfer_style` is how this
start shows a file name too long for the on-screen transfer line:
`scroll`, `bounce` or `truncate`, picked at random; `HBCT` with the name in
header bytes 4-15 forces one until HBC restarts (`hbc.py transfer-style`). `startup` lists the startup steps as
`"name":ms`, the time since `main()` at the end of each, in the order they
finished; `playtime_cleared` (or `playtime_failed`) is the background write that
clears the Wii Menu's play record. `zlib_mem` says where the zlib arena landed
(see Performance), `zlib_peak` is the most of it one stream has used, and
`zlib_heap` the largest zlib allocation that did not fit and went to the heap
(0 if none). `tcp_last_failure` records what IOS returned
during the last receive that failed: `r<n>` per `net_read` result,
`p<events>/<result>` per poll, then the reason. `crash` is `null`, or the
crash an agent app reported before it returned to this HBC (see
[Crash reports](#crash-reports)).

`usb_drives` has one entry per USB slot (`usb`, then `usb2`): its drive's
`vid`, `pid`, `lun` and whether it runs at USB 2 speed, or `why` there is
none ("no mass storage device", "could not open it", "no 512-byte-sector
LUN", ...). `unmountable` maps a device that was there but held no volume HBC
mounts to what its first sector holds, for example `"usb":"GPT: 1 EFI 2
NTFS"`.

`app_list` is the app list as the menu last drew it: `sort` (`name`,
`date` or `custom`), `order` (folder names, as many as fit in 640 bytes),
`slides` (times it slid to a page: start, page turns, a new sort) and
`reloads` (times it was rebuilt in place after file requests changed apps).
File requests that touch an app folder, or `boot.dol`, `boot.elf`,
`meta.xml`, `icon.png` or `theme.zip` at its top, reload that app once
file requests under the apps folder have been quiet for a second, on the
page shown; files deeper in the folder (an app's data) reload nothing but
still hold a pending reload back, so a folder's upload reloads once.

`upload` is `null`, or what became of the last Wiiload (or USB Gecko)
upload: `seq` counts uploads from 1, `result` is `launched`, `installed` (a
ZIP app; `text` is its folder), `theme`, `declined` (a ZIP nobody agreed to)
or `error`, with `error` a code (`read`, `receive`, `uncompress`,
`out_of_memory`, `bad_zip`, `not_wii_app`, `extract`) and `text` the message
HBC showed; `ago_ms` is its age. `hbc.py send` and `run` read `seq` before
an upload and wait for the next result, so they fail at once with HBC's
reason instead of waiting.

Every popup an upload can raise (the load errors, the ZIP question, the
update question) closes itself after 10 s, showing the count on its last
button: OK, or No for a question. Any button press stops the count, for
someone reading it. So a bad upload never leaves HBC waiting for a person,
and an automated test carries on.

### Files

Paths are `<device>:/<path>` with device `sd`, `usb`, `usb2`, `carda` or
`cardb`. `usb` and `usb2` are the first two USB drives (HBC's own driver,
`usbmsd.c`, serves both at once); a request for `sd`, `usb` or `usb2` when it
is not the device the app list comes from mounts it then, and it stays
mounted. FAT and exFAT volumes mount, on an MBR or a GPT disk (on GPT, the
first partition that mounts, the EFI System Partition skipped). HBC
rejects paths containing `..`, `//`, a backslash, a second colon, or a control
character, and paths of 256 bytes or more.

| Op | Action | Reply payload |
| --- | --- | --- |
| `p` | framed upload of `size` bytes (at most 512 MiB) to `path.part`, then rename to `path`; parent directories are created | none |
| `g` | framed download; flags bit 0 allows zlib frames | reply header with the file size, then frames |
| `P` | protocol 1 upload of `size` raw bytes, as `p` without checks | none |
| `G` | protocol 1 download | the file |
| `L` | list a directory | `#list 2`, then a line per entry: `<d\|f> <size> <mtime> <flags> <name>` (below); a listing that reached 256 KiB ends with `! truncated` |
| `S` | one file's or directory's entry | the same line, for that path |
| `C` | checksum a file | u32 size, u32 CRC-32 |
| `D` | delete a file or an empty directory | none |
| `M` | create a directory and its parents | none |

An entry line's `mtime` is the time the entry last changed, in seconds since
1970 by the Wii's clock: FAT keeps local time with no zone, and HBCS `time`
(the Wii's clock now) is read the same way, so a client lines the two up by
their difference (`hbc.py` does). `flags` is `r` for a read-only entry, else
`-`; a directory's size is 0. HBC before 1.10.6 listed only `d <name>` and
`f <size> <name>`, with no version line; that format is gone. The FAT layers
under HBC and the agent report these times (libogc 3's FatFs, libogc2's
libfat) but none can set them, so an upload's time is when the Wii wrote it.

A put to a path ending in `/` is refused with `EISDIR`, and one larger than
the device's free space (plus a cluster: the upload is written beside the
file it replaces) with `ENOSPC`, both before any data is read; a write that
fails partway stops the receive at once. The reply then comes while the
client may still be sending: `hbc.py` checks for it before each frame, so
it reports the Wii's error rather than the reset that follows. After a put or delete
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
| `HBCK` | header bytes 4-5: a count (at most 64), then that many of `udlrabh12w`; queued as controller presses (`h` is HOME, `1` and `2` the remote's buttons, `w` Test's speaker.wav button, which apps check with `hbc_agent_home_pending()`) |
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
`max_us` per drawn frame (`dim_us` and `draw_us` its two parts, averaged:
the dimmed background and the menus), `loops` (every pass of its loop, one
per vertical blank when nothing changes), `loop_max_us` (the longest pass,
the wait for the VI included), `input_max_us` (the longest read of the
remotes and the PC's keys), `slow_loops` (passes over 1/30 s, which the user
sees as a stutter), `bytes` borrowed, and `buffers` (`own`, `lent`, or
`app's` when it drew over the app's framebuffer). Its animations move by
time, not by frame, so a slow frame never slows them.

### The agent and MEM2

While an app runs the agent writes nothing into its MEM2. HBC's records at
0x91800000-0x91801140 (the log target's copy, the crash block, the kept log)
are written only as the app stops: an `HBCN` sets the log target in low
memory, and the agent copies it to MEM2 on the way out, with the kept log.
So an app may build its heap over all of MEM2 with no gap for them. The
agent's lasting memory (its two threads' stacks, and the reload stub's copy
with `guard_reload_stub`: 22 KiB) comes from the top of the MEM2 arena at
`hbc_agent_init()`, as libogc takes WPAD's and the network's buffers, and
the agent's `HBCS` says where: `"agent_mem":{"addr","bytes","from"}`, from
`arena2_top`, or `heap` when the app had already taken the whole arena.
`tests/wii_agent_mem2.py` checks both on a Wii.

### Stopping the agent, and lending the port

An app that leaves other than through `exit()`, such as a loader that reloads
IOS and boots a game, a channel or another `.dol`, calls `hbc_agent_stop()`
first. It ends the agent's threads (the listener, and the monitor that runs
the hang watchdog), closes its sockets, and puts back every hook
`hbc_agent_init()` set: the vertical blank callback, the stack guard's
breakpoint, Reset and Power, the reset function, stdout and stderr, and the
crash handler. A hook the app has replaced since keeps the app's. After that
nothing of the agent's runs or points at its code, so the app may reload
IOS, overwrite memory or jump anywhere. The agent leaves storage and the
network as they are: the app decides what to unmount and what to shut down.
The stop returns within about a second. It returns `-ETIMEDOUT` only when an
agent thread was blocked in IOS; that thread does nothing more when it wakes.
`hbc_agent_init()` starts the agent again afterwards.

```c
#ifdef HBC_AGENT_HAS_STOP     /* SDKs from HBC 1.10.1 on */
    hbc_agent_stop();
#endif
    IOS_ReloadIOS(ios);
```

`hbc_agent_listen(false)` lends the Wiiload port (4299) to an app that runs
its own server for a while, such as a homebrew browser that receives `.dol`
and `.zip` uploads. The agent closes its listening socket, ends a transfer
in progress (its client gets an error), and starts no network until
`hbc_agent_listen(true)`. It returns once the port is free to bind. It is
the same switch as the overlay's DEV "hbc.py connection", which shows it.

`tests/wii_agent_stop.py` checks both on a Wii (agent_app's `stop`,
`restart`, `listen` and `stopcrash` modes).

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

The overlay drives the remote's speaker through
[`sdk/wiispk`](../sdk/wiispk/README.md), which follows Nintendo's SDK
(REVIEW.md 1.7.6 and 1.8.0):
- It puts the remote's link in sniff mode (5 ms) and runs the SDK's start-up
  sequence.
- It encodes each 20-byte report just before sending it, every 6.67 ms:
  4-bit ADPCM at 6 kHz (rate `0x07d0`), or 8-bit PCM at 3 kHz (rate `0x0fa0`).
- It skips a block unencoded when the Bluetooth controller already holds more
  than 3 packets.

Find plays only a chime. Changing a remote's volume in More plays a chirp at
the new level; volume 10 is `0x40`, and PCM gets twice the byte. The Test page
plays one tune in each format: press 1 for ADPCM or 2 for PCM, or point at a
button and press A. Its third button plays `sd:/speaker.wav` (or
`usb:/speaker.wav`): any PCM WAV, loaded in a thread, converted to 6 kHz
mono, up to 60 s.

In `status`, each remote's `spk` is `[sent, skipped, wrapped, worst gap in us,
fewest free ACL buffers, most, sniff]`. `wrapped` counts ticks where lwbt's
buffer count had gone below zero.

### Crash reports

The agent installs a libogc panic handler. On a fatal exception it writes a
196-byte block to MEM2 at `0x91800020`, then lets libogc show its crash
screen for 3 s and return through the reload stub. `hbc_agent_fatal()` and
the hang watchdog (`hbc_agent_alive()`, a thread at the highest priority)
write the same block for the other ways an app stops. HBC takes the block in
`main()`, clears it, and reports it in `HBCS` until `HBCC`:

```json
"crash":{"kind":"exception","code":0,"reason":"",
         "app":"agent_app","exception":3,"name":"DSI","pc":"80004cac",
         "lr":"800047a8","msr":"00009032","cr":"20002494","ctr":"00000000",
         "dar":"00000010","dsisr":"42000000","sp":"8008b160",
         "uptime_ms":1002,"frames":["80004004"],"way_out":"not_checked"}
```

`kind` is `exception`, `fatal` (the app's `hbc_agent_fatal()`, with its own
`code`, which HBC and `hbc.py` show but never interpret, and its `reason`),
`hang` (no `hbc_agent_alive()` for `hang_s` seconds; `pc`, `lr` and the
frames are the stuck thread's, as saved when the watchdog preempted it),
`assert` (`reason` is `file:line: expression`), `abort`, or `stack` (the
main thread ran into the stack guard: a DSI from the breakpoint, with
`dar` the guarded address and DSISR bit `0x00400000`, or the markers found
overwritten). An HBC before 1.9.6 names the last three `exception`. An
`exception` or `stack` crash with `agent crash hook faulted: pc ... dar ...`
in its `reason` is still the app's crash: the agent's hook faulted too, at
that address, after recording it, and libogc 3's crash screen shows the
app's crash (agents from 1.10.2). On libogc2 and 1.x a fault in the hook
goes straight to libogc's own crash screen, which shows that fault; the
block keeps the app's crash, without the note.
`frames` are return addresses found by walking the stack's back chain; a
leaf function's caller is in `lr`. The block layout is `hbc_crash_block` in
`sdk/hbc_agent.h`: magic `HBCC`, version 3, the exception number
(`PPC_EXCPT_*`), `pc`, `msr`, `lr`, `cr`, `ctr`, `dar`, `dsisr`, `sp`,
`uptime_ms`, twelve frames, the app name (20 bytes), `kind`, `code`, the
reason (64 bytes), `way_out` (below), and a rotate-and-XOR check word over
the rest. HBC still reads version 2 blocks (agents before 1.10.14, with
`way_out` `not_checked`) and version 1 blocks (agents before 1.9.0), as
exceptions. `hbc.py crash --elf app.elf` adds function names and lines with
`powerpc-eabi-addr2line`, then the last lines of the kept log.

### The way out

Every way an agent app stops ends in HBC's reload stub at `0x80001800`
(libogc's `__reload()`). The stub turns interrupts off, closes IOS
descriptors 0-31 and launches HBC through ES, waiting on each IPC with no
time limit. When IOS, or one module of it, has stopped answering, the stub
waits for good. On 2026-10-24 USB Loader GX's bench build hung IOS in its
first `WDVD_Read` through d2x cIOS 249 (FRAG mode, stock d2x and the fork
alike): neither the agent nor the app's own guard thread could return, and
the Wii needed a power cycle three times.

From 1.10.14, `hbc_agent_fatal_now()`, the hang watchdog, `assert()`,
`abort()` and the stack guard's marker check run the stub's own steps first
(`sdk/hbc_agent/way_out.c`): the same polled IPC with interrupts off, with
0.5 s for IOS's ack of each request and 1 s for its reply. A late reply to
a request that ran out of time is taken as it comes, since IOS acks and
replies only while the PPC has nothing of its waiting (Y1, Y2 and the IPC
interrupt clear; Dolphin's `WiiIPC::IsReady()`). What answered
decides, and `way_out` in the crash block (and `HBCS`, and `hbc.py crash`'s
`left` line) records it:

| `way_out` | When | What the agent does |
| --- | --- | --- |
| `stub` | every close answered | the reload stub, as before |
| `es_launch` | IOS acked, but a close got no reply (a stuck module), or no stub was found | opens `/dev/es`, reads HBC's ticket views and launches it, as the stub would but without the closes; IOS reloads and resets the PPC into HBC |
| `reset` | that failed, or IOS acked nothing, and AHBPROT is open | clears bit 0 (RSTBINB) of `HW_RESETS` (`0x0d800194`): a system reset that needs no IOS |
| `stm_reset` | AHBPROT closed, IOS still acks | `/dev/stm/immediate` ioctl `0x2001` (hot reset) |
| `stub_anyway` | nothing reset the Wii within 2-3 s | the reload stub anyway, which may wait for good |
| `not_checked` | an exception (interrupts are already off in libogc's crash screen), `hbc_agent_fatal()`'s `exit()`, `HBC_AGENT_NO_IOS_CHECK`, or an older agent | the reload stub |

The kept log has an `agent: way out:` line for each step, with the time the
closes took (a few ms on a healthy IOS) and the first descriptor that did
not answer. `hbc_agent_fatal()` exits through `exit()` as before, unless
IOS does not open `/dev/es` within 3 s: then it leaves the way
`hbc_agent_fatal_now()` does. `exit()` still waits on every device it
unmounts, so an app that may wedge one uses `hbc_agent_fatal_now()` or
`HBC_AGENT_NO_FLUSH`.

Sources for the hardware behaviour, and what is not proven:

- `HW_RESETS` bit 0 is "System reset. Set to zero to reboot system" ([WiiBrew,
  Hollywood Registers](https://wiibrew.org/wiki/Hardware/Hollywood_Registers));
  HBC's own `channel/channelapp/source/panic.c` (upstream since 2008) reboots
  this way, noting it needs AHBPROT. The PPC sees Hollywood's registers only
  with AHBPROT open (`0x0d800064` reads `0xffffffff`): an app with
  `<ahb_access/>` in its `meta.xml`, and an IOS reload that keeps it (USB
  Loader GX patches ES for that). `HBCS` `safety.way_out.hw_reset` says
  whether it is open now.
- The PI reset register (`0xcc003024`) is the GameCube's hot reset (libogc's
  `__reset`, `HW_DOL` only); on a Wii libogc resets through STM instead, and
  Dolphin ignores writes to it on Wii. The agent does not use it.
- STM's hot reset is "This will reset the Wii" ([WiiBrew,
  /dev/stm/immediate](https://wiibrew.org/wiki//dev/stm/immediate)); libogc's
  `STM_RebootSystem()` and `SYS_ResetSystem(SYS_HOTRESET)` use it. It needs
  IOS's STM module to answer.
- Either reset reboots the console through boot2 into the System Menu, or
  Priiloader where installed. Priiloader picks autoboot or its "return to"
  setting from the boot state in NAND, unless MEM1 holds its magic word
  ([Priiloader `main.cpp`](https://github.com/DacoTaco/priiloader/blob/master/src/priiloader/source/main.cpp),
  `CheckMagicWords()`): the agent writes `Abra` at `0x817feff0` first, which
  runs its autoboot. A Wii without Priiloader lands in the System Menu,
  where someone still has to start HBC: the resets are the last resort,
  after the stub and the ES launch, for when the Wii would otherwise stay
  frozen.
- Dolphin models none of this: AHBPROT always reads open, `HW_RESETS` bit 0
  and STM's hot reset do nothing, and its IPC always answers.
  `tests/dolphin_way_out.py` shows the decisions with the agent's test bits,
  and `tests/test_way_out.py` runs `way_out.c` against a model of the IPC
  registers. Neither shows how a real IOS wedges.

On the bench Wii (2026-10-24, `tests/wii_way_out.py`, HBC 1.10.9 installed,
the agent's test bits): healthy closes took 25 ms and left through the stub;
a close forced to get no reply launched HBC through ES, back in HBC in 19 s;
a failed launch with resets off fell back on the stub. One `HW_RESETS` reset
(IOS forced silent) rebooted the console with no IOS help: BootMii (boot2)
showed its menu for about 8 s and went on by itself, Priiloader autobooted
HBC, and HBC answered 33 s after the reset. The kept log at `0x91800100`
survived the reset with the agent's steps in it. Not yet shown: the crash
block across a reset (HBC 1.10.9 does not read version 3), a Wii without
Priiloader (a reset there ends in the System Menu), STM's hot reset, and a
real wedge.

### Safety tools

`HBCS` from an agent app has `safety` (`sdk/hbc_agent/safety.c`):

```json
"safety":{"stack_guard":"breakpoint","main_stack":{"size":131072,"deepest":2104},
          "reset":true,"power":true,"flush_at_exit":true,
          "way_out":{"ios_check":true,"hw_reset":true},
          "frames":{"fps":60,"flips":347,"retraces":862,"worst_ms":16,"late":0,
                    "single_buffer":false},
          "stub":null,"mem_low":null,"alloc_failures":null,
          "cost":{"checks":14,"check_us":153,"retraces":861,"retrace_us":14,"bytes":0}}
```

- `stack_guard`: `breakpoint` (the CPU's DABR watches the doubleword 1 KiB
  above the main stack's bottom, with marker words below it), `marker` (a
  debugger had the DABR), or `off`. `deepest` is the most of the main stack
  used so far (it starts zeroed).
- `reset` and `power`: whether the agent took the buttons (only from
  libogc's defaults). Reset exits to HBC like `HBCX`; Power runs `on_exit`
  and switches off. `flush_at_exit`: a libogc reset function unmounts SD and
  USB on `exit()` and `SYS_ResetSystem()`, after the app's `atexit` handlers.
- `way_out`: `ios_check` unless `HBC_AGENT_NO_IOS_CHECK`; `hw_reset` when
  AHBPROT is open now, so a wedged IOS can be left by a system reset (see
  [The way out](#the-way-out)).
- `frames`: framebuffer flips counted at each vertical blank. `fps` is the
  last second's; `worst_ms` the longest frame and `late` the frames half again
  as long as the average, neither counting time the overlay or a
  `hbc_agent_hold()` paused; `single_buffer` when nothing ever flipped.
- `stub` and `mem_low`: the opt-ins (`guard_reload_stub`, `track_memory`),
  else `null`. `changed_at_ms` is when the reload stub's 6 KiB were first
  found changed (0: never); they are put back on `exit()`, on
  `SYS_ResetSystem()` and on every crash path. `mem_low` holds the lowest free
  MEM1 and MEM2 arena and newlib heap seen, also printed at the end of the
  kept log.
- `alloc_failures`: with `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign`,
  the failed allocations from the app's code: how many, the last size and
  caller; the first four are also printed.
- `cost`: the once-a-second checks and the per-frame callback (count and
  total microseconds), and the bytes the tools hold (the stub copy, the
  monitor thread's stack if they started it).

### Kept log

As an agent app stops (an exception, a fatal, a hang, or `exit()` and a
return from `main()`), the agent copies the last 4 KiB of its stdout and
stderr to MEM2 at `0x91800100` (`hbc_lastlog_block`: magic `HBCL`, why it
was kept, length, uptime, app name, text, check word). It is written only
then, because the app's own MEM2 may cover that address while it runs. HBC
takes it in `main()`, clears it, and serves it with `HBCL`; `HBCS` says
`"lastlog":{"why":"fatal","bytes":812}` or `"lastlog":null`; `why` is
`exit`, `exception`, `fatal`, `hang`, `abort` (assert or abort), `stack` or
`power`. After a crash, fatal, hang or abort the text ends with every
thread: its priority (libogc's scale, 127 highest), state, stack pointer,
the bytes of stack it had left (the main thread, and libogc2's threads), and
the return addresses up its stack chain, nearest first (a waiting thread's
first few are libogc's own wait). A running
agent answers `HBCL` with its output so far, `why` being `live`.
`hbc.py lastlog` prints it. A death with no way out (a freeze with
interrupts off, the power switch) leaves no kept log.

## Workflow

```sh
export HBC_WII=192.168.1.50
python3 tools/hbc.py status
python3 tools/hbc.py run myapp.dol arg1 arg2   # prints the app's output
python3 tools/hbc.py put build/data.bin sd:/apps/myapp/data.bin
python3 tools/hbc.py ls sd:/apps/myapp
```

`run` needs inbound TCP on the log port (4405 by default, `--log-port`) to be
allowed through the PC's firewall. Inside a bench queue job the address and
the port come from the job: `$HBC_WII` (the Wii it got) and
`$WII_BENCH_LOG_PORT` (4300 for `dev`, 4301 for `lrwii`, 4302 for `prod`,
inside the 4300-4315 range the bench firewall rule allows), so a queued
command needs neither `--wii` nor `--log-port`.
