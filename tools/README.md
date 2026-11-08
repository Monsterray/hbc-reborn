# Tools

Command-line tools for working on HBC and on Wii homebrew. Each one takes
`--help` (a description, examples and its exit codes) and does nothing when
imported. Python 3.10 or later; none needs more than the standard library
unless its row says so.

## Every day

| Tool | What it does |
| --- | --- |
| [`hbc.py`](hbc.py) | Develop for the Wii from a PC through HBC's network tools: status, files, send and run apps, their output. The reference is [docs/hbc-py.md](../docs/hbc-py.md). |
| [`wii_paths.py`](wii_paths.py) | Where the shared Wii tools are on this workstation (hbc-reborn, the bench queue, Dolphin, the capture card), with no hard-coded paths. `--all` lists them. |
| [`dsu_pad.py`](dsu_pad.py) | A scripted Wii Remote for Dolphin over the DSU (cemuhook) protocol, so tests drive a menu without touching the desktop. |

## For contributors

| Tool | What it does |
| --- | --- |
| [`wiki.py`](wiki.py) | Builds the GitHub wiki from the README, the guides and the SDK READMEs. It never pushes. |
| [`memmap.py`](memmap.py) | Where everything in an ELF lives: one CSV row per symbol, with its address, size, section and object file. |
| [`mii_res.py`](mii_res.py) | Studies a Wii's Mii face resource, `RFL_Res.dat` ([docs/mii/rendering.md](../docs/mii/rendering.md)). |
| [`fix_line_endings.py`](fix_line_endings.py) | Brings a checkout made before 1.8.3 to LF line endings. |

## Message Board ([`msgboard/`](msgboard/))

| Tool | What it does |
| --- | --- |
| [`stationery.py`](msgboard/stationery.py) | Makes Message Board stationery: the board data that gives a letter its own border, icon and music ([sdk/mboard](../sdk/mboard/README.md)). |
| [`stationery_themes.py`](msgboard/stationery_themes.py) | Five example stationery themes, drawn and scored here. Needs Pillow. |
| [`cdb_log.py`](msgboard/cdb_log.py) | The reference play log writer that sdk/mboard's C writer is checked against byte for byte. |
| [`vff.py`](msgboard/vff.py) | Lists or dumps the FAT16 volume inside a `cdb.vff`. |
| [`nus_sysmenu.py`](msgboard/nus_sysmenu.py) | Downloads the USA Wii Menu 4.3U and its IOS as WADs, the way Dolphin's system update does, for the Wii Menu profile. |
| [`dolphin_sysmenu.py`](msgboard/dolphin_sysmenu.py) | Runs Dolphin on that profile: installs a WAD, or boots the Wii Menu. |
| [`sysconf_cd.py`](msgboard/sysconf_cd.py) | Marks the profile's initial setup done (`IPL.CD`), without which the Wii Menu wipes `cdb.vff` on every boot. |

The profile and what the Wii Menu needs are in
[docs/mboard/play-log.md](../docs/mboard/play-log.md).

## Elsewhere

- [`wii-bench/`](wii-bench/): the bench queue. It now lives in the wii-bench
  repository; this copy stays until the live queue moves over
  ([docs/plans/2026-11-reorg.md](../docs/plans/2026-11-reorg.md), phase 1).
- [`research/`](research/README.md): one-off scripts kept as the record of how
  something was found. They are not maintained.
