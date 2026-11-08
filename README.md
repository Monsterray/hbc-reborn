# The Homebrew Channel (reborn)

[![CI](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml/badge.svg)](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml)

A maintained fork of [The Homebrew Channel](https://github.com/fail0verflow/hbc)
for the Nintendo Wii. It builds with current devkitPro on Windows, Linux, and
macOS, warning-free, and adds a **developer network**: from a PC on the same
LAN you can query the Wii, move files to and from its SD card, launch apps,
and stream their `printf` output back, with checksummed and compressed
transfers.

Current release: **1.14.0**. Title ID `00010001-4F484243` (`OHBC`), so the
channel installs next to the official Homebrew Channel (`LULZ`) instead of
replacing it.

| Part | Where |
| --- | --- |
| Channel app, loader, reload stub | [`channel/channelapp`](channel/channelapp) |
| Developer network (Wii side) | [`channel/channelapp/source/devnet.c`](channel/channelapp/source/devnet.c), [`devstream.c`](channel/channelapp/source/devstream.c) |
| PC client for the developer network | [`tools/hbc.py`](tools/hbc.py) |
| App-side network log header | [`sdk/hbc_netlog`](sdk/hbc_netlog/README.md) |
| In-app agent: the tools and crash reports inside a running app | [`sdk/hbc_agent`](sdk/hbc_agent/README.md) |
| Wii Message Board: posts, mail to another Wii, play history (HBC's play log too) | [`sdk/mboard`](sdk/mboard) |
| Miis: the data, the Mii database, a Wii Remote's Miis | [`sdk/mii`](sdk/mii) |
| Wii Remote speaker driver | [`sdk/wiispk`](sdk/wiispk) |
| Banner, WAD packaging, PyWii | [`channel/banner`](channel/banner), [`channel/title`](channel/title), [`pywii`](pywii) |
| WiiPAX executable packer, host `wiiload` | [`wiipax`](wiipax), [`channel/wiiload`](channel/wiiload) |
| Protocol reference | [`docs/devnet.md`](docs/devnet.md) |
| Message Board: the library, the board's formats | [`docs/mboard.md`](docs/mboard.md), [`docs/messageboard.md`](docs/messageboard.md), [`docs/messageboard-messages.md`](docs/messageboard-messages.md) |
| Miis: the data, `RFL_DB.dat`, the hidden database, a Remote's Miis, the library | [`docs/mii.md`](docs/mii.md) |
| USB keyboard as a Wii Remote | [`channel/channelapp/source/keyboard.c`](channel/channelapp/source/keyboard.c), [`keymap.c`](channel/channelapp/source/keymap.c) |
| Review plan and open items; each release's findings; versions | [`docs/review.md`](docs/review.md), [`docs/history/`](docs/history/), [`CHANGELOG.md`](CHANGELOG.md) |

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
| `put [-r] [--no-verify] LOCAL REMOTE` | Upload a file or, with `-r`, a directory tree; parent directories are created. A `REMOTE` ending in `/` gets the file's name appended. Each file is then read back on the Wii and its size and CRC-32 checked, uploading it again (twice at most) on a mismatch; `--no-verify` skips that. |
| `sync [--delete] [--checksum] [--no-verify] LOCALDIR REMOTEDIR` | Make `REMOTEDIR` match `LOCALDIR`, uploading only what changed: a same-size file the Wii wrote after the local one last changed is skipped without being read; otherwise the CRC-32s decide (`--checksum` uses them for every same-size file). Uploads are read back and checked as `put` does (`--no-verify` skips it). `--delete` also removes remote extras; a file that fails is reported and the rest go on. |
| `rm [-r] [--force] REMOTE` | Delete a file or an empty directory, or with `-r` a tree. A `*` or `?` in the last name deletes every match (`rm 'sd:/logs/*.log'`; folders only with `-r`). The Wii walks the tree or pattern itself in one request. Device roots and `<device>:/apps` itself are refused, by the Wii too, and so is a read-only entry unless `--force`, which clears read-only first (as `del /f` does). |
| `mkdir REMOTE` | Create a directory and its parents. |
| `mv [--replace] FROM TO` | Rename or move a file or folder (also `rename`). A `TO` ending in `/`, or a folder that is there, means "into that folder". On one device the Wii renames in place, at once; between devices (`sd:` to `usb:`) each file is copied, checked by CRC-32, and only then deleted (files up to 512 MiB; a read-only source is refused first; the copies get new times). An existing `TO` is refused unless `--replace` (a file only). HBC 1.10.14 and its agent. |
| `mii-faces` | The same as HOME > HBC > Save Mii faces (`HBCM`): HBC copies the Wii Menu's Mii face models (`RFL_Res.dat`) and the Mii Channel's bodies (`maleBody.brres`, `femaleBody.brres`) and effects (`effect_nigaoe.breff`, `.breft`) to `sd:/mii`, for apps that draw Miis. The files are Nintendo's: keep them on your own SD card, never ship them. HBC 1.10.20 (bodies 1.10.26, effects 1.12.2). |
| `net-hang [wait\|reload]` | What HBC does when a network cleanup hangs inside IOS: carry on without the network until it next starts (the default), or reload itself at once (for a developer's Wii); with no mode, print the current one. |
| `transfer-style [scroll\|bounce\|truncate]` | Force how HBC shows a file name too long for its on-screen transfer line, until it restarts (for testing); with no name, print the current one. |
| `order [--clear] APPS [FOLDER ...]` | HBC's Custom app order (see [Your own app order](#your-own-app-order)): with only `APPS` (e.g. `sd:/apps`), print the apps in that order with their `<sort_id>`; with folders, give them 10, 20, 30... in the order named, editing or making each `meta.xml`; `--clear` removes every `<sort_id>`. |
| `exit` | Ask the running [agent](sdk/hbc_agent/README.md) app to exit to HBC, and wait for HBC. |
| `key KEYS` | Send controller presses to HBC's HOME menu or the running agent app: `h` (HOME, opens or closes its overlay), `u` `d` `l` `r` (D-pad), `a`, `b`. For scripted tests and for driving the overlay from the PC. |
| `screen FILE.png` | Save what the TV shows, from HBC or a running agent app, overlay included. |
| `crash [--elf FILE] [--clear]` | Print the crash an agent app reported: exception, registers, backtrace, and with `--elf` function names and source lines (needs devkitPPC's `addr2line`). `--clear` forgets it. |

`--json` makes `version`, `status`, `ls`, and `crash` print JSON. Options can follow
the command; for `send` and `run`, everything after the file goes to the app,
and `--` ends option parsing. Large transfers show progress on a terminal.

The Wii address comes from `--wii`, then `$HBC_WII`, `$WII_BENCH_IP`, or
`$WIILOAD` (`tcp:ADDR`, the variable the devkitPro `wiiload` uses). With
none of those set, `hbc.py` asks the network (`hbc.py find`, a UDP broadcast)
and uses the one Wii that answers. `hbc.py find` lists every Wii running HBC
or an agent app, with its address and name.

Remote paths are `<device>:/<path>` with device `sd`, `usb`, `usb2` (a
second USB drive), `carda`, or `cardb`; paths containing `..`, `//`, or backslashes are refused. An app you
`put` or `sync` under `sd:/apps/` appears in the menu right away, and one you
remove disappears. The menu reloads once the writes stop (a second of
quiet), on the page it shows, so a whole folder's upload is one quiet reload.
While a file moves, HBC shows it at the bottom right of the screen
("Receiving ridgerac/boot.dol 42%", "Sending save.dat"). Files under a
device's `apps/` directory show the app folder and any subfolders; other
files show their parent folder and name (a file at a device's root, its name). Options (1) > Progress: **Circle** replaces the
percentage with a progress circle in the text colour on a faded track (the
choice is kept in `settings.xml`). The circle fills clockwise from the top,
and turns while the transfer's size is unknown. A name too long for the
line scrolls round, bounces from end to end, or ends in "..." (each HBC
start picks one at random; `hbc.py transfer-style bounce` forces one for
testing). A moving name rests a second at its start, and a bouncing one at
each end; "Receiving" and the percentage (or circle) stay put. A delete
shows "Deleting" with the name sent (a folder or pattern, not each entry
inside it); the circle turns while HBC counts the entries, then fills as
they go.

When receiving a Wiiload upload from `hbc.py`, the popup heading shows the
file name, including its app folder for `boot.dol` or `boot.elf`. The sender's
IP address appears below the progress bar at the lower left, separate from
the percentage on the right. A sender that supplies no name keeps the
"Receiving from <address>" heading.

Uploads and downloads use 64 KiB frames, each with a CRC-32 checked on both
ends, compressed with zlib when that makes them smaller. A corrupted frame
fails the transfer and leaves no partial file. On an 802.11g Wii a typical
ELF moves at about 1.5 MB/s up and 0.9 MB/s down, and compressible data at
4 to 5 MB/s; incompressible data is limited by the Wii's network stack to
about 1.35 MB/s in and 0.6 MB/s out. [docs/devnet.md](docs/devnet.md) has the
measurements.

### Seeing your app's output

Add [`sdk/hbc_netlog`](sdk/hbc_netlog/README.md) to your app (one public-domain
header) and call `hbc_netlog_init()` once; `hbc.py run myapp.dol` then prints
everything the app writes. Its README has the example, the firewall note and
what to do for apps started from the Wii's own menu.

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

### App picker navigation

Hold a D-pad direction to repeat movement in the app picker. In Options
(1), **Grid edges: Turn page** makes Left and Right at an edge switch
pages, keeping the same row where possible. **Wrap rows** keeps the original
movement between rows. Clickable page arrows remain available for pointer
users. The choice is saved with the channel's settings.

### App badges

Options (1) > **App badges: On** marks apps in the list and the grid with a
pill in the tile's top-right corner: a blue **New** on an app you have never
run from HBC, and an amber **Updated** (with a bar along the tile's top edge)
on one whose `boot.dol`, `boot.elf`, `meta.xml` or `icon.png` changed, in
size or date, since you last ran it.

Only a launch from HBC's own app list or app page records a run and clears
the badge. An app sent with Wiiload, USBGecko or the developer network, or
started some other way, does not.

HBC keeps the record on the device the apps are on, one text file each:
`sd:/hbc/apps_seen.txt` for apps on the SD card, `usb:/hbc/apps_seen.txt` for
a USB drive (and likewise for the other devices). It has a line for each app:
the app's folder name, a tab, then the size and modified time of the four
files:

    Wii64	boot.dol=1318128@1762104532 boot.elf=- meta.xml=412@1762104532 icon.png=9211@1762104532

The file is read once when the list is scanned, and written only when
something changed: a launch, or apps that were removed. It is never written
to the Wii's NAND. Delete it to mark every app on that device as new again.
If the device is read-only or fails, the badges simply do not persist. With
badges off (the default) HBC neither reads nor writes the file, so turning
them on for the first time shows every app as new.

A theme can colour the pills with `badge_new` and `badge_updated` in its
`theme.xml`, each with a `<fill>` and a `<text>` colour (red, green, blue and
alpha, 0 to 255, like `font_color`). The defaults are a blue fill `#4fa3e0`
with text `#062a44` for New, and an amber fill `#e8b04a` with text `#3a2604`
for Updated. The text is 12 px; a font entry targeting `badge` changes it.

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

### SDKs for your app

Small libraries for any libogc app live in [`sdk/`](sdk/README.md). Each one's
README has a quick start, the API, and what it changes on the Wii.

| SDK | What it gives your app |
| --- | --- |
| [hbc_netlog](sdk/hbc_netlog/README.md) | Your app's `printf` output on the PC |
| [hbc_agent](sdk/hbc_agent/README.md) | HBC's tools inside your running app: `hbc.py` file commands, status and restart, crash reports, a HOME overlay |
| [wiispk](sdk/wiispk/README.md) | Clean Wii Remote speaker sound (HBC's HOME menu uses it) |
| [mboard](sdk/mboard/README.md) | Message Board posts, mail to other Wiis, the Address Book, the play history (HBC's play log is its writer) |
| [slotled](sdk/slotled/README.md) | The disc-slot light: levels, effects, IOS patterns, the saved setting |
| [mii](sdk/mii/README.md) | Miis: read, make, edit and draw them, the Mii database, a Remote's Miis |

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
| Network start budget and cancellation, and cleanup supervision, on the PC | `python3 tests/test_netinit.py`, `python3 tests/test_netrecovery.py` | C compiler |
| A failed start cleaned up and retried with no input and no reload | build with `EXTRA_CFLAGS=-DHBC_TEST_INIT_FAILURE`, then `python3 tests/dolphin_network_retry.py IMAGE` | Dolphin; never on a Wii |
| A network cleanup that hangs: HBC carries on without a reload; with `--reload` (`net-hang reload`), a fresh IOS and the installed channel again, no loop | build the WAD with `EXTRA_CFLAGS=-DHBC_TEST_STUCK_NET_CLEANUP`, then `python3 tests/dolphin_network_recovery.py [--reload] WAD` | Dolphin; never on a Wii |
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
- Each push bumps both version fields once, `CHANNEL_VERSION_STR` in
  `channel/channelapp/config.h` and `CHANNEL_VERSION` in
  `channel/title/Makefile`, to the same release. Between bumps HBC reports
  the release plus `+N.gHASH` (commits since the bump and git's hash, from
  `build_tag.h`), so each test build still has its own version.
- Text is LF everywhere, on every platform (`.gitattributes`). A few upstream
  files kept exactly as they came are marked `-text` there and keep CRLF. A
  checkout made before 1.8.3 on Windows still has CRLF files; run
  `python tools/fix_line_endings.py` once (it keeps uncommitted changes).
- Never commit Wii keys. Check staged file names before every commit.
- [`AGENTS.md`](AGENTS.md) and the
  [project skill](.agents/skills/hbc-build-and-review/SKILL.md) hold the
  detailed build, Dolphin, and hardware procedures;
  [`docs/research/return-path.md`](docs/research/return-path.md) covers the reload stub and return
  path.

## License

Unless a file header says otherwise, the source code is released under the
GNU General Public License, version 2 or later; see [COPYING](COPYING).
[`sdk/hbc_netlog`](sdk/hbc_netlog/README.md), [`sdk/mboard`](sdk/mboard) and
[`sdk/mii`](sdk/mii) are public domain so apps can include them freely, and
[`sdk/wiispk`](sdk/wiispk) and [`sdk/slotled`](sdk/slotled) are under the zlib
licence. The agent library (`sdk/hbc_agent`) is built from HBC's own sources
and is GPL. [`sdk/README.md`](sdk/README.md) lists every SDK with its licence
and what it changes on the Wii.
