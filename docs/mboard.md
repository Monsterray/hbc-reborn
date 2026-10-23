# Posting to the Wii Message Board from homebrew (sdk/mboard)

`sdk/mboard/mboard.[ch]` lets an app put "played Final Fantasy VII for 42
minutes" on the Wii Message Board. It needs nothing beyond libogc. This note
covers how a post gets onto the board, the file formats, the IOS calls, what
can go wrong, and how the library keeps the board intact. It was worked out
on 2026-10-22 from Dolphin, the Revolution SDK and Wii Menu decompilations,
WiiLink's server, and the bench Wii.

```c
#include "mboard.h"

mboard_post("WiiStation", "Played Final Fantasy VII\nfor 42 minutes.");
mboard_log_play("Final Fantasy VII", "WSFF7", 42 * 60);   // needs AHBPROT
```

| | `mboard_post` | `mboard_log_play` |
| --- | --- | --- |
| On the board | a message on today's page, sender = the title | a line under today's "Today's Play History" |
| Written to | `/shared2/wc24/mbox/wc24recv.{ctl,mbx}` | `/title/00000001/00000002/data/cdb.vff` |
| Shows up | when the Wii Menu next starts | at once |
| Needs | the Wii's Wii number (any set-up Wii has one) | AHBPROT (`<ahb_access/>` in meta.xml) |
| Network, WiiConnect24 service | no | no |

## The full message: a Mii, a photo, board data

`mboard_post_msg()` writes everything else a game's message can carry, in
the same receive-box message:

```c
static uint8_t mii[MBOARD_FACE_SIZE];
mboard_msg m = { 0 };

m.title = "WiiStation";
m.body = "Played Wii Sports\nfor 5 minutes.";
if (mboard_mii(0, mii) == MBOARD_OK)   // the Wii's first Mii, as the icon
    m.face = mii;
m.jpeg = photo; m.jpeg_size = photo_size;   // a JPEG: the board shows it
m.board = arc; m.board_size = arc_size;     // board data: a U8 archive
m.reply = 1;                                // a Reply button
mboard_post_msg(&m);
```

| Field | Header or part | Limit (the SDK's) | Refused with |
| --- | --- | --- | --- |
| `face` | `X-WiiFace`: 74 bytes of `RFLCharData` | a creation ID; the Wii Menu also checks every part ([mii.md](mii.md)) | `MBOARD_E_ARG` (ID 0) |
| `jpeg` | an `image/jpeg` part; the Wii Menu converts it, and files the message as `odh` | starts `FF D8 FF`; the photo and board data together under 0x245B0 | `MBOARD_E_ARG`, `MBOARD_E_TOO_BIG` |
| `board` | an `application/x-wii-msgboard` part: a U8 archive, e.g. `chjump.bin` (a button that opens a channel) | starts `55 AA 38 2D`; at most 0x1E000 | `MBOARD_E_ARG`, `MBOARD_E_TOO_BIG` |
| `reply` | no `X-Wii-MB-NoReply` | | |
| `delay_hours` | `X-Wii-MB-Delay`, the control entry's 0x2C and flag bit 24: held back from the board until then | 1-240 | `MBOARD_E_ARG` |
| `file_year`, `file_month`, `file_day` | `X-Wii-MB-RegDate` | 2000-2035, a real date | `MBOARD_E_ARG` |

The text, the encoded attachments and their headers must also fit the receive
box's largest message (0x31C00), or the result is `MBOARD_E_TOO_BIG`.
[messageboard-messages.md](messageboard-messages.md) has these formats:

- the multipart layout, byte for byte;
- the control entry's attachment fields;
- where the Wii Menu files each kind of record;
- the board record it builds.

`mboard_post()` is `mboard_post_msg()` with only a title and text, and it
writes the same bytes as before.

Two side effects:

- **A valid Mii also goes into the Wii's hidden Mii database.** That is what
  the Wii Menu does with any message's Mii.
- **A JPEG the Wii Menu cannot convert** shows as its question-mark picture.

## To another Wii: the send box

`mboard_send_msg(to, &msg)` sends the same message to another Wii, given
that Wii's 16-digit Wii number. It goes through WiiConnect24's mail server,
so both Wiis need an account that works. Nintendo's server is gone, so
today that means WiiLink's (`@rc24.xyz` in `nwc24msg.cfg`, stage 2). Both
Wiis also need the internet.

**What it writes** is what `NWC24CommitMsg()` writes for a message to any
other Wii (Petari `NWC24MsgCommit.c`): the same headers and parts as
`mboard_post_msg`, with these differences:

- **The SMTP envelope** comes first, and the control entry's offsets count
  it:
  ```
  MAIL FROM: w<this Wii>@rc24.xyz
  RCPT TO: w<the other>@rc24.xyz
  DATA
  ```
- **`To:`** names the other Wii.
- **The text is base64** (`Content-Transfer-Encoding: base64`, 57 bytes to a
  line, as the SDK writes it and the Wii Menu's own letters send it), not raw
  UTF-16 in 8 bits as in a post. WiiLink's server keeps mail as text: its
  `send.go` deletes every NUL byte, and its `receive.go` turns every LF into
  CRLF (WiiLink24/Mail-Server). Raw UTF-16 lost every other byte of its ASCII
  and reached LRWii as CJK characters and question-mark boxes (2026-10-23).
  The headers and attachments were already base64. The entry's 0x70 keeps
  the text's size before encoding (the SDK's `WritePlainText`).
- **The box** is `/shared2/wc24/mbox/wc24send.{ctl,mbx}`, not the receive
  box. The file is `/mb/s0000001.msg`, and the entry's flags have TO_SEND
  (0x10) where a post has TO_RECV (0x20).
- **The box's format** is different too. The control file is 0x4000 bytes
  with 127 entries. The VFF is 2 MiB and FAT12: its 4,075 data clusters
  are under FAT16's minimum. It still has FAT16-sized FATs (0x2000 bytes
  each: the second at 0x2020, the root at 0x4020, data from 0x5020), with
  FAT12 entries packed at their start.

