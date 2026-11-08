# The Homebrew Channel (reborn)

[![CI](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml/badge.svg)](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml)

A maintained fork of [The Homebrew Channel](https://github.com/fail0verflow/hbc)
for the Nintendo Wii. It builds with current devkitPro on Windows, Linux, and
macOS, warning-free, and adds a **developer network**: from a PC on the same
LAN you can query the Wii, move files to and from its SD card, launch apps,
and stream their `printf` output back, with checksummed and compressed
transfers.

Current release: **1.15.0**. Title ID `00010001-4F484243` (`OHBC`), so the
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

## More for developers

- [docs/hbc-py.md](docs/hbc-py.md): every `hbc.py` command, seeing your app's
  output, and a typical edit-build-run loop.
- [docs/devnet.md](docs/devnet.md): the developer network's wire format, for other tools.
- [docs/README.md](docs/README.md): every other document, by reader.

## Using the channel

Hold-to-repeat and page-turning in the app picker, **New** and **Updated**
badges on apps, and your own app order: [docs/using-hbc.md](docs/using-hbc.md).

## SDKs for your app

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

## Building, testing and contributing

[CONTRIBUTING.md](CONTRIBUTING.md) covers the toolchain, building the DOL and
the WAD, every test and what it needs, and the rules for changes.

## License

Unless a file header says otherwise, the source code is released under the
GNU General Public License, version 2 or later; see [COPYING](COPYING).
[`sdk/hbc_netlog`](sdk/hbc_netlog/README.md), [`sdk/mboard`](sdk/mboard) and
[`sdk/mii`](sdk/mii) are public domain so apps can include them freely, and
[`sdk/wiispk`](sdk/wiispk) and [`sdk/slotled`](sdk/slotled) are under the zlib
licence. The agent library (`sdk/hbc_agent`) is built from HBC's own sources
and is GPL. [`sdk/README.md`](sdk/README.md) lists every SDK with its licence
and what it changes on the Wii.
