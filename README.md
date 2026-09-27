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
The 1.1.6 DOL also answered a live version query on the dev Wii.
The retail WAD builds with the current toolchain. It has not yet been installed
or tested on a Wii.

The current channel release is **1.1.6**. The displayed channel version and
retail WAD title version use the same SemVer value. The Wii TMD stores a
16-bit title version, so packaging encodes `major.minor.patch` as 5/6/5 bits
(`major << 11 | minor << 5 | patch`), preserving version order within those
field limits (major 0–31, minor 0–63, patch 0–31). The channel update protocol
also carries a numeric `YYYYMMDDHHMM` release timestamp for availability checks.

## Build instructions

You need devkitPPC and libogc installed, and the DEVKITPRO/DEVKITPPC environment
variables correctly set. The channel app builds with devkitPPC r50-1 and libogc
3.1.0. Make sure you have libogc/libfat, and also install these PowerPC libraries:

* zlib
* libpng
* mxml
* freetype
* bzip2
* brotli

You can obtain binaries of those with
[devkitPro pacman](https://devkitpro.org/wiki/devkitPro_pacman). Simply use

    sudo dkp-pacman -S ppc-zlib ppc-libpng ppc-mxml ppc-freetype ppc-bzip2 ppc-brotli

The host also needs libpng headers to build the banner tools.

The channel app build has also been tested on Intel macOS with the versions
above. From the repository root, run:

    make -C wiipax
    make -C channel/channelapp channel

The resulting `channel/channelapp/channelapp-channel.dol` can be run in Dolphin
or sent to a Wii running Homebrew Channel with wiiload. It draws the menu and
accepts a Wii64 DOL through wiiload on the dev Wii. This build does not need the
Wii common key.
See the [project skill](.agents/skills/hbc-build-and-review/SKILL.md) for an
isolated Dolphin profile command and the dev Wii version query.

The full retail WAD build also needs Python 3, PyCryptodomex, `msgfmt`, SoX,
and host libpng headers. On macOS, install the host tools in their standard
Homebrew locations with `brew install gettext sox libpng`. Install the Python
dependency in a project virtual environment, then build from the repository
root:

    python3 -m venv .venv
    .venv/bin/python -m pip install -r requirements.txt
    make -C wiipax
    make -C channel PYTHON="$(pwd)/.venv/bin/python"
    make -C channel/title PYTHON="$(pwd)/.venv/bin/python" check

The build reads a 16-byte Wii common key from `~/.wii/common-key`. Obtain it
from your own Wii's BootMii `keys.bin`: [BackupMii documents](https://wiibrew.org/wiki/BackupMii)
the common key at offset `0x114` for 16 bytes. Keep `keys.bin` and the extracted
key private; never add either to Git. The repository already includes the
retail ticket, TMD, certificate, and footer templates. The optional `dpki`
target needs separate private signing keys and is not part of this retail
build. The resulting file is `channel/title/channel_retail.wad`. NAND save and
theme storage need the channel's title identity and permissions, so they do
not work properly from a direct DOL launch.

## License

Unless otherwise noted in an individual file header, all source code in this
repository is released under the terms of the GNU General Public License,
version 2 or later. The full text of the license can be found in the COPYING
file.
