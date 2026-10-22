# Message Board messages: how they are stored, and what sets the sender, icon and contents

This note covers how a message sits on the Wii Message Board, and which
fields set what the board shows: the sender name, the Mii icon, the text, a
photo, and board data. It complements three other notes:

- [messageboard.md](messageboard.md): the play log and its `playtimelog`
  records.
- [mboard.md](mboard.md): the `sdk/mboard` library and the receive-box route.
- [mii.md](mii.md): Mii data.

The first version of this note was written on 2026-10-22. It was audited the
same day against two sources:

- the Wii Menu decompilation (koopthekoopa/wii-ipl, 4.3U);
- the Revolution SDK as decompiled in SMGCommunity/Petari (`nwc24/`).

The audit found these errors in the first version:

- The "authenticated board record" offsets belonged to CDB's file header,
  not to the board record.
- The `.../HAEA/<ID0>/<ID1>/...` path is the SD-card backup's location, not
  the board's.
- `HABA_#1` came from a Wii Shop Channel link. `HABA` is the Shop's title
  code, and it appears in the board data of a message on the bench Wii.
- The 0x245B0 limit applies to the attachments together, not to the whole
  message.

This version says only what those sources show. It marks what is still
unproven.

## Two ways a message reaches the board

The board is files in `cdb.vff` (`/title/00000001/00000002/data/cdb.vff`).

1. **Through the WiiConnect24 receive box**
   (`/shared2/wc24/mbox/wc24recv.*`). At start-up, the Wii Menu's
   `ipl::nwc24::Manager::receive()` reads each pending message, builds a
   board record from it, and files it. This is what `sdk/mboard` uses. The
   Wii Menu derives everything itself, so the message needs nothing
   console-specific.
2. **Written straight into `cdb.vff`.** This needs the record layout below,
   and CDB's file header in front of it. Whether a record written this way
   also needs a valid signature is not known (see "The CDB file header"
   below).

## Where a record is filed

Inside `cdb.vff` the root is `CDB:` (`CDBConv.c`, `CDBGenRootPath`):

```
/YYYY/MM/DD/hh/mm/<APP>_<GROUP>/<type>/<SECONDS>.<SERIAL>
```

| Part | What it is |
| --- | --- |
| date folders | the Wii Menu's own clock when it files the message, not the message's Date header. Months are counted from 0 (`calendar.mon`). |
| `<APP>_<GROUP>` | the posting title's 4-letter code and its 2-character maker code. For a Wiiload app under HBC this is `HBLW_01`. Group 0 or 1 makes `receive()` pass no app or group, and the database's default is used instead. |
| `<type>` | `txt` for text only; `odh` when the message has a photo; `dat` for minidata; `log` for the play log. |
| `<SECONDS>` | seconds since 2000-01-01, as 8 hex digits (`%08X`) |
| `<SERIAL>` | a 3-digit serial (`%03d`), `000` for the first record with that time (`CDBConvKeyStrToFileName`) |

The SD-card backup (`CDBFileSystemSD.c`) puts the same tree under
`SD:/private/wii/title/HAEA/<ID-hi>/<ID-lo>/`. The two IDs are the console
ID's two words (below), as `%08X`.

### Proof

The Wii Menu in Dolphin filed two of sdk/mboard's posts:

| Message | Filed at |
| --- | --- |
| text only | `/2026/10/01/12/05/HBLW_01/txt/3279E781.000` |
| with a photo | `/2026/09/22/12/05/HBLW_01/odh/326CB880.000` |

## A record file

Each file is a 0x400-byte CDB file header, then the record
(`CDBFBridgeVF.c`: `offset += 0x400`).

### The CDB file header (CDBAttrBuf, 0x400 bytes)

This is from `libs/RevoEX/include/private/cdb/CDBAttr.h`.

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 8 | `"CDBFILE"`, then the version, 2 |
| 0x08 | 8 | console ID (below) |
| 0x10 | 1 | the description's length |
| 0x14 | | the description, e.g. `"ripl_board_record"` |
| 0x70 | 8 | CDB ID (the number from `cdb.conf`, and a modified count) |
| 0x78 | 4 | the record's size |
| 0x7C | 4 | last modified, seconds since 2000 |
| 0x80 | 32 | key string, `"%010u_GGGG_MM_SSS.typ"`: the time as 10 decimal digits, the app, the group, the serial and the type |
| 0xA0 | 16 | an IV |
| 0xB0 | 20 | HMAC-SHA1 (`CDBAttrSignature`, 0x1BC bytes in all) |

**The signature is unverified.** `CDBRecord.c`, `CDBDatabase.c`, `hmac.c` and
`aes.c` are not decompiled. So these three things are not known:

