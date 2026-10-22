# Message Board messages: authentication, icon and information

How a message is stored on the Wii Message Board, how the console decides a
message is genuine, and which fields set what the board shows — the sender
name, the icon, and the text. This complements [messageboard.md](messageboard.md)
(the play log, `playtimelog` records) and [mboard.md](mboard.md) (the
`sdk/mboard` library). Worked out on 2026-10-22 from the Revolution SDK and
Wii Menu decompilations, and confirmed on the bench Wii and in Dolphin.

## Two ways a message reaches the board

The board is files in `cdb.vff`
(`/title/00000001/00000002/data/cdb.vff`). A message gets there one of two
ways:

1. **Through the WiiConnect24 receive box** (`/shared2/wc24/mbox/wc24recv.*`).
   The Wii Menu's `receive()` reads each pending message at start-up, builds a
   board record, and files it. This is what `sdk/mboard`'s `mboard_post` uses
   (mboard.md). The Wii Menu re-derives everything, so the message needs no
   console-specific signature.
2. **Written straight into the board's storage.** A record placed directly in
   `cdb.vff`'s board tree must carry a per-console authentication field, or the
   Wii Menu rejects it. This is the format below.

The displayed sender name, icon and text are set the same way whichever route
is used.

## The authenticated board record

A board record (distinct from a `playtimelog` record) is filed at:

```
.../title/HAEA/<ID0>/<ID1>/YYYY/MM/DD/HH/MM/HABA_#1/txt/<SECONDS>.000
```

- `HAEA` is the board application; `HABA_#1` is the record group (compare the
  play log's `HAEA_#1/log`).
- `<ID0>` and `<ID1>` are the first and second four bytes of the console key
  (below), upper-case hex.
- The date folders are the record's time; **months are counted from 0**
  (`MM` = calendar month − 1), as everywhere in this system.
- `<SECONDS>` is that time in seconds since 2000-01-01, as 8 hex digits, and
  is also the file name.

### The console key

The key is derived from the console's network MAC address:

```
key = SHA1( MAC[6 bytes] || 0x75 0x79 0x79 )     # 20 bytes
```

The first 8 bytes identify the console; the last 12 bytes are the secret the
authentication field is keyed with.

### The fields that are set

Offsets are bytes from the start of the record. Big-endian.

| Offset | Size | Field |
| --- | --- | --- |
| 0x08 | 8 | console ID — `key[0:8]` (the same 8-byte ID the play log carries, normally read from `data/nocopy/cdbwiiid.dat`) |
| 0x7C | 4 | the record's time, seconds since 2000 |
| 0x80 | 10 | the same time, as 10 ASCII digits |
| 0xB0 | 20 | authentication: HMAC-SHA1 of the whole record, keyed with `key[8:20]`, computed with this field zeroed first |

To authenticate a record written directly to storage: zero 0xB0, fill in the
console ID and the two time fields, then compute `HMAC-SHA1(key[8:], record)`
over the entire record and write the 20-byte result at 0xB0. A record that
arrives through the receive box instead is authenticated by the Wii Menu
itself, so none of this is needed there.

## Setting the sender name, icon and information

What the board shows for a message comes from the message's MIME headers and
attachments, read by the Wii Menu's `receive()` (see mboard.md for the full
header block and the receive path). These are the fields that control the
presentation:

### The sender name

```
X-Wii-AltName: <base64 of the name in UTF-16BE, NUL-terminated>
```

Encoded in 42-byte pieces, one per line (`sdk/mboard` writes exactly this).
This is the name shown as the message's sender; with no alt name the board
falls back to the Wii Friend's stored name. Up to 35 UTF-16 units.

### The icon (a Mii face)

```
X-WiiFace: <base64 of the Mii's character data>
```

The Mii's `RFLCharData` (74 bytes, CRC stripped) base64-encoded. The first
line is laid out so the header key plus 56 base64 characters fill 67
characters, then a CRLF, a space, and the rest. The Wii Menu converts this to
a face and shows it as the message's icon (`RFLiNWC24Msg2CharData` in
`receive()`). Without it, the message shows the default/sender face.

### The message text

```
Content-Type: text/plain; charset=utf-16be
Content-Transfer-Encoding: 8bit

<the body, raw UTF-16BE; U+000A starts a new line>
```

The board shows this as the message body. There is no Subject line on a
board message — the sender name comes from `X-Wii-AltName`.

### Attachments: a picture and stationery

A board message may carry up to two attachments, each its own MIME part with
`Content-Transfer-Encoding: base64`:

| MIME type | On the board |
| --- | --- |
| `image/x-wii-picture` | a photo shown in the message (the Wii's own AJPG-ish picture format; the Wii Menu also accepts `image/jpeg` and converts it) |
| `application/x-wii-msgboard` | the letter's stationery/background (a `letterhead.arc`), which sets the look of the opened message |

Size limits the Wii Menu enforces: a picture under ~0x14000 bytes, message-board
data under 0x1E000, and the whole message under 0x245B0.

### Buttons and timing

Header flags set the buttons and when a message appears:

| Header | Effect |
| --- | --- |
| `X-Wii-MB-NoReply: -` | no Reply button |
| `X-Wii-MB-RegDate: %04X` | the date the message should appear on the board |
| `X-Wii-MB-Delay: %02X` | hold the message a number of hours before showing it |

`X-Wii-AppId` and `X-Wii-Cmd` identify the sending title and command; the app
and group id must be six capital letters or digits or the Wii Menu drops the
message (mboard.md).

## Doing it from homebrew

The straightforward path for an app is the receive-box route in `sdk/mboard`:
set the text and sender with `mboard_post`, and the Wii Menu files it with no
console key needed. Adding an icon or a picture means adding the `X-WiiFace`
or attachment parts to the composed message; the field layout above and the
entry layout in mboard.md are the reference. The authenticated direct-write
format is documented here for completeness and for reading existing board
records, not because `sdk/mboard` needs it.