**After writing**, it resumes KD and calls `/dev/net/kd/request` ioctl 0x0B
(`SEND_MAIL_NOW`). KD then posts the box to the server's `send.cgi` and
deletes each message it sent (Dolphin's `NetKDRequest.cpp`, `WC24Send.cpp`).
If KD refuses, the result is `MBOARD_QUEUED`, and the message waits for
KD's next round.

**WiiLink's server** (WiiLink24/Mail-Server `send.go`):

- It refuses a sender whose `mlid` and `passwd` (from `nwc24msg.cfg`) don't
  match its account.
- It refuses a `From:` that isn't the sender.
- It stores the message for any recipient with an account. It does not look
  at friend lists.

**The receiving Wii Menu** files a letter from any Wii; it uses the friend
list only for the sender's name. The receiving Wii's KD, though, needs the
sender in its address book: LRWii received nothing until both Wiis had
registered each other (Wii Menu, Address Book, Register, Wii Number), and
then filed the letter (2026-10-23). Both also need the WiiConnect24 user
agreement accepted: LRWii's black screen there was an old EULA channel (HAKE
version 1, on IOS20, which Nintendo stubbed), fixed with HAKE version 3 on
IOS56 (`tests/wii_eulacheck.py` finds this).

### Proof

The bench Wii (`@rc24.xyz`) sent to LRWii (192.168.8.243, `@rc24.xyz`) on
2026-10-22 (`tests/wii_mboard.py send LRWii`, job 20261022-183706-3b6d87):

- `mboard_send_msg` returned `MBOARD_OK` in 7,985 ms. Most of that time is
  KD's upload during `SEND_MAIL_NOW`.
- The send box's next ID went from 1 to 2, and the box was empty again: KD
  had posted the message and deleted it.

On the PC, `tests/test_mboard.py` sends two messages into a FAT12 send box
built like the bench Wii's, checks them byte for byte, and checks the
refusals. The send also worked into a copy of the bench Wii's real (empty)
send box.

## Stationery: a letter's own look, icon and music

A letter's border, its icon on the board and a sound are board data:
`mboard_msg.board`, an `application/x-wii-msgboard` part.
[`tools/msgboard/stationery.py`](../tools/msgboard/stationery.py) makes one on
the PC from images and a WAV:

    stationery.py LETTER.png THUMB.png [--sound tune.wav] [--chjump 000100014F484243] -o board.arc

The app ships the archive and passes it as `board`. The layout is the one
WiiLink's letters carry. Repacked with this tool, a copy of one is byte for
byte the same, and the Wii Menu files it (docs/messageboard-messages.md).

