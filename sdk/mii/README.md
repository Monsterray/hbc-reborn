# mii: Miis from homebrew

`mii` gives homebrew what the Wii's own Mii library (RFL) gives games:

- **A Mii as a struct.** It reads and writes the 74 bytes every Wii file
  holds, and checks a Mii as the Wii does, saying why one fails.
- **New Miis.** It makes a plain Mii to edit or a random one, and gives a Mii
  a new creation ID as the Mii Channel does.
- **`.mii` files.** It imports and exports the 74-byte form and the 76-byte
  form with a CRC.
- **The Mii database** (`RFL_DB.dat`). It reads, adds, changes and removes
  the Mii Channel's 100 Miis, and the hidden database of Miis met on letters
  and Remotes.
- **A Wii Remote's Miis.** It reads and writes the 10 Miis a Remote carries.

It is two files, [mii.h](mii.h) and [mii.c](mii.c). They need only libogc
and are in the public domain, so you can copy them into any project. With
`-DMII_NO_OGC` everything but the NAND and Remote calls builds for a PC.

## Use

```c
#include "mii.h"

// Read: every Mii in the Mii Channel.
mii_db db;
mii m;
char name[32];

if (mii_db_load(&db) == MII_OK) {
    for (int i = 0; i < MII_DB_SLOTS; ++i)
        if (mii_db_get(&db, i, &m) == MII_OK) {
            mii_get_name(m.name, name, sizeof(name));
            printf("%d: %s\n", i, name);
        }
    mii_db_free(&db);
}

// Make one and put it in the plaza.
uint32_t seed = gettime();
mii_random(&m, MII_ANY, MII_ANY, &seed);   // or mii_default(&m, MII_FEMALE)
mii_set_name(m.name, "Homebrew");
mii_set_name(m.creator, "My App");
m.fav_color = 4;
mii_make_id(&m);                            // this Wii's clock and MAC

const char *why;
if (mii_check(&m, &why) != MII_OK)
    printf("the Wii would refuse it: %s\n", why);

if (mii_db_load(&db) == MII_OK) {
    int slot = mii_db_add(&db, &m);         // the first free slot
    if (slot >= 0)
        mii_db_save(&db);                   // checked, written, read back
    mii_db_free(&db);
}

// The Miis on Wii Remote 1 (WPAD running, the Remote connected).
mii_remote rm;
if (mii_remote_read(0, &rm) == MII_OK) {
    mii_remote_put(&rm, 0, &m);
    mii_remote_write(0, &rm);               // both copies, each read back
}

// A .mii file: 74 bytes (or 76 with a CRC).
uint8_t raw[MII_SIZE];
mii_pack(&m, raw);
mii_import(file_data, file_size, &m);
```

Each call that can fail returns 0 or more, or a negative `MII_E_*` code
(`mii_strerror()` names it).

## What it will not do

- **It will not write a database it cannot trust.** If the file has a wrong
  CRC, or is not the size the Wii Menu makes, nothing is written
  (`MII_E_FORMAT`). The Wii Menu deletes and recreates a database it cannot
  read, so a repair would only hide that the Miis were already lost.
- **It will not store a Mii the Wii would refuse.** `mii_db_put`,
  `mii_hidden_put` and `mii_remote_put` check the Mii first
  (`MII_E_INVALID`), and `mii_db_put` refuses a Mii already in another
  slot (`MII_E_EXISTS`).
- **It will not save over someone else's write.** `mii_db_save` checks the
  file is still the one it loaded (`MII_E_CHANGED`).

Save from your own program, not while the Mii Channel or a game using RFL
runs: they keep their own copy and would write it back over yours.

## Docs

- [docs/mii.md](../../docs/mii.md): the Mii data, the database, the hidden
  database, a Remote's Miis, the creation ID and the CRCs, each with its
  source, and the proof on a Wii.
- [sdk/mboard](../mboard): a Mii as the icon of a Message Board letter
  (`mboard_msg.face` takes `mii_pack`'s 74 bytes).

Tests: `tests/test_mii.py` on the PC, and `tests/wii_mii.py` on the bench Wii.
