# The Homebrew Channel

This repository contains the public release of the source code for
The Homebrew Channel.

Included portions:

* The Homebrew Channel
* Reload stub
* Banner
* PyWii (includes Alameda for banner creation)
* WiiPAX (LZMA executable packer)

Not included:

* Installer

Note that the code in this repository differs from the source code used to build
the official version of The Homebrew Channel, which includes additional
protection features (i.e. we had to add reverse-DRM to stop scammers from
selling it).

This code is released with no warranty. The channel app DOL has been tested in
Dolphin and on a dev Wii; on the Wii it launched the Wii64 DOL through wiiload.
The 1.1.7 DOL also answered a live version query on the dev Wii. When loaded
through an installed Homebrew Channel, it keeps that channel's return stub so
apps can return to the installed channel after exit.
The retail WAD builds with the current toolchain and has booted to the HBC
menu in an isolated Dolphin NAND. It has not yet been installed on a Wii.

The current channel release is **1.1.8**. The displayed channel version and
retail WAD title version use the same SemVer value. The Wii TMD stores a
16-bit title version, so packaging encodes `major.minor.patch` as 5/6/5 bits
(`major << 11 | minor << 5 | patch`), preserving version order within those
field limits (major 0–31, minor 0–63, patch 0–31). The channel update protocol
also carries a numeric `YYYYMMDDHHMM` release timestamp for availability checks.

## Build instructions

The build uses devkitPro's toolchain and a POSIX shell on every platform:
devkitPPC r50-1, libogc 3.1.0, and GNU make. The repository has no symlinks
and shell scripts are checked out with LF endings, so a plain `git clone`
works on Windows, Linux, and macOS.

### 1. Install devkitPro and the PowerPC libraries

Install devkitPro with [its installer or pacman](https://devkitpro.org/wiki/Getting_Started),
then add the PowerPC libraries:

    dkp-pacman -S wii-dev ppc-zlib ppc-libpng ppc-mxml ppc-freetype ppc-bzip2 ppc-brotli

On Windows, run every command in this README from the **devkitPro MSYS2**
shell (`C:\devkitPro\msys2\msys2_shell.bat`), where the command is `pacman`
and `DEVKITPRO`/`DEVKITPPC` are already set. On Linux and macOS, use `sudo
dkp-pacman` and export `DEVKITPRO=/opt/devkitpro` and
`DEVKITPPC=$DEVKITPRO/devkitPPC`.

### 2. Install the host tools

| Tool | Needed for | Windows (devkitPro MSYS2) | Debian/Ubuntu | macOS (Homebrew) |
| --- | --- | --- | --- | --- |
| C compiler, zlib | WiiPAX, banner tools, host `wiiload`, tests | `pacman -S gcc zlib-devel` | `apt install build-essential zlib1g-dev` | Xcode command-line tools |
| `xxd` | WiiPAX stub embedding | `pacman -S vim` | `apt install xxd` | included |
| `msgfmt` | translations | included | `apt install gettext` | `brew install gettext` |
| Python 3 | WAD packaging and tests | [python.org](https://www.python.org/) | `apt install python3-venv` | `brew install python` |
| libpng headers, SoX | channel banner (WAD only) | see below | `apt install libpng-dev sox` | `brew install libpng sox` |

devkitPro's Windows MSYS2 does not carry host libpng or SoX, so on Windows the
WAD banner needs a separate [MSYS2](https://www.msys2.org/) UCRT64 shell with
`pacman -S mingw-w64-ucrt-x86_64-{gcc,libpng,sox}`, or WSL. The DOL build does
not need either.

### 3. Build and check the channel DOL

From the repository root:

    make -C wiipax
    make -C channel/channelapp channel
    tests/stub_layout.sh
    tests/host_bounds.sh
    python3 tests/dolphin_smoke.py

The result is `channel/channelapp/channelapp-channel.dol`. It does not need
the Wii common key. `tests/dolphin_smoke.py` boots it in a throwaway Dolphin
profile and passes when the running DOL reports this release's version over
Dolphin's emulated network. Set `DOLPHIN` if Dolphin is not in a standard
location. Validated on Windows 11 and Intel macOS.

### 4. Build the retail WAD

Create the project virtual environment (`.venv/Scripts/python` on Windows,
`.venv/bin/python` elsewhere), then build:

    python3 -m venv .venv
    .venv/bin/python -m pip install -r requirements.txt
    make -C channel PYTHON="$(pwd)/.venv/bin/python"
    make -C channel/title PYTHON="$(pwd)/.venv/bin/python" check

The build reads a 16-byte Wii common key from `~/.wii/common-key`. Obtain it
from your own Wii's BootMii `keys.bin`: [BackupMii documents](https://wiibrew.org/wiki/BackupMii)
the common key at offset `0x114` for 16 bytes. Keep `keys.bin` and the extracted
key private. The `keys/` folder is for your own copies; Git ignores everything
in it, along with loose key files anywhere in the tree. The repository already
includes the retail ticket, TMD, certificate, and footer templates. The
optional `dpki` target needs separate private signing keys and is not part of
this retail build. `wadpack.py` refuses a content whose size or SHA-1 does not
match the TMD. The resulting file is `channel/title/channel_retail.wad`. NAND
save and theme storage need the channel's title identity and permissions, so
they do not work properly from a direct DOL launch.

## Testing on a real Wii

See [the project skill](.agents/skills/hbc-build-and-review/SKILL.md) for the
full procedure. In short, the safe order is:

1. **DOL over Wiiload (no NAND writes).** With an existing Homebrew Channel on
   the Wii, send the DOL and confirm its version:

       WIILOAD=tcp:<wii-ip> wiiload channel/channelapp/channelapp-channel.dol
       python3 tests/wii_version.py <wii-ip>

   A power cycle always returns the Wii to its previous state.
2. **Installed WAD (writes NAND).** Only after the DOL passes, and only with a
   BootMii NAND backup and Priiloader in place. This channel's title ID is
   `00010001-4F484243` (`OHBC`), so it installs next to the official Homebrew
   Channel (`LULZ`) instead of replacing it.

## License

Unless otherwise noted in an individual file header, all source code in this
repository is released under the terms of the GNU General Public License,
version 2 or later. The full text of the license can be found in the COPYING
file.
