# Miis: the data, the database, and a Mii in a message

These notes were gathered on 2026-10-22 and 2026-10-23, first while adding
Mii faces to `sdk/mboard` (`mboard_msg.face`, `mboard_mii()`; see
[mboard.md](mboard.md)), then for the Mii library, `sdk/mii` (below). Each
fact names its source. Most are from the Wii Menu decompilation
([koopthekoopa/wii-ipl](https://github.com/koopthekoopa/wii-ipl), 4.3U), whose
`libs/RVLFaceLib` is the RFL library the Wii Menu links. The rest are from
the Revolution SDK decompiled in SMGCommunity/Petari, from Dolphin's source,
and from runs on the bench Wii and in Dolphin. Where a fact is unproven, it
says so.

## Where a Wii keeps its Miis

- **`/shared2/menu/FaceLib/RFL_DB.dat`** holds the Mii Channel's 100 Miis
  and the hidden database (below). Games read it through RFL, and any title
  may read and write it (`NAND_PERM_ALL_RW`, `RFL_NANDAccess.c`).
- **Each Wii Remote** carries up to 10 Miis in its own memory (below).
- **The six guests** (A to F) that games offer are built into RFL
  (`RFL_DefaultDatabase.c`) and are never in the file.
- **The face models** (`RFL_Res.dat`) are on each game's disc, or inside a
  U8 archive in one of the Wii Menu's own content files, not as a loose
  file. The bench Wii's `FaceLib` folder holds only `RFL_DB.dat`. An app
  cannot read the Wii Menu's copy without NAND rights: HBC's HOME >
  HBC > Save Mii faces copies it to the SD card (below, and
  [research/mii-rendering.md](research/mii-rendering.md) for drawing Miis).

## RFL_DB.dat

The file is RFL's `RFLiDatabase` (`RFLi_Database.h`,
`RFLi_HiddenDatabase.h`), big-endian:

| Offset | Size | Contents |
| --- | --- | --- |
| 0x00000 | 4 | `"RNOD"` |
| 0x00004 | 100 × 0x4A | the 100 Miis, each an `RFLiCharData` (below), in the Mii Channel's order |
| 0x01CEC | 4 | bit 31: isolation (mingling off); the rest padding |
| 0x01CF0 | 16 | a special invite (13 bytes), a month and a day (WiiConnect24), a pad byte |
| 0x01D00 | 4 | `"RNHD"`: the hidden database's header |
| 0x01D04 | 2 + 2 | its head and tail (`s16`; -1 for none) |
| 0x01D08 | 10,000 × 12 | its table, one node per hidden Mii |
| 0x1F1C8 | 22 | padding |
| 0x1F1DE | 2 | **the CRC**: CRC-16/XMODEM of bytes 0x00000-0x1F1DD |
| 0x1F1E0 | 10,000 × 0x40 | the hidden Miis' data. Not in the CRC. |

- **An empty slot** is all zeros (`RFLiClrCharInfo`); its creation ID is 0.
- **The size is 0xBE6C0 (779,968) bytes.** That is more than the data needs.
  RFL's format writes blocks of 0x16960 zero bytes from entries 0, 1250, ...
  8750, and the last one runs past entry 10,000 (`RFL_Format.c`). The bench
  Wii's file is that size.
- **There is one CRC.** RFL's `RFLiCalculateCRC` (`RFL_Database.c`) is a
  CRC-16 with polynomial 0x1021 and start 0. RFL runs it over the whole
  0x1F1E0-byte block with the CRC field set to 0. A block whose CRC is right
  gives 0. That is CRC-16/XMODEM of the bytes before the field, checked on
  the bench Wii's file.
- **A bad CRC loses the database.**
  - RFL counts the database as broken (`RFLiFileBrokenType_DBBroken`) and
    reads no Mii from it.
  - If a hidden Mii then arrives, RFL formats the file: it deletes it and
    makes a new, empty one (`RFLiFormatAsync`, `RFL_HiddenDatabase.c`,
    `RFL_Format.c`).
- **How RFL writes.**
  - It keeps the 0x1F1E0-byte block in memory. On a save it writes the block
    whole at offset 0, after a new CRC.
  - A hidden Mii's 0x40 bytes go first, at their own offset.
  - It opens the file with `NANDPrivateSafeOpenAsync` for each write, and has
    no lock. So a program that writes the file while the Mii Channel or a
    game using RFL runs is overwritten at that program's next save.

### The hidden database

The hidden database holds Miis that reach this Wii from elsewhere: on
letters, on a Wii Remote, by mingling. The Mii Channel never shows them, but
games can.

- **A node** is 12 bytes (`RFLiFormatTable`, `RFLi_Format.h`):
  - the creation ID (8 bytes);
  - a `u16`: sex (1 bit), then next (15 bits, signed);
  - a `u16`: padding (1 bit), then prev (15 bits, signed).

  The nodes form a doubly linked list from the oldest (head) to the newest
  (tail). A node is free when its creation ID is 0. There is no free list.
  A free node's next and prev are -1 (`RFLiClearTableData`: `7fff7fff` in
  all 10,000 nodes of the bench Wii's file).
- **The data** is 0x40 bytes (`RFLiHiddenCharData`). It is a Mii's first
  0x36 bytes with its birthday cleared (`RFLiConvertRaw2HRaw`), then 10 zero
  bytes. It keeps no creator's name.
- **Adding** (`RFLiOneDataToHiddenDB`, `writeData_`) takes only a Mii that
  has a valid ID, is not local only, and is not one of the 100. RFL clears
  the favourite flag, then uses the first of these that applies:
  1. the node already holding that ID, in place;
  2. the first free node, linked as the tail;
  3. when all 10,000 are used, the head (the oldest), which becomes the
     tail.
- **Removing** unlinks the node and clears it: ID 0, next and prev -1. The
  0x40 bytes of data stay where they were.
- **One Mii, one place.** Putting a Mii in one of the 100 slots removes its
  ID from the hidden database (`RFL_Database.c`).

## A Wii Remote's Miis

The Remote's memory (EEPROM) holds two copies of an `RFLiCtrlBuffer` of
0x2F0 bytes (`RFLi_Controller.h`, `RFL_Controller.c`). They are at 0x0FCA and
at 0x12BA. Dolphin's `WiimoteEmu.h` has the same range.

| Offset | Size | Contents |
| --- | --- | --- |
| +0x000 | 4 | `"RNCD"` |
| +0x004 | 2 | bit i: slot i came from the hidden database |
| +0x006 | 2 | padding |
| +0x008 | 10 × 0x4A | 10 Miis |
| +0x2EC | 2 | bit i: slot i deleted |
| +0x2EE | 2 | CRC-16/XMODEM of +0x000-+0x2ED |

- **Reading** (`RFL_Controller.c`):
  - RFL takes the first copy whose magic and CRC are right, copy 0 first.
  - It zeroes each deleted slot.
  - It zeroes each Mii that fails the check below or has a temporary ID.
- **Writing**:
  - Each Mii gets favourite 0, and local only 1 when isolation is set.
    `sdk/mii`'s `mii_remote_put` clears favourite; the isolation rule is the
    caller's (`mii_db_isolated`).
  - RFL writes a copy and reads it back, then does the same with the other.
- **The 0x0FCA base comes from Dolphin and WiiBrew.** RFL itself passes face
  offsets 0 and 0x2F0 to `WPADReadFaceData`, which is not decompiled.
  `sdk/mii` uses 0x0FCA. A real Remote proves it: the bench run below read 10
  Miis there that the Mii Channel had written, both copies whole.

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
| 0x20 | face type (3), face colour (3), face texture (4), padding (3), local only (1), kind (2: `type` in RFL, never set) |
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

The creation ID is two big-endian words at 0x18 (`RFL_Database.c`):

- **All zero** means no Mii (`RFLiIsValidID`).
- **The first word's flags**:
  - Bit 31 marks a normal Mii, which is every Mii a Wii makes. Without it the
    Mii is "special" (`RFLiIsSpecialID`), and a special Mii is valid only
    when it is local only.
  - Bit 30 marks a Mii made on a DS (`RFLiIsDSID`).
  - Bit 29 marks a temporary ID, which RFL never stores on a Wii
    (`RFLiIsTemporaryID`, `RFLiIsValidOnNAND`).
- **The first word's low 28 bits** are the creation time: seconds since
  2006-01-01, in 4-second units. RFL computes
  `OSTicksToSeconds((OSGetTime() - 11505369649262175) >> 2)`. The constant
  is 2006-01-01 in the Wii's time base (`RFLiSetCreateID`).
- **The second word** comes from the Wii's Wi-Fi MAC address
  (`createLowAddr_`):
  - its top byte is the sum of the MAC's first three bytes, with the top bit
    cleared unless those bytes are Nintendo's 00:17:AB;
  - its other three bytes are the MAC's last three.
- **A "home" Mii** is one whose second word is this Wii's, without the DS
  flag (`RFLiIsMyHomeID`). RFL makes any other Mii local only when it puts
  it in one of the 100 slots.
- **An `RFLStoreData`** is the 74 bytes and their CRC (76 bytes). The CRC is
  the same CRC-16/XMODEM.

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
  database. RFL skips a Mii that is one of the 100 or local only
  (`checkOneWritableData_`), so posting one of the Wii's own Miis
  (`mboard_mii()`) adds nothing there.

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
  Wii Menu does the rest.
  - A Mii read from the Wii's database already passes.
  - A Mii an app builds itself must meet the ranges above, or it is dropped
    without any error. `sdk/mii`'s `mii_check` says whether it will pass.
- **A contact's Mii in the Address Book** is only the Mii's creation ID (8
  bytes in `nwc24fl.bin`). `mboard_friend_add` and `mboard_friend_set` take a
  Mii and store its ID.
  - The Wii Menu then shows the Mii with that ID: one of the Wii's own, or
    one that came on a letter.
  - The Mii the user chose for a contact in the Wii Menu was stored the same
    way: Testorb's ID, on the bench Wii.
- **`tests/dolphin_mboard.py`'s `test_mii()`** builds a valid Mii from the
  field table above. It is a worked example of packing the bit fields.

## sdk/mii: the library

[`sdk/mii`](../sdk/mii) does all of the above from homebrew:

- a Mii as a struct and back, and the check;
- new Miis and IDs;
- `.mii` files;
- `RFL_DB.dat`: the 100 slots and the hidden database;
- a Wii Remote's Miis.

Its [README](../sdk/mii/README.md) has the API. These are the choices it
makes:

- **It refuses rather than repairs.** Nothing is written to a database
  (`MII_E_FORMAT`) when any of these is wrong, since RFL would already count
  it as lost:
  - the CRC or the size;
  - the hidden database's links. Each next and prev must be -1 or a node.
    The list must run from head to tail through used nodes only, each prev
    must point back, no node may appear twice, and every used node must be
    on the list.

  A CRC does not cover a tool's bug in those links. Following one could loop
  forever or write past the table.
- **It writes as RFL does.**
  - `mii_db_save` first checks the file is the one it loaded
    (`MII_E_CHANGED`).
  - It writes the changed hidden Miis' data first, each run of them on its
    own, then the 0x1F1E0-byte block at offset 0, in one open, and reads it
    all back.
  - A save that stops part way (`MII_E_IO`, `MII_E_VERIFY`) can be called
    again. It then writes over its own half-written file instead of calling
    it someone else's.
  - The file never changes size.
- **It keeps a Remote whole.**
  - `mii_remote_write` reads the Remote first and writes the copy that is not
    whole, then the other, each read back. If it stops part way, the other
    copy still holds a whole set, and a reader takes that one.
  - A Remote whose copies both have the magic but neither a right CRC reads
    as `MII_E_FORMAT`, not as a new Remote's empty set, so a misread can never
    lead to writing over its Miis. RFL fails its load there too.
- **It makes each ID new.** RFL's creation time counts 4-second steps, so two
  Miis made within 4 seconds would share an ID. `mii_make_id` keeps each ID
  at least one step past the last one it made.
- **It checks the flag bits.** `favorite` and `local_only` hold 1 bit each in
  the record, and `kind` 2. `mii_check` refuses a larger value, which
  `mii_pack` would otherwise cut to something else: local only 2 would be
  stored as 0.
- **It reads names strictly.** Bytes that are not UTF-8 become U+FFFD:
  - bad lead bytes;
  - short sequences;
  - overlong forms;
  - surrogates;
  - anything past U+10FFFF.

  A lone surrogate in a name is written back as U+FFFD too.
- **It writes a Remote 8 bytes at a time.** A write report carries up to 16
  bytes, but libogc 3's `wiiuse_write_data` keeps only the low 4 bits of the
  length (`clrlwi r11,r6,28` before it stores the size byte). A 16-byte write
  goes out with size 0, which a Remote ignores without an answer: Dolphin
  logs "WriteData: invalid size: 0". 8 works with any libogc, and 0x2F0 is
  94 writes of 8.
- **It waits for the network module for the MAC.** An app launched from HBC
  starts just after an IOS reload. For the first seconds, opening
  `/dev/net/ncd/manage` fails with -6 (not found), and so does libogc's
  `net_get_mac_address`, because IOS's network module has not registered it
  yet. `mii_wifi_mac` retries for up to 8 s, once, and keeps the answer, or
  the failure.
- **Its random Miis follow RFL's proportions** (`RFLi_MakeRandomFace`): half
  men; children, adults and elders 4:4:2; parts lower for women and children;
  beards on a fifth of grown men; few glasses on children. Each part is
  chosen evenly, not from RFL's tables.
- **It reaches the Remote through wiiuse.**
  - libogc has no public call for a Remote's memory. The library finds
    wiiuse's remote table the way `sdk/wiispk` does, and calls
    `wiiuse_read_data` and `wiiuse_write_data`.
  - Before `WPAD_Init` the table is not set yet; the library looks again on
    the next call.
  - On a libogc whose code does not match, the Remote calls return
    `MII_E_UNSUPPORTED`.

Tests:

- **`tests/test_mii.py`** runs on the PC. It checks:
  - the fields against the table above;
  - every limit, names and IDs;
  - the CRCs, against Python's `binascii.crc_hqx`;
  - the database and Remote layouts, built byte by byte;
  - each defect the 2026-10-24 audit found: flag bits out of range, a
    temporary ID, names that are not UTF-8, hidden-database links a CRC does
    not cover (7 kinds of break), a damaged Remote, and the free-slot calls.

  It also runs on a copy of the bench Wii's own `RFL_DB.dat`. Each Mii in it
  packs back to the same bytes and passes the check, and an add and a
  remove leave the file as it was.
- **`tests/dolphin_mii.py`** runs `tests/mii_app` in Dolphin on a fresh
  NAND that holds a database with one Mii:
  - it adds a Mii, saves it and loads it again (only that slot and the CRC
    change);
  - it puts a Mii into the hidden database and takes it out (the table and
    slots end as they were);
  - it writes a Mii to the emulated Wii Remote 1 and removes it, each read
    back;
  - it removes the Mii again.

  Dolphin keeps an emulated Remote's memory only while it runs, so the Remote
  half runs in one boot.
- **`tests/wii_mii.py`** is the proof on the bench Wii (2026-10-23). Each
  step copies the database before and after through `tests/nandget`:
  - `check`: the database loads in 725 ms, its CRC right. The one Mii there,
    made on that Wii, passes the check, and `mii_is_home` says it was made
    there. So RFL's ID recipe and `mii_wifi_mac` agree with the Mii Channel.
  - `hidden`: a new Mii goes into the hidden database and back out. The table
    and slots then match the copy from before, byte for byte. Only that
    Mii's 0x40 bytes of data remain, as with RFL.
  - `add`: "HBC Mii" goes into slot 1. Only that slot and the CRC changed, the
    CRC is right, and a fresh load holds the same Mii.
  - `restore`: a backup goes back through `tests/nandput`, and the file
    matches it byte for byte.

  - `check` with a real Wii Remote 1, which carries 10 Miis made on other
    Wiis: copy 0 has a right magic and CRC at 0x0FCA, read in 1149 ms, and
    every Mii passes the check.
  - `remote-rewrite`: the Remote's memory is saved first, then its 10 Miis
    are written back to both copies (7.2 s, 188 writes of 8 bytes) and read
    back. Both copies then match the bytes from before, byte for byte. So
    `mii_remote_build` makes exactly what the Mii Channel writes.

  libogc drops an idle Remote after 5 minutes, so a Remote step needs the
  Remote awake (a button pressed) as the job starts. `remote-add` stops
  with `MII_E_FULL` on a Remote with all 10 slots in use; it never
  overwrites a Mii.

## The face models (RFL_Res.dat)

A Mii's 74 bytes say which parts it has; drawing it needs the parts
themselves: `RFL_Res.dat`, Nintendo's shapes and textures for every
faceline, hair style, eye, mouth and so on. Games carry a copy on their disc
(Mario Kart Wii's `/contents/RFLRes01.arc`); the Wii Menu keeps one in a U8
archive in one of its contents. On the bench Wii (System Menu v513) that is
its content 4, a shared content in `/shared1`: 686,372 bytes, version 0x39D.

An app cannot read it: `ES_OpenTitleContent` refuses the Wii Menu's contents
(-1026) and ISFS refuses `/shared1` and the title's content folder (-102)
(`tests/mii_res_probe`, [research/mii-rendering.md](research/mii-rendering.md#62-finding-rfl_resdat)).
So the user provides their own copy, and HBC makes that one press:

- **HOME > HBC > Save Mii faces** (or `hbc.py mii-faces`, request `HBCM`)
  copies it to `sd:/mii/RFL_Res.dat` (`channel/channelapp/source/miires.c`).
  HBC lifts IOS's NAND permission check for the reads, as it does for the
  Message Board's play log (`nandperm.c`; it needs HBC's IOS, with
  AHBPROT), reads only the U8 header, its node table and the one file, and
  puts the check back. It checks the file (18 archives, version 0x39D or
  later) and writes it whole before renaming it into place. Under 2 s on
  the bench Wii.
- **The bodies too** (1.10.26): the same press copies the Mii Channel's
  (title 00010002-48414341) `maleBody.brres` and `femaleBody.brres` out of
  `Channel/Character.szs` (Yaz0, in a U8 archive in its content
  `00000019.app` on the bench Wii) to `sd:/mii`: Nintendo's skeleton, the
  skinned shirt and trousers, and 21 animations
  ([research/mii-bodies.md](research/mii-bodies.md#3-the-mii-channels-body-model)).
  0.7 to 0.9 s for faces and bodies on the bench Wii. Without a Mii Channel
  the faces are still saved, and the toast says so.
- **Keep them on your own SD card.** The files are Nintendo's: an app
  reads them from `sd:/mii` at run time and never ships them.
- **`tools/mii_res.py`** lists a copy (`info`) and dumps every shape as OBJ
  and every texture as PNG (`dump`), for study. Its layout is in the
  research notes; `tests/test_mii_res.py` checks the tool on a synthetic
  file.

## Drawing a Mii (sdk/mii/miidraw)

`miidraw.c` draws a Mii's head from `RFL_Res.dat` as RFL does;
`miibody.c` puts it on the Mii Channel's own body, animated, or
`miidraw.c` builds a body of its own when the bodies are not saved
([the SDK README](../sdk/mii/README.md#drawing-miis-miidraw),
[research/mii-rendering.md](research/mii-rendering.md) and
[research/mii-bodies.md](research/mii-bodies.md) for the sources). HBC's
avatar (1.10.21, the Wii's bodies from 1.10.26) is the first user.

- **The body**: the MDL0's 20 bones and two skinned meshes, posed from
  the CHR0 animations (cubic Hermite keys; rotations the short way
  round, or the right ankle in bodyWait spins), skinned on the GPU as
  NW4R draws a model: each MDL0 object's own display list (its primitives
  moved to `GX_VTXFMT1`), whose indexed matrix loads take each bone's
  matrices from a per-frame palette (`GX_POSMTXARRAY`, `GX_NRMMTXARRAY`;
  a ring of `MII_PALETTES`). Under 2 KB through the FIFO and about 16 us
  of CPU a Mii in a crowd (bones on the paired-single unit, the head as
  display lists; CPU skinning took 0.73 ms). Levels of detail
  (`mii_body_make_lods`, `mii_model_opts.lods`, `pose.lod`), shared parts
  (`mii_cache`) and `pose.reuse` are for crowds: sdk/mii/README.md, "Many
  Miis". Normals use the bone's
  rotation alone (the GPU does not renormalise): within 7 degrees of the
  CPU reference, exact at middle height and build. `mii_body_skin` stays as
  that reference. A change of animation cross-fades (`mii_pose.from_*`,
  quaternions per bone). Height and build scale each bone along its own
  axis, not inherited. The head rides the head bone.
- **Drawn in place**: `mii_view` gives the camera for any viewport (near
  and far fitted to the Mii), and `mii_draw` draws into the frame you are
  building (HBC's avatar does this each frame, with the whole depth range in
  front of its slot, then `gfx_depth_fill` puts the rectangle's depth back).
  The face's layers draw with Z compare but no Z write. `mii_capture` (into a texture, through RGBA6 and a copy) is
  for icons and stills.

- **The head**: RFL's shapes placed by the faceline's anchors, its colour
  tables, and the face (eyes, eyebrows, mouth, mustache, mole) composited
  on the CPU for each expression, at RFL's positions. The shape textures
  keep their own wrap: the glasses are one lens mirrored across.
- **Shown on the bench Wii** (2026-10-24, `tests/wii_avatar.py`): HBC at 60
  frames a second for a minute with the avatar, 19 us of CPU a frame. The
  first build sent each vertex through the GX FIFO (about 240 KB a frame)
  and overran HBC's 256 KB FIFO on the Wii: garbage triangles, then a hang
  that needed a power cycle. Dolphin showed neither. Each part is now a
  display list.
- `tests/test_miidraw.py` checks the library on the PC, and
  `mii_app.dol draw` its icon and capture on a Wii or in Dolphin.

## Sources

The Wii Menu decompilation (koopthekoopa/wii-ipl):

- `libs/RVLFaceLib/include/internal/RFLi_Types.h`: `RFLiCharData`,
  `RFLiHiddenCharData`.
- `include/internal/RFLi_FaceConfig.h`: the limits.
- `include/internal/RFLi_Database.h`, `RFLi_HiddenDatabase.h`,
  `RFLi_Format.h`: `RFLiDatabase`, the hidden database and its nodes.
- `include/internal/RFLi_Controller.h`: `RFLiCtrlBuffer`.
- `src/RFL_Database.c`: the IDs, conversions, name and CRC.
- `src/RFL_HiddenDatabase.c`: adding to and removing from the hidden
  database.
- `src/RFL_Format.c`, `src/RFL_NANDAccess.c`: the file, its size and how it
  is opened.
- `src/RFL_Controller.c`: a Wii Remote's Miis.
- `src/RFL_MakeRandomFace.c`: random Miis.
- `src/RFL_DefaultDatabase.c`: the six guests.
- `src/RFL_DataUtility.c`: `RFLiCheckValidInfo`.
- `src/RFL_NWC24.c`: the message import.
- `src/system/iplNwc24Manager.cpp`: `receive()`.
- `include/system/RIPL_BoardRecord.h`: the record's face offset.

The Revolution SDK (SMGCommunity/Petari):

- `src/RVL_SDK/nwc24/NWC24MsgObj.c`: `NWC24SetMsgFaceData`.
- `src/RVL_SDK/nwc24/NWC24MsgCommit.c`: `WriteXWiiFaceField`.
