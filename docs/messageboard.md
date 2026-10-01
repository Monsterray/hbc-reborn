# The Wii Message Board's play log

How the Wii Menu (4.3U, v513) records play time, worked out in an isolated
Dolphin profile (2026-09-30) so HBC can log each app it runs. Tools are in
`tools/msgboard/`; nothing here was tested on a real Wii's Message Board yet.

## play_rec.dat

`/title/00000001/00000002/data/play_rec.dat`, 128 bytes, big-endian:

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | checksum: the sum of the 31 words that follow |
| 0x04 | 84 | title name, UTF-16BE, NUL-padded |
| 0x58 | 8 | start, in ticks |
| 0x60 | 8 | last seen, in ticks |
| 0x68 | 6 | title ID as ASCII (a disc ID, or a channel's four letters) |
| 0x6e | 18 | zero |

Ticks are 60,750,000 per second since 2000-01-01 in the Wii's local time (the
RTC plus SYSCONF's counter bias, which DEV > Sync clock keeps right). The Wii
Menu writes a record before it launches a title; SDK titles update "last seen"
as they run. At its next start the Wii Menu adds a valid record to the day's
play log message and deletes the file. Only one record survives between two
Wii Menu starts, which is why HBC writes the log itself.

## cdb.vff

`/title/00000001/00000002/data/cdb.vff` (20 MiB) is a FAT16 volume with no
boot sector: a VFF header, FAT copies at 0x20 and 0x14020, a 0x1000-byte root
directory at 0x28020, and 512-byte clusters from 0x29020 (cluster 2).

A day's play log is one file:

    /YYYY/MM/DD/hh/mm/HAEA_#1/log/XXXXXXXX.000

A record belongs to the day its end falls on. When that day has a message,
the Wii Menu rewrites it with the record appended. Otherwise it creates one:
the folders are the record's end time (local, months counted from 0), the
name is that time in seconds since 2000 in hex, the message number is
`cdb.conf` plus one, and `cdb.conf` is updated. Directory entries are 8.3,
attribute 0x10 for folders and 0x20 for the message, FAT timestamps from the
clock; each folder starts with `.` and `..`, and `log` alone also has a
long-name entry (short name `LOG`).

## The playtimelog message

Big-endian; offsets from the start of the file:

| Offset | Size | Field |
| --- | --- | --- |
| 0x000 | 8 | `CDBFILE`, version 2 |
| 0x008 | 8 | the first 8 bytes of `data/nocopy/cdbwiiid.dat` (per console) |
| 0x010 | 4+12 | type length 12, **little-endian** unlike every other field, then `playtimelog\0` |
| 0x070 | 4 | message number: `/cdb.conf` (4 bytes) holds the last one used |
| 0x074 | 4 | number of titles in the list |
| 0x07c | 4 | the newest record's end, seconds since 2000 |
| 0x400 | 4 | `RI_5` |
| 0x40c | 4 | 2 |
| 0x410 | 8 | the clock when the message was written, ticks |
| 0x518 | 16 | text section: type 1, offset 0x148 (from 0x400), size 0x178, 0 |
| 0x528 | 16 | list section: type 3, offset (from 0x400), size 0x668, 0 |
| 0x540 | 4 | CRC-32 of bytes 0x400-0x53f |
| 0x548 | | subject, then the message, UTF-16BE, each NUL-terminated |
| list | 0x668 | `03_0`, 4 zero bytes, then up to 12 entries of 0x88 |
| | 0x18 | zero |

The list starts at the first 32-byte boundary after the text. An entry is a
little-endian 1, a zero word, then a copy of the 128-byte play_rec.dat record.
The text is generated from the list (English shown):

    Today's Accomplishments
    Today's Play History

    <name>
         hh:mm

    ...

    Total Play Time
         hh:mm

## What HBC logs

`channel/channelapp/source/playtime.c` drives `cdblog.c`:

- HBC's own time, from when it started (or when the Wii Menu launched it, by
  the Wii Menu's fresh play_rec.dat) until it leaves: to an app, to the Wii
  Menu, or at power-off. As "Homebrew Channel", ID `OHBC` (or `LULZ`).
- Each app it launches, from the launch until HBC starts again through its
  reload stub. An app from the menu goes by the name in its `meta.xml`, else
  its folder. A Wiiload upload goes by, in order: the name its agent gave
  itself (`cfg.name`, from the kept log), the name its sender gave (`HBCA`:
  `hbc.py send` sends the folder of an `apps/NAME/boot.dol`, the file's
  name, or `--name`), the file it was sent as when that is a `.dol`/`.elf`,
  else "Wiiload". An app's arguments (a ROM, an option) are never its name. The launch is kept in HBC's data folder
  (`playlog.bin`). When the Wii Menu runs in between (the app went there, or
  the Wii was switched off), the app's end is unknown and it is not logged.
- A title already in the day's message gets a longer line: the Wii Menu
  itself would list every session, and a day holds 12 titles.
- HBC still spoils play_rec.dat, so the Wii Menu does not log HBC again.
- `cdb.vff` and `cdbwiiid.dat` are the Wii Menu's: IOS refuses HBC (-102).
  With AHBPROT, HBC lifts IOS's NAND permission check (one Thumb branch in the
  FS module, the signature libruntimeiospatch also uses) for the moment it
  writes, and puts it back. Without AHBPROT, or as a DOL with no installed
  title, there is no play log.
- `HBCS` `startup` gets `playlog_logged` or `playlog_failed`; DEV > Log shows
  each line HBC adds ("Play log: 12 min, added").

## Dolphin profile

`tools/msgboard/dolphin_sysmenu.py` runs a separate profile
(`Documents/hbc-dolphin-sysmenu`). It needed:

- The Wii Menu, downloaded like Dolphin's online update
  (`tools/msgboard/nus_sysmenu.py`: the title list from Dolphin's NetUpdateSOAP
  stand-in, encrypted files from Nintendo's CDN, packed into a WAD that Dolphin
  installs). IOS80 is not needed under Dolphin's IOS emulation.
- `data/nocopy/`, which a WAD install does not create; without it the Wii Menu
  cannot write `cdbwiiid.dat`.
- SYSCONF `IPL.CD` and `IPL.CD2` set to 1 (`tools/msgboard/sysconf_cd.py`).
  Dolphin's default 0 means initial setup is not done: the Wii Menu then resets
  SYSCONF and deletes `cdb.vff` on every boot.
- A scripted Wii Remote (`tools/msgboard/dsu_pad.py`, the DSU protocol) to
  press A past the Health and Safety screen: the Wii Menu adds play_rec.dat to
  the log only after that.

Checked in Dolphin with `tools/msgboard/cdb_log.py`, the reference for HBC's
writer: an entry added to the Wii Menu's message, and a day's message created
from nothing, were both accepted; the Wii Menu then merged a play_rec.dat
record into each, keeping ours. A created message with the type length
written big-endian was not: the Wii Menu dropped its record without a word.
