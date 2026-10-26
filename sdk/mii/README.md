# mii: Miis from homebrew

`mii` gives homebrew what the Wii's own Mii library (RFL) gives games:

- **A Mii as a struct.** It reads and writes the 74 bytes every Wii file
  holds, and checks a Mii as the Wii does, saying why one fails.
- **New Miis.** It makes a plain Mii to edit or a random one, and gives a Mii
  a new creation ID as the Mii Channel does.
- **`.mii` files.** It imports them, and exports the 74-byte form and the
  76-byte form with a CRC.
- **The Mii database** (`RFL_DB.dat`). It reads, adds, changes and removes
  the Mii Channel's 100 Miis, and the hidden database of Miis met on letters
  and Remotes.
- **A Wii Remote's Miis.** It reads and writes the 10 Miis a Remote carries.

It is two files, [mii.h](mii.h) and [mii.c](mii.c), in the public domain:
copy them into your project. [example/](example) is a whole program that
uses them (CI builds it).

## Building

- Compile `mii.c` as C.
- Link with `-lwiiuse -lbte -logc -lm`, in that order, as
  [example/Makefile](example/Makefile) does. The library uses wiiuse for the
  Remote and libogc's network code for the Wi-Fi MAC, even if your app uses
  neither.
- With `-DMII_NO_OGC`, the parts that only touch bytes build on a PC, for
  tools and tests.

## Use

Each call that can fail returns 0 or more, or a negative `MII_E_*` code
(`mii_strerror()` names it). The snippets below need:

```c
#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>   // gettime()
#include <wiiuse/wpad.h>
#include "mii.h"
```

### Read every Mii

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

### Make a Mii and add it to the plaza

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
static u8 tex[160 * 256 * 4] ATTRIBUTE_ALIGN(32);
mii m;

/* Once, outside the frame loop (about 0.1 s; any thread). */
if (mii_res_load(&res, NULL) == MII_OK &&          /* sd:/mii/RFL_Res.dat */
        mii_get(0, &m) == MII_OK &&                /* or any Mii you have */
        mii_model_make(&res, &m, 128, &model) == MII_OK)
    mii_res_free(&res);                            /* the model keeps what it needs */

/* Each frame, before you draw anything: the Mii, posed, into a texture with
 * alpha. Then set your GX state again and draw tex like any RGBA8 texture. */
