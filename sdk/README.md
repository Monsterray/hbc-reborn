# HBC's SDKs for homebrew apps

Each library here works in any libogc app, not only with HBC. Most are one
or two files you copy into your project.

| SDK | What it gives your app | Licence | Changes on the Wii |
|---|---|---|---|
| [hbc_netlog.h](hbc_netlog.h) | your app's `printf` output on the PC, through `tools/hbc.py log` | public domain | none lasting: a TCP connection to the PC while the app runs, and a copy of the last output in MEM2 for HBC to show |
| [hbc_agent](hbc_agent/README.md) | HBC's tools inside your running app: files, status, restart, crash reports, a HOME overlay | **GPL v2+** | listens on TCP 4299 while the app runs; writes files only when the PC asks |
| [wiispk](wiispk/README.md) | clean Wii Remote speaker sound | zlib | none |
| [slotled](slotled/README.md) | the disc-slot light: levels, effects, IOS patterns, the saved setting | zlib | only the light, except `slotled_save_level`, which writes one byte of SYSCONF |
| [mboard](mboard/README.md) | Message Board posts, mail to other Wiis, the address book, the play history | public domain | writes the WiiConnect24 mail boxes and address book on the NAND; the play history needs AHBPROT |
| [mii](mii/README.md) | Miis: read, make, edit and draw them, the Mii database, a Remote's Miis | public domain | writes the Mii database or a Remote's Miis only when the app saves |

Plans: [docs/wc24-plan.md](../docs/wc24-plan.md) (sdk/wc24, all of
WiiConnect24). Ideas for later are at the top of [REVIEW.md](../REVIEW.md)'s
findings.

## Licences

HBC itself is GPL v2 or later ([COPYING](../COPYING)). The SDKs are
licensed so that apps of any licence can use them, apart from the agent:

- **Public domain** (`hbc_netlog.h`, `mboard`, `mii`): no conditions. Copy,
  change and ship them in anything, open or closed, with no credit needed.
- **zlib** (`wiispk`, `slotled`): use them in anything, including closed
  apps, with three conditions:
  - Don't claim you wrote the original.
  - Mark changed copies as changed.
  - Keep the licence notice at the end of the `.c` file.
  - Credit in your app's documentation is welcome but not required.
- **GPL v2+** (`hbc_agent`): it is built from HBC's own code. An app that
  *ships* with it linked in must be GPL-compatible. For any other app, keep
  the agent in development builds only, for example behind an `#ifdef`.

A file's header says its licence; it wins over this table.
