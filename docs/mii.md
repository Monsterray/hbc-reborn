# Miis: the data, the database, and a Mii in a message

These notes were gathered on 2026-10-22 while adding Mii faces to
`sdk/mboard` (`mboard_msg.face`, `mboard_mii()`; see
[mboard.md](mboard.md)). Each fact names its source. Most are from the Wii
Menu decompilation ([koopthekoopa/wii-ipl](https://github.com/koopthekoopa/wii-ipl),
4.3U), whose `libs/RVLFaceLib` is the RFL library the Wii Menu links. The
rest are from the Revolution SDK decompiled in SMGCommunity/Petari, and from
runs in Dolphin. Where a fact is unproven, it says so.

## Where a Wii keeps its Miis

- **`/shared2/menu/FaceLib/RFL_DB.dat`** is the Mii Channel's database, and
  games read it through RFL. Its layout is RFL's in-memory `RFLiDatabase`
  (`RFLi_Database.h`):

  | Offset | Size | Contents |
  | --- | --- | --- |
  | 0x0000 | 4 | `"RNOD"` |
  | 0x0004 | 100 × 0x4A | 100 Miis, each an `RFLiCharData` (below) |
  | 0x1CEC | 1 bit | the isolation flag |

  Anything after 0x1CEC was not read here. An empty slot is all zeros, and
  its creation ID is 0. `mboard_mii(slot, face)` reads slot 0-99 and returns
  `MBOARD_E_NO_MII` for an empty slot.

- **A hidden database** holds Miis that arrive from elsewhere: messages, and
  the Mii Channel's exchange (`RFLiOneDataToHiddenDB`, `addHiddenDB`). It
  stores `RFLiHiddenCharData`, which is an `RFLiCharData` less its last 10
  bytes (`RFLiConvertRaw2HRaw` copies `sizeof(RFLiHiddenCharData) - 10`).
  Its file name and layout were not looked at.

## RFLCharData: 74 bytes (0x4A)

`RFLiCharData` is in `RFLi_Types.h`. The fields are big-endian `u16` bit
fields. The first field listed in each word is in that word's top bits,
because the compiler packs from the most significant bit on PowerPC. An
`RFLStoreData` is the same 74 bytes plus a 2-byte CRC (76 bytes). The
message header (`X-WiiFace`) carries the 74 bytes, with no CRC.

| Offset | Fields, from the top bit down (width in bits) |
| --- | --- |
| 0x00 | padding (1), sex (1), birth month (4), birth day (5), favourite colour (4), favourite (1) |
| 0x02 | name: 10 UTF-16 units, NUL-padded |
| 0x16 | height (u8) |
| 0x17 | build (u8) |
| 0x18 | creation ID: 8 bytes (two `u32`, below) |
| 0x20 | face type (3), face colour (3), face texture (4), padding (3), local only (1), type (2) |
| 0x22 | hair type (7), hair colour (3), hair flip (1), padding (5) |
| 0x24 | eyebrow type (5), eyebrow rotation (5), padding (6) |
| 0x26 | eyebrow colour (3), eyebrow scale (4), eyebrow Y (5), eyebrow X (4) |
| 0x28 | eye type (6), eye rotation (5), eye Y (5) |
| 0x2A | eye colour (3), eye scale (4), eye X (4), padding (5) |
| 0x2C | nose type (4), nose scale (4), nose Y (5), padding (3) |
| 0x2E | mouth type (5), mouth colour (2), mouth scale (4), mouth Y (5) |
| 0x30 | glasses type (4), glasses colour (3), glasses scale (4), glasses Y (5) |
| 0x32 | mustache (2), beard type (2), beard colour (3), beard scale (4), beard Y (5) |
| 0x34 | mole (1), mole scale (4), mole Y (5), mole X (5), padding (1) |
| 0x36 | creator's name: 10 UTF-16 units |

### The creation ID

The creation ID is two words at 0x18 (`RFL_Database.c`):

- **`RFLiIsValidID`:** an ID is valid unless both words are 0.
- **Bit 31 of the first word** marks a normal Mii, and it is set when a Mii
  is made (`dst[LOWADDR] |= 1 << FLAG_NORMAL`). An ID without bit 31 is
  "special" (`RFLiIsSpecialID`). A special Mii counts as valid only when it
  is local only.
- **The first word's low 28 bits** are the creation time
  (`date & 0x0FFFFFFF`).
- **Other bits:** DS Miis (`RFLiIsDSID`) and temporary IDs
  (`RFLiIsTemporaryID`) have flag bits of their own in the same word. Their
  bit numbers were not read.

## What makes a Mii valid

`RFLiCheckValidInfo` (`RFL_DataUtility.c`) is the check RFL makes on a Mii
that comes in from outside, for example on a message. Every part must be in
range; the limits are in `RFLi_FaceConfig.h`.

| Part | Allowed |
| --- | --- |
| face | type ≤ 7, colour ≤ 5, texture ≤ 11 |
| hair | type ≤ 71, colour ≤ 7, flip ≤ 1 |
| eyes | type ≤ 47, colour ≤ 5, scale ≤ 7, rotation ≤ 7, X ≤ 12, Y ≤ 18 |
| eyebrows | type ≤ 23, colour ≤ 7, scale ≤ 8, rotation ≤ 11, X ≤ 12, **3 ≤ Y** ≤ 18 |
| nose | type ≤ 11, scale ≤ 8, Y ≤ 18 |
| mouth | type ≤ 23, colour ≤ 2, scale ≤ 8, Y ≤ 18 |
| beard | mustache ≤ 3, type ≤ 3, colour ≤ 7, scale ≤ 8, Y ≤ 16 |
| glasses | type ≤ 8, colour ≤ 5, scale ≤ 7, Y ≤ 20 |
| mole | type ≤ 1, scale ≤ 8, X ≤ 16, Y ≤ 30 |
| body | height ≤ 128, build ≤ 128 |
| personal | sex ≤ 1; a real birthday (`RFLiCheckBirthday`); favourite colour below `RFLFavoriteColor_Max`; the name's first unit not 0 (`RFLiIsValidName`) |
| ID | not special, unless local only |

**The eyebrow Y is the trap.** It is the only lower bound in the table, so
an all-zero face fails the check. The first Dolphin run used such a face, and
the Wii Menu filed the message without the Mii: the board record's face
offset was 0. With `test_mii()` (every part in range), the Wii Menu kept the
face: the record held all 74 bytes at offset 0x1F8 (`tests/dolphin_mboard.py
--full`, 2026-10-22).

## A Mii as a message's icon

- **Sending.** The SDK's `NWC24SetMsgFaceData` takes the `RFLCharData`
  (`sizeof` = 0x4A), and `NWC24CommitMsg()` writes it after the alt name
  (`WriteXWiiFaceField`, Petari `NWC24MsgCommit.c`):

  ```
  X-WiiFace: <base64 of bytes 0-41>
   <base64 of bytes 42-73>
  ```

  It is base64 in 42-byte pieces, each line a space, the base64 and CRLF:
  "X-WiiFace: " and 56 characters, then " " and 44 characters.
  `mboard_msg.face` writes exactly this.
- **Receiving** (`ipl::nwc24::Manager::receive()`, `iplNwc24Manager.cpp`):
  1. `RFLiNWC24Msg2CharData` reads the face back from the message.
  2. `MiiManager::isValid` checks it. That class is not decompiled; it
     probably makes the checks above.
  3. If the Mii passes, it goes into the **hidden database**
     (`addHiddenDB`), and the board record keeps the face. Its offset is at
     0x124 of the `RIPL_BoardRecord`, and the face is
     `NWC24_FACE_DATA_SIZE` = 0x4A bytes.
  4. If it fails, the message is still filed, without a Mii.

  So a post with a Mii also tries to leave that Mii in the Wii's hidden
  database. Posting one of the Wii's own Miis (`mboard_mii()`) adds a copy
  of it there. Whether RFL skips a Mii already in the database was not
  checked.

  In Dolphin the hidden-database save was refused, and RFL logged:

  ```
  [RFL]NWC24ReadMsgFaceData: errcode=0
  [RFL] Isolation Flag is Valid.
  ```

  The Dolphin profile has no `RFL_DB.dat`, so its database is presumably
  isolated. The board record kept the face anyway, so the icon does not
  depend on the hidden database. On a real Wii, with the Mii Channel set up,
  the save is expected to succeed. That is not checked yet.
- **RFL's own import** (`RFLiNWC24Msg2...`, `RFL_NWC24.c`) refuses a Mii in
  each of these cases:
  - a broken creation ID ("Invalid CreateID");
  - a local-only Mii ("char Isolation Flag is Valid");
  - the database's isolation flag ("Isolation Flag is Valid");
  - a part out of range ("Invalid parts data").

  It clears the favourite flag before it saves the Mii to the hidden
  database.
- **Messages from the Mii Channel.** A message whose app ID is the Mii
  Channel's (`TITLE_NIGAOE_ALL`) only delivers its Mii to the hidden
  database. The Wii Menu then deletes it and does not show it on the board.

## In sdk/mboard

- **`mboard_mii(slot, face)`** reads one Mii from `RFL_DB.dat`. Any title
  may read that file.
- **`mboard_msg.face`** checks only that the creation ID is not 0, and the
  Wii Menu does the rest. A Mii read from the Wii's database already passes.
  A Mii an app builds itself must meet the ranges above, or it is dropped
  without any error.
- **A contact's Mii in the Address Book** is only the Mii's creation ID (8
  bytes in `nwc24fl.bin`). `mboard_friend_add` and `mboard_friend_set` take a
  Mii and store its ID.
  - The Wii Menu then shows the Mii with that ID: one of the Wii's own, or
    one that came on a letter.
  - The Mii the user chose for a contact in the Wii Menu was stored the same
    way: Testorb's ID, on the bench Wii.
- **`tests/dolphin_mboard.py`'s `test_mii()`** builds a valid Mii from the
  field table above. It is a worked example of packing the bit fields.

## Sources

The Wii Menu decompilation (koopthekoopa/wii-ipl):

- `libs/RVLFaceLib/include/internal/RFLi_Types.h`: `RFLiCharData`.
- `include/internal/RFLi_FaceConfig.h`: the limits.
- `include/internal/RFLi_Database.h`: `RFLiDatabase`.
- `src/RFL_Database.c`: the IDs, conversions and name.
- `src/RFL_DataUtility.c`: `RFLiCheckValidInfo`.
- `src/RFL_NWC24.c`: the message import.
- `src/system/iplNwc24Manager.cpp`: `receive()`.
- `include/system/RIPL_BoardRecord.h`: the record's face offset.

The Revolution SDK (SMGCommunity/Petari):

- `src/RVL_SDK/nwc24/NWC24MsgObj.c`: `NWC24SetMsgFaceData`.
- `src/RVL_SDK/nwc24/NWC24MsgCommit.c`: `WriteXWiiFaceField`.