- which bytes the HMAC covers;
- which key signs it;
- whether a read from NAND checks it at all.

`CDBRecordFileReadAttrBuf` checks only the magic, the version and the
description. The IV, and `CDBRecordBackupToSD`, suggest that the signing and
encryption are for the SD backup.

### The console ID and key

This is from `CDBSystem.c`, `CDBInitHashedMacAddr`.

```
key = SHA1( wireless MAC[6] || 0x75 0x79 0x79 )     # 20 bytes
```

- **The MAC** is the wireless one (`NCDGetWirelessMacAddress`).
- **The console ID** is `key[0:8]` (`CDBGetWiiId`). The same 8 bytes are in
  every record header, and in `data/nocopy/cdbwiiid.dat`. That file is 40
  bytes: the 20-byte SHA-1 and 20 zero bytes (`CDBGetDeviceKey`).
- **`CDBGetWiiIdKey`** returns `key[8:20]`. That this is the HMAC key is
  likely but unproven.

### The board record (RIPL_BoardRecord)

This is from `include/system/RIPL_BoardRecord.h`; `iplCdbManager.cpp`
writes it.

| Offset | Size | Field |
| --- | --- | --- |
| 0x000 | 4 | `'RI_5'` |
| 0x004 | 8 | x and y on the board (`f32`) |
| 0x00C | 4 | flags: byte 0 opt-out, byte 3 the type (0 Memo, 1 Letter, 2 play log, 3 system update) |
| 0x010 | 8 | `OSTime` |
| 0x018 | 0x100 | the sender's address (`NWC24FriendAddr`) |
| 0x118 | 2 | address type |
| 0x11A | 2 | no reply: 0 means Reply is available, 1 means it is not |
| 0x11C | 4 | offset of the title (the alt name) |
| 0x120 | 4 | offset of the body text |
| 0x124 | 4 | offset of the Mii face, 0x4A bytes; 0 means none |
| 0x128 | 2 × 12 | attachments: type (1 picture, 2 board data, 3 play log, 4 minidata), offset, size |
| 0x140 | 4 | CRC-32 (`NETCalcCRC32`) of the 0x140 bytes before it |

The record's data follows the header. Attachments are aligned to 32 bytes.

In Dolphin, the record for sdk/mboard's full test post had:

