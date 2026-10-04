# The Homebrew Channel (reborn)

[![CI](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml/badge.svg)](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml)

A maintained fork of [The Homebrew Channel](https://github.com/fail0verflow/hbc)
for the Nintendo Wii. It builds with current devkitPro on Windows, Linux, and
macOS, warning-free, and adds a **developer network**: from a PC on the same
LAN you can query the Wii, move files to and from its SD card, launch apps,
and stream their `printf` output back, with checksummed and compressed
transfers.

Current release: **1.9.7**. Title ID `00010001-4F484243` (`OHBC`), so the
channel installs next to the official Homebrew Channel (`LULZ`) instead of
replacing it.

| Part | Where |
| --- | --- |
| Channel app, loader, reload stub | [`channel/channelapp`](channel/channelapp) |
| Developer network (Wii side) | [`channel/channelapp/source/devnet.c`](channel/channelapp/source/devnet.c), [`devstream.c`](channel/channelapp/source/devstream.c) |
| PC client for the developer network | [`tools/hbc.py`](tools/hbc.py) |
| App-side network log header | [`sdk/hbc_netlog.h`](sdk/hbc_netlog.h) |
| In-app agent: the tools and crash reports inside a running app | [`sdk/hbc_agent.h`](sdk/hbc_agent.h), [`sdk/hbc_agent`](sdk/hbc_agent) |
| Banner, WAD packaging, PyWii | [`channel/banner`](channel/banner), [`channel/title`](channel/title), [`pywii`](pywii) |
| WiiPAX executable packer, host `wiiload` | [`wiipax`](wiipax), [`channel/wiiload`](channel/wiiload) |
| Protocol reference | [`docs/devnet.md`](docs/devnet.md) |
| Review notes and hardware results | [`REVIEW.md`](REVIEW.md) |

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
| `ls REMOTE` | List a directory, e.g. `sd:/apps`. Lines are `d name` or `f size name`. |
| `get [-r] REMOTE [LOCAL]` | Download a file, or with `-r` a directory tree. |
| `put [-r] LOCAL REMOTE` | Upload a file or, with `-r`, a directory tree; parent directories are created. A `REMOTE` ending in `/` gets the file's name appended. |
| `sync [--delete] LOCALDIR REMOTEDIR` | Make `REMOTEDIR` match `LOCALDIR`, uploading only files whose size or CRC-32 differ; `--delete` also removes remote extras. |
| `rm [-r] REMOTE` | Delete a file or an empty directory, or with `-r` a tree. Device roots and `<device>:/apps` itself are refused. |
| `mkdir REMOTE` | Create a directory and its parents. |
| `exit` | Ask the running [agent](#keeping-the-tools-inside-your-app) app to exit to HBC, and wait for HBC. |
| `key KEYS` | Send controller presses to HBC's HOME menu or the running agent app: `h` (HOME, opens or closes its overlay), `u` `d` `l` `r` (D-pad), `a`, `b`. For scripted tests and for driving the overlay from the PC. |
| `screen FILE.png` | Save what the TV shows, from HBC or a running agent app, overlay included. |
| `crash [--elf FILE] [--clear]` | Print the crash an agent app reported: exception, registers, backtrace, and with `--elf` function names and source lines (needs devkitPPC's `addr2line`). `--clear` forgets it. |

`--json` makes `version`, `status`, `ls`, and `crash` print JSON. Options can follow
the command; for `send` and `run`, everything after the file goes to the app,
and `--` ends option parsing. Large transfers show progress on a terminal.

The Wii address comes from `--wii`, then `$HBC_WII`, `$WII_BENCH_IP`, or
`$WIILOAD` (`tcp:ADDR`, the variable the devkitPro `wiiload` uses).

Remote paths are `<device>:/<path>` with device `sd`, `usb`, `carda`, or
`cardb`; paths containing `..`, `//`, or backslashes are refused. An app you
`put` or `sync` under `sd:/apps/` appears in the menu right away, and one you
remove disappears.

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
python3 tools/hbc.py status                         # IOS, AHBPROT, memory after a run
```

To install an app permanently, `sync` or `put -r` its folder (with
`boot.dol` and `meta.xml`) to `sd:/apps/<name>/`, or `send` a ZIP.

`hbc_netlog_init()` gives up within about 5 s when the PC is unreachable, and
`run` and `log` clear HBC's log target when they exit, so a stale target
never stalls later apps.

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

A status strip shows the app, the clock, and the four remotes' player LEDs
and batteries, over five buttons:

| Button | What it does |
| --- | --- |
| DEV | Actions: Restart app, Pause, Save, Log (the app's recent output), Reset remotes, Sync clock (sets the Wii's clock from an NTP server, keeping its time zone), Log to PC, the crash screen's time, and the `hbc.py` connection. Info: time, play time, network, SD space, the app, and MEM1 and MEM2 free, used and total in KB, then five pages read when one opens: System (console and model, region, serial, console ID, Wii Menu, IOS and AHBPROT, boot2, CPU, Hollywood revision, language), Video (mode, cable, TV type, 16:9, 480p, PAL 60, sound, sensor bar, paired remotes), Storage (SD and USB size and free space, NAND free, GameCube memory cards), USB (each device on the ports, by vendor and product ID) and Network (IP and netmask, gateway, DNS, MAC, the Wii Menu connection in use). |
| (app slot) | Whatever the app puts there with `hbc_agent_set_slot(0, ...)`; blank otherwise. |
| Exit | The Homebrew Channel, System Menu, Restart Wii, Power off. |
| Shot | Saves the game's frame to `sd:/screenshots/<app>-NNN.bmp`. An app can replace it with `hbc_agent_set_slot(1, ...)`. |
| WiiMote | A card per remote (Find: a chime that gets louder; More: battery, extension, MotionPlus, rumble on or off, speaker volume with a chirp, Test with an ADPCM and a PCM tune and a WAV player for `speaker.wav` on SD or USB, Calibrate, Disconnect), and Settings: connect a remote, disconnect all, sensor bar, IR sensitivity, auto power-off, rumble for all. |

An app can give a slot a menu of its own instead of one action
(`hbc_agent_set_slot_menu`), with buttons and live info rows. HBC's own HOME
menu is this overlay, set up in
[`channel/channelapp/source/home.c`](channel/channelapp/source/home.c): a
commented example of every step, from the config to lending the overlay its
framebuffers and handling Exit through HBC's own shutdown. Its HBC slot
holds what the old HOME menu had: HBC's and IOS's versions, the Wii's IP,
About, Launch BootMii, Exit to System Menu, and Shutdown.

Point a Wii Remote at the screen and HBC's hand cursor follows it, turning as you twist the remote: the
button under it lights up and A presses it, and pointing at a bar button
while a menu is open switches straight to that menu. The overlay turns on
IR for every connected remote while it is open and gives each back its
previous mode when it closes. The D-pad moves the highlight when no remote
points at the screen, A chooses, B goes back one level, and HOME closes
everything. Controller settings last until the app exits; nothing is
written to the Wii's own settings, except that Connect remote may save the
new remote in the Wii's pairing list as the Sync button would. Link the
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
| Developer network on a real Wii | `python3 tests/wii_devnet.py WII-IP` | Wii in any HBC |
| Upload popups close themselves and report back (bad file, unanswered ZIP, `--yes`) | `python3 tests/wii_upload_popups.py WII-IP` | Wii in any HBC |
| The agent's safety tools on a real Wii (assert, abort, deadlock, failed malloc, frame pacing, stub guard, Reset, stack overflow) | `make -C tests/agent_app`, then `python3 tests/wii_agent_safety.py --wii WII-IP` | Wii in any HBC |
| DEV > Info and `hbc.py hw` on a real Wii | `python3 tests/wii_info.py WII-IP` | Wii in any HBC |
| Installed channel on a real Wii | `python3 tests/wii_devnet.py --installed --expect 1.9.7 WII-IP` | installed channel running |
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
[`sdk/hbc_netlog.h`](sdk/hbc_netlog.h) is public domain so apps can include
it freely. The agent library (`sdk/hbc_agent`) is built from HBC's own
sources and is GPL.
