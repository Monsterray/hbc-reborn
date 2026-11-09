# SDKs for Wii homebrew

*Small libraries for any libogc app, from the Homebrew Channel reborn.*

Each SDK works in any libogc app, not only with HBC. Most are one or two files
you copy into your project. Each folder has a README with a quick start, the
API, and exactly what the library changes on the Wii.

## The SDKs

| SDK | What it gives your app | Licence | Changes on the Wii |
| --- | --- | --- | --- |
| [hbc_netlog](hbc_netlog/README.md) | Your app's `printf` output on the PC, through `tools/hbc.py log` | Public domain | Nothing lasting: a TCP connection to the PC while the app runs, and a copy of the last output in MEM2 for HBC to show |
| [hbc_agent](hbc_agent/README.md) | HBC's tools inside your running app: files, status, restart, crash reports, a HOME overlay | **GPL v2+** | Listens on TCP 4299 while the app runs; writes files only when the PC asks |
| [wiispk](wiispk/README.md) | Clean Wii Remote speaker sound | zlib | Nothing saved; the remote's Bluetooth link stays in sniff mode once set, as the Wii's own software leaves it |
| [slotled](slotled/README.md) | The disc-slot light: levels, effects, IOS patterns, the saved setting | zlib | Only the light, except `slotled_save_level`, which writes one byte of SYSCONF |
| [mboard](mboard/README.md) | Message Board posts, mail to other Wiis, the address book, the play history | Public domain | Writes the WiiConnect24 mail boxes and address book on the NAND; the play history needs AHBPROT |
| [mii](mii/README.md) | Miis: read, make, edit and draw them, the Mii database, a Remote's Miis | Public domain | Writes the Mii database or a Remote's Miis only when the app saves |
| [usbguard](usbguard/README.md) | An app that survives a stuck USB drive: guarded calls, mounts and IOS reloads, a warning, the software unplug | Public domain | Nothing, except `usbguard_free`, which disables the stuck drive's USB port until it is replugged |

Planned: [sdk/wc24](../docs/plans/wc24.md), all of WiiConnect24. Ideas for later
are in [docs/review.md](../docs/review.md)'s open items.

## Using one

Copy the SDK's files into your project, or point your compiler at its folder:

```make
CFLAGS += -I<hbc-reborn>/sdk/mboard -I<hbc-reborn>/sdk/hbc_netlog
```

`hbc_agent`, `wiispk`, `slotled` and `usbguard` can also be built as a library with
`make -C sdk/<name>`; their READMEs give the link flags.

`hbc_agent.h` and `hbc_netlog.h` used to sit directly in `sdk/`. They moved into
their folders in November 2026; forwarding headers at the old paths keep
`-I<hbc-reborn>/sdk` builds working until HBC 1.16.0, which deletes them. A build that still uses them prints a note saying what to change.

## Licences

HBC itself is GPL v2 or later ([COPYING](../COPYING)). The SDKs are licensed
so that apps of any licence can use them, apart from the agent:

- **Public domain** (`hbc_netlog`, `mboard`, `mii`, `usbguard`): no conditions. Copy,
  change and ship them in anything, open or closed, with no credit needed.
- **zlib** (`wiispk`, `slotled`): use them in anything, including closed apps,
  on three conditions:
  - don't claim you wrote the original;
  - mark changed copies as changed;
  - keep the licence notice at the end of the `.c` file.

  Credit in your app's documentation is welcome but not required.
- **GPL v2+** (`hbc_agent`): it is built from HBC's own code. An app that
  *ships* with it linked in must be GPL-compatible. For any other app, keep the
  agent in development builds only, for example behind an `#ifdef`.

A file's header says its licence; it wins over this page.

## Adding an SDK

An SDK is a folder, `sdk/<name>/`, with its source, a header named after it,
and a `README.md` in the same shape as the others:

1. `# <name>`, an italic one-line tagline, and an at-a-glance table: Files,
   Needs, Licence, Changes on the Wii, Since.
2. A Contents line, then these sections in order: **What it does** (a few
   bullets), **Quick start** (a short example that compiles), **Add it to your
   app** (files, flags, link order), **API** (a table: every public name, one
   line each), any sections of its own, **What it changes on the Wii** (a
   table: part, what changes, how long it lasts), **Limits**, **Licence**.

Write for people first: what it does and a working example before the details.
Then for agents: exact names, error values and limits in tables, so a search
finds them. Every number comes from a measurement or the code; say which.
Add a host test for the parts that run without a Wii, and a row in the table
above.