- **The letter** is 512 x 376. The tool cuts it into a 9-slice frame of TPL
  textures: `img/my_Letter_a.tpl` to `_i.tpl`, 64 | 384 | 64 wide and 144 /
  168 / 64 high. They are RGB5A3 and keep their alpha, in an LZ77-compressed
  U8, `letter_LZ.bin`. Keep the middle light: the Wii Menu writes the text
  over it in a dark colour.
- **The board icon** is 144 x 96 (`thumbnail_LZ.bin`,
  `img/my_LetterS_b.tpl`).
- **The sound** is `sound.bns`: mono DSP-ADPCM, 8 kHz like WiiLink's (the Wii
  Menu takes 4-48 kHz, 1 or 2 channels). The tool's encoder decodes back at
  47 dB SNR on a tone. The Wii Menu plays it once, 300 ms after the letter
  opens, muting the board's music, and fades it as the letter closes.
- **`chjump.bin`**, optional, adds the "start" button for a channel. The Wii
  Menu shows it only if that channel is installed.
- **The whole archive** must stay within 0x1E000 bytes. The letter must
  unpack to less than 400 KiB, and the icon to less than 30 KiB.

[`tools/msgboard/stationery_themes.py`](../tools/msgboard/stationery_themes.py)
makes five examples, drawn and scored in code:

| Theme | Size | Music |
| --- | --- | --- |
| `ocean` | 74,560 bytes | 5.0 s, an arpeggio |
| `sunset` | 50,880 bytes | none |
| `forest` | 51,616 bytes | none |
| `nightsky` | 80,416 bytes | 6.4 s, "Twinkle Twinkle Little Star" |
| `arcade` | 60,832 bytes | 2.0 s, a chiptune |

`tests/test_stationery.py` reads each encoder back with a decoder of its
own, and builds every theme. `tests/wii_mboard.py send WII --board ARC
--body TEXT --no-photo` sends one from the bench Wii.

## The address book

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
creation ID from the Wii's Mii database (`RFL_DB.dat`): its own Miis, or the
hidden database of Miis that came on messages.

```c
mboard_friend list[MBOARD_FRIENDS];
int n = mboard_friends(list, MBOARD_FRIENDS);         /* every contact */

static uint8_t mii[MBOARD_FACE_SIZE];
mboard_mii(0, mii);
mboard_friend_add(wii_number, "LR Wii", mii);         /* pending, as the Wii Menu adds one */
mboard_friend_set(slot, "Living Room", mii);          /* nickname and Mii only */
mboard_friend_remove(slot);
```

Each call works as the SDK's `NWC24WriteFriendInfo`, `UpdateFriendInfo` and
`DeleteFriendInfo` do, byte for byte (`tests/test_mboard.py`). It pauses KD
around the file, since KD reads and writes it too.

- **Adding** leaves the contact pending; KD sends the registration and sets
  the confirmed bit when the other Wii's arrives.
- **The address cannot change:** remove the contact and add it again.
- **Not done:** setting the confirmed bit by hand (KD would never register
  the contact) and writing Miis into `RFL_DB.dat` (it has a CRC).
- **On the bench Wii** only: `tests/wii_mboard.py friends [--set SLOT NICK]`.

## How games post: the SDK's loop-back

The brief assumed that Mario Party 8 and 9 post through the WiiConnect24 send
path. They do not post at all. MP8's `main.dol` has no NWC24 code (the
mariopartyrd/marioparty8 decomp agrees). MP9 links only the SDK's start-up and
shutdown calls (`NWC24iSetRtcCounter`, `NWC24SuspendScheduler`). Their board
entries are the Wii Menu's own play history, from `play_rec.dat`
(docs/messageboard.md).

The send path is not the route either. KD's `IOCTL_NWC24_SEND_MAIL_NOW` POSTs
the send box to the mail server named in `nwc24msg.cfg`, and Dolphin and
WiiLink's server (WiiLink24/Mail-Server) both show no local delivery. A
self-addressed mail would need a registered account, the internet, and a
server round trip.

