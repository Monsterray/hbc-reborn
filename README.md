# The Homebrew Channel (reborn)

[![CI](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml/badge.svg)](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml)

A maintained fork of [The Homebrew Channel](https://github.com/fail0verflow/hbc)
for the Nintendo Wii. It builds with current devkitPro on Windows, Linux, and
macOS, warning-free, and adds a **developer network**: from a PC on the same
LAN you can query the Wii, move files to and from its SD card, launch apps,
and stream their `printf` output back, with checksummed and compressed
transfers.

Current release: **1.12.1**. Title ID `00010001-4F484243` (`OHBC`), so the
channel installs next to the official Homebrew Channel (`LULZ`) instead of
replacing it.

| Part | Where |
| --- | --- |
| Channel app, loader, reload stub | [`channel/channelapp`](channel/channelapp) |
| Developer network (Wii side) | [`channel/channelapp/source/devnet.c`](channel/channelapp/source/devnet.c), [`devstream.c`](channel/channelapp/source/devstream.c) |
| PC client for the developer network | [`tools/hbc.py`](tools/hbc.py) |
| App-side network log header | [`sdk/hbc_netlog.h`](sdk/hbc_netlog.h) |
| In-app agent: the tools and crash reports inside a running app | [`sdk/hbc_agent.h`](sdk/hbc_agent.h), [`sdk/hbc_agent`](sdk/hbc_agent) |
| Wii Message Board: posts, mail to another Wii, play history (HBC's play log too) | [`sdk/mboard`](sdk/mboard) |
| Miis: the data, the Mii database, a Wii Remote's Miis | [`sdk/mii`](sdk/mii) |
| Wii Remote speaker driver | [`sdk/wiispk`](sdk/wiispk) |
| Other controllers: Wii U Pro, GameCube adapter for Wii U, USB keyboard and mouse | [`sdk/wiiinput`](sdk/wiiinput) |
| Banner, WAD packaging, PyWii | [`channel/banner`](channel/banner), [`channel/title`](channel/title), [`pywii`](pywii) |
| WiiPAX executable packer, host `wiiload` | [`wiipax`](wiipax), [`channel/wiiload`](channel/wiiload) |
| Protocol reference | [`docs/devnet.md`](docs/devnet.md) |
| Message Board: the library, the board's formats | [`docs/mboard.md`](docs/mboard.md), [`docs/messageboard.md`](docs/messageboard.md), [`docs/messageboard-messages.md`](docs/messageboard-messages.md) |
| Miis: the data, `RFL_DB.dat`, the hidden database, a Remote's Miis, the library | [`docs/mii.md`](docs/mii.md) |
| Review notes and hardware results | [`REVIEW.md`](REVIEW.md) |

Besides Wii Remotes (with a Nunchuk, Classic Controller or guitar) and
GameCube controllers in any of the four ports, HBC can be driven with a
Wii U Pro Controller, the GameCube controller adapter for Wii U, a USB
keyboard, or a USB mouse as the pointer; see [`sdk/wiiinput`](sdk/wiiinput).

This code differs from the official HBC build, which adds protection
features, and comes with no warranty. The installer is not included.

## Quick start: develop on a real Wii

You need a Wii running any Homebrew Channel, on the same LAN as your PC, and
Python 3.10+ on the PC. No other PC software is needed: `hbc.py` speaks
Wiiload itself.

```sh
export HBC_WII=192.168.1.50        # your Wii's IP (PowerShell: $env:HBC_WII = "...")

# 1. Run this HBC build on the Wii without installing anything
python3 tools/hbc.py send channel/channelapp/channelapp-channel.dol
python3 tools/hbc.py wait          # prints the version once it is up

# 2. Develop against it
python3 tools/hbc.py status                        # JSON: version, IOS, AHBPROT, memory, SD
python3 tools/hbc.py run build/myapp.dol arg1      # send an app and print its output
python3 tools/hbc.py put assets.bin sd:/apps/myapp/assets.bin
python3 tools/hbc.py ls sd:/apps/myapp
```

Sending the DOL runs it from RAM: nothing is written to NAND, and a power
cycle returns the Wii to its previous state. The developer commands need this
fork (1.2.0 or later), either sent as a DOL like this or installed as the WAD.

## `hbc.py` reference

`python3 tools/hbc.py` on its own prints an overview of every command, and
`hbc.py help COMMAND` explains one with examples. The table below is the
same list.

```
python3 tools/hbc.py [--wii ADDR] [--json] [--log-port PORT] [--timeout S] COMMAND ...
```

| Command | What it does |
| --- | --- |
| `version` | Print the running HBC version (`HBCV`). |
| `status` | Print JSON status: version, protocol, IOS and revision, AHBPROT, free memory, loader stack use, startup and app-scan time, IP, app count, mounted device, log target, and timing of the last transfer. |
| `hw` | The Wii's hardware and settings, as DEV > Info shows them (`HBCH`): console, serial, console ID, Wii Menu, IOS, boot2, CPU; video mode, cable, TV settings; SD, USB, NAND and memory cards; USB devices; IP, gateway, DNS, MAC, the connection in use. |
| `wait [SECONDS]` | Wait until HBC answers (default 90 s). |
| `send FILE [ARG ...]` | Send a DOL, ELF, or ZIP over Wiiload. A ZIP is installed to the SD card after you confirm on the Wii, or at once with `--yes`. Fails with HBC's reason when the Wii cannot load it. |
| `run FILE [ARG ...]` | Register for logs, send `FILE`, and print its output until it exits (`--timeout`, default 300 s). Fails at once with HBC's reason when the Wii cannot load it. |
| `log` | Register for logs and print app output until Ctrl+C. Use it when you launch apps from the Wii itself. |
| `ls REMOTE` | List a directory, e.g. `sd:/apps`. Lines are `d DATE TIME name` or `f size DATE TIME name` (when it last changed, by the Wii's clock), then the FAT attributes set: `r` read-only, `h` hidden, `s` system, `a` archive. |
| `stat REMOTE` | One file or directory: size, when it last changed, its attributes. |
| `attrib [--recursive] [+r\|-r] [+h\|-h] [+s\|-s] [+a\|-a] REMOTE...` | Show or change FAT attributes, as Windows' `attrib` does. A read-only file cannot be written over or deleted, nor a read-only folder deleted, until `-r`. HBC 1.10.15 and its agent. |
| `get [-r] REMOTE [LOCAL]` | Download a file, or with `-r` a directory tree. |
| `put [-r] LOCAL REMOTE` | Upload a file or, with `-r`, a directory tree; parent directories are created. A `REMOTE` ending in `/` gets the file's name appended. |
| `sync [--delete] [--checksum] LOCALDIR REMOTEDIR` | Make `REMOTEDIR` match `LOCALDIR`, uploading only what changed: a same-size file the Wii wrote after the local one last changed is skipped without being read; otherwise the CRC-32s decide (`--checksum` uses them for every same-size file). `--delete` also removes remote extras; a file that fails is reported and the rest go on. |
| `rm [-r] [--force] REMOTE` | Delete a file or an empty directory, or with `-r` a tree. Device roots and `<device>:/apps` itself are refused, and so is a read-only entry unless `--force`, which clears read-only first (as `del /f` does). |
| `mkdir REMOTE` | Create a directory and its parents. |
| `mv [--replace] FROM TO` | Rename or move a file or folder (also `rename`). A `TO` ending in `/`, or a folder that is there, means "into that folder". On one device the Wii renames in place, at once; between devices (`sd:` to `usb:`) each file is copied, checked by CRC-32, and only then deleted (files up to 512 MiB; a read-only source is refused first; the copies get new times). An existing `TO` is refused unless `--replace` (a file only). HBC 1.10.14 and its agent. |
| `mii-faces` | The same as HOME > HBC > Save Mii faces (`HBCM`): HBC copies the Wii Menu's Mii face models (`RFL_Res.dat`) and the Mii Channel's bodies (`maleBody.brres`, `femaleBody.brres`) and effects (`effect_nigaoe.breff`, `.breft`) to `sd:/mii`, for apps that draw Miis. The files are Nintendo's: keep them on your own SD card, never ship them. HBC 1.10.20 (bodies 1.10.26, effects 1.12.2). |
| `transfer-style [scroll\|bounce\|truncate]` | Force how HBC shows a file name too long for its on-screen transfer line, until it restarts (for testing); with no name, print the current one. |
| `order [--clear] APPS [FOLDER ...]` | HBC's Custom app order (see [Your own app order](#your-own-app-order)): with only `APPS` (e.g. `sd:/apps`), print the apps in that order with their `<sort_id>`; with folders, give them 10, 20, 30... in the order named, editing or making each `meta.xml`; `--clear` removes every `<sort_id>`. |
| `exit` | Ask the running [agent](#keeping-the-tools-inside-your-app) app to exit to HBC, and wait for HBC. |
| `key KEYS` | Send controller presses to HBC's HOME menu or the running agent app: `h` (HOME, opens or closes its overlay), `u` `d` `l` `r` (D-pad), `a`, `b`. For scripted tests and for driving the overlay from the PC. |
| `screen FILE.png` | Save what the TV shows, from HBC or a running agent app, overlay included. |
| `crash [--elf FILE] [--clear]` | Print the crash an agent app reported: exception, registers, backtrace, and with `--elf` function names and source lines (needs devkitPPC's `addr2line`). `--clear` forgets it. |

`--json` makes `version`, `status`, `ls`, and `crash` print JSON. Options can follow
the command; for `send` and `run`, everything after the file goes to the app,
and `--` ends option parsing. Large transfers show progress on a terminal.

The Wii address comes from `--wii`, then `$HBC_WII`, `$WII_BENCH_IP`, or
`$WIILOAD` (`tcp:ADDR`, the variable the devkitPro `wiiload` uses).

Remote paths are `<device>:/<path>` with device `sd`, `usb`, `usb2` (a
second USB drive), `carda`, or `cardb`; paths containing `..`, `//`, or backslashes are refused. An app you
`put` or `sync` under `sd:/apps/` appears in the menu right away, and one you
remove disappears. The menu reloads once the writes stop (a second of
quiet), on the page it shows, so a whole folder's upload is one quiet reload.
While a file moves, HBC shows it at the bottom right of the screen
("Receiving boot.dol 42%", "Sending save.dat"). A name too long for the
line scrolls round, bounces from end to end, or ends in "..." (each HBC
start picks one at random; `hbc.py transfer-style bounce` forces one for
testing). A moving name rests a second at its start, and a bouncing one at
each end; "Receiving" and the percentage stay put.

Uploads and downloads use 64 KiB frames, each with a CRC-32 checked on both
ends, compressed with zlib when that makes them smaller. A corrupted frame
fails the transfer and leaves no partial file. On an 802.11g Wii a typical
ELF moves at about 1.5 MB/s up and 0.9 MB/s down, and compressible data at
4 to 5 MB/s; incompressible data is limited by the Wii's network stack to
about 1.35 MB/s in and 0.6 MB/s out. [docs/devnet.md](docs/devnet.md) has the
measurements.

### Seeing your app's output

Copy [`sdk/hbc_netlog.h`](sdk/hbc_netlog.h) into your project (it is public
domain and needs only libogc) and call `hbc_netlog_init()` once, after any
video or console setup:

```c
#include "hbc_netlog.h"

int main(int argc, char **argv) {
    // ... VIDEO_Init, console setup, etc. ...
    hbc_netlog_init();                   // 0 when connected; harmless otherwise
    printf("hello from the Wii, argc=%d\n", argc);
    return 0;                            // the log closes at exit()
}
```

Then run it from the PC:

```sh
python3 tools/hbc.py run myapp.dol level1
python3 tools/hbc.py run myapp.dol -- --verbose   # "--" before app arguments that start with "-"
```

`stdout` and `stderr` still reach any on-screen console you set up first.
When you start apps from the Wii's own menu instead, keep
`python3 tools/hbc.py log` running on the PC: HBC remembers the PC's address
for the apps it launches, also after one returns to the installed channel. Call `hbc_netlog_close()` if your app leaves
without `exit()`. [`tests/netlog_app`](tests/netlog_app) is a complete
example.

The PC must accept inbound TCP on the log port: 4405 by default, or pick one
with `--log-port`. On Windows, allow it once in Windows Defender Firewall.

### A typical loop

```sh
make && python3 tools/hbc.py run myapp.dol          # build, run, watch the log
python3 tools/hbc.py sync dist/myapp sd:/apps/myapp # install: only changed files move
python3 tools/hbc.py put data/level1.bin sd:/apps/myapp/level1.bin
python3 tools/hbc.py get sd:/apps/myapp/save.dat    # pull a file the app wrote
python3 tools/hbc.py mv sd:/apps/myapp sd:/apps/myapp-old   # rename, at once
python3 tools/hbc.py status                         # IOS, AHBPROT, memory after a run
```

To install an app permanently, `sync` or `put -r` its folder (with
`boot.dol` and `meta.xml`) to `sd:/apps/<name>/`, or `send` a ZIP.

`hbc_netlog_init()` gives up within about 5 s when the PC is unreachable, and
`run` and `log` clear HBC's log target when they exit, so a stale target
never stalls later apps.

### Your own app order

Options > Sort applications by: **Custom** lists apps by the `<sort_id>`
number in their `meta.xml`, lowest first. Apps without one follow, by name,
and apps with the same number go by name. Any whole number works, negative
too:

```xml
<app version="1">
  <name>WiiXplorer</name>
  <sort_id>10</sort_id>
  ...
</app>
```

Edit the files on the card, or let `hbc.py order` do it from the PC:

```sh
python3 tools/hbc.py order sd:/apps                                # the order now
python3 tools/hbc.py order sd:/apps wiixplorer usbloader_gx nintendont
```

The second gives those three 10, 20 and 30 and leaves the rest as they are;
steps of 10 leave room to put an app between two others by hand. An app's
update that brings its own `meta.xml` drops its number, so run `order`
again afterwards.

### Keeping the tools inside your app

Link the agent into a development build and `hbc.py` keeps working while
your app runs: `status`, the file commands, and `log` reach the app instead
of HBC, `run` and `send` ask it to exit to HBC before sending the next
build, and a crash comes back as a report instead of a frozen screen.

```sh
make -C sdk/hbc_agent            # builds sdk/hbc_agent/libhbcagent.a
```

```c
#include <fat.h>
#include "hbc_netlog.h"
#include "hbc_agent.h"

int main(int argc, char **argv) {
    // ... video setup ...
    fatInitDefault();                 // mount first; the agent serves what the app mounted
    hbc_netlog_init();
    hbc_agent_init(NULL);             // one low-priority thread, about 20 KiB while idle
    while (!hbc_agent_exit_requested()) {
        // ... one frame ...
    }
    return 0;                         // back to HBC
}
```

Link with `-I<hbc-reborn>/sdk -L<hbc-reborn>/sdk/hbc_agent -lhbcagent -lfat -lz -logc`.

That is for devkitPro's current libogc (3.x). For an app on libogc2
(Extrems' fork) or libogc 1.x, build the agent against that libogc and link
the copy it makes, with the devkitPPC the app uses:

```sh
make -C sdk/hbc_agent OGC=libogc2        # sdk/hbc_agent/libogc2/libhbcagent.a
make -C sdk/hbc_agent OGC=libogc-1.8.23  # sdk/hbc_agent/libogc-1.8.23/libhbcagent.a
```

Everything works the same there, crash reports included (the agent adds its
own exception entry, `sdk/hbc_agent/ogc_exc.S`, to the older libogc's
exception table). The one difference: those libogcs' WPAD has no pairing
call, so the overlay's Connect asks for the Wii's SYNC button.

#### Safety tools

The agent also guards the app, at no cost until something goes wrong:

- **Stack overflow.** The CPU's data breakpoint watches the main thread's
  stack 1 KiB above its bottom; the first write there stops the app with a
  `stack` report at the exact instruction. Markers below it catch a frame
  big enough to step over it.
- **`assert()` and `abort()`** come back as reports (`assert` with
  `file:line: expression`) instead of a frozen screen.
- **Reset and Power**, when the app left libogc's defaults: Reset exits to
  HBC, Power switches off after the app's `on_exit`.
- **SD and USB are written back** when the app exits, after its own
  `atexit` handlers, so a quick exit does not lose a save.
- **Frame pacing** in `hbc.py status` (`safety.frames`): frames a second,
  the longest frame, frames that ran late.
- **Every thread's state** at the end of the kept log after a crash, fatal,
  hang or abort: priority, state, stack left and where it is waiting.

Two cost a little time each second, so an app asks for them:
`guard_reload_stub` keeps a copy of the loader's reload stub and puts it back
before exit (an app that overwrites low memory still gets back to HBC), and
`track_memory` records the lowest free MEM1, MEM2 and heap. Linking with
`-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign` also counts
failed allocations with their size and caller. `no_safety` leaves out any of
the defaults; `docs/devnet.md` has the details and `tests/agent_cost.py`
measures what they cost.

Then:

```sh
python3 tools/hbc.py run myapp.dol        # again and again: each run replaces the last
python3 tools/hbc.py status               # the app's name, uptime, memory, agent stack
python3 tools/hbc.py get sd:/apps/myapp/save.dat   # while the app runs
python3 tools/hbc.py crash --elf myapp.elf         # after a crash: where it happened
```

#### The HOME overlay

Call `hbc_agent_home()` when HOME is pressed, and the agent shows its
overlay over your game's last frame, in the Homebrew Channel's style:

```c
WPAD_ScanPads();
if ((WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) || hbc_agent_home_pending())
    hbc_agent_home(rmode);            // returns when the user closes it
```

On a GameCube controller, HBC opens HOME with Z and START together, so
START alone stays a game's pause button; do the same with
`(PAD_ButtonsDown(0) & PAD_BUTTON_START) && (PAD_ButtonsHeld(0) & PAD_TRIGGER_Z)`
and set `gc_pads` so the overlay reads the controller too.

A status strip shows the app, the clock, and the four remotes' player LEDs
and batteries, over five buttons:

| Button | What it does |
| --- | --- |
| DEV | Actions: Restart app, Pause, Save, Log (the app's recent output), Reset remotes, Sync clock (sets the Wii's clock from an NTP server, keeping its time zone), Log to PC, the crash screen's time, and the `hbc.py` connection. Info: time, play time, network, SD space, the app, and MEM1 and MEM2 free, used and total in KB, then five pages read when one opens: System (console and model, region, serial, console ID, Wii Menu, IOS and AHBPROT, boot2, CPU, Hollywood revision, language), Video (mode, cable, TV type, 16:9, 480p, PAL 60, sound, sensor bar, paired remotes), Storage (SD and USB size and free space, NAND free, GameCube memory cards), USB (each device on the ports, by vendor and product ID) and Network (IP and netmask, gateway, DNS, MAC, the Wii Menu connection in use). |
| (app slot) | Whatever the app puts there with `hbc_agent_set_slot(0, ...)`; blank otherwise. |
| Exit | The Homebrew Channel, System Menu, Restart Wii, Power off. |
| Shot | Saves the game's frame to `sd:/screenshots/<app>-NNN.bmp`. An app can replace it with `hbc_agent_set_slot(1, ...)`. |
| WiiMote | A card per remote (Find: a chime that gets louder; More: battery, extension, MotionPlus, rumble on or off, speaker volume with a chirp, Test with an ADPCM and a PCM tune and a WAV player for `speaker.wav` on SD or USB, Calibrate, Disconnect; Test and Calibrate switch a MotionPlus on to show and measure its gyro, and switch it back off as they close, so the app gets the remote back as it left it), and Settings: connect a remote, disconnect all, sensor bar, IR sensitivity, auto power-off, rumble for all. |

An app can give a slot a menu of its own instead of one action
(`hbc_agent_set_slot_menu`), with buttons and live info rows. HBC's own HOME
menu is this overlay, set up in
[`channel/channelapp/source/home.c`](channel/channelapp/source/home.c): a
commented example of every step, from the config to lending the overlay its
framebuffers and handling Exit through HBC's own shutdown. Its HBC slot
holds what the old HOME menu had: HBC's and IOS's versions, the Wii's IP,
About, Launch BootMii, Exit to System Menu, and Shutdown, plus Restart
network, which shuts the network down and joins it again (for a Wi-Fi
connection that went stale), and Save Mii faces, which copies the Wii
Menu's Mii face models (`RFL_Res.dat`) and the Mii Channel's male and
female bodies with their animations (`maleBody.brres`,
`femaleBody.brres`) and effects (`effect_nigaoe.breff`, `.breft`) to
`sd:/mii` for apps that draw Miis
([docs/mii.md](docs/mii.md#the-face-models-rfl_resdat)).

Point a Wii Remote at the screen and HBC's hand cursor follows it, turning as you twist the remote: the
button under it lights up and A presses it, and pointing at a bar button
while a menu is open switches straight to that menu. The overlay turns on
IR for every connected remote while it is open and gives each back its
previous mode when it closes. The D-pad moves the highlight when no remote
points at the screen, A chooses, B goes back one level, and HOME closes
everything. Controller settings last until the app exits; nothing is
written to the Wii's own settings, except that Connect remote may save the
new remote in the Wii's pairing list as the Sync button would, and Reconnect
in player order saves the new order. Reconnect works like the Wii Menu's:
every remote disconnects, then each one you press 1 and 2 on (any button, for
a paired one) becomes the next player, 1 to 4; Done, a fourth remote, or
closing the overlay ends it (libogc 3 builds only). A GameCube controller's
START closes the overlay, like HOME. Link the
overlay with `-lwiiuse -lbte` (which WPAD apps already use); while open it
borrows three framebuffers, about 1.8 MB at 640x480. Set `on_save` and
`on_restart` in `hbc_agent_config` to enable Save and Restart app.

#### What the agent costs

Measured on a real Wii with `tests/agent_cost.py` (HBC 1.7.1, 640x480):

| | Cost |
| --- | --- |
| Code | 25.6 KB for the agent (network, files, crash reports), 77 KB more for the overlay (of which about 30 KB are its fonts and pointer) |
| Static data | 17 KB for the agent (half of it the Log page's 8 KB of recent output), 21 KB for the overlay (its threads' stacks and state) |
| Start-up | `hbc_agent_init()`: 13 KB of heap and 16 KB of MEM1 arena (its thread's 12 KB stack, of which it uses 3 to 4.5 KB) |
| Idle | 5 wake-ups a second to check for a connection, 140 µs each: 0.07% of the CPU |
| File transfers | about 28 KB of heap kept after the first (the rest is freed after each); zlib and CRC work of 9 to 130 ms per upload and 50 to 620 ms per download of 2 to 5 MB, on the agent's thread |
| HOME overlay, while open | 1.8 MB: a copy of the frame and two framebuffers (HBC lends the framebuffers from low MEM1); 7.9 ms average and 14 ms at most to draw a frame that changed, nothing for frames that did not, so it keeps 60 fps |
| HOME overlay, closed | nothing: no thread, no memory |

`hbc.py status` shows the live figures: `agent_idle_wakes`, `agent_idle_us`,
`agent_request_ms`, `agent_stack_used`, `heap_arena`, and `overlay` (frames
drawn, average and worst frame time, memory borrowed) after the overlay
was open.

The agent's thread runs below your main thread, so a loop that waits for
each frame never loses time to it; an app that never blocks starves it and
the tools time out. It holds no transfer buffers while idle and frees them
after each transfer (about 280 KiB, plus up to 260 KiB of zlib state for a
compressed download). If your app calls `net_init()` itself, do it before
`hbc_agent_init()`: libogc's `net_init()` hangs if it runs while another
thread is starting the network. After a crash the agent shows libogc's crash
screen for 3 s and returns to HBC, which keeps the report until you read or
clear it; `run` prints it and exits with status 3. A plain `wiiload` to a
running agent app fails once and leaves the Wii in HBC, so the retry works.
[`sdk/hbc_agent.h`](sdk/hbc_agent.h) has the options (priority, crash
screen time, exit callback) and [`tests/agent_app`](tests/agent_app) is a
complete example. The agent contains HBC's own code, so it is GPL like the
rest of HBC: keep it in development builds (for example behind an
`#ifdef`), or release your app under a compatible license.

### Wii Remote speaker audio in your app

[`sdk/wiispk`](sdk/wiispk) is the speaker driver HBC's HOME menu uses, as a
library of its own. It is two files that need only libogc, under the zlib
licence (not GPL), so any app can copy them in. It plays sound on the remote's
speaker cleanly, where libogc's `WPAD_SendStreamData` stutters and turns to
noise, and it loads any PCM WAV file. [Its README](sdk/wiispk/README.md) has
the API and explains what libogc gets wrong.

### Posting to the Wii Message Board from your app

[`sdk/mboard`](sdk/mboard) is two files that need only libogc, in the public
domain, so any app can copy them in. They put a message on the Wii Message
Board the way a game does, or add a line to the day's play history:

```c
#include "mboard.h"

mboard_post("WiiStation", "Played Final Fantasy VII\nfor 42 minutes.");
mboard_log_play("Final Fantasy VII", "WSFF7", 42 * 60);   // needs AHBPROT
```

Beyond a simple post:

- **`mboard_post_msg()`** adds what a game's message can carry:
  - a Mii as its icon (`mboard_mii()` reads one from the Wii's Mii database);
  - a photo;
  - stationery (`tools/msgboard/stationery.py` makes a border, a board icon
    and music);
  - a button that opens a channel;
  - a Reply button;
  - a delay, and the date it is filed under.
- **`mboard_send_msg()`** sends the same message to another Wii by its Wii
  number, through the WiiConnect24 mail server (WiiLink's today).
- **`mboard_friends()`** and `mboard_friend_add`, `_set` and `_remove` read
  and edit the Address Book.

Only sending needs the internet, and nothing deletes what is already on the
board. HBC's own play log is this library's writer.

Docs:

- [`docs/mboard.md`](docs/mboard.md): the API, the files and IOS calls, what
  can go wrong, and the proof on a Wii.
- [`docs/messageboard-messages.md`](docs/messageboard-messages.md): how the
  board stores a message and what sets the sender, icon and contents.
- [`docs/messageboard.md`](docs/messageboard.md): the play history.
- [`docs/mii.md`](docs/mii.md): Mii data.

### Miis in your app

[`sdk/mii`](sdk/mii) is two files, in the public domain. They give an app
what the Wii's own Mii library gives games. Compile `mii.c` as C and link
with `-lwiiuse -lbte -logc -lm`.

```c
#include <gccore.h>
#include <ogc/lwp_watchdog.h>   // gettime()
#include "mii.h"

mii_db db = { 0 };
mii m;
uint32_t seed = (uint32_t) gettime();

mii_random(&m, MII_ANY, MII_ANY, &seed);   // or mii_default(), or edit any field
mii_set_name(m.name, "Homebrew");
m.height = 90;                             // 0 shortest .. 128 tallest
if (mii_make_id(&m) == MII_OK && mii_db_load(&db) == MII_OK) {
    if (mii_db_add(&db, &m) >= 0)          // into the Mii Channel's plaza
        mii_db_save(&db);
    mii_db_free(&db);
}
```

- **A Mii as a struct**, packed to and from the 74 bytes every Wii file
  holds. `mii_check()` says whether the Wii would accept it, and why not.
- **`.mii` files**: the 74-byte form and the 76-byte form with a CRC.
- **The Mii database**: the Mii Channel's 100 Miis, and the hidden database
  of Miis met on letters and Remotes. It reads, adds, changes and removes
  them. It never writes a database whose CRC is wrong.
- **A Wii Remote's 10 Miis**: `mii_remote_read()` and `mii_remote_write()`.

[Its README](sdk/mii/README.md) has the API, worked examples and what each
error means. [`sdk/mii/example`](sdk/mii/example) is a whole program, and
[`docs/mii.md`](docs/mii.md) has the formats and their sources.

`miidraw.c` and `miidraw.h` draw a Mii with GX: the head as the Wii draws
it, from the user's own face models (`sd:/mii/RFL_Res.dat`, saved with HOME
> HBC > Save Mii faces), all seven expressions, and a body, posed. Capture
it into a texture with alpha, make a head icon, or draw it in your own 3D
scene ([the SDK README](sdk/mii/README.md#drawing-miis-miidraw)). HBC's
avatar is the worked example: your favourite Mii stands beside the app
list, breathes, looks around, blinks, smiles and waves, looks at the Wii
Remote's pointer, and A on it shows the next Mii
([`avatar.c`](channel/channelapp/source/avatar.c)).

## Installing the channel on a Wii

Run the DOL over Wiiload first (above). Install the WAD only after that works,
and only with a **BootMii NAND backup** and **Priiloader** in place. The WAD is
fakesigned, so install it with a WAD manager running on an IOS that accepts
fakesigned titles. Start the installed channel from the Wii Menu; apps exit
back to it.

Build `channel/title/channel_retail.wad` as described below. Do not install a
WAD built before 1.1.9: those carry a corrupted Wii Menu icon layout. The 1.3.0
WAD is installed on the project's dev Wii and passes the developer-network
suite there, including apps exiting back to it.

### Replacing the original Homebrew Channel

The default WAD installs as its own channel (`OHBC`), next to the original
Homebrew Channel (`LULZ`). To have only one, build the WAD with the original's
title ID and install it over the original:

```sh
make -C channel TITLE=LULZ PYTHON="$(pwd)/.venv/bin/python"
```

- **What changes:** the channel, its ticket and its TMD carry `00010001-4C554C5A`.
  The ticket's title key is re-encrypted for the new ID (`pywii-tools/retitle.py`).
  Apps that exit to the Homebrew Channel by title ID then come back to this
  one.
- **Version:** its TMD version (`major << 11 | ...`, 0x0900 for 1.8.0) is higher
  than the original's, so WAD managers treat it as an update.
- **Settings:** HBC keeps `settings.xml` in the title's NAND data folder, as
  the original does, so this build should pick up the original's. That is not
  yet checked on a Wii; at worst the defaults apply.
- **The OHBC channel:** once the LULZ build runs, delete the separate OHBC
  channel from Wii Settings > Data Management > Channels.
- **Undoing it:** reinstall the original Homebrew Channel with its own installer.

## Building

Every platform uses devkitPro (devkitPPC r50-1, libogc 3.1.0), GNU make, and a
POSIX shell. The tree has no symlinks and keeps LF endings on shell scripts,
so a plain `git clone` works everywhere.

### 1. Toolchain and libraries

Install devkitPro with [its installer or pacman](https://devkitpro.org/wiki/Getting_Started),
then the PowerPC libraries:

```sh
dkp-pacman -S wii-dev ppc-zlib ppc-libpng ppc-mxml ppc-freetype ppc-bzip2 ppc-brotli
```

- **Windows:** run everything from the devkitPro MSYS2 shell
  (`C:\devkitPro\msys2\msys2_shell.bat`), where the command is `pacman` and
  `DEVKITPRO`/`DEVKITPPC` are set for you.
- **Linux and macOS:** use `sudo dkp-pacman` and export
  `DEVKITPRO=/opt/devkitpro` and `DEVKITPPC=$DEVKITPRO/devkitPPC`.

### 2. Host tools

| Tool | Needed for | Windows (devkitPro MSYS2) | Debian/Ubuntu | macOS (Homebrew) |
| --- | --- | --- | --- | --- |
| C compiler, zlib | WiiPAX, banner tools, host `wiiload`, tests | `pacman -S gcc zlib-devel` | `apt install build-essential zlib1g-dev` | Xcode command-line tools |
| `xxd` | WiiPAX stub embedding | `pacman -S vim` | `apt install xxd` | included |
| `msgfmt` | translations | included | `apt install gettext` | `brew install gettext` |
| Python 3.10+ | banner, WAD packaging, tools, tests | [python.org](https://www.python.org/) | `apt install python3-venv` | `brew install python` |

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt     # Windows: .venv/Scripts/python
```

### 3. Channel DOL

```sh
make -C wiipax
make -C channel/channelapp channel
```

This produces `channel/channelapp/channelapp-channel.dol`, the file you send
over Wiiload. It needs no Wii keys.

### 4. Retail WAD

```sh
make -C channel PYTHON="$(pwd)/.venv/bin/python"
make -C channel/title PYTHON="$(pwd)/.venv/bin/python" check
```

The build reads a 16-byte Wii common key from `~/.wii/common-key` (or from
`$WII_KEYS_DIR`). Take it from your own Wii's BootMii `keys.bin`, where
[BackupMii documents](https://wiibrew.org/wiki/BackupMii) it at offset `0x114`.
Keep `keys.bin` and the key private: put your copies in `keys/`, which Git
ignores along with loose key files anywhere in the tree. `wadpack.py` refuses
any content whose size or SHA-1 does not match the TMD, and a failed build
leaves no partial WAD. NAND saves and themes need the installed channel's
title identity, so they do not work from a direct DOL launch.

`TITLE=LULZ` (four characters, default `OHBC`) sets the title ID everywhere
it is used: see [Replacing the original Homebrew Channel](#replacing-the-original-homebrew-channel).

The displayed version and the WAD title version share one `major.minor.patch`
value; the 16-bit TMD field packs it as `major << 11 | minor << 5 | patch`.

## Testing

| Check | Command | Needs |
| --- | --- | --- |
| Unit tests (PyWii, `hbc.py` protocol) | `python3 -m unittest discover -s tests -p 'test_*.py'` | Python |
| Release version fields agree | `python3 tests/release_version.py` | Python |
| Reload stub memory layout | `tests/stub_layout.sh` | built DOL |
| Host tool bounds | `tests/host_bounds.sh` | C compiler |
| Boot in Dolphin | `python3 tests/dolphin_smoke.py` | Dolphin |
| Developer network in Dolphin | `make -C tests/netlog_app` then `python3 tests/dolphin_smoke.py --devnet` | Dolphin |
| Installed WAD in Dolphin, including app exit back to it | `python3 tests/dolphin_smoke.py --devnet channel/title/channel_retail.wad 120` | Dolphin, WAD |
| What the agent costs on a real Wii (memory, CPU, frame time) | `python3 tests/agent_cost.py WII-IP` | Wii in any HBC |
| Overlay layout on the PC, every page | `python3 tests/overlay_preview/preview.py` (needs a C compiler and Pillow; writes PNGs) | C compiler |
| In-app agent in Dolphin: status, files, overlay, exit, Wiiload, crash report | `make -C tests/agent_app` then `python3 tests/dolphin_smoke.py --agent channel/title/channel_retail.wad 120` | Dolphin, WAD |
| The agent's way out past a stuck IOS ([docs](docs/devnet.md#the-way-out)): each fallback, from test bits | `make -C tests/agent_app` then `python3 tests/dolphin_way_out.py` | Dolphin, WAD |
| Developer network on a real Wii | `python3 tests/wii_devnet.py WII-IP` | Wii in any HBC |
| The overlay with a MotionPlus remote: Test switches it on (the gyro, live), closing switches it back off | `python3 tests/wii_mplus.py WII-IP [--chan N] [--hbc]` (`--hbc`: HBC's own HOME menu) | Wii in any HBC, a MotionPlus remote |
| What a Wii's DNS answers from a fresh network start (WiiLink's user agreement server, Pi-hole), read-only | queue `python tests/wii_dns.py [NAME...]` (build `tests/dnscheck` first) | the bench queue |
| Why User Agreements will not open: the hidden EULA channel (HAKx), the IOS it needs, and IOS56, read-only | queue `python tests/wii_eulacheck.py` (build `tests/eulacheck` first) | the bench queue |
| Message Board library (`sdk/mboard`) on the PC: posts, the full message, sending, play history, byte for byte | `python3 tests/test_mboard.py`, `python3 tests/test_cdblog.py` | C compiler |
| Message Board library with the real Wii Menu 4.3U filing the post (`--full` for the Mii, photo and board data) | `python3 tests/dolphin_mboard.py --restore` (build `tests/mboard_app` first) | Dolphin, the Wii Menu profile |
| Message Board stationery (a letter's border, board icon, music): every encoder read back, the five example themes | `python3 tests/test_stationery.py` | Pillow |
| Message Board library on the bench Wii: `backup`, `id`, `post`, `full`, `send WII`, `board`, `friends`, `restore` | queue `python tests/wii_mboard.py STEP` (it refuses any other Wii) | the bench queue |
| Mii library (`sdk/mii`) on the PC: every field and limit, IDs, CRCs, the database, the hidden database and a Remote's Miis byte for byte (`MII_REAL_DB` adds a Wii's own `RFL_DB.dat`) | `python3 tests/test_mii.py` | C compiler |
| Mii library in Dolphin: a database on its NAND (add, hidden, remove) and the emulated Wii Remote's Miis, each written and read back | `python3 tests/dolphin_mii.py` (build `tests/mii_app` first) | Dolphin |
| Mii drawing (`sdk/mii/miidraw.c`) on the PC: a synthetic face resource builds a Mii with every part and each expression's face where RFL puts it, a damaged one is refused (`MII_REAL_RES` and `MII_REAL_DB` add your own files: every Mii builds) | `python3 tests/test_miidraw.py` | C compiler |
| Mii effects (`sdk/mii/miifx.c`) on the PC: synthetic REFF and REFT files play each feature (forms, rate, curves, fields, the flip-book, child emitters, the pools), damaged or cut files are refused (`MII_REAL_EFFECTS`, a folder with your own `effect_nigaoe.breff` and `.breft`, adds the Mii Channel's) | `python3 tests/test_miifx.py` | C compiler |
| HBC's avatar on the bench Wii: ready, 60 frames a second for a minute, ten TV pictures | queue `python tests/wii_avatar.py` | the bench queue |
| Mii drawing on a Wii: `mii_icon` (normal, smiling) and a full-body `mii_capture`, saved to `sd:/mii_test` and checked | `mii_app.dol draw [SLOT]` (Dolphin: with `Graphics.Hacks.EFBToTextureEnable=False`) | Wii or Dolphin |
| Mii library on the bench Wii: `backup`, `check`, `add`, `remove ID`, `edit ID FIELD=N...`, `hidden`, `remote-add ID`, `remote-remove ID`, `remote-rewrite`, `restore` | queue `python tests/wii_mii.py STEP` (build `tests/mii_app`, `tests/nandget` and `tests/nandput` first; it refuses any other Wii) | the bench queue |
| Upload popups close themselves and report back (bad file, unanswered ZIP, `--yes`) | `python3 tests/wii_upload_popups.py WII-IP` | Wii in any HBC |
| The agent and MEM2: nothing written into HBC's records while an app runs, the log target kept as it stops, the agent's memory at the top of the MEM2 arena | `python3 tests/wii_agent_mem2.py WII-IP` (`--dol` for another build) | Wii in any HBC |
| `hbc_agent_stop()` (then an IOS reload, a re-init, a crash) and `hbc_agent_listen()` (the app's own server on 4299) on a real Wii | `make -C tests/agent_app`, then `python3 tests/wii_agent_stop.py --wii WII-IP` (`--dol` for another build) | Wii in any HBC |
| File times and flags from HBC and an app's agent (libogc 3, libogc2), and a re-sync that reads nothing | `python3 tests/wii_file_meta.py WII-IP` | Wii in any HBC |
| HOME > HBC > Restart network (keys checked against `overlay_ui` before each A) | `python3 tests/wii_net_restart.py WII-IP` | Wii in any HBC |
| File errors from HBC and an app's agent: a missing folder, a missing local file, a 400 MB upload to a card without room (`ENOSPC` at once) | `python3 tests/wii_file_errors.py WII-IP` (SD with under 400 MB free), or `--dolphin` | Wii in any HBC, or Dolphin |
| The agent's safety tools on a real Wii (assert, abort, deadlock, failed malloc, frame pacing, stub guard, Reset, stack overflow) | `make -C tests/agent_app`, then `python3 tests/wii_agent_safety.py --wii WII-IP` | Wii in any HBC |
| DEV > Info and `hbc.py hw` on a real Wii | `python3 tests/wii_info.py WII-IP` | Wii in any HBC |
| Installed channel on a real Wii | `python3 tests/wii_devnet.py --installed --expect 1.10.7 WII-IP` | installed channel running |
| Every `meta.xml` option through the menu, driven by a scripted Wii Remote: arguments, `ahb_access`, `no_ios_reload`, author/coder, version, descriptions, icon, the Name, Date and Custom sorts, Load as the dialog's default | `python3 tests/dolphin_meta.py [OUTDIR]` | Dolphin (no other test Dolphin running) |
| App list while files arrive (one in-place reload, no page slides) and `hbc.py order` (file requests only) | `python3 tests/wii_app_list.py WII-IP` | Wii in any HBC |
| Both USB drives through HBC: list, write and read back 1 MiB on each (file requests only) | `python3 tests/wii_usb2.py WII-IP` | Wii in any HBC, two USB drives |
| HOME overlay frame time in HBC, first opening and later (HOME presses only) | `python3 tests/wii_overlay_speed.py WII-IP`; right after an app returns: `python3 tests/wii_overlay_return.py WII-IP` | Wii in any HBC |
| In-app agent on a real Wii, with its speed next to HBC's | `python3 tests/wii_agent.py WII-IP` | Wii in any HBC |
| Throughput on a real Wii | `python3 tests/wii_netbench.py WII-IP` | Wii in any HBC |
| MEM1, MEM2 and locked-cache speed on a real Wii | `make -C tests/membench`, then `python3 tools/hbc.py run tests/membench/membench.dol sd:/path/to/sample` | Wii in any HBC |
| Start an installed title from HBC | `python3 tools/hbc.py send tests/launch_title/launch_title.dol 000100014f484243` | Wii in any HBC |

The Dolphin tests use a throwaway profile, pass every setting on the command
line, and delete the profile when they pass. Set `DOLPHIN` if Dolphin is not
in a standard location. Dolphin uses the PC's own sockets, so it cannot show
the Wii's network quirks; measure speed and network reliability on hardware.

The real-Wii tests send the DOL over Wiiload, clean up after themselves on
the SD card (`sd:/hbctest`, `sd:/hbcbench`), and leave the Wii in HBC. They
need inbound TCP on the log port (`--log-port`) allowed on the PC.

When several people or agents share one Wii, queue hardware jobs through
[`tools/wii-bench`](tools/wii-bench/README.md): one queue and one dispatcher,
which starts a job only once HBC has been idle for 20 s. Workstations on Windows,
Linux and macOS take turns through a lease server in Docker; the bench README has
the steps for [the server](tools/wii-bench/README.md#setting-up-the-lease-server-from-clone-to-a-running-container)
and for [each workstation](tools/wii-bench/README.md#setting-up-each-workstation).

```sh
python3 tools/wii-bench/wiibench.py add --name "devnet" --cwd . -- python3 tests/wii_devnet.py --log-port 4300
python3 tools/wii-bench/wiibench.py wait <id>
```

## Contributing

- [CI](.github/workflows/ci.yml) builds the DOL, banner, and TMD in the
  `devkitpro/devkitppc` container with warnings as errors. It also builds the
  host tools and runs the Python tests on Windows, Linux, and macOS. Pass
  `EXTRA_CFLAGS=-Werror` to any `make` to check locally.
- Every commit bumps both version fields, `CHANNEL_VERSION_STR` in
  `channel/channelapp/config.h` and `CHANNEL_VERSION` in
  `channel/title/Makefile`, to the same release.
- Text is LF everywhere, on every platform (`.gitattributes`). A few upstream
  files kept exactly as they came are marked `-text` there and keep CRLF. A
  checkout made before 1.8.3 on Windows still has CRLF files; run
  `python tools/fix_line_endings.py` once (it keeps uncommitted changes).
- Never commit Wii keys. Check staged file names before every commit.
- [`AGENTS.md`](AGENTS.md) and the
  [project skill](.agents/skills/hbc-build-and-review/SKILL.md) hold the
  detailed build, Dolphin, and hardware procedures;
  [`WII_DEVELOPMENT.md`](WII_DEVELOPMENT.md) covers the reload stub and return
  path.

## License

Unless a file header says otherwise, the source code is released under the
GNU General Public License, version 2 or later; see [COPYING](COPYING).
[`sdk/hbc_netlog.h`](sdk/hbc_netlog.h), [`sdk/mboard`](sdk/mboard) and
[`sdk/mii`](sdk/mii) are public domain so apps can include them freely, and
[`sdk/wiispk`](sdk/wiispk) is under the zlib licence. Parts of
[`sdk/wiiinput`](sdk/wiiinput) (from RiftWii and libwupc) are GPL-3.0-or-later,
so the channel built with them is distributed under GPL-3.0. The agent library (`sdk/hbc_agent`) is built from HBC's own
sources and is GPL.
