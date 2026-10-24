# mboard: the Wii Message Board from homebrew

`mboard` uses the Wii Message Board as games do:

- it posts messages to this Wii's board;
- it sends them to other Wiis;
- it adds lines to the day's play history;
- it reads and edits the Address Book.

It is two files, [mboard.h](mboard.h) and [mboard.c](mboard.c). They need
only libogc and are in the public domain, so you can copy them into any
project.

## Use

```c
#include "mboard.h"

// A message on today's page; the Wii Menu files it when it next starts.
mboard_post("WiiStation", "Played Final Fantasy VII\nfor 42 minutes.");

// The same with a Mii icon, a photo and a Reply button.
static uint8_t mii[MBOARD_FACE_SIZE];
mboard_msg m = { 0 };
m.title = "WiiStation";
m.body = "Played Wii Sports\nfor 5 minutes.";
if (mboard_mii(0, mii) == MBOARD_OK)
    m.face = mii;
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
mboard_friend_set(list[0].slot, "Living Room", mii);
```

Each call returns a result of 0 or more, or a negative `MBOARD_E_*` code
(`mboard_strerror()` names it). The library checks every file before it
writes, and never deletes anything already on the board.

A letter's own stationery (its border, its icon on the board, and music) is
board data. Make it on the PC with
[tools/msgboard/stationery.py](../../tools/msgboard/stationery.py), and pass
it as `m.board`.
[tools/msgboard/stationery_themes.py](../../tools/msgboard/stationery_themes.py)
makes five examples.

## Docs

- [docs/mboard.md](../../docs/mboard.md): every call, the files and IOS
  calls, what can go wrong, and the proof on a Wii.
- [docs/messageboard-messages.md](../../docs/messageboard-messages.md): how
  the board stores a message, and what sets the sender, icon, contents and
  stationery.
- [docs/messageboard.md](../../docs/messageboard.md): the play history.
- [docs/mii.md](../../docs/mii.md): Mii data, and what makes a Mii valid.
  [sdk/mii](../mii) builds, checks and stores Miis; `mii_pack()` gives the 74
  bytes `m.face` takes.

The Homebrew Channel's own play log is this library's writer.
