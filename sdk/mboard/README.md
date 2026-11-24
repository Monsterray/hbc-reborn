# mboard

*Post to the Wii Message Board, the way a game does.*

| | |
| --- | --- |
| **Files** | [`mboard.h`](mboard.h) and [`mboard.c`](mboard.c) |
| **Needs** | libogc; the internet only for sending; AHBPROT only for the play history |
| **Licence** | Public domain |
| **Changes on the Wii** | Writes the WiiConnect24 mail boxes and Address Book on the NAND; the play history needs AHBPROT (see [below](#what-it-changes-on-the-wii)) |
| **Since** | HBC 1.10.9 |

**Contents:** [What it does](#what-it-does) · [Quick start](#quick-start) ·
[Add it to your app](#add-it-to-your-app) · [API](#api) ·
[What it changes on the Wii](#what-it-changes-on-the-wii) · [Details](#details) ·
[Limits](#limits) · [Docs](#docs) · [Licence](#licence)

## What it does

- Posts messages to this Wii's board.
- Sends them to other Wiis, through the WiiConnect24 mail server (WiiLink's
  today).
- Adds lines to the day's play history.
- Reads and edits the Address Book.

Only sending needs the internet, and nothing deletes what is already on the
board. HBC's own play log is this library's writer.

## Quick start

```c
#include "mboard.h"

// A message on today's page; the Wii Menu files it when it next starts.
mboard_post("WiiStation", "Played Final Fantasy VII\nfor 42 minutes.");

// The same with a Mii icon, a photo and a Reply button.
static uint8_t face[MBOARD_FACE_SIZE];
mboard_msg m = { 0 };
m.title = "WiiStation";
m.body = "Played Wii Sports\nfor 5 minutes.";
if (mboard_mii(0, face) == MBOARD_OK)
    m.face = face;
m.jpeg = photo;
m.jpeg_size = photo_size;
m.reply = 1;
mboard_post_msg(&m);

// To another Wii, by its 16-digit Wii number (both on WiiLink, each in the
// other's Address Book).
mboard_send_msg(1234567890123456ull, &m);

// A line in today's play history (needs <ahb_access/> in meta.xml).
mboard_log_play("Final Fantasy VII", "WSFF7", 42 * 60);

// The Address Book: every contact, and changing one.
mboard_friend list[MBOARD_FRIENDS];
int n = mboard_friends(list, MBOARD_FRIENDS);
mboard_friend_set(list[0].slot, "Living Room", face);
```

Each call returns a result of 0 or more, or a negative `MBOARD_E_*` code
(`mboard_strerror()` names it). The library checks every file before it
writes, and never deletes anything already on the board.

## Add it to your app

Copy `mboard.h` and `mboard.c` into your project. For the play history, add
`<ahb_access/>` to your `meta.xml`.

## API

Every call returns 0 or more on success, or a negative `MBOARD_E_*` code.
Call from one thread at a time, with ISFS free to use. Only `mboard_send_msg`
touches the network (through KD).

**Post and send**

| Name | What it does |
| --- | --- |
| `int mboard_post(title_utf8, body_utf8)` | A message on this Wii's board, with no Reply button. A `\n` in the body starts a new line. |
| `int mboard_post_msg(&msg)` | The same, with everything in the `mboard_msg` (see [the message](#the-message)). |
| `int mboard_send_msg(to_wii_number, &msg)` | The same message to another Wii by its 16-digit Wii number. `MBOARD_OK` when KD took the upload request, `MBOARD_QUEUED` when it waits for KD's next round. The other Wii files it when it next fetches mail, if each Wii has the other in its Address Book. |
| `int mboard_mii(slot, face)` | Reads slot 0-99 of this Wii's Mii database (`/shared2/menu/FaceLib/RFL_DB.dat`) into `MBOARD_FACE_SIZE` (74) bytes, for `mboard_msg.face` or a contact's Mii. `MBOARD_E_NO_MII` for an empty slot. |
| `const char *mboard_strerror(result)` | Names a result code. |

**Play history**

| Name | What it does |
| --- | --- |
| `int mboard_log_play(name_utf8, id, seconds)` | A line in today's "Today's Play History", ending now, as the Wii Menu logs a disc. `name`: UTF-8, up to 40 UTF-16 units kept. `id`: 1-6 ASCII letters or digits that tell titles apart (a disc's game ID, a channel's four letters). Lines with the same name and ID on one day are added together. Needs AHBPROT: IOS's NAND permission check is lifted for the write and put back. Returns `MBOARD_OK` (a new line today), `MBOARD_COMBINED` (the title's line today got longer) or `MBOARD_CREATED` (today's first line, a new message). |

**Address Book** (`/shared2/wc24/nwc24fl.bin`). Each call pauses KD around the file, as KD reads and writes it too.

| Name | What it does |
| --- | --- |
| `int mboard_friends(out, max)` | Every contact, up to `max`; returns how many. At most `MBOARD_FRIENDS` (100). |
| `int mboard_friend_add(wii_number, nick_utf8, mii)` | Adds a Wii as the Wii Menu does: pending until both Wiis have registered each other (KD sends the registration). `nick`: UTF-8, 1-11 UTF-16 units. `mii`: a Mii (74 bytes) to show for it, or NULL. Returns the slot, `MBOARD_E_FULL`, or `MBOARD_E_ARG` (a number already there, or bad). |
| `int mboard_friend_set(slot, nick_utf8, mii)` | Changes a contact's nickname and Mii (either NULL: keep it). The address cannot change: remove the contact and add it again. Returns the slot. |
| `int mboard_friend_remove(slot)` | Removes a contact. |

**Cores for any storage.** The calls above read the console's settings and
clock, open the files through IOS, and call these. `tests/host/mboard_host` runs
them on PC images. Define `MBOARD_NO_OGC` to build `mboard.c` without libogc:
then only these exist (HBC builds it so, for its play log).

| Name | What it does |
| --- | --- |
| `mboard_utf16(out, max, utf8)` | UTF-8 to UTF-16, at most `max` units; a bad sequence becomes U+FFFD. Returns the units written. |
| `mboard_post_box(ctl, &mbx, &env, &msg)` | Posts into `wc24recv.ctl` (`MBOARD_RECV_CTL_SIZE`, 0x8000) and `wc24recv.mbx`. `ctl` is updated in place; write it back when the result is `MBOARD_OK`. |
| `mboard_send_box(ctl, &mbx, &env, to, &msg)` | The same for `wc24send.ctl` (`MBOARD_SEND_CTL_SIZE`, 0x4000) and `wc24send.mbx`. This Wii's own number as `to` is `MBOARD_E_ARG`: post that instead. |
| `mboard_play_vff(&cdb, &session, now, wiiid)` | Writes a play session into the Wii Menu's `cdb.vff`. `wiiid`: the console's 8-byte ID (`data/nocopy/cdbwiiid.dat`) for a new message; NULL takes it from a message already in the file. |
| `mboard_book_parse`, `_add`, `_set`, `_remove` | The Address Book on a whole `nwc24fl.bin` image (`MBOARD_FRIENDS_SIZE`, 32864 bytes), changed in place; write it back when the result is 0 or more. |

Types: `mboard_io { ctx, read, write }` reads and writes a file by byte offset
(0 on success). `mboard_env` holds the Wii number, the mail domain
(`"@rc24.xyz"` for WiiLink, `"@wii.com"`), the posting title's four letters
and maker code, the UTC time and the local time in `MBOARD_TICKS_PER_SEC`
(60,750,000 a second since 2000-01-01, libogc's `gettime()`).

### The message

`mboard_msg` fields:

| Field | Meaning |
| --- | --- |
| `title` | UTF-8: the sender's name. At most `MBOARD_TITLE_MAX` (34) UTF-16 units; more is cut. |
| `body` | UTF-8; `\n` starts a new line. At most `MBOARD_BODY_MAX` (1000) UTF-16 units; more is cut. |
| `face` | A Mii, `MBOARD_FACE_SIZE` (74) bytes, shown as the icon; or NULL. |
| `jpeg`, `jpeg_size` | A photo, a JPEG (the Wii Menu converts it); or NULL. |
| `board`, `board_size` | Board data, a U8 archive (`application/x-wii-msgboard`): stationery and/or a `chjump.bin`, a button that opens a channel; or NULL. At most `MBOARD_BOARD_MAX` (0x1E000). |
| `reply` | Nonzero: a Reply button. |
| `delay_hours` | 0, or hold it back this long before it shows (1-`MBOARD_DELAY_MAX`, 240). |
| `file_year`, `file_month`, `file_day` | 0, or the date it is filed under: year 2000-2035, month 1-12. |

The photo and board data together must stay under `MBOARD_ATTACH_TOTAL`
(0x245B0). These are the SDK's `NWC24SetMsgAttached()` limits.

A Mii is 74 bytes. `mboard_mii()` reads one from the Wii's Mii database, and
[sdk/mii](../mii) builds, checks and stores Miis; `mii_pack()` gives the 74
bytes `m.face` takes.

### Results

| Code | Value | Meaning |
| --- | --- | --- |
| `MBOARD_OK` | 0 | Done; for `mboard_log_play`: a new line today. |
| `MBOARD_COMBINED` | 1 | `mboard_log_play`: the title's line today got longer. |
| `MBOARD_CREATED` | 2 | `mboard_log_play`: today's first line, a new message. |
| `MBOARD_QUEUED` | 3 | `mboard_send_msg`: in the send box; KD would not send it now, so it goes on WiiConnect24's next round. |
| `MBOARD_E_IO` | -1 | A NAND read or write failed part way. |
| `MBOARD_E_FORMAT` | -2 | A file is not as expected: nothing was written. |
| `MBOARD_E_FULL` | -3 | A mailbox, today's 12 play titles, or the Address Book is full. |
| `MBOARD_E_MEM` | -4 | Out of memory. |
| `MBOARD_E_ARG` | -5 | A bad argument: empty text, a bad ID, Wii number, nickname, date or slot, or a Mii with no creation ID. |
| `MBOARD_E_BUSY` | -6 | WiiConnect24 is using its files now: try again later. |
| `MBOARD_E_NO_ID` | -7 | This Wii has no Wii number (`nwc24msg.cfg`). |
| `MBOARD_E_DENIED` | -8 | IOS refused a file (for `cdb.vff`: no AHBPROT). |
| `MBOARD_E_TOO_BIG` | -9 | A photo or board data over its limit, or the whole message. |
| `MBOARD_E_NO_MII` | -10 | `mboard_mii()`: no Mii in that slot. |

## What it changes on the Wii

| Part | What changes | How long it lasts |
| --- | --- | --- |
| Posting | Writes into this Wii's WiiConnect24 receive box, as a game's message to its own Wii is delivered. The Wii Menu files it on the board the next time it starts. | On the board like any other message; the library itself never deletes one (`mboard.c`), and a full receive box refuses the post rather than dropping old mail. |
| Sending | Writes into the send box and asks KD (IOS's WiiConnect24 service) to upload it now. | Until KD has sent it. |
| Play history | Writes the Wii Menu's own `cdb.vff`. IOS's NAND permission check is lifted for the write and put back. Needs AHBPROT. | For good. |
| Address Book | Reads and edits `/shared2/wc24/nwc24fl.bin`, with KD paused around the file. | For good. |

Posting needs the Wii's Wii number (any Wii that is set up has one), and no
internet, no WiiConnect24 service and no AHBPROT. Sending needs both Wiis to
have a WiiConnect24 account that works (WiiLink's, today) and each other in
the Address Book. The library writes all of its change or none of it as far as
a reader sees: a file it does not expect is left alone (`MBOARD_E_FORMAT`),
nothing is deleted to make room (`MBOARD_E_FULL`), and new data lands before
anything that points to it.

## Details

- **Stationery:** a letter's own stationery (its border, its icon on the
  board, and music) is board data. Make it on the PC with
  [tools/msgboard/stationery.py](../../tools/msgboard/stationery.py), and pass
  it as `m.board`.
  [tools/msgboard/stationery_themes.py](../../tools/msgboard/stationery_themes.py)
  makes five examples.
- **Channel button:** a `chjump.bin` in the board data adds a button that
  opens a channel.
- **Mii icon:** `mboard_mii()` reads a Mii from the Wii's Mii database.
- **Delay and filing date:** `delay_hours` holds a message back;
  `file_year`, `file_month` and `file_day` choose the date it is filed under.
- **Play log:** HBC's own play log is this library's writer.

## Limits

- Call from one thread at a time, with ISFS free to use.
- The play history needs AHBPROT (`<ahb_access/>` in `meta.xml`).
- Today's play history holds at most 12 titles (`MBOARD_E_FULL`).
- Sending needs a working WiiConnect24 account on both Wiis and each in the
  other's Address Book. The address of an existing contact cannot change.
- `MBOARD_E_BUSY`: WiiConnect24 is using its files; try again later.
- Unexpected file formats are left alone, never repaired.

## Docs

- [docs/mboard/README.md](../../docs/mboard/README.md): every call, the files and IOS
  calls, what can go wrong, and the proof on a Wii.
- [docs/mboard/storage.md](../../docs/mboard/storage.md): how
  the board stores a message, and what sets the sender, icon, contents and
  stationery.
- [docs/mboard/play-log.md](../../docs/mboard/play-log.md): the play history.
- [docs/mii/README.md](../../docs/mii/README.md): Mii data, and what makes a Mii valid.

## Licence

Public domain: copy, change and ship it in anything, open or closed, with no
credit needed.