Games that do post (Super Mario Galaxy's letters) use the SDK's
`NWC24CommitMsg()`. With exactly one recipient equal to the Wii's own number,
it writes the message straight into the **receive** box: `LoopBackEnable`
(SMGCommunity/Petari `NWC24MsgCommit.c`). The message gets no SMTP envelope
and never leaves the console. The Wii Menu files new receive-box messages onto
the board when it starts. `mboard_post` writes exactly what that call writes
for a `NWC24_MSGTYPE_WII_MENU` message with an alt name, UTF-16 text and no
attachment, as SMG sends it.

## The files

### nwc24msg.cfg (read only)

`/shared2/wc24/nwc24msg.cfg`, 0x400 bytes, big-endian, with
`nwc24msg.cbk` as its backup. Fields: `WcCf`, version 8, the Wii number (u64
at 0x08), the ID-generation count (0x10, < 32), the stage (0x14: 1 generated,
2 registered), and the account domain (0x18, e.g. `@wii.com`, or `@rc24.xyz`
after WiiLink's patcher). The checksum at 0x3FC is the sum of the 255 words
before it. The library accepts the file as the SDK's `CheckConfig()` does. It
only needs the number and the domain; the registration stage does not matter.

### wc24recv.ctl

32 KiB: a 0x80-byte header, then 255 entries of 0x80 bytes (the SDK's
`NWC24iMBCHeader`/`NWC24iMBCEntry`).

| Header | |
| --- | --- |
| 0x00 | `WcTf`, version 4 |
| 0x08 | messages in the box |
| 0x0C | capacity, 255 |
| 0x10 | total message bytes |
| 0x14 | file size, 0x8000 |
| 0x18 | next message id (1 to 1,000,000, then wraps) |
| 0x1C | first free entry's offset (0 when full) |
| 0x20 | oldest message id |
| 0x24 | free space in the .mbx, as the VF library counts it |

Free entries are chained through their 0x0C word (the app-id field of a used
entry). A post fills the entry at the head of the chain and moves the head to
that entry's link:

| Entry | Value for a post |
| --- | --- |
| 0x00 id, 0x04 flags | next id; `0x200329` (for recipient + for menu + initialised + delivering + in the receive box + bit 21) |
| 0x08 length | the message's exact length (the file is padded) |
| 0x0C app id, 0x32 group id | the posting title's four letters and maker code |
| 0x10 | where the text part starts |
| 0x18 command | `0x00040001`, the default LED pattern |
| 0x20 from | the Wii number; 0x28 created, minutes since 1900 UTC |
| 0x30, 0x31 | 1 recipient, 0 attachments |
| 0x34 / 0x38 / 0x40 / 0x44 | From address, To address, charset, transfer encoding, each as offset \| size << 20 |
| 0x48 / 0x4C | text offset and size; 0x70 its size before encoding |
| 0x68 / 0x6C | `0x00010000` (an unused attachment slot's type); 0x78 `0x80000000` |

### wc24recv.mbx

A 7 MiB VFF, the Wii's FAT volume in a NAND file. It has a 0x20-byte header
(`VFF `, 0xFEFF, volume size, cluster size / 16), two FATs, a 0x1000-byte
root directory, and 512-byte clusters. Its 14,336 clusters make it FAT16, with
FATs at 0x20 and 0x7020, root at 0xE020, data from 0xF020. Messages are
`/mb/rNNNNNNN.msg`. The VF library writes lower-case names with one long-name
entry plus the 8.3 entry, as it does for `mb` itself. The 2 MiB send box is
FAT12, which the library refuses; Dolphin's VFF code treats it as FAT16.

Two details are not in any write-up and matter:

- **Message files are zero-padded to a multiple of 32 bytes.** The SDK's
  buffered VF write flushes whole 32-byte blocks, and its reader
  (`NWC24FStreamInit`) reads whole blocks too, so it gets a short read from a
  file that ends inside one. The Wii Menu then drops the message:
  `NWC24ReadMsgMBOptOutFlag` returns `NWC24_ERR_FILE_READ`, and `receive()`
  deletes it without filing it. All four messages KD had delivered on the
  bench Wii are padded this way (1064 → 1088 bytes, 1786 → 1792, …). The
  control entry keeps the exact length.
- **The VF library never uses the volume's last cluster.** The free space it
  records is one cluster less than the FAT shows free, both on the bench
  Wii's box and in Dolphin. The library matches it.

### The message

As `NWC24CommitMsg()` writes it to the receive box:

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
cut at 34 UTF-16 units (35 with the NUL) and a body at 1,000.

## The IOS calls

| Call | What for |
| --- | --- |
| `/dev/net/kd/request` ioctl 2 (`TRY_SUSPEND_SCHEDULER`, 32 bytes in and out) | stop KD from touching the mailboxes, as `NWC24OpenLib()` does; a negative result → `MBOARD_E_BUSY` |
| `/dev/net/kd/request` ioctl 3 (`RESUME_SCHEDULER`) | always, after the post |
| `/dev/net/kd/time` ioctl 24 (`GET_TIME_DIFFERENCE`) | UTC = this + the raw RTC (libogc's clock less SYSCONF's counter bias); without it, or more than a day from the local clock, the local clock |
| ISFS open/read/write | `nwc24msg.cfg`, `wc24recv.ctl`, `wc24recv.mbx`; `cdb.vff` and `nocopy/cdbwiiid.dat` for the play history |
| ES_GetTitleID, ISFS_GetAttr | the app id and group id (the running title and its data folder's group) |
| IOS FS permission check, patched | `mboard_log_play` only, for the moment it writes `cdb.vff` (HBC's `playtime.c` patch) |

## What the Wii Menu does with it

From the Wii Menu decompilation (koopthekoopa/wii-ipl,
`ipl::nwc24::Manager::receive()`, 4.3U, the same binary as the Dolphin
profile's), checked by tracing it in Dolphin:

1. For each receive-box message of type `WII_MENU`, `WII_MENU_SHARED` or
   `PUBLIC`, the app and group id must be six capital letters or digits
   (`is_valid_app_id`), unless the group id is 0 or 1.
2. It reads the X-Wii-MB headers, the alt name and the text. Any file error
   here drops the message. That is how the unpadded files were lost.
3. It files a `ripl_board_record` in `cdb.vff` under
   `/YYYY/MM/DD/hh/mm/<APP>_<GROUP>/txt/` (months from 0). The date is the
   Wii Menu's own clock, not the message's.
4. It deletes the message from the receive box, filed or not.

## What can go wrong, and what the library does about it

| Risk | Guard |
| --- | --- |
| A file not as expected (an older or newer format, a broken free chain, a FAT12 volume, an unknown VFF header) | checked before anything is written; `MBOARD_E_FORMAT` and no write |
| The receive box is full | `MBOARD_E_FULL`. The SDK deletes the oldest messages to make room; the library never deletes anything |
| KD downloading mail at the same moment | its scheduler is suspended first (`MBOARD_E_BUSY` when it will not), and resumed after |
| Power lost part way | the message's data clusters are written first, then both FATs, then its directory entries, then the control file in one write. A failure leaves at most clusters nothing points to, or a file no entry refers to; the next post takes the same id and reuses that file |
| Two copies of the FAT disagreeing | both are written from one in-memory FAT, only the changed sectors |
| The message's id already in use | refused (`MBOARD_E_FORMAT`) |
| Other messages, filed or pending | never touched; checked byte for byte on the bench Wii (4 pending WiiLink messages) |
| `cdb.vff` (play history) | the writer HBC has used since 1.8.9; checked byte for byte against the reference the Wii Menu accepted; refuses a day that already has 12 titles |

## Proof

- **PC** (`tests/test_mboard.py`): posts into an empty box (with and without
  `mb`), a long message over several clusters, two posts in a row, a
  half-finished post retried, ten refusals that change nothing, and a post
  into a copy of the bench Wii's real receive box (`MBOARD_REAL_BOX`). The
  play history is compared with `tools/msgboard/cdb_log.py`, byte for byte.
  The full message (every option at once, each alone, board data at its
  limit, a large photo, and ten option refusals) is compared with a rebuild
  from the SDK's format strings, and Python's `email` package must read
  each part back as it went in.
- **Dolphin** (`tests/dolphin_mboard.py --restore`): the real Wii Menu 4.3U
  files the post as
  `/2026/10/01/12/05/HBLW_01/txt/3279E781.000` holding the text. Every file
  `cdb.vff` had is kept. `tests/nandput` then restores the box and
  `cdb.vff` byte for byte. `--ipl-symbols` gives Dolphin a map of the Wii
  Menu and logs its import: the trace that found the padding.
- **Dolphin, the full message** (`tests/dolphin_mboard.py --full --restore`,
  2026-10-22). `mboard_post_msg` posted text with a Mii (`test_mii()`), a
  2,869-byte JPEG, and board data: a `chjump.bin` that opens HBC. The Wii
  Menu filed it as `/2026/09/22/12/05/HBLW_01/odh/326CB881.000`, an `RI_5`
  record holding:
  - the text;
  - the Mii's 74 bytes (face offset 0x1F8);
  - the photo, as a 1,156-byte picture (attachment type 1);
  - the 128 bytes of board data, unchanged (type 2).

  Every file `cdb.vff` had was kept, and the restore put all three files
  back byte for byte. An earlier run with an all-zero Mii filed the message
  without the face: the eyebrow Y must be 3 or more ([mii.md](mii.md)).
- **Bench Wii, the full message** (2026-10-22). Two jobs ran
  `tests/wii_mboard.py`:
  - **`full`** (job 20261022-130042-3ebca3) posted the Wii's own Mii from
    database slot 0, byte for byte, with the photo, a `chjump.bin` that opens
    HBC, and Reply.
  - **`board --full --already`** (job 20261022-133332-027b7e) then found
    what the Wii Menu had filed: `/2026/09/22/13/26/HBLW_01/odh/326CCB98.000`,
    holding the Mii, a 42,252-byte picture (Dolphin's conversion of the same
    JPEG was 1,156 bytes), the board data, and Reply available. Every file
    `cdb.vff` had was kept.

  The user saw the message on the board with the Mii and the photo; its
  button opened HBC, and Reply opened the reply screen (no contacts to send
  to).
- **Bench Wii** (192.168.8.213, IOS58, Wii Menu 4.3U, WiiLink-registered,
  2026-10-22), through the queue (`tests/wii_mboard.py`):
  - `post` (job 20261022-114201-8b8b7c): `mboard_post` returned
    `MBOARD_OK` in 163 ms. The new message `/mb/r0000007.msg` went in beside
    the 4 messages KD had delivered (ids 2-5), and those, their control
    entries, both FATs and the recorded free space all checked out.
    `mboard_log_play` added "Final Fantasy VII 00:42" to today's play history
    in 346 ms. `cdb.vff` kept all 26 files; only today's play history
    changed.
  - `board` (job 20261022-114929-b16cf8): the user started the Wii Menu,
    opened the board and went back to HBC. The Wii Menu had filed
    `/2026/09/22/11/50/HBLW_01/txt/326CB51F.000` (sender "WiiStation",
    "Played Final Fantasy VII\nfor 42 minutes.") and kept every file
    `cdb.vff` had. The user saw the WiiStation message on the board. The 4
    WiiLink messages stayed in the receive box, untouched.
  - The bench's KD time difference put UTC exactly a day behind the
    (correct) local clock. The first post's Date header said 21 Oct; the
    library now ignores KD values further from the local clock than any time
    zone is. The Wii Menu files a message under its own clock, so the board
    was not affected.
  - Under HBC, `ES_GetTitleID` fails for a Wiiload app, so the app id is
    `HBLW` with group `01`; the board files it under `HBLW_01`.

## Restore path

`tests/nandget` copies a NAND folder to the SD card (`--unlock` for the Wii
Menu's). `tests/nandput SD-DIR NAND-DIR FILE...` puts files back over the
originals: same size only, read back and compared. The bench Wii's state from
before the first write (`cdb.vff`, `/shared2/wc24` and its mailboxes) is kept
outside the repository (it holds the WiiConnect24 password), in the backup
job's run folder (20261022-111223-6bbb42) and
`Documents/wii-backups/bench-mboard-20261022`. To put it back:

    wiibench.py add --name mboard-restore --cwd REPO --timeout 2400 -- \
      C:/Python312/python.exe tests/wii_mboard.py restore BACKUP_DIR

That also undoes everything the Wii Menu has filed since. The restore ran in
Dolphin (`--restore`); on the bench Wii it has not been needed.

## For an app using it

- Copy `mboard.c` and `mboard.h`; build with libogc. Call from one thread,
  not during disc or NAND-heavy work.
- `mboard_post` is the one to use without AHBPROT. Its message shows up
  when the Wii Menu next starts, not while the app runs.
- `mboard_log_play` needs `<ahb_access/>` in meta.xml. HBC 1.8.9 and later
  already log the app itself (its meta.xml name), so the day's total counts
  the app's session and its games' lines both.
- `MBOARD_E_BUSY` is worth one retry a few seconds later. The other
  errors are not worth retrying.
- `mboard_post_msg` reads the photo and board data from the caller's
  memory while it posts, and needs about 1.4 times their size in free
  memory for the encoded message.
