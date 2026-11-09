# mii

*Miis in your homebrew: read them, make them, save them, draw them.*

| | |
| --- | --- |
| **Files** | [`mii.h`](mii.h) and [`mii.c`](mii.c) for the data; [`miidraw.h`](miidraw.h), [`miidraw.c`](miidraw.c), [`miibody.c`](miibody.c) and [`miifx.c`](miifx.c) for drawing |
| **Needs** | libogc 3 (HBC's) or libogc2; `-lwiiuse -lbte -logc -lm` on the Wii. The parts that only touch bytes build on a PC with `-DMII_NO_OGC`. Drawing needs the user's own Wii files on the SD card |
| **Licence** | Public domain |
| **Changes on the Wii** | Only when you save: the Mii database or a Wii Remote's Miis (see [below](#what-it-changes-on-the-wii)) |
| **Since** | HBC 1.10.11 (drawing: 1.10.21) |

**Contents:** [What it does](#what-it-does) · [Quick start](#quick-start) ·
[Add it to your app](#add-it-to-your-app) · [API](#api) ·
[More examples](#more-examples) · [Drawing Miis (miidraw)](#drawing-miis-miidraw) ·
[Versions](#versions) · [For someone to add](#for-someone-to-add) ·
[Docs and tests](#docs-and-tests) ·
[What it changes on the Wii](#what-it-changes-on-the-wii) · [Limits](#limits) ·
[Licence](#licence)

## What it does

`mii` gives homebrew what the Wii's own Mii library (RFL) gives games:

- **A Mii as a struct.** It reads and writes the 74 bytes every Wii file
  holds, and checks a Mii as the Wii does, saying why one fails.
- **New Miis.** It makes a plain Mii to edit or a random one, and gives a Mii
  a new creation ID as the Mii Channel does.
- **`.mii` files.** It imports them, and exports the 74-byte form and the
  76-byte form with a CRC.
- **The Mii database** (`RFL_DB.dat`). It reads, adds, changes and removes
  the Mii Channel's 100 Miis, and the hidden database of Miis met on letters
  and Remotes. It never writes a database whose CRC is wrong.
- **A Wii Remote's Miis.** It reads and writes the 10 Miis a Remote carries
  (`mii_remote_read()` and `mii_remote_write()`).
- **Drawing.** It draws a Mii with GX: the head as the Wii draws it, from the
  user's own face models (`sd:/mii/RFL_Res.dat`, saved with HOME > HBC > Save
  Mii faces), all seven expressions, and a body, posed. Capture it into a
  texture with alpha, make a head icon, or draw it in your own 3D scene.
  HBC's avatar ([avatar.c](../../channel/channelapp/source/avatar.c)) is the
  worked example: your favourite Mii stands beside the app list, breathes,
  looks around, blinks, smiles and waves, looks at the Wii Remote's pointer,
  and A on it shows the next Mii.

[example/](example) is a whole program that uses the data part (CI builds it
with `make -C sdk/mii/example`). It lists this Wii's Miis and the Miis on Wii
Remote 1; A adds a random Mii named "Example" to the Mii Channel's plaza (it
asks first), HOME leaves.

## Quick start

Each call that can fail returns 0 or more, or a negative `MII_E_*` code
(`mii_strerror()` names it). The snippets need:

```c
#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>   // gettime()
#include <wiiuse/wpad.h>
#include "mii.h"
```

Read every Mii:

```c
mii_db db = { 0 };
mii m;
char name[MII_NAME_UTF8_MAX];

int r = mii_db_load(&db);      // MII_E_NO_FILE: the Mii Channel has never been opened
if (r < 0) {
    printf("%s\n", mii_strerror(r));
    return;
}
for (int i = 0; i < MII_DB_SLOTS; ++i)
    if (mii_db_get(&db, i, &m) == MII_OK) {
        mii_get_name(m.name, name, sizeof(name));
        printf("%d: %s, likes %s\n", i, name, mii_color_name(m.fav_color));
    }
mii_db_free(&db);
```

Make a Mii and add it to the plaza:

```c
uint32_t seed = (uint32_t) gettime();
mii m;
const char *why;

mii_random(&m, MII_ANY, MII_ANY, &seed);   // or mii_default(&m, MII_FEMALE)
mii_set_name(m.name, "Homebrew");
mii_set_name(m.creator, "My App");
m.height = 90;               // 0 shortest .. 128 tallest (64 the middle)
m.build = 40;                // 0 thinnest .. 128 heaviest
m.birth_month = 10;          // 0/0 for no birthday
m.birth_day = 24;
m.fav_color = MII_GREEN;
m.favorite = 1;              // starred in the Mii Channel
m.local_only = 0;            // 0: Mingle on
if (mii_make_id(&m) < 0 || mii_check(&m, &why) != MII_OK)
    return;                  // why says what the Wii would refuse

mii_db db = { 0 };
if (mii_db_load(&db) == MII_OK) {
    int slot = mii_db_add(&db, &m);         // the first free slot, or MII_E_FULL
    int r = slot < 0 ? slot : mii_db_save(&db);
    if (r == MII_E_IO || r == MII_E_VERIFY)
        r = mii_db_save(&db);               // a save that did not finish may be retried
    // MII_E_CHANGED: someone else wrote the file; load it again and add again
    mii_db_free(&db);
}
```

## Add it to your app

- Copy `mii.h` and `mii.c` into your project (and the drawing files, if you
  draw). Compile the `.c` files as C.
- Link with `-lwiiuse -lbte -logc -lm`, in that order, as
  [example/Makefile](example/Makefile) does. The library uses wiiuse for the
  Remote and libogc's network code for the Wi-Fi MAC, even if your app uses
  neither.
- With `-DMII_NO_OGC`, the parts that only touch bytes build on a PC, for
  tools and tests.
- **Your own matrix maths.** libogc's `guMtxConcat` (paired-single
  `ps_guMtxConcat`) loads a constant with a `psq_l` through `r13`, whose
  offset has 12 bits. devkitPPC's linker scripts put the small-data base
  32 KiB past `.sdata`, so the offset is cut short and the load reads the
  wrong word: every product it makes is off. The library uses the C one.
  For your own calls, use `c_guMtxConcat`, or set the base at the start of
  `.sdata` in your linker script (`_SDA_BASE_ = .;`, as HBC's `newrvl.ld`
  does). Check with `powerpc-eabi-objdump -d app.elf | grep 'psq_l.*(r13)'`:
  the last-but-one number must be 0.
- **Builds with libogc 3 and libogc2.** HBC uses libogc 3; every function
  here also compiles and links with devkitPPC r41-2 and libogc2.
- **Your GX FIFO in MEM1** (if you draw). Make it a static array
  (`static u8 fifo[SIZE] ATTRIBUTE_ALIGN(32);`), not `memalign` from the
  heap: once MEM1's part of the heap runs out, the heap gives MEM2, and a
  GPU fed its commands from MEM2 stalled at random on a real Wii (never in
  Dolphin). HBC lost days to this when linking this library grew its .bss
  ([docs/history/1.10.md](../../docs/history/1.10.md), 1.10.26). A Mii adds
  under 2 KB of commands a frame (its lists and arrays stay in memory, where
  the GPU reads them).

## API

Rules for every call:

- A call that can fail returns 0 or more, or a negative `MII_E_*` code
  ([errors](#errors)).
- No call changes the `mii` you pass in. `mii_db_put` and the others store a
  copy, which may differ (`local_only`, `favorite`): read the slot back to
  see what was stored.
- Call from one thread. The Wii calls share static buffers.
- The Wii calls block ([times](#limits)): call them outside your frame loop.

### Read and check

| Name | What it does |
| --- | --- |
| `const char *mii_strerror(int result)` | Names an `MII_E_*` code. |
| `const char *mii_color_name(int fav_color)` | `"red"` to `"black"`; `"?"` out of range. |
| `void mii_unpack(const uint8_t raw[MII_SIZE], mii *m)` | The 74 bytes to the struct. Checks nothing. |
| `void mii_pack(const mii *m, uint8_t raw[MII_SIZE])` | The struct to the 74 bytes of a `.mii` file. Keeps only each field's own bits (a value out of range is cut). |
| `int mii_check(const mii *m, const char **why)` | `MII_OK` if the Wii would accept the Mii (RFL's `RFLiCheckValidInfo`, every field in range, creation ID not 0); else `MII_E_INVALID`, and `*why` (if not NULL) names the first problem, such as "eyebrow Y below 3". The Wii drops a Mii it refuses on a letter or a Remote without a word, so check one you built yourself. |
| `int mii_set_name(uint16_t name[MII_NAME_MAX], const char *utf8)` | UTF-8 in. Returns the UTF-16 units kept, at most 10. A longer name is cut at a whole character; bytes that are not UTF-8 become U+FFFD. |
| `int mii_get_name(const uint16_t name[MII_NAME_MAX], char *utf8, size_t size)` | UTF-8 out. Returns the length written, always NUL ended. A buffer under `MII_NAME_UTF8_MAX` bytes may cut the name, at a whole character. |
| `int mii_get(int slot, mii *m)` | One Mii from the 100 slots, without keeping the database. |

### Make and edit

| Name | What it does |
| --- | --- |
| `void mii_default(mii *m, int sex)` | A plain Mii to edit: every part in range, named "Mii", height and build in the middle, no creation ID. `sex` is `MII_MALE` or `MII_FEMALE`. |
| `void mii_random(mii *m, int sex, int age, uint32_t *seed)` | A random Mii in the Wii's proportions. `sex`: `MII_MALE`, `MII_FEMALE`, `MII_ANY`. `age`: `MII_CHILD`, `MII_ADULT`, `MII_ELDER`, `MII_ANY`. `*seed` is any number (the time, say) and is advanced, so a loop makes a different Mii each time. Named "Mii", no creation ID. |
| `int mii_make_id(mii *m)` | A new creation ID on `m`, from this Wii's clock and Wi-Fi MAC. Each call gives a new ID, even within the same 4 seconds. |
| `int mii_is_home(const mii *m)` | 1 if `m` was made on this Wii. |
| `int mii_wifi_mac(uint8_t mac[6])` | This Wii's Wi-Fi MAC, which RFL builds its IDs from. Needs no `net_init()`. Just after an app is launched from HBC the first call waits up to 8 s for IOS's network module. The answer is kept; after `MII_E_IO` the next call asks again at once. |
| `int mii_id_empty(id)`, `int mii_same_id(a, b)` | Is the creation ID all zero (no Mii), and are two IDs equal. |
| `uint32_t mii_id_flags(id)`, `uint32_t mii_id_time(id)` | The flags (`MII_ID_NORMAL`, `MII_ID_DS`, `MII_ID_TEMP`) and the creation time: seconds since 2006-01-01 00:00 (local time on the Wii that made it), a multiple of 4. |
| `void mii_id_make(id, uint64_t ticks, const uint8_t mac[6])` | The ID RFL makes, from the time alone (`ticks`: libogc's `gettime()`, 60.75 MHz since 2000-01-01). Two IDs made within the same 4 seconds are the same; `mii_make_id` avoids that. |
| `uint32_t mii_id_home(const uint8_t mac[6])` | The ID's second word for a MAC. A Mii whose second word matches was made on that Wii (RFL's "home" Mii). |
| `uint16_t mii_crc16(const void *data, size_t size)` | CRC-16/XMODEM (polynomial 0x1021, start 0), as every Mii CRC on the Wii. |
| `void mii_store(raw, uint8_t out[MII_STORE_SIZE])` | The 74 bytes plus the CRC: the 76-byte form (RFLStoreData). |
| `int mii_import(const void *file, size_t size, mii *m)` | A 74-byte (`.mii`) or 76-byte file. The CRC of the 76-byte form must match (`MII_E_CRC`). Any other size, such as a 3DS or Wii U Mii, is `MII_E_ARG`. The Mii is not checked: call `mii_check` before storing it. |

### The Mii database

`/shared2/menu/FaceLib/RFL_DB.dat`: 100 slots in the Mii Channel's order (its
plaza), and the hidden database of up to 10,000 Miis this Wii has met (on
letters, from Remotes, by mingling), oldest first, that the Mii Channel never
shows but games can see. Start from a zeroed `mii_db`, or one `mii_db_free`
emptied; read its fields only through these calls.

| Name | What it does |
| --- | --- |
| `int mii_db_load(mii_db *db)` | Reads `RFL_DB.dat` (about 780 KB, from the heap) and checks it. Any title may. `MII_E_NO_FILE` if the Mii Channel has never been opened. If the Wi-Fi MAC cannot be read, `db->home` stays 0 and other Wiis' Miis are stored as given. |
| `int mii_db_save(mii_db *db)` | Writes the changes back, then reads them back. `MII_E_CHANGED` if someone else wrote the file since the load. After `MII_E_IO` or `MII_E_VERIFY`, call it again. |
| `int mii_db_attach(mii_db *db, uint8_t *file, size_t size)` | Uses a database already in memory (`MII_DB_FILE_SIZE` bytes, kept by you and not freed by `mii_db_free`); `MII_E_FORMAT` unless the Wii could read it. For PC tools. On a Wii, `mii_db_save` would write it over the Wii's file only if that file still has the CRC this one had. |
| `void mii_db_free(mii_db *db)` | Frees what `mii_db_load` took. |
| `int mii_db_get(db, slot, mii *m)` | A copy of the slot's Mii; `MII_E_EMPTY` for none. |
| `int mii_db_count(db)` | How many slots are used. |
| `int mii_db_free_slot(db)` | The first free slot, or `MII_E_FULL`. |
| `int mii_db_find(db, id)` | The slot holding that creation ID, or `MII_E_EMPTY`. |
| `int mii_db_put(db, slot, m)` | Puts a copy of `m` in the slot, replacing what is there. Must pass `mii_check` and not have a temporary ID (`MII_E_INVALID`), and must not be in another slot (`MII_E_EXISTS`). As RFL does: a hidden-database entry with the same creation ID is removed (one Mii, one place), and a Mii made on another Wii (`db->home` known and not matching) is stored local only. |
| `int mii_db_add(db, m)` | The same in the first free slot; returns the slot, or `MII_E_FULL`. |
| `int mii_db_remove(db, slot)` | Empties the slot. |
| `int mii_db_isolated(db)` | 1 if the Mii Channel's mingling is off for the whole Wii ("isolation"). RFL then stores Miis put on a Remote as local only; do the same. |
| `int mii_hidden_count(db)` | How many hidden Miis. |
| `int mii_hidden_first(db)`, `int mii_hidden_next(db, index)` | Walk the hidden database oldest first: `for (i = mii_hidden_first(db); i >= 0; i = mii_hidden_next(db, i))`. Both return -1 at the end. An index stays the Mii's until it is removed. |
| `int mii_hidden_get(db, index, m)` | A hidden Mii. Hidden Miis keep no birthday, favourite or creator's name. |
| `int mii_hidden_find(db, id)` | The index holding that creation ID. |
| `int mii_hidden_put(db, m)` | Adds or updates a copy, as RFL does: in its own place if there, else in a free one, else in place of the oldest. Returns the index. `MII_E_INVALID` if it fails `mii_check`, has a temporary ID or is local only; `MII_E_EXISTS` if it is in the 100 slots. Isolation is not checked. |
| `int mii_hidden_remove(db, index)` | Removes a hidden Mii. |

### A Wii Remote's Miis

Ten slots (`MII_REMOTE_SLOTS`) in the Remote's own memory (EEPROM): the Mii
Channel's "Wii Remote" page. A slot is empty when its creation ID is 0. The
Remote holds two copies, from `MII_REMOTE_ADDR` (`0x0FCA`), each
`MII_REMOTE_BLOCK` (`0x2F0`) bytes: "RNCD", the slots, a CRC.

| Name | What it does |
| --- | --- |
| `int mii_remote_read(int chan, mii_remote *r)` | Reads channel 0 to 3 (about 1 s). Call `WPAD_Init()` first and have the Remote connected (`WPAD_Probe(chan, NULL) == WPAD_ERR_NONE`; a few seconds after `WPAD_Init`, or a button press). |
| `int mii_remote_read_raw(int chan, uint8_t mem[2 * MII_REMOTE_BLOCK])` | Both copies as the Remote holds them, to keep as a backup; `mii_remote_parse` reads them. |
| `int mii_remote_write(int chan, const mii_remote *r)` | Writes `r` to both copies, each read back (about 8 s). It reads the Remote first and writes first the copy that is not whole, so the other survives an interruption. |
| `int mii_remote_parse(const uint8_t *mem, size_t size, mii_remote *r)` | Parses both copies (2 x `MII_REMOTE_BLOCK` bytes). Like RFL it drops slots marked deleted and Miis the Wii would refuse, so writing the set back removes those. `MII_E_FORMAT` if a copy is there but neither copy's CRC is right: do not write over that Remote. |
| `void mii_remote_build(const mii_remote *r, uint8_t block[MII_REMOTE_BLOCK])` | One copy, for writing. |
| `int mii_remote_get(r, slot, m)` | A copy of the slot's Mii; `MII_E_EMPTY` for none. |
| `int mii_remote_free_slot(r)` | The first empty slot, or `MII_E_FULL`. |
| `int mii_remote_put(r, slot, m)` | A copy of the Mii into the slot you name, used or not: find an empty one with `mii_remote_free_slot`. Stored not favourite (RFL clears it), and as one of the Wii's own (its secret bit cleared). `MII_E_INVALID` unless it passes `mii_check`. When `mii_db_isolated`, set `local_only` first. |
| `int mii_remote_remove(r, slot)` | Empties the slot. |

### Drawing

Declared in `miidraw.h`. Described under [Drawing Miis](#drawing-miis-miidraw).

| Name | What it does |
| --- | --- |
| `mii_res_load(&res, path)`, `mii_res_attach`, `mii_res_free` | The face models (`RFL_Res.dat`; path `NULL`: `sd:/mii/RFL_Res.dat`). |
| `mii_model_make(&res, &m, mask_size, &model)` | Builds a Mii with every expression. |
| `mii_model_build(&res, &m, &opts, &model)` | The same with `mii_model_opts` (face size, expressions, cache, levels, GPU faces); `opts` NULL: 128 px, every expression. |
| `mii_model_set_mask_size`, `mii_model_make_faces`, `mii_model_free`, `mii_mesh_free` | Resize a model's faces, draw its pending GPU faces, free it. |
| `mii_cache_new`, `mii_cache_free`, `mii_cache_stats` | One cache shared by a crowd. |
| `mii_body_load(&body, path)`, `mii_body_attach`, `mii_body_free`, `mii_model_set_body` | The Wii's own body (`maleBody.brres`, `femaleBody.brres`) under a model. |
| `mii_body_anim(&body, name)`, `mii_anim_frame`, `mii_anim_expression`, `mii_fade_weight` | Animations by name, their frame and face, cross-fade weight. |
| `mii_body_make_lods`, `mii_body_lod_for`, `mii_pixels_tall` | Coarser levels for Miis far off, and the choice of level. |
| `mii_view`, `mii_draw` | A camera that frames the Mii; draw it into your scene. |
| `mii_capture`, `mii_icon`, `mii_icon_fx` | Draw into a texture with alpha; a head icon (with effects). |
| `mii_body_pose`, `mii_body_world`, `mii_body_matrices`, `mii_body_matrices_posed`, `mii_body_skin`, `mii_body_shadow`, `mii_model_bounds`, `mii_strips`, `mii_tex_rgb5a3`, `mii_tex_rgb5a3_size` | The posed bones and matrices, the shadow's placement, bounds, triangle strips and texture packing, for tests and your own drawing. |

### Effects

Declared in `miidraw.h`, built from `miifx.c`.

| Name | What it does |
| --- | --- |
| `mii_fx_load(&fx, breff_path, breft_path)`, `mii_fx_attach`, `mii_fx_free` | Read the Mii Channel's effect files (`NULL`: `sd:/mii/effect_nigaoe.*`). |
| `mii_fx_find(&fx, name)` | An emitter by name (`"Nc_zzz01"`), or -1. |
| `mii_fx_init(&fxs, &fx, seed)` | Starts a `mii_fx_state`. |
| `mii_fx_play(&fxs, emitter, at)`, `mii_fx_move`, `mii_fx_stop`, `mii_fx_playing` | Start an emitter at a world matrix, follow, stop it, ask if it still runs. |
| `mii_fx_place(&model, &pose, head, at)`, `mii_fx_place_in(..., place, at)` | A place in the Mii's own space (head or ground), or in the world for a Mii drawn with a `place`. |
| `mii_fx_step(&fxs, frames)`, `mii_fx_draw(&fxs, view)` | Move on (60 frames a second) and draw. |
| `mii_fx_particles(&fxs, out, max)` | Lists each particle (PC tests). |

### Errors

| Code | What to do |
| --- | --- |
| `MII_E_NO_FILE` | No database: the Mii Channel has never been opened. The library does not create one. For drawing: a file the user has not saved yet; ask them to run Save Mii faces. |
| `MII_E_FORMAT` | The database or a Remote's set is not as the Wii makes it. Leave it alone: nothing was written. |
| `MII_E_CHANGED` | Someone else saved the database since you loaded it. Load it again and make the change again. |
| `MII_E_IO`, `MII_E_VERIFY` | A save did not finish. Call `mii_db_save` again. |
| `MII_E_INVALID` | The Mii fails the Wii's check: `mii_check(&m, &why)` says why. |
| `MII_E_EXISTS` | That creation ID is in another slot. |
| `MII_E_FULL` | No free slot. |
| `MII_E_EMPTY` | No Mii there. |
| `MII_E_CRC` | A 76-byte Mii whose CRC does not match. |
| `MII_E_ARG` | A bad argument: a slot out of range, a NULL, a bad size. |
| `MII_E_MEM` | Out of memory. |
| `MII_E_DENIED` | IOS refused the file. |
| `MII_E_NO_REMOTE`, `MII_E_TIMEOUT` | Wake the Remote and try again. libogc drops a Remote left idle for 5 minutes. |
| `MII_E_UNSUPPORTED` | The Remote calls need devkitPro's libogc 3.x. |

Limits that the structs carry: `MII_SIZE` 74, `MII_STORE_SIZE` 76,
`MII_NAME_MAX` 10 UTF-16 units (also the creator's name), `MII_NAME_UTF8_MAX`
31, `MII_ID_SIZE` 8, `MII_DB_SLOTS` 100, `MII_HIDDEN_SLOTS` 10000,
`MII_DB_FILE_SIZE` `0xBE6C0` (779,968 bytes, as the Wii Menu makes it),
`MII_REMOTE_SLOTS` 10. Favourite colours run `MII_RED` to `MII_BLACK` (0 to
11). The ranges of every field are in the comments of the `mii` struct in
[mii.h](mii.h); `brow_y` (3 to 18) is the only part with a lower limit above
0.

## More examples

### Change a Mii

```c
mii_db db = { 0 };
mii m;

if (mii_db_load(&db) == MII_OK && mii_db_get(&db, 3, &m) == MII_OK) {
    m.height = 128;
    m.local_only = 1;        // Mingle off
    if (mii_db_put(&db, 3, &m) == MII_OK)
        mii_db_save(&db);
}
mii_db_free(&db);
```

`mii_db_put` stores a copy. It makes a Mii made on another Wii local only,
as RFL does, so read the slot back to see what was stored.

### A .mii file

```c
uint8_t buf[MII_STORE_SIZE + 1];
FILE *f = fopen("sd:/mii/friend.mii", "rb");   // after fatInitDefault()
size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;
mii m;

if (f)
    fclose(f);
if (mii_import(buf, n, &m) == MII_OK && mii_check(&m, NULL) == MII_OK)
    ;                        // add it as above (keep its creation ID)

uint8_t raw[MII_SIZE], with_crc[MII_STORE_SIZE];
mii_pack(&m, raw);           // the 74-byte .mii
mii_store(raw, with_crc);    // the 76-byte form
```

### A Mii on Wii Remote 1

```c
WPAD_Init();                 // once, early: a Remote connects a few seconds later
u64 end = gettime() + secs_to_ticks(10);
while (WPAD_Probe(0, NULL) != WPAD_ERR_NONE && gettime() < end)
    usleep(100 * 1000);

mii_remote rm;
if (mii_remote_read(0, &rm) == MII_OK) {   // about 1 s; MII_E_FORMAT: leave it alone
    int slot = mii_remote_free_slot(&rm);  // never write over the user's Miis
    if (slot >= 0 && mii_remote_put(&rm, slot, &m) == MII_OK)
        mii_remote_write(0, &rm);          // about 8 s: both copies, each read back
}
```

If `mii_db_isolated()` says the Wii has mingling off, set `m.local_only = 1`
before putting it on a Remote, as RFL does.

### A Mii on a Message Board letter

```c
#include "mboard.h"                        // sdk/mboard

uint8_t face[MII_SIZE];
mii_pack(&m, face);                        // m passed mii_check, or the icon is dropped
mboard_msg msg = { 0 };
msg.title = "My App";
msg.body = "Hello from my Mii.";
msg.face = face;
mboard_post_msg(&msg);
```

See [sdk/mboard](../mboard) for the letter side.

## Drawing Miis (miidraw)

[miidraw.h](miidraw.h) and [miidraw.c](miidraw.c) draw a Mii with GX: its
head as the Wii draws it, from the console's own face models, and a body to
go with it. HBC's avatar ([channel/channelapp/source/avatar.c](../../channel/channelapp/source/avatar.c))
is the worked example.

```c
#include "mii.h"
#include "miidraw.h"

static mii_res res;
static mii_model model;
static mii_body body;
mii m;
int ok;

/* Once, outside the frame loop (about 0.1 s; any thread). */
ok = mii_res_load(&res, NULL) == MII_OK;           /* sd:/mii/RFL_Res.dat */
ok = ok && mii_get(0, &m) == MII_OK;               /* or any Mii you have */
ok = ok && mii_model_make(&res, &m, 128, &model) == MII_OK;
mii_res_free(&res);                                /* the model keeps what it needs */
if (ok && mii_body_load(&body, m.sex == MII_FEMALE ? MII_BODY_FEMALE_PATH
                                                   : MII_BODY_MALE_PATH) == MII_OK)
    mii_model_set_body(&model, &body);             /* the Wii's own; else the library's */

/* Each frame, in your 3D pass: a viewport where the Mii stands, the camera
 * that frames it there, then draw. Set your GX state again after. */
if (ok) {
    Mtx44 proj;
    Mtx view;
    mii_pose pose = { 0 };

    pose.anim = 1 + mii_body_anim(&body, "bodyWait");   /* 0: stand still */
    pose.frame = seconds * 60;
    pose.expression = MII_EXPR_SMILE;
    GX_SetViewport(x, y, 128, 208, 0, 1);
    mii_view(&model, &pose, 128.0f / 208, 0.04f, proj, view);
    GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
    mii_draw(&model, &pose, view, NULL);
}
```

### The user's files

- **The face models are the user's.** `RFL_Res.dat` is Nintendo's: never
  ship it. HBC's HOME > HBC > Save Mii faces (or `hbc.py mii-faces`) copies
  the Wii's own to `sd:/mii/RFL_Res.dat`, where `mii_res_load(&res, NULL)`
  looks. An app cannot read the Wii Menu's copy itself. `MII_E_NO_FILE`
  means: ask the user to save it.
- **The bodies and effects are the user's too.** Save Mii faces also copies
  `maleBody.brres` and `femaleBody.brres` from the Mii Channel to `sd:/mii`.
  The effect files (`effect_nigaoe.breff`, `effect_nigaoe.breft`) are
  Nintendo's as well: the user copies their own to `sd:/mii`. Nothing ships
  them.

### Building a model

- **`mii_model_make`** builds everything once: the head's shapes placed as
  RFL places them, the face composited for all seven expressions
  (`MII_EXPR_*`), the parts RFL colours with TEV tricks baked into plain
  textures, a body in the Mii's colours (shirt in the favourite colour,
  trousers grey or gold for a special Mii) scaled by height and build, and
  each part as a GX display list. As RFL keeps them: textures RGB5A3,
  shapes as s16 vertices drawn indexed. About 280 KB with 128 px faces;
  most of it is the seven faces.
- **`mii_model_build`** takes `mii_model_opts`: the face size (32 to 256
  px) and which expressions to build (`MII_EXPR_BIT`; the normal face is
  always built, and one not built draws as it). With 128 px faces and the
  normal one only, a Mii takes about 67 KB; with 32 px, about 37 KB.

### Ways to draw it

- **In place, each frame:** `mii_view()` gives the projection and camera
  that frame the Mii in a viewport of the aspect you pass; set your
  viewport there and `mii_draw()`. Give the Mii a wide depth range: its
  face's layers break up on a real Wii in a thin one (Dolphin hides it).
  HBC's avatar does this: the whole range in front of its slot, then its
  rectangle's depth put back.
- **`mii_capture()`** into a texture with alpha, framed whole (or the head:
  `pose.head_only`). It draws twice the size and box-filters down, so the
  edges are smooth. For icons and stills.
- **`mii_icon()`**: a head icon framed as the Wii Menu's.
- **`mii_draw()`** into your own 3D scene, with your projection and camera.

**`mii_pose`**: turn, head yaw/pitch/roll, arms out and forward, legs, a bob,
breathing, the expression. All zero stands still.

### The Wii's own bodies

With `sd:/mii/maleBody.brres` and `femaleBody.brres`, `mii_body_load` reads
one and `mii_model_set_body` puts it under a model: Nintendo's skeleton, the
shirt and trousers skinned on the GPU from the Mii Channel's own display
lists (a matrix per bone a frame), and its animations. Pose it with
`pose.anim = 1 + mii_body_anim(&body, "bodyWait")` (or `bodyGreeting`,
`bodyWalk`, `bodyRun`, `bodyTalk`, ...) and `pose.frame` at 60 a second; 0
stands still.

- **Cross-fade.** When you change animation, keep the one before in
  `pose.from_anim` / `from_frame` with
  `pose.from_weight = mii_fade_weight(seconds_since, 0.4f)`. A looping
  animation plays on as it fades; a finished one-shot (a wave) holds its
  last frame, since frames wrap and it would otherwise fade toward its own
  start.
- **Loop or once.** Each `mii_anim` says which it is (`loop`, from the file:
  `bodyWait`, `bodyWalk`, `bodyRun`, `bodyTalk`, `bodyTalk_01` and
  `bodyCatnap_00` go round; the rest play once). `mii_anim_frame(&body,
  anim, seconds, &done)` gives the frame for either: wrapped for a looping
  one, held at the end for a one-shot, with `done` set once it has ended.
- **Height and build** scale each bone as Nintendo's later libraries do.
- **Without the files** the model keeps a body of the library's own.

```c
static mii_body body;
if (mii_body_load(&body, m.sex == MII_FEMALE ? MII_BODY_FEMALE_PATH
                                             : MII_BODY_MALE_PATH) == MII_OK)
    mii_model_set_body(&model, &body);           /* body outlives model */
pose.anim = 1 + mii_body_anim(&body, "bodyWait");
pose.frame = seconds * 60;
```

### The Mii Channel's look (1.4.0)

Read from the body's own materials:

- **Shading.** The shirt and trousers use no lamp. Each one's colour is
  lifted by the square of `env0`, a 32 × 32 light texture, looked up by
  where each normal points as the camera sees it. This gives the soft
  highlight on the chest. A body file without `env0` is lit as before.
- **Shadow.** `pose.shadow = 1` draws the body's own shadow under the
  feet: a 75-unit square with a radial texture, in the material's colour,
  on the floor at any height and build (1.5.1; before, it rose and sank
  with the leg length, up a tall Mii's legs, under a short one's feet).
  The animation sizes it: it shrinks to nothing as the Mii is lifted
  (`bodyGetDown`) and stretches as the Mii falls flat (`bodySlip_00`). For
  a Mii carried above the ground, set `pose.shadow_drop` to the distance
  down to the ground; the shadow stays there and is smaller the higher the
  Mii is. `mii_body_shadow` gives its placement, for drawing it yourself.
- **Faces.** `mii_anim_expression(&body, pose.anim, pose.frame, face)`
  gives a face for the animation: eyes shut asleep (`bodyCatnap_00`), a
  smile for `bodyGreeting`, surprise when flung or slipping, the mouth
  going for `bodyTalk`. Where the animation calls for none, it returns
  your `face`. The file does not say which face goes with which animation,
  so these choices are the library's own (see
  [For someone to add](#for-someone-to-add)).

### Effects (`miifx.c`)

The Mii Channel's particle effects: a sneeze's splash, speech balloons, dust
from running, slipping and landing, the Zzz of a nap, the sparkles of a
favourite and of a changed face part. They are in its
`Character.szs/effect` as `effect_nigaoe.breff` (14 emitters) and
`effect_nigaoe.breft` (6 textures). These are Nintendo's, like the bodies:
the user copies their own to `sd:/mii`, and nothing ships them.

- `mii_fx_load` reads them. A `mii_fx_state` (about 25 KB, no allocation
  after that) plays up to 16 emitters and 128 particles; when it is full,
  the oldest goes, and `fxs.replaced` counts the emitters that went that
  way, by `mii_fx_play` or by the children particles start in
  `mii_fx_step` (1.5.0).
- `mii_fx_play` starts an emitter, placed by a matrix in the world that you
  give. `mii_fx_place(&model, &pose, head, at)` makes it in the Mii's own
  space: on the head bone as posed (`head` 1: the sneeze in front of the face
  and where it points, talk balloons before the mouth, zzz over the head; the
  same place the animations give as `pSphere14`), or on the ground under the
  Mii's lowest bone (0: dust where the feet, or the back, hit), turned with
  the Mii and sized by its height. For a `head_only` pose the chin is the
  origin.
- That is the world only for a Mii drawn without a `place`: for one drawn
  with `mii_draw(&model, &pose, view, place)`, use
  `mii_fx_place_in(&model, &pose, head, place, at)` (1.5.0), or its effects
  play at the world's origin.
- Placed after the Mii is drawn, with `pose.reuse = 1`, they take the bones
  it was drawn with and pose nothing again (`mii_body_pose` too).
- `mii_icon_fx` draws an icon with the effects over it: play them on a
  `head_only` pose's place, scaled to about half, to keep them inside.
- `mii_fx_step` moves the effects on, at 60 frames a second. `mii_fx_draw`
  draws them as squares that face the camera, coloured and blended as the
  file says, and leaves Z writes on (1.5.0: an emitter's Z mode with writes
  off, left to the frame's copy, kept that copy from clearing Z, and the next
  frame drew nothing where the Miis had stood: a crowd of them in the clear
  colour). Draw them after the opaque parts of the scene.
- `Nc_zzz01` goes on until `mii_fx_stop`.
- On the PC, `mii_fx_particles` lists each particle (`tests/host/test_miifx.py`).

```c
static mii_fx fx;
static mii_fx_state fxs;
if (mii_fx_load(&fx, NULL, NULL) == MII_OK)      /* sd:/mii/effect_nigaoe.* */
    mii_fx_init(&fxs, &fx, 1);
float head[3][4];
mii_fx_place_in(&model, &pose, 1, place, head);  /* place: as mii_draw's, or NULL */
int zzz = mii_fx_play(&fxs, mii_fx_find(&fx, "Nc_zzz01"), head);
/* each frame, after mii_draw(&model, &pose, view, place): */
pose.reuse = 1;                                  /* the bones just drawn */
mii_fx_place_in(&model, &pose, 1, place, head);
mii_fx_move(&fxs, zzz, head);                    /* follow the head */
mii_fx_step(&fxs, 1);
mii_fx_draw(&fxs, view);                         /* after the Miis */
```

### Many Miis

A crowd is what the Wii's CPU and memory allow, not its GPU, until the CPU's
part is small. What helps, all measured on the bench Wii
([docs/history/1.11.md](../../docs/history/1.11.md), 1.11.6 to 1.11.10):

- One `mii_cache` for the crowd (`mii_model_opts.cache`): Miis share each
  part that comes out the same, byte for byte (the same hair, face shape,
  textures). With 32 px faces and the normal one only, a random Mii then
  takes about 12 to 20 KB; the bench Wii held 5,855.
- `mii_body_make_lods(&body, NULL, 0)` once per body,
  `mii_model_opts.lods = MII_HEAD_LODS` for each Mii, then each frame
  `pose.lod = mii_body_lod_for(&body, pixels_tall)`: four coarser levels of
  body and head for Miis under 90, 39, 30.5 and 25 pixels tall (chosen by
  eye on a real Wii), each changing the Mii by under half a pixel there,
  made on the Wii from the user's own files. A head of about 490 triangles
  goes to about 380, 340, 270 and 180 (the hair folds at half the
  tolerance: its thin pieces cracked), the body's 750 to about 490, 410, 320
  and 240, all drawn as strips. The GPU's time in a crowd is vertices.
  `tests/apps/mii_app parade` walks Miis through the levels over lines on the
  grass where each begins.
- Drawing is the hardware's work: the bones on the paired-single unit, each
  head level a display list the GPU fetches itself, the matrix palette's
  cache lines claimed with `dcbz`. A Mii far off costs the CPU about 16 us a
  frame. Build with `-DMII_PROFILE` to see where (`mii_profile`).
- `mii_model_opts.gpu_faces = 1`: faces drawn by the GPU, as RFL draws them
  (`RFLiMakeTexture`): about 0.08 ms each against 1 to 6 ms on the CPU,
  within 0.1 of 255 of the CPU's on average. They use the EFB, so call
  `mii_model_make_faces` between frames on the thread that draws.
- Faces by distance: `mii_model_set_mask_size` (32 px far, 128 near); with
  GPU faces it marks them, and `mii_model_make_faces` draws them between
  frames.
- Walk in step: Miis at the same frame of the same animation pose once (the
  body remembers its last 16 poses), so give a crowd a few phases, not one
  each.
- `pose.reuse` for a Mii far off: draw it with the bones its last draw
  posed, posing every second or fourth frame. Place its effects after the
  draw with `reuse` set too: they take those bones, not posed again.

```c
/* Once: a cache for the crowd, the bodies' levels. */
mii_cache *cache = mii_cache_new();
mii_body_make_lods(&body, NULL, 0);
mii_model_opts o = { 32, MII_EXPR_BIT(MII_EXPR_NORMAL), cache, MII_HEAD_LODS, 1 };
for (i = 0; i < n; ++i)
    if (mii_model_build(&res, &miis[i], &o, &crowd[i]) == MII_OK)
        mii_model_set_body(&crowd[i], &body);

/* Each frame, before drawing: faces still to draw (the GPU, between frames). */
for (i = 0; i < n; ++i)
    if (crowd[i].faces_pending)
        mii_model_make_faces(&crowd[i], &res, clear_colour);
/* ... set your GX state, projection and camera, then for each Mii: */
float px = mii_pixels_tall(&crowd[i], view, place, proj, rmode->efbHeight);
pose.lod = mii_body_lod_for(&body, px);
pose.reuse = pose.lod >= 3 && (frame + i) % 2;   /* far: pose every other frame */
mii_draw(&crowd[i], &pose, view, place);

/* At the end: the models, then the cache and the body. */
```

### GX rules

- **Between frames.** `mii_capture` and `mii_icon` draw in the EFB's corner
  (RGBA6 while they draw), copy it out and clear it: call them after your
  last screen copy and before you draw the next frame, then set your GX
  state again (pixel format, viewport, scissor, projection, copy filter,
  vertex format, colour channel). `mii_draw` changes the vertex format,
  lighting, TEV, Z, culling and blending.
- **Z writes on at your copy.** A copy that clears (`GX_CopyDisp(xfb,
  GX_TRUE)`) clears Z only while Z writes are on (`GX_SetZMode`), on the
  Wii and in Dolphin. `mii_draw`, `mii_fx_draw` and `mii_model_make_faces`
  leave them on; if your own drawing ends with them off, turn them on
  before the copy, or the next frame is drawn against this one's Z.
- **Testing in Dolphin.** An EFB copy stays on Dolphin's GPU unless
  `Graphics.Hacks.EFBToTextureEnable=False`, so a CPU read of `mii_capture`'s
  texture sees zeros there; drawing it works either way.
- **On the PC.** With `-DMII_NO_OGC` everything but drawing builds
  (`tests/host/miidraw_host` writes a Mii's meshes as OBJ and its faces as RGBA;
  with `MII_BODY`, a Wii body posed by `MII_ANIM` and `MII_FRAME`;
  `tests/host/miifx_host` plays an effect and prints its particles).
- **Files.** `miidraw.c` (faces, the built body, drawing), `miibody.c` (the
  Wii's bodies) and `miifx.c` (the effects) with `mii.c`.

The head's numbers come from RFL as the CC0 decompilations show it; the
code is this library's own. [docs/mii/rendering.md](../../docs/mii/rendering.md)
has the sources.

## Versions

| Part | Version | History |
| --- | --- | --- |
| `mii.h`, `mii.c` (`MII_VERSION`) | 1.1.1 | First shipped with HBC 1.10.11 ([CHANGELOG.md](../../CHANGELOG.md)) and proven on a real Wii Remote in 1.10.12; `MII_VERSION` began at 1.1.0 with the 1.10.13 audit; 1.1.1 (HBC 1.11.0): after a failed MAC lookup (`MII_E_IO`) the next call asks again at once instead of keeping the failure. |
| `miidraw.h` and the drawing files (`MIIDRAW_VERSION`) | 1.5.1 | See below. |

The two parts have their own version numbers. "sdk/mii 2.0" in the
changelog (HBC 1.11.2 and 1.11.3) is the name of the plan for the next
drawing API, [docs/plans/mii-sdk-2.md](../../docs/plans/mii-sdk-2.md), not a release: its
first steps shipped as drawing versions 1.1.0 to 1.5.1, and the 2.0 API itself
is not started.

The drawing code first shipped with HBC 1.10.21. Its history, from
`miidraw.h`:

- **1.1.0** (HBC 1.11.0): the Wii's body, `mii_view`.
- **1.1.1**: its depth.
- **1.2.0** (HBC 1.12.0): crowds.
- **1.2.1**: levels by eye, hair whole.
- **1.3.0**: animations loop or play once (`mii_anim_frame`), 4-byte rotation
  keys (`bodyWallow`).
- **1.4.0**: the Mii Channel's look (env shading, the shadow, faces for the
  animations).
- **1.4.1**: the C matrix product, not libogc's paired-single one.
- **1.5.0**: Z writes left on (the effects, GPU faces: a crowd in the clear
  colour), `mii_fx_place_in`, `mii_fx_state.replaced`, `pose.reuse` for
  `mii_body_pose` and `mii_fx_place`.
- **1.5.1**: the shadow on the floor at every height and build, not lifted
  with the feet.

## For someone to add

The SDK uses everything in the Mii Channel's body and effect files (1.5.0).
These are the parts it does not have yet. Each one needs knowledge from
outside those files.

- **Height and build as the Wii does them.** Bones are scaled with the
  formula of Nintendo's later Mii library, the Wii U's (FFL), because the
  Wii's own formula is not in the data. It lives in the Mii Channel's
  program, or in RFL as games use it. To check it: pose a tall, thin Mii
  and a short, wide one in the Mii Channel in Dolphin, and compare with
  `mii_body_pose`.
- **Wii U and 3DS Miis.** `mii_import` takes only the Wii's 74- and 76-byte
  forms. The Wii U and 3DS store a Mii in 96 bytes (FFLStoreData, Ver3),
  with more hair styles, eye and lip colours, and a different creation ID.
  Converting one to a Wii Mii means choosing the nearest Wii part for each
  new one. Nintendo's own Mii transfer between Wii and Wii U is a reference
  for those choices.
- **Wii U drawing.** FFL lights each part with its own material: a rim
  light, a specular light on the hair, and textures in more formats. The
  Wii has none of these, so drawing a Wii U Mii as the Wii U does would
  need FFL's shader values (FFLShaderData) and a TEV version of them.
- **The Mii Channel's faces and effects timing.** No file says which face
  or effect goes with which frame of an animation. `mii_anim_expression`
  and HBC's effect cues are this library's choices, by eye (HBC finds the
  landings from the bones: dust where the lowest one comes down). The real ones
  are in the Mii Channel's program, as is the matrix each effect is placed
  by. Talk and the sneeze fit the head, run smoke the body's root, and zzz
  neither exactly.
- **pSphere14.** Nineteen animations move a node that the body does not
  have. It is the head bone's place in model space, baked in, and the
  effects on the head are placed by it: `mii_fx_place` uses the head bone,
  which gives the same place without reading it.

## Docs and tests

- [docs/mii/README.md](../../docs/mii/README.md): the Mii data, the database, the hidden
  database, a Remote's Miis, the creation ID and the CRCs, each with its
  source, and the proof on a Wii.
- [docs/plans/mii-sdk-2.md](../../docs/plans/mii-sdk-2.md): the plan for sdk/mii 2.0.
- [docs/mii/rendering.md](../../docs/mii/rendering.md):
  the sources for the drawing.
- [sdk/mboard](../mboard): Message Board letters with a Mii as the icon.
- [example/](example): a whole program (`make -C sdk/mii/example`).

Tests: `tests/host/test_mii.py` on the PC, `tests/dolphin/dolphin_mii.py` in Dolphin, and
`tests/wii/wii_mii.py` on the bench Wii. For drawing: `tests/host/test_miidraw.py`,
`tests/host/test_miifx.py`, `tests/host/miidraw_host`, `tests/host/miifx_host` and
`tests/apps/mii_app`.

## What it changes on the Wii

| Part | What changes | How long it lasts |
| --- | --- | --- |
| Memory | `mii_db_load` reads the database (about 780 KB) from the heap; a model takes about 37 to 280 KB (see [Building a model](#building-a-model)); a `mii_fx_state` about 25 KB. | Until you free it or the app ends. |
| `RFL_DB.dat` | `mii_db_save` writes the file `/shared2/menu/FaceLib/RFL_DB.dat`, in one open, hidden Mii data before the table that points to it, and reads it back. Only when you call it. | Permanent, in the Wii's NAND. |
| A Wii Remote | `mii_remote_write` writes both copies of the Remote's Miis, each read back. | Permanent, in the Remote. |
| The screen | Drawing changes GX state; see [GX rules](#gx-rules). | Until you set yours again. |

Reading changes nothing. The library installs nothing and writes no files on
the SD card.

## Limits

- **Time.** The Wii calls block:
  - loading the database takes about 0.7 s, and saving about 0.2 s;
  - the first call that needs the Wi-Fi MAC waits up to 8 s just after an app
    starts;
  - reading a Remote takes about 1 s, and writing one about 8 s.

  Call them outside your frame loop, from one thread.
- **What it refuses to do.**
  - **Write a database it cannot trust.** If the file's CRC, size or hidden
    database links are wrong, nothing is written (`MII_E_FORMAT`). The Wii
    Menu deletes and recreates a database it cannot read, so a repair would
    only hide that the Miis were already lost.
  - **Store a Mii the Wii would refuse.** Every put checks the Mii first.
  - **Save over someone else's write** (`MII_E_CHANGED`).
  - **Treat a damaged Remote as an empty one.** A Remote whose two copies are
    both broken reads as `MII_E_FORMAT`.
  - **Pick a Remote slot for you.** `mii_remote_put` writes the slot you name,
    so use `mii_remote_free_slot`.
- **Interrupted writes.** `mii_remote_write` writes first the copy that is
  not whole, then the other, each read back, so a whole set survives an
  interruption.
- **Save from your own program,** not while the Mii Channel or a game using
  RFL runs. They keep their own copy and would write it back over yours.
- **The database must exist.** The library does not create `RFL_DB.dat`
  (`MII_E_NO_FILE`).
- **Wii Miis only.** `mii_import` does not take Wii U or 3DS Miis.
- **Drawing needs the user's files** (`RFL_Res.dat`, and for the Wii's bodies
  and effects the other Mii Channel files). Without the bodies the model keeps
  a body of the library's own.
- **Remote calls** need devkitPro's libogc 3.x (`MII_E_UNSUPPORTED`
  otherwise).

## Licence

Public domain: copy, change and ship it in anything, open or closed, with no
credit needed. The face models, bodies and effects the drawing code reads are
Nintendo's and are not part of it: never ship them.
