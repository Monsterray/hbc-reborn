# The Homebrew Channel (reborn)

[![CI](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml/badge.svg)](https://github.com/Monsterray/hbc-reborn/actions/workflows/ci.yml)

A maintained fork of [The Homebrew Channel](https://github.com/fail0verflow/hbc)
for the Nintendo Wii. It builds with current devkitPro on Windows, Linux, and
macOS, warning-free, and adds a **developer network**: from a PC on the same
LAN you can query the Wii, move files to and from its SD card, launch apps,
and stream their `printf` output back, with checksummed and compressed
transfers.

Current release: **1.4.0**. Title ID `00010001-4F484243` (`OHBC`), so the
channel installs next to the official Homebrew Channel (`LULZ`) instead of
replacing it.

| Part | Where |
| --- | --- |
| Channel app, loader, reload stub | [`channel/channelapp`](channel/channelapp) |
| Developer network (Wii side) | [`channel/channelapp/source/devnet.c`](channel/channelapp/source/devnet.c), [`devstream.c`](channel/channelapp/source/devstream.c) |
| PC client for the developer network | [`tools/hbc.py`](tools/hbc.py) |
| App-side network log header | [`sdk/hbc_netlog.h`](sdk/hbc_netlog.h) |
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

```
python3 tools/hbc.py [--wii ADDR] [--json] [--log-port PORT] [--timeout S] COMMAND ...
```

| Command | What it does |
| --- | --- |
| `version` | Print the running HBC version (`HBCV`). |
| `status` | Print JSON status: version, protocol, IOS and revision, AHBPROT, free memory, loader stack use, startup and app-scan time, IP, app count, mounted device, log target, and timing of the last transfer. |
| `wait [SECONDS]` | Wait until HBC answers (default 90 s). |
| `send FILE [ARG ...]` | Send a DOL, ELF, or ZIP over Wiiload. A ZIP is installed to the SD card after you confirm on the Wii. |
| `run FILE [ARG ...]` | Register for logs, send `FILE`, and print its output until it exits (`--timeout`, default 300 s). |
| `log` | Register for logs and print app output until Ctrl+C. Use it when you launch apps from the Wii itself. |
| `ls REMOTE` | List a directory, e.g. `sd:/apps`. Lines are `d name` or `f size name`. |
| `get [-r] REMOTE [LOCAL]` | Download a file, or with `-r` a directory tree. |
| `put [-r] LOCAL REMOTE` | Upload a file or, with `-r`, a directory tree; parent directories are created. A `REMOTE` ending in `/` gets the file's name appended. |
| `sync [--delete] LOCALDIR REMOTEDIR` | Make `REMOTEDIR` match `LOCALDIR`, uploading only files whose size or CRC-32 differ; `--delete` also removes remote extras. |
| `rm [-r] REMOTE` | Delete a file or an empty directory, or with `-r` a tree. Device roots and `<device>:/apps` itself are refused. |
| `mkdir REMOTE` | Create a directory and its parents. |

`--json` makes `version`, `status`, and `ls` print JSON. Options can follow
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
for the next app it launches. Call `hbc_netlog_close()` if your app leaves
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
| Developer network on a real Wii | `python3 tests/wii_devnet.py WII-IP` | Wii in any HBC |
| Installed channel on a real Wii | `python3 tests/wii_devnet.py --installed --expect 1.4.0 WII-IP` | installed channel running |
| Throughput on a real Wii | `python3 tests/wii_netbench.py WII-IP` | Wii in any HBC |
| Start an installed title from HBC | `python3 tools/hbc.py send tests/launch_title/launch_title.dol 000100014f484243` | Wii in any HBC |

The Dolphin tests use a throwaway profile, pass every setting on the command
line, and delete the profile when they pass. Set `DOLPHIN` if Dolphin is not
in a standard location. Dolphin uses the PC's own sockets, so it cannot show
the Wii's network quirks; measure speed and network reliability on hardware.

The real-Wii tests send the DOL over Wiiload, clean up after themselves on
the SD card (`sd:/hbctest`, `sd:/hbcbench`), and leave the Wii in HBC. They
need inbound TCP on the log port (`--log-port`) allowed on the PC. On the
project's workstation, queue hardware jobs through the shared bench:
`python C:/tools/wii-bench/wiibench.py add --name NAME --cwd DIR -- CMD`.

## Contributing

- [CI](.github/workflows/ci.yml) builds the DOL, banner, and TMD in the
  `devkitpro/devkitppc` container with warnings as errors. It also builds the
  host tools and runs the Python tests on Windows, Linux, and macOS. Pass
  `EXTRA_CFLAGS=-Werror` to any `make` to check locally.
- Every commit bumps both version fields, `CHANNEL_VERSION_STR` in
  `channel/channelapp/config.h` and `CHANNEL_VERSION` in
  `channel/title/Makefile`, to the same release.
- Keep each file's existing line endings; some PyWii and Wiiload files use
  CRLF. Check with `git -c core.whitespace=cr-at-eol diff --check`.
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
it freely.
