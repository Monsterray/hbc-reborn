# sdk/mboard: the Wii Message Board from homebrew

`sdk/mboard/mboard.[ch]` lets an app use the Wii Message Board as a game
does. It can put "played Final Fantasy VII for 42 minutes" on the board, send
a letter to another Wii, add to the day's play history, and read and edit the
Address Book. It needs nothing beyond libogc, and the files are public
domain.

This note covers each call, the files and IOS calls behind them, what can
go wrong, and the proof on real Wiis. It was worked out in October 2026 from:

- Dolphin;
- the Revolution SDK (SMGCommunity/Petari) and Wii Menu (koopthekoopa/wii-ipl)
  decompilations;
- WiiLink's mail server (WiiLink24/Mail-Server);
- the bench Wii and LRWii.

[messageboard-messages.md](messageboard-messages.md) has the board's
message and record formats, [messageboard.md](messageboard.md) the play
history, and [mii.md](mii.md) Mii data.

```c
#include "mboard.h"

mboard_post("WiiStation", "Played Final Fantasy VII\nfor 42 minutes.");
mboard_log_play("Final Fantasy VII", "WSFF7", 42 * 60);   // needs AHBPROT
```

## At a glance

| Call | What it does | Writes | Needs | Network |
| --- | --- | --- | --- | --- |
| `mboard_post(title, text)` | a message on this Wii's board, shown when the Wii Menu next starts | `/shared2/wc24/mbox/wc24recv.{ctl,mbx}` | the Wii's Wii number (any Wii set up has one) | no |
| `mboard_post_msg(&msg)` | the same, with a Mii, a photo, stationery or a channel button, Reply, a delay or a filing date | the same | the same | no |
| `mboard_send_msg(number, &msg)` | that message to another Wii | `/shared2/wc24/mbox/wc24send.{ctl,mbx}`, then KD uploads it | a WiiConnect24 account on both Wiis (WiiLink's today), each in the other's Address Book | yes |
| `mboard_log_play(name, id, seconds)` | a line in today's "Today's Play History", at once | `/title/00000001/00000002/data/cdb.vff` | AHBPROT (`<ahb_access/>` in meta.xml) | no |
| `mboard_mii(slot, face)` | one Mii from this Wii's Mii database | nothing | | no |
| `mboard_friends`, `mboard_friend_add`, `_set`, `_remove` | the Address Book: every contact; add a Wii; change a nickname or Mii; remove one | `/shared2/wc24/nwc24fl.bin` | | no |

Every call returns a result of 0 or more, or a negative `MBOARD_E_*` code
(`mboard_strerror()` names it). A call writes all of its change or none of
it, as far as a reader sees.

## In an app

- **Setup.** Copy `mboard.c` and `mboard.h`, and build with libogc. Call
  from one thread, not during disc or NAND-heavy work.
- **When posts show.** A post shows when the Wii Menu next starts, not while
  the app runs. Without AHBPROT, `mboard_post` and `mboard_post_msg` are the
  calls to use.
- **The play history.** `mboard_log_play` needs `<ahb_access/>` in
  meta.xml. HBC 1.8.9 and later already log the app itself (its meta.xml
  name), so the day's total counts the app's session and its games' lines
  both.
- **Sending** needs, on both Wiis:
  - WiiConnect24 on, with its user agreement accepted;
  - WiiLink's mail patch (`@rc24.xyz` in `nwc24msg.cfg`);
  - the other Wii registered in the Address Book.

  The letter arrives when the other Wii next fetches mail.
- **Retries.** `MBOARD_E_BUSY` (KD is using its files) is worth one retry a
  few seconds later. The other errors are not worth retrying.
- **Memory.** A photo and board data are read from the caller's memory while
  the message is written, and the encoded message needs about 1.4 times
  their size in free memory.

## Posting a message

`mboard_post(title, text)` posts a title (the sender's name) and text, with
no Reply button. `mboard_post_msg(&msg)` posts everything else a game's
message can carry:

```c
static uint8_t mii[MBOARD_FACE_SIZE];
mboard_msg m = { 0 };

m.title = "WiiStation";
m.body = "Played Wii Sports\nfor 5 minutes.";
if (mboard_mii(0, mii) == MBOARD_OK)   // the Wii's first Mii, as the icon
    m.face = mii;
m.jpeg = photo; m.jpeg_size = photo_size;   // a JPEG: the board shows it
m.board = arc; m.board_size = arc_size;     // stationery and/or a channel button
m.reply = 1;                                // a Reply button
mboard_post_msg(&m);
```

| Field | Header or part | Limit (the SDK's) | Refused with |
| --- | --- | --- | --- |
| `title`, `body` | `X-Wii-AltName`; the text part | 34 and 1,000 UTF-16 units (more is cut) | `MBOARD_E_ARG` (empty) |
| `face` | `X-WiiFace`: 74 bytes of `RFLCharData` | a creation ID; the Wii Menu also checks every part ([mii.md](mii.md)) | `MBOARD_E_ARG` (ID 0) |
| `jpeg` | an `image/jpeg` part; the Wii Menu converts it, and files the message as `odh` | starts `FF D8 FF`; the photo and board data together under 0x245B0 | `MBOARD_E_ARG`, `MBOARD_E_TOO_BIG` |
| `board` | an `application/x-wii-msgboard` part: a U8 archive of stationery (below) and/or `chjump.bin` (a button that opens a channel) | starts `55 AA 38 2D`; at most 0x1E000 | `MBOARD_E_ARG`, `MBOARD_E_TOO_BIG` |
| `reply` | no `X-Wii-MB-NoReply` | | |
| `delay_hours` | `X-Wii-MB-Delay`, the control entry's 0x2C and flag bit 24: held back from the board until then | 1-240 | `MBOARD_E_ARG` |
| `file_year`, `file_month`, `file_day` | `X-Wii-MB-RegDate` | 2000-2035, a real date | `MBOARD_E_ARG` |

The text, the encoded attachments and their headers together must fit the
receive box's largest message, 0x31C00 bytes, or the result is
`MBOARD_E_TOO_BIG`. `mboard_post` writes exactly what `mboard_post_msg` with
only a title and text does.

Two side effects:

- **A valid Mii also goes into the Wii's hidden Mii database.** The Wii Menu
  does that with any message's Mii.
- **A JPEG the Wii Menu cannot convert** shows as its question-mark picture.

## Sending to another Wii

`mboard_send_msg(to, &msg)` sends the same message to another Wii, given
that Wii's 16-digit Wii number, through the WiiConnect24 mail server.
Nintendo's server is gone, so both Wiis need WiiLink's account (`@rc24.xyz`
in `nwc24msg.cfg`, stage 2), and the internet.

**What it writes** is what `NWC24CommitMsg()` writes for a message to any
other Wii (Petari `NWC24MsgCommit.c`). It is the same as a post, except:

- **An SMTP envelope** comes first, and the control entry's offsets count it:
  ```
  MAIL FROM: w<this Wii>@rc24.xyz
  RCPT TO: w<the other>@rc24.xyz
  DATA
  ```
- **`To:`** names the other Wii.
- **The text is base64** (`Content-Transfer-Encoding: base64`, 57 bytes to a
  line), as the SDK writes it and the Wii Menu's own letters are sent, not
  raw UTF-16 in 8 bits as in a post.
  - **Why:** WiiLink's server keeps mail as text. Its `send.go` deletes every
    NUL byte, and its `receive.go` turns every LF into CRLF.
  - **What went wrong:** raw UTF-16 lost every other byte of its ASCII, and
    the text reached LRWii as CJK characters and question-mark boxes
    (2026-10-23).
  - The headers and attachments were already base64.
  - The entry's 0x70 keeps the text's size before encoding (the SDK's
    `WritePlainText`).
- **The box** is the send box, `/shared2/wc24/mbox/wc24send.{ctl,mbx}`:
  - the file is `/mb/s0000001.msg` (a post's is `r0000001.msg`);
  - the entry's flags have TO_SEND (0x10) where a post has TO_RECV (0x20);
  - the box's format differs ("The files", below).

**After writing**, it resumes KD and calls `/dev/net/kd/request` ioctl 0x0B
(`SEND_MAIL_NOW`). KD posts the box to the server's `send.cgi`, and deletes
each message it sent (Dolphin's `NetKDRequest.cpp`, `WC24Send.cpp`). If KD
refuses, the result is `MBOARD_QUEUED`, and the message waits for KD's next
round.

**WiiLink's server** (`send.go`):

- refuses a sender whose `mlid` and `passwd` (from `nwc24msg.cfg`) don't match
  its account;
- refuses a `From:` that isn't the sender;
- stores the message for any recipient with an account, without looking at
  friend lists.

**The receiving Wii.** Its Wii Menu files a letter from any Wii, and uses the
friend list only for the sender's name. Its KD, though, needs the sender in
its Address Book. Both Wiis also need the WiiConnect24 user agreement
accepted.

- **The Address Book:** LRWii received nothing until both Wiis had
  registered each other (Wii Menu, Address Book, Register, Wii Number). Then
  it filed the letter (2026-10-23).
- **The user agreement:** LRWii's User Agreements screen stayed black. Its
  EULA channel was an old one, HAKE version 1, which runs on IOS20, and
  Nintendo stubbed IOS20. HAKE version 3, on IOS56, fixed it.
  `tests/wii_eulacheck.py` finds this.

## Stationery: a letter's own look, icon and music

A letter's border, its icon on the board and a sound are board data,
`mboard_msg.board`. [`tools/msgboard/stationery.py`](../tools/msgboard/stationery.py)
makes it on the PC from images and a WAV:

    stationery.py LETTER.png THUMB.png [--sound tune.wav] [--chjump 000100014F484243] -o board.arc

The app ships the archive and passes it as `board`. The layout is the one
WiiLink's letters carry. Repacked with this tool, a copy of one is byte for
byte the same, and the Wii Menu files it. The Wii Menu's own rules for every
part are in [messageboard-messages.md](messageboard-messages.md).

- **The letter** is 512 x 376.
  - **The textures.** The tool cuts it into a 9-slice frame of TPL textures,
    `img/my_Letter_a.tpl` to `_i.tpl`: 64 | 384 | 64 wide, and 144 / 168 /
    64 high. They are RGB5A3 and keep their alpha, in an LZ77-compressed U8,
    `letter_LZ.bin`.
  - **The middle stays light.** The Wii Menu writes the text over it in a
    dark colour.
  - **The top band stays plain and dark** (about the first 60 rows). The Wii
    Menu writes the sender's name there in a light colour, which is why
    WiiLink's letters have a solid banner. A light top made the name
    unreadable on the first arcade letter.
- **The board icon** is 144 x 96 (`thumbnail_LZ.bin`, `img/my_LetterS_b.tpl`).
- **The sound** is `sound.bns`: mono DSP-ADPCM, 8 kHz as in WiiLink's letters.
  - **Formats.** The Wii Menu takes 4-48 kHz, in 1 or 2 channels.
  - **Quality.** The tool's encoder decodes back at 47 dB SNR on a tone.
  - **When it plays.** The Wii Menu plays it once, 300 ms after the letter
    opens. It mutes the board's music meanwhile, and fades the sound out as
    the letter closes.
  - **Loudness.** The board plays loud, so the themes keep their tunes at
    about a quarter of full scale.
- **`chjump.bin`**, optional, adds the "start" button for a channel. The Wii
  Menu shows it only if that channel is installed.
- **Size limits.** The whole archive must stay within 0x1E000 bytes. The
  letter must unpack to less than 400 KiB, and the icon to less than 30 KiB.

[`tools/msgboard/stationery_themes.py`](../tools/msgboard/stationery_themes.py)
makes five examples, drawn and scored in code:

| Theme | Size | Music |
| --- | --- | --- |
| `ocean` | 74,560 bytes | 5.0 s, an arpeggio |
| `sunset` | 50,880 bytes | none |
| `forest` | 69,280 bytes | none |
| `nightsky` | 95,200 bytes | 7.0 s, an original dreamy tune with an echo |
| `arcade` | 60,768 bytes | 2.0 s, a chiptune |

## The play history

`mboard_log_play(name, id, seconds)` adds a line to today's "Today's Play
History", ending now, as the Wii Menu logs a disc. Lines with the same name
and ID on one day are added together. It refuses a day that already has 12
titles (`MBOARD_E_FULL`).

- **Where it writes.** `cdb.vff` belongs to the Wii Menu, so IOS's NAND
  permission check is lifted for the moment of the write and put back (HBC's
  `playtime.c` patch).
- **The writer** is the one HBC's own play log uses: HBC builds `mboard.c`
  without its libogc half for it.
- **The format** is in [messageboard.md](messageboard.md).

## The Address Book

The Wii Menu's Address Book is `/shared2/wc24/nwc24fl.bin` (the SDK's
`NWC24FriendList.c`).

- **The header**, 0x360 bytes:
  - `WcFl`, version 2;
  - 100 slots, and the number in use;
  - 100 bits that KD sets once two Wiis have registered each other;
  - a changed flag at 0x3C;
  - each slot's Wii number, from 0x40.
- **Then 100 entries** of 0x140 bytes:
  - the type (1 a Wii, 2 an e-mail address);
  - a status (1 pending; KD's bit, not this, says confirmed);
  - the nickname, 12 UTF-16 units with its NUL;
  - the contact's Mii, as its creation ID (8 bytes at 0x20; 0 for none);
  - the address (a Wii's number, at 0x40).
- **No checksum.**

The Mii itself is not in the file. The Wii Menu shows the Mii with that
creation ID from the Wii's Mii database, `RFL_DB.dat`. That holds the Wii's
own Miis, plus a hidden database of Miis that came on messages.

```c
mboard_friend list[MBOARD_FRIENDS];
int n = mboard_friends(list, MBOARD_FRIENDS);         /* every contact */

static uint8_t mii[MBOARD_FACE_SIZE];
mboard_mii(0, mii);
mboard_friend_add(wii_number, "LR Wii", mii);         /* pending, as the Wii Menu adds one */
mboard_friend_set(slot, "Living Room", mii);          /* nickname and Mii only */
mboard_friend_remove(slot);
```

Each call writes what the SDK's `NWC24WriteFriendInfo`, `UpdateFriendInfo`
and `DeleteFriendInfo` write, byte for byte. Each pauses KD around the file,
since KD reads and writes it too. The file-level cores are
`mboard_book_parse`, `_add`, `_set` and `_remove`.

- **Adding** leaves the contact pending. KD sends the registration, and sets
  the confirmed bit when the other Wii's arrives.
- **The address cannot change:** remove the contact and add it again.
- **A nickname** is 1-11 UTF-16 units. More is refused, not cut.
- **Not done:**
  - setting the confirmed bit by hand (KD would never register the contact);
  - writing Miis into `RFL_DB.dat` (it has a CRC).

## How it works

### How games post: the SDK's loop-back

The first plan assumed that Mario Party 8 and 9 post through the
WiiConnect24 send path. They do not post at all:

- **Mario Party 8:** its `main.dol` has no NWC24 code (the
  mariopartyrd/marioparty8 decomp agrees).
- **Mario Party 9:** it links only the SDK's start-up and shutdown calls
  (`NWC24iSetRtcCounter`, `NWC24SuspendScheduler`).
- **Their board entries** are the Wii Menu's own play history, from
  `play_rec.dat` ([messageboard.md](messageboard.md)).

Games that do post, such as Super Mario Galaxy with its letters, use the SDK's
`NWC24CommitMsg()`. With exactly one recipient equal to the Wii's own number,
it writes the message straight into the **receive** box (`LoopBackEnable`,
Petari `NWC24MsgCommit.c`). The message gets no SMTP envelope and never
leaves the console. The Wii Menu files new receive-box messages onto the
board when it starts.

`mboard_post` writes exactly what that call writes for a
`NWC24_MSGTYPE_WII_MENU` message, as SMG sends it: an alt name, UTF-16 text
and no attachment. A message to any other Wii goes to the send box instead,
and from there to the mail server (`mboard_send_msg`).

### The files

#### nwc24msg.cfg (read only)

`/shared2/wc24/nwc24msg.cfg` is 0x400 bytes, big-endian, with `nwc24msg.cbk`
as its backup.

| Offset | Field |
| --- | --- |
| 0x00 | `WcCf`, version 8 |
| 0x08 | the Wii number (u64) |
| 0x10 | the ID-generation count (under 32) |
| 0x14 | the stage: 1 generated, 2 registered |
| 0x18 | the account domain: `@wii.com`, or `@rc24.xyz` after WiiLink's patcher |
| 0x3FC | the checksum: the sum of the 255 words before it |

The library accepts the file as the SDK's `CheckConfig()` does. It needs only
the number and the domain.

#### The control files: wc24recv.ctl and wc24send.ctl

Each is a 0x80-byte header, then 0x80-byte entries (the SDK's
`NWC24iMBCHeader` and `NWC24iMBCEntry`):

| | Receive box | Send box |
| --- | --- | --- |
| Size | 32 KiB | 16 KiB |
| Entries | 255 | 127 |

| Header | |
| --- | --- |
| 0x00 | `WcTf`, version 4 |
| 0x08 | messages in the box |
| 0x0C | capacity: 255, or 127 |
| 0x10 | total message bytes |
| 0x14 | file size: 0x8000, or 0x4000 |
| 0x18 | next message ID (1 to 1,000,000, then wraps) |
| 0x1C | first free entry's offset (0 when full) |
| 0x20 | oldest message ID |
| 0x24 | free space in the .mbx, as the VF library counts it |

Free entries are chained through their 0x0C word, which is the app-ID field
of a used entry. A message fills the entry at the head of the chain, and
moves the head to that entry's link.

| Entry | Value for a post |
| --- | --- |
| 0x00 ID, 0x04 flags | the next ID; `0x200329`: for recipient, for menu, initialised, delivering, in the receive box (0x20; 0x10 for the send box), bit 21. With attachments, also bit 16 (multipart). With a delay, also bit 24. |
| 0x08 length | the message's exact length (the file is padded) |
| 0x0C app ID, 0x32 group ID | the posting title's four letters and maker code |
| 0x10 | where the first boundary, or the text part, starts |
| 0x18 command | `0x00040001`, the default LED pattern |
| 0x20 from | the Wii number |
| 0x28 | when it was made, in minutes since 1900 UTC |
| 0x2C | with a delay: the minute it may show |
| 0x30, 0x31 | 1 recipient; the number of attachments |
| 0x34 / 0x38 / 0x40 / 0x44 | From address, To address, charset, transfer encoding, each as offset \| size << 20 |
| 0x48 / 0x4C | the text part's offset and size |
| 0x50 + 8i | each attachment's base64 span: offset, size |
| 0x60 + 4i | each attachment's size before encoding |
| 0x68 + 4i | each attachment's MIME type; `0x00010000` (text/plain) in an unused slot |
| 0x70 | the text's size before encoding |
| 0x78 | `0x80000000` |

#### The message files: wc24recv.mbx and wc24send.mbx

Each is a VFF, the Wii's FAT volume in a NAND file. A VFF has:

- a 0x20-byte header: `VFF `, 0xFEFF, the volume size, the cluster size / 16;
- two FATs;
- a 0x1000-byte root directory;
- 512-byte clusters.

| | Receive box | Send box |
| --- | --- | --- |
| Size | 7 MiB | 2 MiB |
| Clusters | 14,336 | 4,075 data clusters |
| FAT type | FAT16 | FAT12 (under FAT16's minimum of 4,085) |
| FATs at | 0x20, 0x7020 | 0x20, 0x2020 |
| Root at | 0xE020 | 0x4020 |
| Data from | 0xF020 | 0x5020 |
| A message file | `/mb/r0000007.msg` | `/mb/s0000001.msg` |

The send box's FATs are as long as FAT16 ones would be, two bytes per
cluster, with the FAT12 entries packed at their start. The library reads and
writes both FAT types. Dolphin's VFF code treats the send box as FAT16.

The VF library writes lower-case names: one long-name entry plus the 8.3
entry, as it does for `mb` itself. Two more details are not in any write-up,
and they matter:

- **Message files are zero-padded to a multiple of 32 bytes.** The control
  entry keeps the exact length.
  - **Why:** the SDK's buffered VF write flushes whole 32-byte blocks, and
    its reader (`NWC24FStreamInit`) reads whole blocks too. So it gets a short
    read from a file that ends inside a block.
  - **What happens then:** the Wii Menu drops the message.
    `NWC24ReadMsgMBOptOutFlag` returns `NWC24_ERR_FILE_READ`, and `receive()`
    deletes the message without filing it.
  - **Evidence:** all four messages KD had delivered on the bench Wii are
    padded this way (1064 → 1088 bytes, 1786 → 1792, …).
- **The VF library never uses the volume's last cluster.** The free space it
  records is one cluster less than the FAT shows free, both on the bench
  Wii's box and in Dolphin. The library matches it.

#### The message

A post, as `NWC24CommitMsg()` writes it to the receive box:

```
Date: 22 Oct 2026 18:30:05 -0000
From: w1234567890123456@wii.com
To: w1234567890123456@wii.com
Message-Id: <00001...@wii.com>          id %05X, number %08X%08X, minutes %08X, domain
X-Wii-AppId: 2-4F484243-3031            2 = for the Wii Menu; app id; group id
X-Wii-Cmd: 00040001
X-Wii-AltName: AFcAaQBp...AAA=          the sender name, UTF-16BE + NUL, base64 in 42-byte lines
X-Wii-MB-NoReply: -
MIME-Version: 1.0
Content-Type: text/plain; charset=utf-16be
Content-Transfer-Encoding: 8bit

<the text, raw UTF-16BE; U+000A starts a line>
```

There is no Subject: the board shows the alt name as the sender. A title is
cut at 34 UTF-16 units (35 with the NUL), and a body at 1,000.

The other header lines and the multipart layout are in
[messageboard-messages.md](messageboard-messages.md):

- `X-WiiFace`, `X-Wii-MB-RegDate` and `X-Wii-MB-Delay`;
- the multipart layout for a photo and board data.

A message to another Wii differs as "Sending to another Wii" says.

### The IOS calls

| Call | What for |
| --- | --- |
| `/dev/net/kd/request` ioctl 2 (`TRY_SUSPEND_SCHEDULER`, 32 bytes in and out) | stop KD touching its files, as `NWC24OpenLib()` does; a negative result → `MBOARD_E_BUSY` |
| `/dev/net/kd/request` ioctl 3 (`RESUME_SCHEDULER`) | always, afterwards |
| `/dev/net/kd/request` ioctl 0x0B (`SEND_MAIL_NOW`, 32 bytes out) | `mboard_send_msg`: upload the send box now; a negative result → `MBOARD_QUEUED` |
| `/dev/net/kd/time` ioctl 24 (`GET_TIME_DIFFERENCE`) | UTC = this + the raw RTC (libogc's clock less SYSCONF's counter bias). Without it, or more than a day from the local clock, the local clock. |
| ISFS open, read and write | `nwc24msg.cfg`, the two mailboxes, `nwc24fl.bin`; `cdb.vff` and `nocopy/cdbwiiid.dat` for the play history; `/shared2/menu/FaceLib/RFL_DB.dat` read for `mboard_mii` |
| ES_GetTitleID, ISFS_GetAttr | the app ID and group ID: the running title, and its data folder's group |
| IOS FS permission check, patched | `mboard_log_play` only, for the moment it writes `cdb.vff` (HBC's `playtime.c` patch) |

### What the Wii Menu does with a received message

This is from the Wii Menu decompilation (koopthekoopa/wii-ipl,
`ipl::nwc24::Manager::receive()`, 4.3U, the same binary as the Dolphin
profile's), checked by tracing it in Dolphin:

1. For each receive-box message of type `WII_MENU`, `WII_MENU_SHARED` or
   `PUBLIC`, the app and group ID must be six capital letters or digits
   (`is_valid_app_id`), unless the group ID is 0 or 1.
2. It reads the X-Wii-MB headers, the alt name and the text. Any file error
   here drops the message; that is how the unpadded files were lost.
3. It files a `ripl_board_record` in `cdb.vff` under
   `/YYYY/MM/DD/hh/mm/<APP>_<GROUP>/<type>/`, months counted from 0. The type
   is `txt`, or `odh` with a photo. The date is the Wii Menu's own clock, not
   the message's.
4. It deletes the message from the receive box, filed or not.

## What can go wrong, and what the library does about it

| Risk | Guard |
| --- | --- |
| A file not as expected: an older or newer format, a broken free chain, an unknown VFF header, a FAT neither FAT12 nor FAT16, an address book that is not `WcFl` version 2 | checked before anything is written; `MBOARD_E_FORMAT`, and no write |
| A mailbox is full | `MBOARD_E_FULL`. The SDK deletes the oldest messages to make room; the library never deletes anything |
| KD using its files at the same moment | its scheduler is suspended first (`MBOARD_E_BUSY` when it will not), and resumed after |
| Power lost part way through a post | a message's data clusters are written first, then both FATs, then its directory entries, then the control file in one write. A failure leaves at most clusters nothing points to, or a file no entry refers to. The next post takes the same ID and reuses that file |
| Two copies of the FAT disagreeing | both are written from one in-memory FAT, only the changed sectors |
| The message's ID already in use | refused (`MBOARD_E_FORMAT`) |
| Other messages, filed or pending | never touched; checked byte for byte on the bench Wii (4 pending WiiLink messages) |
| A mail server rewriting the message | to another Wii the text goes base64, which no server changes |
| A photo or board data too big | refused before anything is written (`MBOARD_E_TOO_BIG`) |
| `cdb.vff` (the play history) | the writer HBC uses; checked byte for byte against the reference the Wii Menu accepted; refuses a day that already has 12 titles |
| The Address Book | the whole file is read, changed in memory and written back in one write, with KD paused; only what the SDK's own calls change is changed; a number already in the book, or a full book, is refused |

## Proof

### On the PC

- **`tests/test_mboard.py`, posts:**
  - into an empty box, with and without `mb`;
  - a long message over several clusters, and two posts in a row;
  - a half-finished post retried;
  - ten refusals that change nothing;
  - a post into a copy of the bench Wii's real receive box (`MBOARD_REAL_BOX`).
- **The same file, the full message:** every option at once, each alone,
  board data at its limit, a large photo, and ten option refusals. Each is
  compared with a rebuild from the SDK's format strings, and Python's
  `email` package must read each part back as it went in.
- **The same file, sending:** two messages into a FAT12 send box built like
  the bench Wii's.
  - Each goes through WiiLink's two rewrites (NUL bytes deleted, LF made
    CRLF), and its text must still read back.
  - Sending into a copy of the bench Wii's real, empty send box also worked.
- **The same file, the Address Book:** add, set and remove, byte for byte as
  the SDK writes them, and the refusals.
- **The play history:** the same sessions are compared with
  `tools/msgboard/cdb_log.py` byte for byte (`tests/test_cdblog.py`, and
  `tests/test_mboard.py`).
- **`tests/test_stationery.py`:** reads each stationery encoder back with a
  decoder of its own, and builds all five themes.

### In Dolphin, with the real Wii Menu 4.3U

- **`tests/dolphin_mboard.py --restore`.**
  - **Filed:** the Wii Menu filed the post as
    `/2026/10/01/12/05/HBLW_01/txt/3279E781.000`, holding the text, and
    every file `cdb.vff` had was kept.
  - **Restored:** `tests/nandput` then restored the box and `cdb.vff` byte
    for byte.
  - **Tracing:** `--ipl-symbols` gives Dolphin a map of the Wii Menu and
    logs its import. That trace found the padding.
- **`--full --restore`** (2026-10-22). `mboard_post_msg` posted text with a
  Mii (`test_mii()`), a 2,869-byte JPEG, and a `chjump.bin` that opens HBC.
  The Wii Menu filed it as `/2026/09/22/12/05/HBLW_01/odh/326CB881.000`, an
  `RI_5` record holding:
  - the text;
  - the Mii's 74 bytes (face offset 0x1F8);
  - the photo, as a 1,156-byte picture (attachment type 1);
  - the 128 bytes of board data, unchanged (type 2).

  Every file `cdb.vff` had was kept, and the restore put all three files back
  byte for byte. An earlier run with an all-zero Mii filed the message
  without the face, because a Mii's eyebrow Y must be 3 or more
  ([mii.md](mii.md)).

### On the bench Wii

The bench Wii is `dev` (192.168.8.213): IOS58, Wii Menu 4.3U, registered with
WiiLink. Every job ran through the queue (`tests/wii_mboard.py`).

- **A post** (job 20261022-114201-8b8b7c). `mboard_post` returned
  `MBOARD_OK` in 163 ms.
  - The new message, `/mb/r0000007.msg`, went in beside the 4 messages KD had
    delivered (IDs 2-5). Those messages, their control entries, both FATs and
    the recorded free space all checked out.
  - `mboard_log_play` added "Final Fantasy VII 00:42" to today's play
    history in 346 ms. `cdb.vff` kept all 26 files; only today's play
    history changed.
- **Filed** (job 20261022-114929-b16cf8).
  - **The board:** the user started the Wii Menu and opened the board. The
    Wii Menu had filed `/2026/09/22/11/50/HBLW_01/txt/326CB51F.000` (sender
    "WiiStation", "Played Final Fantasy VII\nfor 42 minutes.") and kept every
    file `cdb.vff` had.
  - **Seen:** the user saw the message on the board.
  - **Untouched:** the 4 WiiLink messages stayed in the receive box.
- **The full message** (jobs 20261022-130042-3ebca3, then
  20261022-133332-027b7e). It carried the Wii's own Mii from database slot 0,
  byte for byte, the photo, a `chjump.bin` that opens HBC, and Reply.
  - **Filed:** the Wii Menu filed `/2026/09/22/13/26/HBLW_01/odh/326CCB98.000`,
    holding the Mii, a 42,252-byte picture, the board data, and Reply
    available. Dolphin's conversion of the same JPEG was only 1,156 bytes.
  - **Seen:** the user saw it with the Mii and the photo. Its button opened
    HBC, and Reply opened the reply screen.
- **Sending to LRWii** (192.168.8.243), from 2026-10-22.
  - **Upload:** `mboard_send_msg` returned `MBOARD_OK` in 4-9 s, most of it
    KD's upload. Each time the send box's next ID went up by one and the box
    was empty again: KD had posted the message and deleted it.
  - **Arrival:** LRWii filed the letters once both Wiis had each other in the
    Address Book.
  - **Readable:** the text read correctly once it went base64 (2026-10-23).
- **Stationery** (2026-10-23). All five themes went from the bench Wii to
  LRWii, and the user saw each with its border, board icon and music. The
  sender's name needed a dark top band, and the music half its first volume.
- **The Address Book** (2026-10-23).
  - **Read:** "Main Wii" (pending) and "LivingRoom" (confirmed). The Mii the
    user had chosen for "Main Wii" in the Wii Menu was stored as Testorb's
    creation ID.
  - **Written:** `friends --set 1 -` gave LivingRoom that Mii, changing
    exactly those 8 bytes of `nwc24fl.bin`.
- **Two notes from the runs:**
  - **The clock:** the bench Wii's KD time difference put UTC exactly a day
    behind the correct local clock. The library ignores a KD value further
    from the local clock than any time zone. The Wii Menu files under its
    own clock anyway.
  - **The app ID:** under HBC, `ES_GetTitleID` fails for a Wiiload app, so
    the app ID is `HBLW` with group `01`, and the board files the message
    under `HBLW_01`.

Not yet tried on a Wii:

- a delay;
- a filing date;
- adding or removing a contact.

## Restore path

- **Copying out:** `tests/nandget` copies a NAND folder to the SD card
  (`--unlock` for the Wii Menu's).
- **Putting back:** `tests/nandput SD-DIR NAND-DIR FILE...` puts files back
  over the originals. It writes same-size files only, then reads them back
  and compares.
- **WiiConnect24 paused:** both pause KD around `/shared2/wc24`. A copy that
  did not once waited an hour.
- **The backup:** the bench Wii's state from before the first write
  (`cdb.vff`, `/shared2/wc24` and its mailboxes) is kept outside the
  repository, because it holds the WiiConnect24 password. It is in the backup
  job's run folder (20261022-111223-6bbb42) and in
  `Documents/wii-backups/bench-mboard-20261022`.

To put it back:

    wiibench.py add --name mboard-restore --cwd REPO --timeout 2400 -- \
      C:/Python312/python.exe tests/wii_mboard.py restore BACKUP_DIR

That also undoes everything the Wii Menu has filed since. The restore ran in
Dolphin (`--restore`); on the bench Wii it has not been needed.