mii_pose pose = { 0 };
pose.arm_raise[0] = 2.3f;                          /* the right arm up: a wave */
pose.expression = MII_EXPR_SMILE;
mii_capture(&model, &pose, 160, 256, 0.04f, 1, (GXColor) { 0, 0, 0, 255 }, tex);
```

- **The face models are the user's.** `RFL_Res.dat` is Nintendo's: never
  ship it. HBC's HOME > HBC > Save Mii faces (or `hbc.py mii-faces`) copies
  the Wii's own to `sd:/mii/RFL_Res.dat`, where `mii_res_load(&res, NULL)`
  looks. An app cannot read the Wii Menu's copy itself. `MII_E_NO_FILE`
  means: ask the user to save it.
- **`mii_model_make`** builds everything once: the head's shapes placed as
  RFL places them, the face composited for all seven expressions
  (`MII_EXPR_*`), the parts RFL colours with TEV tricks baked into plain
  textures, a body in the Mii's colours (shirt in the favourite colour,
  trousers grey or gold for a special Mii) scaled by height and build, and
  each part as a GX display list. About 1 MiB.
- **Ways to draw it:**
  - In place, each frame: `mii_view()` gives the projection and camera
    that frame the Mii in a viewport of the aspect you pass; set your
    viewport (and a depth range) there and `mii_draw()`. HBC's avatar does
    this (channel/channelapp/source/avatar.c).
  - `mii_capture()` into a texture with alpha, framed whole (or the head:
    `pose.head_only`). It draws twice the size and box-filters down, so the
    edges are smooth. For icons and stills.
  - `mii_icon()`: a head icon framed as the Wii Menu's.
  - `mii_draw()` into your own 3D scene, with your projection and camera.
- **`mii_pose`**: turn, head yaw/pitch/roll, arms out and forward, legs,
  a bob, breathing, the expression. All zero stands still.
- **The Wii's own bodies.** With `sd:/mii/maleBody.brres` and
  `femaleBody.brres` (Save Mii faces copies them from the Mii Channel too),
  `mii_body_load` reads one and `mii_model_set_body` puts it under a model:
  Nintendo's skeleton, the shirt and trousers skinned on the CPU, and the
  Mii Channel's animations. Pose it with `pose.anim = 1 +
  mii_body_anim(&body, "bodyWait")` (or `bodyGreeting`, `bodyWalk`,
  `bodyRun`, `bodyTalk`, ...) and `pose.frame` at 60 a second; 0 stands
  still. Height and build scale each bone as Nintendo's later libraries do.
  Without the files the model keeps a body of the library's own.

  ```c
  static mii_body body;
  if (mii_body_load(&body, m.sex == MII_FEMALE ? MII_BODY_FEMALE_PATH
                                               : MII_BODY_MALE_PATH) == MII_OK)
      mii_model_set_body(&model, &body);           /* body outlives model */
  pose.anim = 1 + mii_body_anim(&body, "bodyWait");
  pose.frame = seconds * 60;
  ```
- **Between frames.** `mii_capture` and `mii_icon` draw in the EFB's corner
  (RGBA6 while they draw), copy it out and clear it: call them after your
  last screen copy and before you draw the next frame, then set your GX
  state again (pixel format, viewport, scissor, projection, copy filter,
  vertex format, colour channel). `mii_draw` changes the vertex format,
  lighting, TEV, Z, culling and blending.
- **Testing in Dolphin**: an EFB copy stays on Dolphin's GPU unless
  `Graphics.Hacks.EFBToTextureEnable=False`, so a CPU read of `mii_capture`'s
  texture sees zeros there; drawing it works either way.
- **On the PC**: with `-DMII_NO_OGC` everything but drawing builds
  (`tests/miidraw_host` writes a Mii's meshes as OBJ and its faces as RGBA;
  with `MII_BODY`, a Wii body posed by `MII_ANIM` and `MII_FRAME`).
- **Files**: `miidraw.c` (faces, the built body, drawing) and `miibody.c`
  (the Wii's bodies) with `mii.c`.

The head's numbers come from RFL as the CC0 decompilations show it; the
code is this library's own. [docs/research/mii-rendering.md](../../docs/research/mii-rendering.md)
has the sources.

## Errors

| Code | What to do |
| --- | --- |
| `MII_E_NO_FILE` | No database: the Mii Channel has never been opened. The library does not create one. |
| `MII_E_FORMAT` | The database or a Remote's set is not as the Wii makes it. Leave it alone: nothing was written. |
| `MII_E_CHANGED` | Someone else saved the database since you loaded it. Load it again and make the change again. |
| `MII_E_IO`, `MII_E_VERIFY` | A save did not finish. Call `mii_db_save` again. |
| `MII_E_INVALID` | The Mii fails the Wii's check: `mii_check(&m, &why)` says why. |
| `MII_E_EXISTS` | That creation ID is in another slot. |
| `MII_E_FULL` | No free slot. |
| `MII_E_NO_REMOTE`, `MII_E_TIMEOUT` | Wake the Remote and try again. libogc drops a Remote left idle for 5 minutes. |
| `MII_E_UNSUPPORTED` | The Remote calls need devkitPro's libogc 3.x. |

## Good to know

- **Time.** The Wii calls block:
  - loading the database takes about 0.7 s, and saving about 0.2 s;
  - the first call that needs the Wi-Fi MAC waits up to 8 s just after an app
    starts;
  - reading a Remote takes about 1 s, and writing one about 8 s.

  Call them outside your frame loop, from one thread.
- **What it refuses to do.**
  - **Write a database it cannot trust.** If the file's CRC, size or hidden
    database links are wrong, nothing is written. The Wii Menu deletes and
    recreates a database it cannot read, so a repair would only hide that the
    Miis were already lost.
  - **Store a Mii the Wii would refuse.** Every put checks the Mii first.
  - **Save over someone else's write.**
  - **Treat a damaged Remote as an empty one.**
  - **Pick a Remote slot for you.** `mii_remote_put` writes the slot you name,
    so use `mii_remote_free_slot`.
- **Save from your own program,** not while the Mii Channel or a game using
  RFL runs. They keep their own copy and would write it back over yours.

## Docs

- [docs/mii.md](../../docs/mii.md): the Mii data, the database, the hidden
  database, a Remote's Miis, the creation ID and the CRCs, each with its
  source, and the proof on a Wii.
- [sdk/mboard](../mboard): Message Board letters with a Mii as the icon.

Tests: `tests/test_mii.py` on the PC, `tests/dolphin_mii.py` in Dolphin, and
`tests/wii_mii.py` on the bench Wii.