- `RI_5` at the start;
- a picture of 1,156 bytes at 0x200 (the Wii Menu's conversion of the JPEG);
- the 128 bytes of board data at 0x6A0, unchanged.

## What sets the sender, icon and contents

These are the message's MIME headers and parts. `receive()` reads them, and
`NWC24CommitMsg()` writes them (Petari `NWC24MsgCommit.c`). The SDK writes
them in this order:

1. Date, From, To, Message-Id
2. X-Wii-AppId, X-Wii-Cmd
3. X-Wii-AltName, X-WiiFace
4. X-Wii-MB-NoReply, X-Wii-MB-RegDate, X-Wii-MB-Delay
5. MIME-Version, then the parts

The three `X-Wii-MB-*` headers are written only for a board message.
sdk/mboard writes the same, byte for byte (`tests/test_mboard.py` rebuilds
them from the SDK's format strings).

| Header | What it sets |
| --- | --- |
| `X-Wii-AltName:` | the sender's name: UTF-16BE with its NUL, as base64 in 42-byte pieces, each line `" " base64 CRLF`; up to 35 units. A board message has no Subject. |
| `X-WiiFace:` | the icon: a Mii's 74-byte `RFLCharData` (no CRC), base64 in 42-byte pieces. The Wii Menu shows it only if the Mii is valid, and also adds it to the hidden Mii database ([mii.md](mii.md)). |
| `X-Wii-MB-NoReply: -` | no Reply button |
| `X-Wii-MB-RegDate: %04X` | the date it is filed under: `(year - 2000) << 9 \| month << 5 \| day`, month 1-12, year 2000-2035 (`NWC24SetMsgMBRegDate`) |
| `X-Wii-MB-Delay: %02X` | hours to hold it back (see below) |
| `X-Wii-AppId: 2-AAAAAAAA-GGGG` | the type (2 = Wii Menu), the app ID and the group ID. The app and group must be six capital letters or digits, or the Wii Menu drops the message. |
| `X-Wii-Cmd: %08X` | the LED pattern (`NWC24SetMsgLedPattern`, with bit 18 set) |

**The message type** must be WII_MENU (2), MENU_SHARED (0) or PUBLIC (4).

**The record type** comes from the app ID:

- App ID `HAEA`, or 2: a Memo, or a system update with
  `X-Wii-MB-UpdateSW`.
- Any other app ID: a Letter.

**Two messages are special:**

- A Mii Channel message (`TITLE_NIGAOE_ALL`) only delivers its Mii, and is
  deleted.
- A message whose delay has not passed stays in the receive box until it
  has.

### A delay

`NWC24SetMsgMBDelay` is allowed only for a message to the console itself.
At commit, `CorrectHeadersForMBDelay` calls `NWC24SetMsgDesignatedTime` with
the delay in minutes, at most 14,400 (240 hours). That changes two fields of
the control entry:

- **0x2C** becomes the UTC minute (from 1900) at which the message may be
  shown.
- **Bit 24** of the entry's flags is set.

### The parts: text, photo, board data

Without attachments, the text is the whole body. With them (`NWC24SetMsgAttached`, at most two):

```
MIME-Version: 1.0
Content-Type: multipart/mixed;
 boundary="Boundary-NWC24-<minutes %08X><message id %05X>"

--Boundary-...
Content-Type: text/plain; charset=utf-16be
Content-Transfer-Encoding: 8bit

<the text>
--Boundary-...
Content-Type: image/jpeg;
 name=a0000007.jpg
Content-Transfer-Encoding: base64
Content-Disposition: attachment;
 filename=a0000007.jpg

<base64, 76 characters (57 bytes) to a line; the last line, then an empty line>
--Boundary-...--
```

**The part name** is a letter (`a` for the first part, `b` for the second),
then the message ID as 7 digits, then a suffix:

| Type | Suffix |
| --- | --- |
| `image/jpeg` | `jpg` |
| `image/x-wii-picture`, `application/x-wii-msgboard`, `application/x-wii-minidata` | `wii` |
| `application/octet-stream` | `dat` |

**Each line ends in CRLF.** The final line of base64 is followed by an empty
line even when nothing is left over.

**The control entry** for a message with attachments differs from a
text-only one:

| Offset | Field |
| --- | --- |
| flags | bit 16 (multipart) set |
| 0x10 | offset of the first boundary |
| 0x31 | number of attachments |
| 0x50 + 8i | each attachment's base64 span: offset, size |
| 0x60 + 4i | each attachment's size before encoding |
| 0x68 + 4i | each attachment's MIME type |

Unused slots of 0x68 + 4i keep `text/plain`.

| MIME type | Enum | SDK limit | What the board does |
| --- | --- | --- | --- |
| `image/jpeg` | 0x20000 | the total below | converts it to the Wii's picture format (ODH, 0x14000 bytes of output at most); shows `my_question.jpg` if that fails; files the record as `odh` |
| `image/x-wii-picture` | 0x20001 | 0x14000 | shows it; `receive()` accepts under 0x15400 |
| `application/x-wii-msgboard` | 0x30001 | 0x1E000 | board data, stored as attachment type 2 |
| `application/x-wii-minidata` | 0x30002 | 0x80 | `receive()` accepts under 0x2800; the record type is `dat` |

**Limits.** The attachments together must stay under 0x245B0 bytes. The
encoded text and attachments together must stay under the receive box's
largest message, 0x31C00 (`CheckMsgBoxSpace`). The Wii Menu keeps one
picture and one board-data part.

### Board data: a U8 archive

The one board-data part seen on the bench Wii is a Wii Shop Channel message,
from KD in 2013 (`r0000003.msg`). It is a U8 archive (`55 AA 38 2D`) holding
one file, `chjump.bin`, 0x56 bytes, big-endian:

| Offset | Value |
| --- | --- |
| 0x00 | `"ChJp"` |
| 0x04 | its size |
| 0x08 | 1 |
| 0x0C | 0 |
| 0x10 | the title to open: `00010002` `48414241` (`HABA`, the Wii Shop Channel) |
| 0x18 | the argument's offset, 0x20 |
| 0x1C | the argument's length, NUL included, padded to 2 bytes |
| 0x20 | the argument: `/startup?initpage=showTitle&titleId=0001000148435845` |

That is a "go to this channel" button. `tests/dolphin_mboard.py`'s
`chjump_arc()` rebuilds this archive byte for byte, and builds others. The
Wii Menu kept a `chjump.bin` that opens HBC in the record unchanged. Whether
the board shows a working button for it has not been checked on screen.
Stationery (`letterhead.arc`) is said to travel the same way, but no sample
was examined.

## Doing it from homebrew

Use sdk/mboard. `mboard_post()` sends text, and `mboard_post_msg()` adds a
Mii, a JPEG, board data, Reply, a delay and a filing date ([mboard.md](mboard.md)).
None of these needs the console key. The record and header layouts above are
for reading existing board records, and for any later work on writing
records straight into `cdb.vff`.
