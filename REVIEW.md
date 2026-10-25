# Project review

## Plan

Review each subsystem in this order. For every finding, record the trigger,
effect, smallest safe fix, and a check that can show whether the fix works.
Prefer repair to removal. Rank confirmed defects before cleanup and future
work.

| Subsystem | What to trace | Check |
| --- | --- | --- |
| Build and dependencies | Tool versions, paths, generated files, and clean builds | Build the DOL and retail WAD from a clean tree on current devkitPro |
| Startup and loader | Linker entry, memory layout, IOS handoff, DOL/ELF loading, and failure paths | Inspect the matching ELF; boot the DOL in isolated Dolphin and on a dev Wii when needed |
| Channel services | App discovery, XML, SD/NAND access, themes, network, and Wiiload | Exercise valid and malformed inputs; inspect the latest Dolphin boot log |
| UI, media, and language | Assets, rendering, input, audio, translations, and banner data | Build assets; inspect the menu and launch flow in Dolphin |
| WAD and Python tools | Python 3 paths, archive sizes, content hashes, ticket/TMD data, and secret handling | Build with a private common key; decrypt and compare both content hashes |
| WiiPAX and Wiiload | Host-tool bounds, formats, transfer errors, and platform assumptions | Build and run focused format/transfer checks |
| Documentation and release | Contributor steps, version fields, ignored artifacts, and test limits | Follow README on this Mac; verify Git excludes keys and generated files |

Use the Wii64 project's Dolphin workflow as a reference for an isolated
profile, the latest boot segment, and guest fault addresses. A WAD build check
does not prove that the WAD is safe to install on a real Wii.

## Findings

The review found these defects in the current tree. Each has a focused fix in
this change set. Build checks passed; a normal Dolphin boot does not replace a
malformed-input test.

| Area | Fixed defect | Check |
| --- | --- | --- |
| Startup and loader | Truncated ELF/DOL headers, section offsets, and load ranges could pass unchecked arithmetic. | Channel DOL builds; malformed-image checks remain future work. |
| Channel services | Long app directory names and metadata arguments could overrun fixed buffers. Theme font targets could share a pointer and free it twice. | Channel DOL builds; bounds and ownership paths reviewed. |
| Channel services | A host-only update URL could dereference null; a failed NAND stat leaked a descriptor; `no_ios_reload` set the wrong flag. | Channel DOL builds; metadata and launch branches reviewed. |
| UI, media, and language | No confirmed defect in the banner, asset, or translation build path. | Clean channel build produced the banner and language assets. |
| WAD and Python tools | Retail `check` required optional DPKI keys. Changed version or packer scripts could leave a stale WAD. Python 3 division returned floats in `toblocks`. | Clean retail build and `check` pass; title version is `0x104`. |
| WiiPAX and Wiiload | An oversized upload argument could overrun its buffer. Short USB Gecko writes used the original byte count. ELF section names could read past their string table. | `tests/host_bounds.sh` passes; a forced short-write check sent exactly 5,000 bytes. |
| Documentation | `channel/README` reads as current release instructions despite describing v1.1.2. | It now points to current build instructions while retaining historical text. |

### 1.1.8 review

| Area | Fixed defect | Check |
| --- | --- | --- |
| Channel services | Theme zip entry sizes were unbounded; `0xffffffff` wrapped `pmalloc(size + 1)` to zero before a copy into it. Zip entries could contain `..` and extract outside `/apps/<name>`. | Entries above `MAX_THEME_ZIP_SIZE` are skipped; `..` rejects the zip. |
| Channel services | Long `meta.xml` arguments overran `result->args`; a long app name overran the loading caption. Uncompressed Wiiload transfers dropped their arguments. | Arguments are bounded by `ARGS_MAX_LEN`; the caption uses `snprintf`; arguments are copied for both transfer kinds. |
| Channel services | A short SD read looped forever; the TCP command reset wrote the Gecko state; NAND read errors freed blob memory with `free`; a malformed second `theme.xml` left dangling theme strings. | Read loop stops at EOF; each state and allocator is matched; freed strings are cleared. |
| Host tools | GCC 15 (C23) rejected wiiload's own `bool`; small incompressible files failed `compress2`; a failed Gecko block and a failed transfer still exited 0. | `stdbool.h`, `compressBound`, and failing exit codes. |
| Build | Git symlinks broke Windows checkouts. In particular, `retail/00000001` became an 11-byte text file that `wadpack.py` would have packed as channel content. | Symlinks removed; `wadpack.py` checks each content's size and SHA-1 against the TMD. |
| Tests | `stub_layout.sh` allowed a stub up to `0x1800` bytes, but the return-title magic starts at `0x1700`. `host_bounds.sh` relied on dead stripping that PE linkers do not perform. | Limit is `0x1700`; the check links the real WiiPAX objects. |

The 1.1.8 DOL built on Windows 11 with devkitPPC r50-1 and libogc 3.1.0.
`tests/dolphin_smoke.py` booted it in a fresh Dolphin profile, and it
answered `HBCV` with `1.1.8`. Still open: the SD-app descriptor leaks on the
loader's out-of-memory paths, and a failed `ES_Launch` in the stub waits
forever for a second acknowledgment, so its system-menu fallback cannot run.

### 1.1.9 review

| Area | Fixed defect | Check |
| --- | --- | --- |
| UI, media, and language | `mkicon.py` wrote the binary icon layout in text mode. macOS stored it as UTF-8, so the 1.1.7 WAD's `icon.brlyt` begins `RLYT c3 be c3 bf` instead of `RLYT fe ff`; Windows could not build it at all. **Do not install a WAD built before 1.1.9.** | The layout is written as bytes; every other banner output is byte-identical to the macOS build. |
| Build | The banner needed host libpng and SoX. | `png2tpl.py` (25 of 25 textures identical) and `wav2raw.py` (both sounds identical) use only the standard library. |
| Build | 47 compiler warnings, an assembler warning per embedded file (`bin2s` output fed to `as` without the preprocessor), and linker and `elf2dol` warnings about executable segments. | Clean builds pass with `EXTRA_CFLAGS=-Werror`; the channel DOL is byte-identical to the 1.1.8 build. |
| WAD and Python tools | Fourteen PyWii tools were still Python 2, and `ec.py` compared, divided, and hashed with Python 2 semantics. | `tests/test_pywii.py` covers ECDSA, U8 archives, the ticket and TMD templates, and every tool's usage path. With a fake key, the ticket, TMD, and WAD match the 1.1.8 tools byte for byte. |
| Documentation and release | No CI. | `.github/workflows/ci.yml` builds in `devkitpro/devkitppc` and runs host checks on Windows, Linux, and macOS. |

### 1.2.0 developer network

HBC 1.2.0 adds `HBCS` status, `HBCF` SD/USB file requests, and `HBCN` app log
registration on port 4299 (`docs/devnet.md`), with `tools/hbc.py` and
`sdk/hbc_netlog.h`. `tests/dolphin_smoke.py --devnet` passed in Dolphin on
Windows 11: status, a 100 KB put/get round trip, listing, deletion, four
rejected paths, and a logged `netlog_app` run with arguments.
`tests/test_hbc_tool.py` checks the client framing against a fake server.
Not yet run on the dev Wii.

### 1.2.2 dev Wii run

`tests/wii_devnet.py` passed on the bench Wii (IOS58 rev 6175) after three
hardware-only defects were fixed:

| Defect | Evidence | Fix |
| --- | --- | --- |
| IOS closes sockets with a reset, dropping a reply still queued. | `HBCS` returned its 8-byte header, then `WSAECONNRESET`. | `tcp_close()` half-closes and drains for up to 1 s before `net_close`. |
| On a non-blocking socket with a full send buffer, IOS reports a 2048-byte `net_write` as complete after queueing 2040 bytes. | Every download lost 8 bytes at stream offset 4 KiB and ended with stale buffer bytes; a PC-written text file came back broken at `0xff8`. | Devnet replies are sent with the socket in blocking mode. |
| The reload stub drops the app's log socket without a FIN, so the PC never saw the log end. | All app output arrived, then the PC waited until timeout. | `hbc_netlog.h` closes the socket from an `atexit` handler; the PC treats a reset as the end. |

The WAD build also found that an MSYS2 login shell gives native Python no
`USERPROFILE`, so PyWii looked for keys under a literal `~`, and a failed
`wadpack.py` left a 3,884-byte partial WAD. Keys now resolve through
`key_dir()` (or `$WII_KEYS_DIR`), `wadpack.py` writes to a temporary name,
and the title Makefile deletes the target of any failed step.

### 1.3.0 network performance and installed-WAD return

Measured on the bench Wii with `tests/wii_netbench.py`, protocol 1 moved
0.62 MB/s up and 0.42 MB/s down. Four causes, each checked on hardware:

| Cause | Change | Effect |
| --- | --- | --- |
| `tcp_read`/`tcp_write` slept 20 ms whenever IOS had no data ready, in 2 KiB steps. | Wait with `net_poll`; 16 KiB IOS blocks (libogc's 64 KiB network heap is the limit). | Up 0.62 to 0.90 MB/s. |
| The loader thread (48) ran below the UI thread (64). | Transfers run at 80. | Worst status round trip 531 to 47 ms. |
| Network and SD ran strictly in turn. | A worker thread and four MEM2 buffer pairs overlap them. | A 5.3 MB ELF upload dropped from 6.0 s of combined work to 3.7 s. |
| Every byte crossed the slowest link. | Per-frame zlib, stored when it does not shrink, with a CRC-32 on every frame. | ELF 1.45 MB/s up and 0.91 down; zeros 4.2 and 4.9 MB/s. |

Socket buffer sizes, blocking receives, and zlib level 6 on the Wii were
measured and rejected (docs/devnet.md). Incompressible data stays bound by
the IOS stack at about 1.35 MB/s received and 0.63 MB/s sent. Every framed
transfer is checked end to end; a frame with a bad CRC is rejected with
`EBADMSG` and leaves no file.

A stack `z_stream` passed to `inflateInit` without zeroing made zlib call a
garbage allocator; Dolphin caught the jump into BSS. `hbc.py` also decoded
the Wii's newlib error numbers with the host's table.

The retail WAD now installs into an isolated Dolphin NAND and passes the
full developer suite from there. With cheats enabled, so Dolphin does not
substitute its `HBReload` hook, `netlog_app`'s exit ran HBC's own reload
stub and the installed title relaunched and answered `HBCV` with 1.3.0. This
closes the installed-WAD return test on Dolphin; a real Wii install remains
a separate, deliberate step. The 1.3.0 DOL passed `tests/wii_devnet.py` on
the bench Wii.

### 1.3.1 installed WAD on the dev Wii

The 1.3.0 retail WAD was installed on the bench Wii. Started from the Wii
Menu, it answered `HBCS` as 1.3.0 (IOS58 rev 6175, AHBPROT, 14 apps, SD), and
`tests/wii_devnet.py --installed --expect 1.3.0` passed: framed and raw
transfers, CRC rejection, path checks, the network log, and
`netlog_app`'s exit relaunching the installed channel through HBC's own
reload stub. This is the first real-hardware pass of the installed-title
return path.

Starting the installed title with `tests/launch_title` (libogc
`WII_LaunchTitle`) sent from the stock HBC left a black screen: ping still
answered and port 4299 refused, consistent with ES never completing the IOS
reload, and Reset returned to the stock HBC. The same launcher starts the
channel in Dolphin within 5 s, so real IOS behaves differently here; start
installed titles from the Wii Menu on hardware.

### 1.4.0 fixes and optimizations

From the 1.3.6 review, each verified in the code first:

| Kind | Change | Check |
| --- | --- | --- |
| Fix | `tcp_read`/`tcp_write` treated end of stream like "no data yet" and spun for the 10 s timeout above the UI, which was the real cause of the "bare connect holds the loader" behavior. A read of 0 right after `net_poll` reports the socket readable or hung up now ends the request, and the request header must arrive within 2 s. | Bare connect then status: 0.05 s on the Wii. |
| Fix | An app launched during a transfer could unmount the card under the transfer's worker. `loader_deinit` now calls `devnet_abort()`, which shuts the socket and waits up to 3 s; uploads delete their partial file. | Code path; not forced on hardware. |
| Fix | Apps put under `<device>:/apps/` never appeared: `AE_CMD_SCAN` ignores a mounted card. Puts and deletes queue the app folder and the menu reloads that entry. | 14 → 15 → 14 apps on the Wii. |
| Fix | The reload stub waited forever for a second acknowledgment when IOS refused `ES_Launch`, so its system-menu fallback never ran. It now also accepts the refusal reply. | Success path in Dolphin and on the Wii; Dolphin stalls the PPC on a missing title, so the failure path needs hardware. |
| Fix | A log target outlived its PC: `hbc.py` now clears it on exit, HBC restores a live target after a restart, and `hbc_netlog_init` connects non-blocking with a 2 s limit and a bounded `net_init`. | Dolphin and Wii log runs. |
| Fix | A put to `.../` created a file named after the directory (now `EISDIR`); SD-app descriptors leaked on loader out-of-memory paths; `devstream` leaked message queues when its worker failed to start. | `EISDIR` checked on the Wii. |
| Optimization | Wiiload uploads now run above the UI and inflate in 16 KiB pieces as they arrive, so compressed data needs no full-size buffer. | Wii runs; throughput is link-bound and unchanged. |
| Optimization | `tcp_close` drains with `net_poll` instead of 10 ms sleeps; `hbc.py send` compresses at level 9. | Status median 30.9 ms. |
| Measurement | `HBCS` reports `heap_free`, the loader stack high-water mark, `init_ms`, and `scan_ms`. | Wii: 968 ms to the menu, 300 ms to scan 14 apps, 2,824 of 8,192 stack bytes. |
| Tools | `hbc.py` gained `get`/`put`/`rm -r`, `sync` (size then CRC-32 through the new op `C`), progress, `--json`, safer paths, and Windows fixes; listings mark truncation. `tests/launch_title` now writes the stub's return-title words and exits instead of calling `WII_LaunchTitle`, and it relaunched the installed channel on the Wii. | 24 unit tests; `dolphin_smoke.py --devnet` and `wii_devnet.py` pass. |

### 1.9.9: hbc.py answered while Options is open

- The Options dialog runs its own loop and never woke HBC's network thread,
  so `hbc.py` timed out while it was open (found taking a picture of the new
  five-device layout in Dolphin). It now does, like the popups since 1.9.5.

### 1.10.16: audit of rename, move and attributes

An independent review (2026-10-24) of 1.10.14 and 1.10.15 found these,
each now fixed and tested:

- **Data loss: a name FAT reads as the source.** FatFs drops a name's
  trailing dots and spaces and folds case, non-ASCII too.
  - `mv --replace sd:/x.txt sd:/x.txt.`, or `Été.txt` to `été.txt`, found
    the source at the destination. A byte compare saw "something else", so
    it unlinked the source, then the rename failed.
  - `mv sd:/a sd:/a.` moved a folder into itself.
  - Now paths with a `.` component, or a name ending in `.` or a space, are
    `EINVAL`.
  - A destination with the source's `stat` (type, size, time, inode where
    given) and a name that differs only in case is the source itself: a
    rename, never a replace.
  - A folder is refused when it is the destination or any folder above it,
    however it is spelt.
- **Moving a big file between devices failed.** `remote_is_dir` asked with
  op C, a CRC of the whole file, which timed out on a large one. It now uses
  op S. The help's ISO example was wrong too: the Wii takes uploads up to
  512 MiB, so a move between devices refuses bigger files up front.
- **A half-deleted source.** A read-only entry stopped a move between
  devices after the copy, part way through deleting the source. It is now
  refused before anything is copied, and a copy that fails is removed again.
- **Read-only was replaced on libogc2 and 1.x.** `mv --replace` onto a
  read-only file worked there, since libfat's `unlink` does not check. It is
  now `EACCES` everywhere.
- **Smaller fixes:**
  - `attrib +x` crashed; it is now a usage error.
  - Setting a bit on libogc 3 no longer needs the `f_chmod` offset.
  - A stale `errno` is cleared before `FAT_setAttr`.
  - An early op R answer drains the destination first, so the client gets
    the answer, not a reset.
  - The Wii refuses to rename `<device>:/apps` from any client.
  - `rm --force` clears read-only only when a delete was refused for it.
  - `mv` and `attrib` have `--json`.
  - The rm help and the op A row are fixed.
- **Proof:**
  - `tests/test_hbc_tool.py`: 4 new tests, 48 in all.
  - `check_move` adds trailing dots and spaces, a `.` component, a non-ASCII
    case rename with `--replace` (the file survives), a folder into itself by
    another case, `--replace` onto a read-only file, and `sd:/APPS`. It
    passes in Dolphin on HBC and on the agent.
  - On the bench Wii (2026-10-24), all pass:
    - `wii_devnet.py`;
    - `wii_file_meta.py`: attributes on HBC, and on the libogc 3 and libogc2
      agent apps;
    - `wii_agent.py`: the whole run, including the crash, fatal and exit
      log checks that had lost their last lines twice under 1.10.14.

### 1.10.15: FAT attributes, as a normal file system shows them

- **Listings and stat** (`devfile.c`, HBC and the agent) carry every FAT
  attribute: `r` read-only, `h` hidden, `s` system, `a` archive (`-` for
  none). They come from libfat's `FAT_getAttr` on every libogc. Before, the
  field held only `r`, from `stat`, which showed the card's read-only folders
  (made on a Mac) as writable.
- **Op `A`** sets and clears attributes (value and mask in `size`). Protocol 8.
- **Errors a normal FS gives:**
  - deleting a read-only entry: `EACCES`;
  - deleting a folder that is not empty: `ENOTEMPTY` (FatFs says `FR_DENIED`
    for both);
  - a put over a read-only file: `EACCES`, before any data.
- **libogc 3.1.0's `FAT_setAttr` cannot clear a bit.** It calls FatFs as
  `f_chmod(fs, path, 0xff, attr)`: the attributes given become the mask, and
  the value is all ones (disassembled from its libfat.a). So
  `FAT_setAttr(path, 0)` changes nothing, and read-only can never come off.
  - On libogc 3, `devfile.c` calls `f_chmod` itself, on the FatFs object
    `FAT_setAttr` uses. It reads that object's offset from `FAT_setAttr`'s own
    code, and uses it only when that code calls `f_chmod`.
  - libogc2 and 1.x write the byte whole, the directory bit kept.
- **`hbc.py`:**
  - `attrib [--recursive] [+r|-r] [+h|-h] [+s|-s] [+a|-a] PATH...`, as
    Windows' `attrib`;
  - `ls` and `stat` show the attributes;
  - `rm --force` clears read-only first, as `del /f` does;
  - `rm` on a read-only entry says so, and how;
  - `attrib` and `rm --force` refuse an HBC before 1.10.15 before sending
    anything.
- **Proof:**
  - `tests/test_hbc_tool.py`: 4 new tests, on a fake Wii that has attributes.
  - `agent_checks.check_attrs` sets and clears each attribute, checks the
    refusals, and checks that read-only survives a rename.
    - Dolphin: it passes on HBC and on the agent.
    - The bench Wii (`wii_file_meta.py`, 2026-10-24): it passes on HBC, on
      the libogc 3 agent app and on the libogc2 agent app. So clearing works
      both ways: through `f_chmod` on libogc 3, through `FAT_setAttr` on
      libogc2.
- **Build note.** The libogc2 agent library and test app need devkitPPC
  r41-2. With the default gcc 16 the app fails to link: `__stack_addr`,
  `__gxregs` and others are undefined.

### 1.10.14: rename and move (HBCF op R, hbc.py mv)

- **New op `R`** in `devfile.c`, which HBC and the agent share, so both have
  it. Protocol 7.
  - **Request:** the path is the source; `size` gives the length of the
    destination path, which follows it.
  - **Reply:** the destination as it ended up.
  - **Into a folder:** a destination ending in `/`, or a folder that is
    there, means "into that folder". Missing folders above the destination
    are made.
  - **Refusals:**
    - another device: `EXDEV`;
    - something already there: `EEXIST`, unless flags bit 1, which replaces
      a file only (`EISDIR` for a folder there, `ENOTDIR` for a folder onto a
      file);
    - a folder into itself, or a device root: `EINVAL`.
  - **Case only:** a case-only rename on FAT finds the source itself and goes
    ahead.
  - **Apps:** a rename under `apps/` reloads both app entries.
- **`hbc.py mv [--replace] FROM TO`** (also `rename`). Between devices it
  copies each file, checks its CRC-32, and only then deletes the source.
  - It asks the version (`HBCV`) first and refuses an HBC or agent before
    1.10.14 with what it needs.
  - Sending blind is not safe. An older HBC answers op `R` with `ENOSYS`
    without reading the destination that follows the request. Closing with
    unread data then resets the connection, and on Windows the reset can
    arrive before the answer. The new test against the fake Wii failed this
    way about 1 run in 5, until the client asked first.
- **Proof:**
  - `tests/test_hbc_tool.py`: 4 new tests against the fake Wii, which got
    op `R`.
  - `agent_checks.check_move` runs on HBC (`check_devnet`, with the app list
    following a renamed app folder) and on the agent.
  - Dolphin (`dolphin_smoke.py --devnet` and `--agent` on the WAD): all pass.
  - The bench Wii: `wii_devnet.py` (this tree's DOL) passes everything, and
    `wii_agent.py` passes rename and move.
- **Found on the bench Wii: `sd:/hbctest` and `sd:/hbcbench` would not
  delete.** Both were empty folders from 2026-10-12, and deleting them gave
  `EACCES`.
  - They are not damaged: they have FAT's read-only attribute. The card was
    filled from a Mac on 2026-10-12, and `apps`, `wii`, `screenshots` and
    other folders made then are read-only too (`attrib`, or Windows' `dar--`).
  - FatFs refuses to delete a read-only entry with `FR_DENIED`, which libogc
    reports as `EACCES`. It reports a folder that is not empty the same way.
  - HBC's listing showed these folders as writable (`-`). Its `r` flag comes
    from `st_mode`, and libogc's `stat` does not carry a folder's read-only
    bit, so a read-only folder looks writable over the network.
  - Op `R` renamed them, which a read-only entry allows, to
    `sd:/hbctest-stuck-20261024` and `sd:/hbcbench-stuck-20261024`. That was
    the first rename on a real Wii.
  - chkdsk also found 6 lost chains (192 KB), unrelated: another tool's
    probe logs and "fs test" files from 2026-10-18, written before a power
    loss or crash.
- **Fixed in the tests** (both predate rename):
  - Two checks still read folder listings in the pre-1.10.6 format, so they
    never matched. `dolphin_smoke.py --devnet` failed on the blob listing.
    `agent_checks` took an existing `sd:/screenshots` for one it had made,
    and tried to delete it. FAT refused, because the user's screenshots are
    there, so nothing was lost.
  - A failed `wii_agent.py` now asks the agent app to exit, so the Wii is
    left in HBC, as the bench queue needs.
- **Open: the app log loses its last lines now and then.**
  - On the bench Wii, `wii_agent.py` passed rename and move in all three runs
    (2026-10-24).
  - The second and third runs each failed later, at a different network-log
    check in the crash, fatal and exit sequence: the app's last lines never
    reached the PC's log server. The run before them passed those checks.
  - Op `R` does not touch the log path. The next step is to see whether
    1.10.13 does the same.

### 1.10.13: sdk/mii audit, robust and easy to use

Two independent reviews (2026-10-24) covered `sdk/mii`. One checked the code
against RFL's decompiled sources. The other read the docs as a new developer
would, writing and compiling the code for six tasks.

- **Fixed: data loss or corruption.**
  - **A Remote write could lose every Mii.** `mii_remote_write` always wrote
    copy 0 first. When copy 0 was the only whole copy (an earlier write
    stopped in copy 1), a second interruption left neither whole. It now
    reads the Remote and writes the copy that is not whole first.
  - **A damaged Remote read as a new, empty one.** When both copies had the
    magic and neither CRC was right, the read returned `MII_OK`, and the next
    write erased its 10 Miis. That case is now `MII_E_FORMAT`.
  - **Hidden-database links were trusted.** A file with a right CRC could
    still carry a next or prev outside the table, a loop, a prev that does
    not point back, a used node off the list, or a head or tail past the
    table. Following such a link wrote past the table into the CRC and hidden
    Mii data, or looped forever. `mii_db_attach` now checks every link and
    refuses such a file, and an out-of-range link reads as -1.
  - **A failed save could never be retried.** After `MII_E_IO` or
    `MII_E_VERIFY` the file was half ours, and the next save called it
    "changed by someone else". A save that did not finish may now write over
    its own half-written file.
  - **Hidden Miis saved too much.** The changed hidden Miis were kept as one
    [first, last) range, so changing entries 0 and 9999 rewrote about 640 KB,
    unrelated entries included. A bitmap now marks each changed Mii, and each
    run is written and read back on its own.
- **Fixed: wrong results.**
  - **Flag fields.** `mii_check` did not range-check `favorite`, `local_only`
    (1 bit each) or `kind` (2 bits), and `mii_pack` cut larger values. A
    special-ID Mii with local only 2 passed the check and was stored with the
    bit at 0, which RFL rejects.
  - **Temporary IDs.** `mii_check` counted a temporary ID as "special"; RFL's
    `RFLiIsSpecialID` does not.
  - **Names.** `mii_set_name` accepted overlong forms (an embedded NUL),
    surrogates, short sequences and code points past U+10FFFF. It now writes
    U+FFFD for each. `mii_get_name` wrote a lone surrogate as invalid UTF-8,
    and now writes U+FFFD.
  - **Duplicate IDs.** IDs count 4-second steps, so two Miis made within 4
    seconds got the same one: `mii_db_add` failed with `MII_E_EXISTS`, or
    `mii_hidden_put` overwrote the first Mii. Each `mii_make_id` is now at
    least one step past the last.
  - **The Remote table before `WPAD_Init`.** A lookup made too early failed,
    and the failure was kept: every later Remote call said
    `MII_E_NO_REMOTE`. Now only a found table is kept, and a libogc whose
    code does not match says `MII_E_UNSUPPORTED`.
  - **The MAC read.** A non-zero NCD status counted as a good MAC, and each
    failed call waited another 8 s. Both are fixed.
  - **A missing database** (Mii Channel never opened) said "no Mii there".
    It now says `MII_E_NO_FILE`.
  - **A file of the wrong size** was `MII_E_IO` (load), or was read anyway
    (`mii_get`). Both now give `MII_E_FORMAT`.
- **For developers.**
  - **Building.** The header says how: compile `mii.c` as C and link
    `-lwiiuse -lbte -logc -lm`. libwiiuse needs `-lm`; the first build of the
    example failed without it. `mii.c` now also compiles as C++.
  - **New calls and names:**
    - `mii_db_free_slot` and `mii_remote_free_slot`;
    - `mii_color_name` and `MII_RED` ... `MII_BLACK`;
    - `MII_NAME_UTF8_MAX` and `MII_VERSION`;
    - `mii_db_free` in the PC build.
  - **Every function's contract** is now in mii.h: what it changes, its
    errors, how long it blocks, the Remote's setup, and ownership.
  - **sdk/mii/README.md** is rewritten:
    - each example now compiles; a harness built every snippet with devkitPPC
      at `-Wall -Werror`. Before, they called `gettime()` without its header
      and overwrote Remote slot 0;
    - a table of errors and what to do about each.
  - **`sdk/mii/example`** is a whole program, built in CI.
  - **sdk/mboard's README** named a variable `mii`, which clashed with
    mii.h's type. It is now `face`.
- **Proof:**
  - `tests/test_mii.py`: 24 tests, 6 of them new, one per defect class. Each
    new test fails with its fix reverted: the link check, the damaged Remote
    and the flag limits were checked that way.
  - `tests/dolphin_mii.py`: all checks pass.
  - The bench Wii (`tests/wii_mii.py`):
    - `check`: the real database passes the new link check, and two IDs made
      back to back differ.
    - `hidden`: the table and slots end byte for byte as they were.
    - `remote-rewrite`: the 10 Miis on Wii Remote 1, both copies, 8.2 s, byte for byte as
      before.

### 1.10.12: sdk/mii on a real Wii Remote

- `mii_remote_read_raw()` returns both copies as the Remote holds them, for a
  backup. `tests/mii_app remote-rewrite` (`wii_mii.py remote-rewrite`) uses it
  to save the Remote's memory, then writes its own Miis back unchanged.
- On the bench Wii (2026-10-23) Wii Remote 1 carried 10 Miis made on other
  Wiis, so `remote-add` stopped with `MII_E_FULL` and wrote nothing.
  - `check`: copy 0 at 0x0FCA has a right magic and CRC, and every Mii passes
    the check. That proves the 0x0FCA base.
  - `remote-rewrite`: both copies were written in 7.2 s (188 writes of 8
    bytes) and read back. Both then matched the bytes from before, byte for
    byte: what `mii_remote_build` makes is what the Mii Channel wrote.

### 1.10.11: sdk/mii, Miis from homebrew

- New: `sdk/mii`, two public-domain files that need only libogc:
  - a Mii as a struct, its check (with the reason it fails), new and random
    Miis, creation IDs, and `.mii` files;
  - `RFL_DB.dat`'s 100 slots and its hidden database;
  - a Wii Remote's 10 Miis.

  Formats and sources are in docs/mii.md. It never writes a database whose CRC
  or size is wrong.
- Three things found on the way, each now in the code and docs/mii.md:
  - **libogc 3's `wiiuse_write_data` keeps only the low 4 bits of the
    length** (`clrlwi r11,r6,28` in libwiiuse.a). A 16-byte write goes out
    with size 0. Dolphin logs "WriteData: invalid size: 0", and a Remote
    sends no ack, so the write waits forever. `sdk/mii` writes 8 bytes at a
    time. `sdk/wiispk` writes 7 bytes at most, so it is not affected. Any
    other caller of `wiiuse_write_data` must stay under 16 on libogc 3.
  - **The Wi-Fi MAC from NCD ioctlv 8 into a MEM1 buffer came back with its
    last two bytes wrong** on the bench Wii, every time. libogc's
    `net_get_mac_address`, whose 6-byte buffer is in its MEM2 net heap, gave
    the MAC whose home word matches the Mii made on that Wii. Just after
    HBC's IOS reload, both fail with -6 for a few seconds, until IOS's
    network module registers `/dev/net/ncd/manage`. `mii_wifi_mac` waits up
    to 8 s, once.
  - **A free hidden-database node is ID 0 with next and prev -1**
    (`RFLiClearTableData`), not all zeros: the bench Wii's file has
    `7fff7fff` in all 10,000. The first bench run of `wii_mii.py hidden`
    zeroed a node it freed, and its byte compare caught it. It still read as
    free, and the restore put the original back.
- Proof:
  - `tests/test_mii.py` (18 tests, the PC) also runs on a copy of the bench
    Wii's own database (`MII_REAL_DB`).
  - `tests/dolphin_mii.py` passes all 13 checks: a database on Dolphin's NAND,
    and the emulated Remote written and read back.
  - On the bench Wii (`tests/wii_mii.py`, 2026-10-23):
    - `check` loads the database in 725 ms with its CRC right. The Mii made
      there passes the check and reads as made there.
    - `hidden`: a Mii into the hidden database and out. Afterwards the table
      and slots match, byte for byte; only that Mii's 0x40 bytes of data
      remain, as with RFL.
    - `add` puts "HBC Mii" in slot 1. Only that slot and the CRC changed, the
      CRC is right, and a reload holds the same Mii.
    - `backup` and `restore` put the original file back, byte for byte.
- Not yet on a real Remote in 1.10.11 (see 1.10.12).

### 1.10.10: the overlay and MotionPlus

- libogc reports a MotionPlus only once an app switches it on
  (`WPAD_SetMotionPlus`, which probes for it): before, the agent showed no
  "+ M+", no gyro on Test and measured none on Calibrate in an app that does
  not use MotionPlus itself. Test and Calibrate now switch it on for their
  remote, and the agent switches it back off on Test's close and the
  overlay's, for the remotes it switched on (only where one was found: a
  switch-off probes again otherwise). HBCS "remotes" adds "mplus" (none,
  present, active), "mplus_ours" and "gyro".
- `tests/wii_mplus.py` on the bench Wii (2026-10-23, a remote with a
  MotionPlus): "none" before; on Test "active" at once, the gyro 7986/8632/8693
  at rest (spread 12-14 over 11 reads); after closing "present", not ours.
  Calibrate's gyro uses the same switch-on; its result is shown only on screen.
- 1.10.10: HBC links this tree's agent (channel/channelapp/Makefile builds
  sdk/hbc_agent first), so HBC's HOME menu does the same. `wii_mplus.py --hbc`
  ran this tree's HBC DOL on the bench Wii and passed: active on Test (gyro
  7991/8634/8693), off again after HOME closed.

### 1.10.9: sdk/mboard, posts to the Wii Message Board

- HBC's play log is now sdk/mboard's writer: `playtime.c` calls
  `mboard_play_vff()`, and `channel/channelapp/Makefile` builds `mboard.c`
  with `-DMBOARD_NO_OGC`. `cdblog.c`, `cdblog.h` and `tests/cdblog_host` are
  gone. The two writers were the same code, checked against the same
  reference. `mboard_session` has `cdblog_session`'s layout, so HBC's saved
  state (`playlog.bin`) still reads, and the result codes are the same
  numbers. `tests/test_cdblog.py` and `tools/msgboard/c_then_menu.py` drive
  `tests/mboard_host play`.

- **sdk/mboard** (docs/mboard.md): a message on today's page through the
  SDK's loop-back into the WiiConnect24 receive box, and a Play History
  line through the play log's cdb.vff writer. Proven on PC images, in
  Dolphin with the real Wii Menu, and on the bench Wii: the Wii Menu filed
  the post and kept every file.
- **Message files must be padded to 32 bytes.** The Wii Menu's reader
  reads whole 32-byte blocks and silently drops a message whose file ends
  inside one. Found by giving Dolphin a symbol map of the Wii Menu
  (koopthekoopa/wii-ipl), so its `OSReport` lines and traced calls reach the
  log, and by its GDB stub for return values.
- **Open, `hbc.py send`:** an app without the agent that is done in under
  10 s has HBC back before `upload_outcome()` sees it go quiet. The new HBC
  starts its upload count at 0, so `send` waits 60 s and fails although
  the app ran (`nandget` of a small folder, every time on 2026-10-22). Fix
  when a test needs `send` to succeed for quick apps: treat an upload count
  lower than `before`, or a new HBC start, as "launched".
  `tests/wii_mboard.py` works around it by checking the app's own report.
- **Open, Dolphin:** its VFF code (`VFFUtil.cpp`) treats the 2 MiB
  `wc24send.mbx` as FAT16; the Wii writes it as FAT12 (`f0 ff ff ff 0f`).
  Matters only for Dolphin's own send path.

- The full message (`mboard_post_msg`): a Mii face, a JPEG, board data, Reply,
  delay and filing date, as `NWC24CommitMsg()` writes them (Petari's
  `NWC24MsgCommit.c` format strings, rebuilt independently in
  `tests/test_mboard.py`). `mboard_mii()` reads the Wii's Mii database. The
  Wii Menu filed it with every part, in Dolphin and on the bench Wii, and
  the user saw it on the bench board: the Mii, the photo, the button that
  opened HBC, and Reply (docs/mboard.md, Proof). Delay and filing date are
  checked on the PC only.
- docs/messageboard-messages.md audited against the Wii Menu and SDK
  decompilations, and corrected:
  - its "authenticated record" offsets were CDB's file header;
  - its `HAEA/<ID0>/<ID1>` path is the SD backup's;
  - `HABA` is the Wii Shop Channel, from a `chjump.bin`;
  - 0x245B0 limits the attachments, not the message.

  The CDB signature (what the HMAC covers, and whether NAND reads check it)
  is still unverified: those files are not decompiled. Mii notes are in
  docs/mii.md.
- `mboard_send_msg()`: the message to another Wii through the send box (the
  VFF layer now writes FAT12) and KD's `SEND_MAIL_NOW`. The bench Wii's KD
  took it and emptied the box (docs/mboard.md, "To another Wii"). Delivery
  on the other Wii (LRWii) is to be checked by a person. Not known: whether
  the receiving KD needs the sender in its address book.
- tests/nandget waited about an hour inside a NAND read of
  /shared2/wc24/mbox (job 20261022-172858-0193ca, 17:30-18:28, 2026-10-22):
  - its files on the SD card are dated 18:28, and HBC came back at 18:29;
  - the queue held the Wii as frozen meanwhile, its freeze data showing only
    a fresh HBC;
  - the queue's earlier "used outside the queue" line was the job before's
    (a single failed probe).

  The likely cause is KD, which owns those files: nandget read them without
  pausing it. nandget and nandput now pause KD (ioctl 2, then 3) around
  any /shared2/wc24 path, as sdk/mboard does, and nandget times each file.
  Trigger to look again: a copy that still takes minutes.

### 1.10.8: Z+START, and Reconnect in player order

- GameCube controller: HBC opens HOME with Z and START together (either
  first), leaving START alone to apps; the overlay still closes on START.
- WiiMote > Controller settings > Reconnect in player order, as the Wii
  Menu's Reconnect: libogc 3's `WPAD_Search()` drops every remote and clears
  the active list, so each remote joins in the next free slot as it is
  pressed (1+2, or any button if paired); libogc saves the order to
  SYSCONF. Stops at Done, a fourth remote (once a slot was seen empty: the
  drop is not instant), or as the overlay closes. libogc2 and 1.x have no
  search call: the button is greyed out there.
- Checked on the PC (overlay walk, preview render) only: both need a person
  with a GameCube controller or remotes on a real Wii.

#### "Used outside the queue" needs two missed probes

Tool work, no version of its own.

- **What was wrong:** 13 of the 18 `outside` events in 2026-10-17..23 lasted exactly 15 s, the
  busy-probe interval (`BUSY_STEP`). Each was one `HBCV` probe that got no answer, followed by
  HBC answering the next one. That is a probe that timed out, not a person at the Wii. Those
  events filled the errors page and the new lanes with flags.
- **The fix:**
  - `wait_idle` counts the probes that miss (`probes`). An event is logged only after two or
    more (`used_outside`).
  - One miss still restarts the full idle wait, as before.
  - The errors page, `report` and the lanes read older events the same way (`real_outside`):
    one without a count is real only when it lasted longer than 15 s. The 885 s and 135 s
    spells of 10-22 stay; the 15 s ones go.
  - `WII_BENCH_BUSY_STEP` shortens the probe interval, for tests only.
- **Checked:**
  - `test_one_probe_without_an_answer_is_not_use_outside_the_queue`.
  - The real-dispatcher outside-use test, now with a 1 s probe interval and a 4 s busy spell.
  - The lanes test, where a 15 s event no longer gets a flag.

#### The monitor: a line, a card and a lane per Wii

Tool work, no version of its own. Five designs were mocked up; the one built combines three:

- **Every page:** one line holds every Wii, problems first (`✖ ▲ ▶ ● ○`). It has three
  lengths, a sentence, a short form and only the times, and the monitor takes the longest
  that fits.
- **The queue page** has a card per Wii, as many across as fit: state and for how long, job,
  id, who queued it, a progress bar against the timeout, the lease, the next two jobs, the
  last job and its HBC, and the window's figures.
- **The history page** has a lane per Wii over the window: jobs (ok or failed), the hold after
  a freeze, waits for HBC, use outside the queue, and what runs now, with jobs, failures,
  busy % and the longest wait on the right.
- **How it's built:** `wii_board()` in the snapshot describes it all. The monitor sends its
  width with each request, so the lanes fill the terminal. The bash side forks nothing.
- **Fixes along the way:**
  - The monitor picks a UTF-8 locale, so `${#s}` counts characters and the box-drawing
    characters pad correctly.
  - Events from before the pool count as the first Wii's.
- **Checked:**
  - `test_the_board_and_the_lanes`.
  - The monitor's pages in `MonitorTest`.
  - The real queue at 150, 100, 60 and 40 columns.

#### The bench queue drives several Wiis

Tool work in 1.10.8, no version of its own. See `tools/wii-bench/README.md`, "Several Wiis".

- **The pool:** `WII_BENCH_WIIS="name=ip[:port],..."` on the lease server (`compose.yaml`, or
  `serve --wii`), or on a workstation without one (or in its state folder's `wiis` file).
  With none set, there is one Wii, `bench`, at each workstation's `WII_BENCH_IP`, as before.
- **The server:** `Leases` keeps a lease per Wii. A free Wii goes to the earliest-received
  job that may use it, so a job pinned to a busy Wii (`add --wii NAME`) doesn't hold up the
  rest. A waiter says which job it waits with (`head`) and which Wii it wants (`want`). One
  that says nothing (a dispatcher from before this change) only gets the first Wii, the only
  one it knows how to reach. `status` gains `wiis`, and keeps `holder` and `left` for the
  first Wii. Grant, release and expiry events carry `slot`.
- **The dispatcher** runs one turn per Wii at once. Each turn claims its own pending job
  (`Claims`), waits for a Wii, and stacks the jobs that follow on that Wii, as one turn did
  before. The idle wait, the HBC check, `--collect`, the freeze bundle and the wait after a
  hard freeze each work on the turn's own Wii. Without a server, the same lease logic runs
  in-process (`LocalLeases`), so one code path serves both.
- **What a job gets:** `WII_BENCH_IP`/`WII_BENCH_PORT`, plus `HBC_WII`/`HBC_PORT` so its
  `hbc.py` goes to the right Wii unasked, `WII_BENCH_WII`, and `WII_BENCH_LOG_PORT` (4300 +
  the Wii's place in the pool). Two jobs on one workstation never share a network-log port.
  `tools/hbc.py` takes that port as its default inside a job (jobs run without a shell, so
  `$WII_BENCH_LOG_PORT` can't go in the `add` command line). One firewall rule for
  4300-4315 covers 16 Wiis. A single shared port would need a relay on each PC, plus a
  change to every program that listens for the log; the range needs neither.
- **Changed state files:** `dispatcher.state` is now per job (`jobs`), and still has the
  latest job's `job`/`detail` for older readers. `recovering.json` became `recovering/ID.json`.
- **Shown:**
  - `status` and `setup` print a line per Wii.
  - The monitor shows a line per Wii and a WII column when there's more than one.
  - Job records and events carry `wii`.
  - `report` gets a per-Wii table, and hand-overs pair by Wii.
  - `dispatcher.log` tags each turn's lines with its Wii.
- **The bench's pool** (`compose.yaml`, 2026-10-22): `dev=192.168.8.213`, `lrwii=192.168.8.243
  pinned`, `prod=192.168.8.200 pinned`. A `pinned` Wii takes only jobs added with `--wii` and
  its name. Unpinned jobs never land on the production consoles and never wait for them, so
  the rule that they take no ordinary queue jobs still holds. A pinned job there waits for HBC
  to be idle, like any job, so a game being played is never interrupted.
- **Checked:**
  - `MultiWiiTest`:
    - the pool parser;
    - a lease per Wii;
    - a pinned job that doesn't block the other Wii;
    - an old client held to the first Wii;
    - a pinned-only Wii that turns away jobs for any Wii;
    - two real jobs at once on two fake HBCs without a server, each with its own Wii's
      `HBC_PORT` and log port;
    - pinned jobs through a server, with history, `status` and `snapshot`.
  - 70 tests pass on Windows and 70 on Linux (WSL).
  - There is only one real Wii so far: nothing has run on two real Wiis.

#### The bench queue's week of 10-12 to 10-19

Tool work in 1.10.8, no version of its own.

- **The history:** 968 events, 317 jobs (247 ok), 216 turns. No job records logged twice,
  and no job named `python.exe`.
  - **A hard freeze recovered:** the MacBook's "Save Manager source--wii" (10-17 16:04) left
    nothing answering. Its dispatcher kept the Wii until someone reset it 408 s later, then
    saved its data.
  - **A stuck app's agent answered:** a Wii64 job (10-18 15:19) got its status, log and
    screen saved.
  - 14 hand-overs, median 1 ms.
- **Found and fixed:**
  - **A lease expired under a running job** (10-17 14:29). This PC took it at 12:57, waited
    43 min for HBC, then ran a 49-minute job. The server saw no renewal for over 60 s
    somewhere in the job, so another workstation could have been given the Wii mid-run.
    The renew thread caught only network errors, silently; any other exception ended it.
    - It now catches everything and logs a failing streak past 20 s.
    - The job's record keeps the longest gap (`lease_gap_s`, `lease_lost`).
    - The errors page and `report` flag a gap over 30 s.
  - **The Wii was used outside the queue.** 3 of the 4 waits for HBC over 5 min (41, 13 and
    7 min) came right after a queue job had left the Wii in HBC. A turn that finds the Wii
    busy anyway logs an `outside` event, with what it found and who last left the Wii in HBC
    (`released_by`, from the server). The errors page and `report` show it.
    - HBC doesn't record who sends an app. Adding the sender's address to `HBCS` would name
      them: a channel change, for when it's wanted.
  - **The turn named its first job.** In a stacked turn, the server and monitor said "held
    for" the turn's first job while another ran. The running job's name and id
    (`holder.job`) now go with each renew.
  - **Commands that weren't there:** three jobs failed in under 6 s (exit 9009, 127). `add`
    now refuses a program it can't find (`--no-check` to override).
  - **A slow return after a busy spell:** once HBC answered again, `wait_idle` kept probing
    at the busy pace (15 s), so the quiet period took 30 s instead of 20. While testing that,
    a crash in the new busy-time accounting was caught before it shipped.
- **Not the queue's to fix:**
  - No job used `--collect` all week. The agents that need it work in other repositories and
    read their own instructions.
  - 59 failures were the agents' own tests (the MacBook's Save Manager check alone failed
    15 times).
  - Two timeouts, one at the 1,800 s default: a job that long should set `--timeout`.
- **Checked:**
  - Tests for: the renew thread surviving errors other than network errors; outside use on
    a real dispatcher; the turn named after its running job; `add` refusing a missing
    command; both warnings on the errors page and in `report`.
  - 103 tests on Windows and 67 on Linux pass.

### 1.10.7: the agent leaves the app's MEM2 alone

- From USB Loader GX's handoff (2026-10-14), checked against the code: of
  HBC's MEM2 records at 0x91800000-0x91801140, the crash block and kept log
  were already written only as an app stops, but `HBCN` (`hbc.py log`)
  wrote the log target's 32-byte copy at 0x91800000 while it ran. Apps with
  a MEM2 heap (GX, WiiXplorer) split it around the 4,416 bytes for that:
  GX's largest free block fell from 46.6 MB to 28.4 MB. Now `HBCN` writes
  only the low-memory block, and `lastlog_write()`, which every way out of
  the app reaches (exit, exception, fatal, hang, abort, stack overflow,
  power), copies it to MEM2 first. The records were not moved to the top
  of MEM2: libogc's start-up takes that (the IPC heap) before HBC could
  read them.
- The agent's lasting memory (12 KiB and 4 KiB thread stacks, the 6 KiB
  stub copy) comes from one block taken from the top of the MEM2 arena
  (`agent_lasting()`), as libogc's WPAD and network buffers are, not from
  malloc() in the middle of the app's heap; from the heap only when the
  app took the whole arena. Kept for the run: a stop and a new init reuse
  it, except a slice whose thread a stop abandoned. The transfer and
  overlay buffers still come from malloc() and are freed after each use:
  reserving their peak (560 KiB to 2.4 MB) would cost apps more than the
  room they leave at the top.
- `mem1_free`/`mem2_free` in HBCS were the SYS arena's unclaimed size, not
  an app heap's free memory (GX read 1.7 MB with most of its heap free):
  renamed `arena1_free`/`arena2_free`, in HBC and the agent. The handoff's
  callback for an app heap's figures is left out until an app needs it.
- CI had failed since 1.9.9, two ways. The devkitPPC job's install step
  re-synced devkitPro's package lists (`dkp-pacman -Sy`) for libraries the
  `devkitpro/devkitppc` image already has, and pkg.devkitpro.org sometimes
  answers GitHub's runners 403; it now fetches only what `dkp-pacman -T`
  says is missing. And `test_wiibench`'s monitor test failed every time
  the tests ran: `monitor.sh` killed its snapshot coprocess on quit and on
  restart without waiting for it, which in the CI container (whose first
  process reaps nothing) left a zombie the test saw as alive. It now waits
  for it (`snap_stop`).
- `tests/wii_agent_mem2.py` on the bench Wii, libogc 3 and libogc2 builds:
  the app's pattern over the records untouched while HBCN changed the
  target, HBC had the target after the app stopped, the block at
  0x933b1740 from the arena's top.

### 1.10.6: file times and flags, and a sync that need not read

- The file requests are HBC-Reborn's own (1.2.0; upstream HBC has only
  Wiiload), and every client is in projects maintained here, so the
  listing changed in place: op `L` answers `#list 2` and then
  `<d|f> <size> <mtime> <flags> <name>` per entry; op `S` gives one path's
  line; HBC's and the agent's HBCS carry `time`, the Wii's clock. The old
  format is gone: `hbc.py` says to update an HBC or agent that still sends
  it. Its Python `list_dir()` still returns `(type, size, name)` for
  scripts that unpack three (WiiStation's bench scripts); `list_entries()`
  and `stat_remote()` give the rest.
- What the FAT layers can do (from the libraries' symbols): libogc 3's
  FatFs and libogc2's libfat fill `st_mtime` and the write bits (FAT's
  read-only attribute); libogc 1.8.23 uses libogc2's libfat. None sets a
  time through the devoptab (`utimes_r` is not filled in; FatFs has
  `f_utime`, but behind dvm's volume, not a path). So an upload's time is
  when the Wii wrote it, not the PC file's; hidden and system attributes
  are not exposed by `stat`.
- `hbc.py sync` uses it: a same-size file whose Wii time, lined up with the
  PC's clock through HBCS `time` (FAT stores local time without a zone),
  is at least 2 s (FAT's step) after the local file's change is taken as
  the same without the Wii reading it; otherwise the CRC-32s decide, so a
  wrong Wii clock only costs reading. `--checksum` compares every same-size
  file, for a Wii copy changed on the Wii since at the same size (which the
  date rule would keep). A daylight saving change between the upload and
  the sync shifts the line-up by an hour.
- `hbc.py ls` shows dates and read-only, `--json` every field; `hbc.py stat`.
- `tests/wii_file_meta.py` on the bench Wii: a new file's time matched the
  Wii's clock (+0 s) from HBC, the libogc 3 agent and the libogc2 agent;
  a re-sync of a folder with a 50 MB file read nothing (0.1 s).

### 1.10.5: sync with big files, and the agent's stack figure

- `hbc.py sync` timed out on a folder holding large files that were already
  on the Wii at the same size: the Wii reads and CRCs the whole file before
  it answers op `C`, and the request had the default 15 s timeout (a
  400-700 MB disc image takes far longer at SD speed). The first such file
  ended the sync. Now the checksum's timeout grows with the file's size (15
  s plus 1 MiB/s, below a slow SD card's rate), the local CRC is read 1 MiB
  at a time instead of loading the file, and a file that fails is reported
  and skipped: the sync carries on, lists the failures at the end and
  exits non-zero.
- The agent's `agent_stack_used` read about 12,280 of 12,288 bytes since
  1.10.1: the rewrite of the thread start in `hbc_agent_init()` (f0fe12d)
  dropped the `memset(stack, 0, AGENT_STACK)` that `stack_used()` depends on
  (it finds the lowest non-zero byte). Put back; the real use is about
  1.6 KB.
- Future work, full directory and file metadata: done in 1.10.6, below.

### 1.10.4: Restart network, and file errors that say what happened

- HOME > HBC > Restart network: the loader's TCP thread closes its
  listening socket, `net_deinit()`s and starts the network again, as at
  start-up. The menu sets a flag that `loader_signal_threads()` hands to
  the thread on the next frame it is free (it holds its lock while it
  works). HBCS now carries `overlay_ui` from HBC too, so
  `tests/wii_net_restart.py` checks the focus before every A, and
  `net_starts` (times the network came up), which the test waits on: a
  restart can be over between two polls, so a gap in answers proved
  nothing (one run saw none).
- Uploads too big for the card's free space are refused with `ENOSPC`
  before any data is read (`statvfs`), and a framed upload stops receiving
  as soon as its write worker fails, instead of taking the whole transfer
  first. `hbc.py` checks for the Wii's answer before each frame, since the
  Wii closes after refusing and the reset would drop the reply. Errors from
  the Wii name the remote path, and a missing local path says it is one.
  `tests/wii_file_errors.py` on the bench Wii (SD 0.2 GB free): a 400 MB
  `.img` refused in about 0.5 s, from HBC and from an app's agent, nothing
  left behind; a missing folder and a missing local file reported as such.
- The on-screen transfer line with a name too long for it: each start picks
  scroll (round, with a gap), bounce (end to end, resting at each) or
  truncate ("...") at random, HBCS `transfer_style`. The name's room is
  measured once, then narrowed per frame while the line does not fit (wide
  letters further along the name), so the percentage always shows; windows
  move by whole UTF-8 characters. The word stays at the left and the
  percentage at the right (drawn on their own); the name moves between.
  Time-based: a second still at the start of each name (both moving
  styles), a second at each end of a bounce, a step each 133 ms. A bounce
  travels to where the name's end fits, measured once: the first version
  took its travel from the narrowed window, so the way back jumped, and
  then stopped short of the last letters (".i" for ".img"). `HBCT` /
  `hbc.py transfer-style` forces a style for tests (the packed DOL's
  argv goes to its stub, so a launch argument could not). Checked in
  Dolphin per style: a strip every 8 frames through a 120 MB upload with
  a 46-character name.
- Open: a Wiiload upload that arrives while HBC's HOME overlay is open is
  accepted by the TCP thread, which then waits for the app list to load it;
  until someone closes the overlay, HBC answers nothing (not even the
  `hbc.py key h` that would close it). Seen when a failed bench test sent
  agent_app with the overlay still open; the bench Wii needed a person.
  Trigger to fix: any unattended use that can upload while HOME is open.
  The likely fix: refuse the upload (or close the overlay) when it
  handshakes while the overlay runs.

### 1.10.3: transfers on screen, volatile interrupt flags, a steadier list reload

- An audit of state shared between threads, callbacks and interrupts:
  `main.c`'s `should_exit`, `shutdown` and `gdb`, set by the Reset and
  Power callbacks in interrupt context and polled by the main loop, are now
  `volatile` (the loop's calls happened to force a reload). Everything else
  is volatile already, guarded by `_CPU_ISR_Disable` (a compiler barrier in
  libogc 3 and libogc2), waited on through a mutex or condition variable,
  read through a call from another file, or used by one context only.
- While `hbc.py` moves a file, every screen shows it at the bottom right,
  left of the network icons: "Receiving boot.dol 42%", "Sending
  save.dat 7%" (`devfile_progress()`, drawn in `view_plot()`; devstream
  now counts the file's bytes, `raw`, beside the wire's). Checked in
  Dolphin with frames dumped during a 20 MB put and get.
- The app list's reload after uploads waited for one second without a
  finished file request, so a single slow upload (the bench's Wi-Fi: 12
  files in 2.5 s) could reload mid-burst. It now also waits while a file
  request is open, and the quiet second starts when one ends.

#### The bench queue: a manage page, jobs that say whose they are, data after a freeze

Tool work in 1.10.3, no version of its own.

- **Asked for:** a way to clear out stuck jobs that an agent did not recognize as its own; why
  so many errors; and data from a run that froze the Wii hard enough to need a power-off.
- **Errors, 10-09 to 10-12** (121 jobs, 22 failed):
  - **Natural (the bench finding things):** WiiStation's crash on Quit, the libogc3 crash
    and hang tests, the channel's "burst" check, the freeze triage that found a stale
    `boot.dol`, the MacBook's experiments.
  - **Agent mistakes the tools now catch earlier or name better:** a 400 MB `.img` to a full
    SD card, a missing local path, and listings of SD folders that don't exist.
  - **The queue's own:**
    - jobs named `python.exe`;
    - each of this PC's job records logged twice (a kept connection's retry repeated the
      post);
    - no way to clear a stuck queue;
    - a freeze losing the run's data.
- **Names:** a job given no `--name` is named after its script and first argument
  (`wii_lab.py fmt_hw`, `hardware_run.sh gpu_survey`), not its interpreter. Every one of
  WiiStation's jobs on 10-12 was `python.exe`.
- **Records once:** the server logs a job's record once, whatever the retries.
- **Cancel and stop, anywhere:**
  - `cancel ID...`, `--agent`, `--host` and `--all`, and `stop ID`.
  - A job on another workstation goes through the lease server (`/request`). The server
    drops a cancelled job from every list at once, and hands the request to the owning
    workstation's dispatcher with its next acquire or renew (a long poll answers at once).
    The dispatcher cancels it and acknowledges.
  - A cancelled job leaves a record with exit `cancelled`, so the agent waiting on it hears
    so. It is neither a failure nor the Wii's last job.
  - A stop file ends a running job like a timeout (`stopped`).
- **The manage page:** the monitor's page 5 lists every job running, waiting or queued on
  every workstation, with agent and project folder. Click or `j`/`k`/arrows to select; `c`,
  `a`, `w`, `X` and `g` act, each after `y`.
- **Freezes:** after a job that left the Wii out of HBC:
  - **The app's agent answers:** status, the app's output so far and the screen are saved in
    `runs/ID/freeze/agent/`, and `--collect` fetches through the agent. On 10-09 16:40
    WiiStation's agent answered while its app was stuck, and nothing was kept.
  - **Nothing answers:** the dispatcher keeps the Wii and waits for HBC (a reset or a power
    cycle). It then saves `freeze/after/` (HBC's kept log and crash report, kept by a
    RESET, lost by a power cut) and collects the job's Wii paths, before any other job.
    `stop ID` or `g` gives the wait up.
  - The README says what an app must do for its data to survive a power cut: stream it to
    the PC, flush it to SD.
- **Checked:**
  - Tests for the server's relay (owner, acknowledgement, cancels kept off lists) and for
    names.
  - End to end: a cancel from this workstation reached the other's dispatcher, its `wait`
    returned "cancelled"; `cancel --all`; `stop` of a running job from another workstation.
  - A stuck app's agent saving `freeze/agent/` and serving `--collect`.
  - The wait for a frozen Wii, saving and then given up.
  - The live manage page under a pseudo-terminal: select, cancel with `y`, `X` declined.
  - 94 tests on Windows and 62 on Linux pass.

### 1.10.2: a crash inside the agent's crash hook froze the Wii

- USB Loader GX in Dolphin (2026-10-09, MMU on): its ThreadedTask thread
  overran its stack, and instead of libogc's crash screen the emulated Wii
  froze with PC in the DSI vector (0x34c), SP near 0x80000070 and
  translation off. The crash block held one DSI in `safety_on_death`
  (`lwz r6,0(r10)`, DAR 0x398, so `stk_lo` read 0x388), frames all
  `agent_panic` and `PPCExcptDefaultHandler`.
- Why it recursed: a fault inside an exception handler arrives with
  MSR[RI] clear. libogc 3's `PPCExcptEntryGeneral` then enters
  `PPCExcptDefaultHandler` with cr0 equal, which keeps the faulting r1
  instead of loading `__ppc_excpt_sp`, and calls `PPCExcptCurPanicFn`, the
  agent's hook, again. A hook that faults every time recurses down from
  the exception stack (0x8110c780 in GX) through the main stack, `.bss`
  and the vectors. libogc2 and 1.8.23 send such a fault to their own
  `default_exceptionhandler` instead, so they show a crash screen.
- Why `stk_lo` was 0x388: not `__ppc_main_sp`. In GX's `boot.elf` it is
  libogc 3's weak `.sdata` word (0x81068394), 0x81108780 = `s_mainStack`
  + 0x20000 at link, never written after; `stack_guard_init()` reads it
  correctly (`lwz` from 0x81068394 in the disassembly) and rejects any
  `stk_lo` under 0x80000000, and only it and `safety_stop()` store
  `stk_lo` (0x8107a444). The recursion that reached 0x80000088 ran its
  frames over 0x8107a444 on the way; the crash block, rewritten at every
  level, kept only the last, which read the frame word left there. The
  fault that started the recursion is not recoverable from that block.
- Fix (`agent.c`, `safety.c`): the hook records the app's crash once. A
  fault inside it notes `agent crash hook faulted: pc ... dar ...` in the
  block's reason and the log, writes the kept log if it had not been,
  puts the first DAR and DSISR back, and hands libogc the app's own
  exception frame (still on the stack above); a fault in that too goes to
  `__reload()`. What the hook reads is checked first (`agent_ram()`): the
  markers' addresses, each thread record, the stub copy, the app name, the
  stack walk's saved LR. `hbc.py crash` shows an exception's reason.
- `tests/dolphin_ogc_crash.py` now takes libogc 3 too, and `--bad-guard`
  (`stk_lo` = 0x388 before the crash) and `--hook-fault`
  (`safety_on_death`'s first instruction a load from 0x388). Against
  1.10.1's agent both froze: `--bad-guard` with PC 0x34c and SP 0x80000070,
  as in GX. Now all three runs pass on libogc 3 and on libogc2 (where
  `--hook-fault` shows libogc2's screen for the hook's fault, the app's
  crash in the block). The libogc 1.8.23 agent builds; its test app needs
  a libfat this machine does not have.
- USB Loader GX needs no change: it uses libogc 3's default 128 KiB main
  stack, which is what the agent assumes without `main_stack_size`.

#### The bench queue: each job's data kept, and jobs in the order received

Tool work in 1.10.2, no version of its own.

- **Asked for:** agents throwing as many runs at the Wii as they can, never losing a run's
  data, fetching it whenever, and runs going in the order received, stacked only when
  received one after the other.
- **Data lost before this:**
  - 16 of the last 200 jobs (vbagx) wrote to one fixed folder, `.bench/out`, so each run
    overwrote the last.
  - WiiStation fetched SD logs with a separate `collect_after_failure` job, which any run
    getting the Wii in between could overwrite first.
- **Each job's data:**
  - Every job gets `runs/<id>/` (`$WII_BENCH_OUT`).
  - `add --collect PATH` copies `sd:/...` or `usb:/...` from the Wii (a file or a folder,
    through `hbc.py get`), or a path on the workstation. It runs after the job, after the
    HBC check and before the next job gets the Wii.
  - A path that can't be collected is in the record's `collected` and on the monitor's
    errors page.
  - `wiibench.py runs [--mine|--agent|--name]` lists jobs, live and archived;
    `results ID [--to DIR]` shows and copies one.
  - **Rollover:** a job past 14 days, or the oldest while the live runs pass 20 GB, goes
    into `runs/archive/YYYY-MM/<id>.tar.gz` (record, log, run folder), with `index.jsonl`.
    The oldest months go past 100 GB. The limits are `WII_BENCH_KEEP_DAYS`,
    `WII_BENCH_RUNS_MAX_GB` and `WII_BENCH_ARCHIVE_MAX_GB`.
  - `hbc.py` takes `HBC_PORT` (tests only).
- **Receive order:**
  - The lease server numbers each job when it first hears of it (`add` tells it at once)
    and grants the Wii to the waiter with the earliest-received job.
  - A renew answers `next_is_mine`. The holder keeps its turn while its next job is the next
    received (stacked), and lets go when another workstation's came in first. This replaces
    1.8.8-1.10.1's one job each in turn.
  - Between stacked jobs: one HBC answer for the same agent, 2 s for another.
  - `status` and the monitor list waiters and every queued job in that order.
  - An old server can't say whose job is next, and the old rule applies; an old dispatcher
    counts as one job, received when it joined the line.
- **Checked:**
  - Two runs writing the same local folder and the same SD file each kept their own, with a
    whole SD folder too. A missing path was recorded as such.
  - `runs`, `results --to`, rollover into the archive, and `results --to` from it.
  - With two workstations, a1, a2, then a rival: a1, a2 stacked, then the rival. With a1,
    then b1 on another workstation, then a2: three turns, a1, b1, a2.
  - 85 tests on Windows and 54 on Linux pass.
  - **On the bench Wii,** through the homeserver's older server: a job wrote
    `sd:/hbcbench/collect.txt`, and `--collect` fetched it into `runs/<id>/wii/` with the
    job's content, beside a local file in `local/`; `results` listed both, and a second job
    removed the file.
  - The old server can't number jobs, so that second job went one-each-in-turn behind
    WiiStation's 12-minute upload. The rebuilt server orders them as received, which would
    have run that upload (received 12:00) before both (12:05).
- **Fixed while testing:**
  - A workstation whose dispatcher was still starting lost its place to a job received
    later. The Wii now waits up to 10 s (`RESERVE`) for the workstation with the
    earliest-received job, while its queue is fresh.
  - `runs` orders jobs that finish in the same second by when their records were written.

### 1.10.1: hbc_agent_stop() and hbc_agent_listen()

- For apps that leave other than through `exit()` (USB Loader GX boots
  games after `IOS_ReloadIOS`) and apps that run their own Wiiload server
  for a while (its Homebrew Browser). Before, the agent restarted the
  network on the new IOS as soon as its socket failed, the hang watchdog
  could fire during a long boot, and the retrace callback, stack guard,
  crash handler, Reset/Power, reset function and stdout/stderr pointed at
  code the next program does not have.
- `hbc_agent_stop()`: each agent thread runs while the generation it
  started with is current; the stop moves on, waits up to a second for both
  threads (the listener and the monitor), joins them, then puts every hook
  back unless the app replaced it since. A thread blocked in IOS past that
  is abandoned: it touches nothing when it wakes, and the stop closes the
  listening socket itself. A failed `hbc_agent_init()` now undoes itself
  through the same path, and `hbc_agent_init()` works again after a stop.
  The atexit handlers are registered once and do nothing after a stop.
- `hbc_agent_listen(bool)` is the overlay's "hbc.py connection" switch for
  the app; it returns once the port is free, aborting a transfer with
  `devfile_abort()` (HBC's own path before a launch).
- alloc_wrap.c is still pulled in only by `--wrap=malloc`: safety.c's
  reference to it stays weak, so USB Loader GX's own `__wrap_*` link.
- Built against libogc 3, libogc2 and libogc 1.8.23 with `-Werror`; not
  against libogc 2.11 here (no Docker for `devkitpro/devkitppc:20250527`),
  whose code path is libogc2's and 1.8.23's (`_exceptionhandlertable`).
- `tests/wii_agent_stop.py` on the bench Wii: stop then IOS reload and
  12 s past the 5 s watchdog (no report, no network start), re-init,
  the app's own server on 4299, a crash after the stop (libogc's screen,
  no report).

### 1.10.0: a Custom app order, and no page cycling while files arrive

- The pages cycling during network traffic: every file request under
  `<device>:/apps/<name>/` queued that app for a reload, and each reload
  was `browser_gen_view(BA_REFRESH, app)`: the full slide transition (about
  half a second) to the changed app's page. `put -r` or `sync` of an app (or
  a queue job installing one, like Nintendont's on the bench) slid once per
  file, back and forth when the user was on another page. Now only the app
  folder and the five files the list reads count, they are taken once file
  requests have been quiet for 1 s, and the list is rebuilt in place
  (`BA_RELOAD`: no slide, the selected app kept, else the page kept).
  HBCS `app_list` counts slides and reloads; `tests/wii_app_list.py`
  checks a 12-file burst gives one reload and no slide.
- An app's dialog opens on Load, not Back, so A, A runs an app from the
  D-pad. The pointer still clears the highlight as before (kept on
  purpose: a person pointing picks the button).
- HBC stopped answering hbc.py while an app's dialog or About was open
  (only the list and, since 1.9.9, Options signalled the loader threads);
  every view does now.
- `tests/dolphin_meta.py` runs every `meta.xml` option through the menu in
  Dolphin with a scripted Wii Remote, launching agent_app in its new
  `meta` mode (prints argv, IOS, AHBPROT). Two findings on the way:
  `tools/msgboard/dsu_pad.py`'s server thread died on Windows' UDP
  "connection reset" (an ICMP port unreachable reported on the next
  receive), so pad input stopped after Dolphin's client restarted; it now
  carries on, and takes steps on stdin so a test keeps one server. And the
  msgboard profile cannot return from an app to HBC, so the test uses
  dolphin_smoke's throwaway profile with a pad mapping written into it.
- Custom sort: `<sort_id>` in `meta.xml` (a whole number), lowest first,
  apps without one after by name. `settings.xml` stores it as sort order 2;
  an older HBC reading 2 falls back to Name. Options shows three tiny sort
  buttons. `hbc.py order` reads and writes the ids.
- Checked in Dolphin (`tests/dolphin_meta.py`) and on the bench Wii
  (`tests/wii_app_list.py`).

#### The bench queue's first five days of logs (2026-10-02 to 10-07)

Tool work in 1.10.0, no version of its own.

- **What the history held:** 717 events, 214 turns and 74 jobs from this PC (60 ok, 14
  failed). Every job reached the server's history, and every one has its HBC check.
  - All 74 checks found HBC back, recording 1.9.3 to 1.10.0 as the channel was updated.
    70 found it there as the job ended. Three "DEV Info pages" jobs left the Wii in an app
    for 50-65 s after they failed.
  - All 215 releases left the Wii in HBC. No lease expired.
  - No dispatcher crash since the 1.9.4 fix, and no lease lost or server unreachable.
  - The 14 failures were this PC's test scripts on 10-04, during 1.9.5-1.10.0, not the queue.
- **Found:**
  - **The MacBook held the lease 1,457 min against this PC's 121.** Two of its turns took
    10.4 h (10-03 15:02 to 10-04 01:28, "wiixplorer-memory-test-settings-restoration") and
    5.9 h ("WiiXplorer v0.1.2 FTP retry on 2121"), longer than any job timeout. A dispatcher
    takes the lease and then waits for HBC to be idle, holding the lease all the while. So
    these were most likely a Wii left in WiiXplorer, with the MacBook's next job waiting for
    it. Nobody else queued meanwhile, so nobody was held up, but nothing showed it.
  - **The MacBook's dispatcher still predates 1.9.1:** 164 turns and no job records.
  - **`report` said "nothing went wrong" over 14 failed jobs**, and still printed hand-overs
    as "0.0 s".
- **Fixed:**
  - **A turn says what it's doing:** waiting for HBC (and why), running a job, or checking
    HBC after it.
    - The holder sends the phase with each renew, and at once when it changes; the server
      shows it in `status` (`phase`, `phase_s`).
    - The time a turn waits for HBC is added up into the release (`hbc_wait_s`).
    - The monitor's status line shows "HOST has it for JOB (5m): waiting for HBC: Wii busy
      or off for 4m" on every workstation.
    - Its errors page warns after 5 min (`Wii not in HBC`), and so do the history and
      `report`, for a wait over 10 min.
    - The lease is still held while waiting: the next workstation would only wait on the
      same Wii.
  - **`report` lists** failed jobs, long waits for HBC, and workstations taking turns
    without job records, and prints hand-overs in ms.
- **Checked:** tests for the phase accounting (two waiting phases add up, the job does not),
  the monitor's warning and `report`'s lines. 77 tests on Windows and 46 on Linux pass.

#### Every workstation's queue on the lease server

Tool work in 1.10.0, no version of its own.

- **Asked:** "why can't I see the full queue that is waiting on the server?" A dispatcher asks
  for the lease for its next job only. So the server knew one job per workstation, and a
  workstation with eight queued looked like one waiter; the monitor's QUEUED HERE was this
  workstation's folder.
- **Now:**
  - Each dispatcher sends its whole queue (50 jobs at most, and the total) with every acquire,
    renew and release. `add` and `cancel` send it at once (`/queue`), and so does the job's
    start, through the "running" phase.
  - The server keeps one list per workstation and state folder (`qid`). It drops a list not
    refreshed for 60 s, and ignores one older than what it has (a long poll's request from
    20 s ago). It sends each job's age, not its time.
  - `status` lists every workstation's queue.
  - The monitor's QUEUED, EVERY WORKSTATION shows them in the order the turns will come
    (`queue_order`), this workstation's in cyan:
    - the workstations in line take one job each, in line order;
    - a workstation with more queued goes to the back after each job;
    - the holder rejoins at the back after its job;
    - a pre-1.10.0 waiter counts as its one job.
- **Checked:**
  - Tests for the server's lists (newer wins, stale ignored, dropped on release and after
    60 s) and the turn order, including an old dispatcher in line.
  - End to end: `add` on one workstation shows in another's snapshot and `status` at once,
    and `cancel` takes it away.
  - 80 tests on Windows and 49 on Linux pass.

#### A queue that looked stuck (2026-10-08)

- **What happened:** the jobs queued here at 11:29 and 11:31 sat for 25 min. This PC's
  dispatcher had taken the lease at 11:19 and was waiting for HBC. The Wii answered ping, but
  not on TCP 4299: an app had exited to the original HBC instead of hbc-reborn. The queue had
  not touched the Wii since 02:28, when HBC came back after its last job.
- **What the queue got wrong, now fixed:**
  - **The turn's job had been cancelled.** The turn waited on under the gone job's name, and
    with nothing left queued it would have held the lease for ever.
    - `wait_idle` now follows the head of the queue, renaming the turn on the server at once
      (`Turn.rename`).
    - It gives the turn up when nothing is left.
    - It checks the local queue (not the Wii) every second between its 15 s probes of a
      busy Wii.
  - **The monitor said "dispatcher: idle"** because the state named the cancelled job. Now
    only a "running" state goes stale with its job.
  - **"waiting for HBC: Wii busy or off for 0s".** The container's rebuild made the holder
    reclaim its lease, and a reclaim didn't start the phase clock, so the 5-minute warning
    never came. A reclaim now starts it, and an unknown duration is left blank instead of 0.
  - **The warning says what to look for:** an app, a crash screen, the Wii Menu after a power
    cycle, or another HBC. When HBC is back but is a stock HBC (`hbc_version` "stock HBC"),
    the monitor warns that hbc.py jobs will fail there.
  - Windows' exit code 4294967295 now reads as -1.
- **Checked:**
  - A dispatcher waiting on a busy fake HBC: cancelling its turn's job renamed the turn to
    the next one, and cancelling that one gave the lease up within 5 s, with the wait
    counted in `hbc_wait_s`.
  - A reclaim mid-wait counted the phase.
  - A stock HBC after a job was warned about.
  - 83 tests on Windows and 52 on Linux pass. The homeserver needs a rebuild for the reclaim
    fix.

### 1.9.8: two USB drives, GPT disks, cached reads of the frame

- libogc's USB storage driver serves one drive: its command buffer,
  transfer mode, timeout and "mounted" flag are globals, so a second handle
  races the first on the buffer and unplugging either unmounts both (the
  same in libogc 3.1 and libogc2; the libogc 3.1 source is now in
  `C:\projects\libogc3-src`, in the local code graph). HBC has its own,
  `usbmsd.c`, adapted from libogc 3.1's (zlib) with all of that per drive,
  serving `usb` and a new `usb2` (device 4, so `settings.xml`'s stored
  preference keeps its meaning). The Options dialog shows five device
  buttons, in the theme's tiny buttons.
- `hbc.py` file requests mount `sd`, `usb` or `usb2` on demand when it is
  not the app list's device (`devfile_mount_hook`).
- libogc 3's FAT (FatFs behind dvm) finds partitions only in an MBR. A GPT
  disk now has each partition tried at its first sector, the EFI System
  Partition skipped (it mounted first, an empty 200 MB volume). HBCS
  `usb_drives` and `unmountable` say what a drive held when nothing mounted.
- Checked on the bench Wii (`tests/wii_usb2.py`) with a SanDisk Ultra Fit
  (MBR, 30.8 GB) and a Toshiba drive (GPT, 3.8 GB data partition): both
  listed, 1 MiB written and read back identical on each, at about 0.5 MB/s
  over Wi-Fi.
- Cached and uncached aliases: the frame HBCP sends (`hbc.py screen`) and
  the overlay's restore of an app framebuffer it drew over now go through
  the cached alias with one `DCFlushRange`, like the overlay's frozen frame in
  1.9.7; everything else that uses an uncached alias is a GPU or VI target
  the CPU does not read (HBC's framebuffer) or the crash screen. The loader
  flushes and invalidates what it copies before running it, and IOS
  buffers are flushed before and invalidated after.

### 1.9.7: the HOME overlay at 60 fps

- HBC's overlay drew a frame in 35 ms (28 fps at best; with a remote
  pointing, every frame is redrawn). HBC lends its framebuffers, so the
  overlay reads HBC's last frame in place, and it did so through the
  uncached address it got from `VIDEO_GetCurrentFramebuffer()`: 600 KB a
  frame, a bus transaction a byte. It now flushes that range once when it
  opens and reads through the cache (nothing writes the frame meanwhile).
  The background dim also goes a 32-bit pixel pair at a time instead of a
  byte.
- Animations stepped by frames drawn, so slow frames stretched and jerked
  them; they now step by sixtieths of a second elapsed (at most 8 at once).
- Measured on the bench Wii (`tests/wii_overlay_speed.py`,
  `tests/agent_cost.py`): HBC's overlay 35.7 ms a frame to 5.2 (3.2 ms the
  dim, 1.3 the menus), the in-app overlay 8.7 to 6.5; no pass of the loop
  over one frame (16.7 ms) in either. HBCS `overlay` now says where the
  time goes (`dim_us`, `draw_us`, `loop_max_us`, `input_max_us`,
  `slow_loops`).
- A report of the overlay being slow only on its first opening right after
  an app returns did not reproduce on the bench (no remote is connected
  there; `tests/wii_overlay_return.py`): each opening drew at the same speed,
  and the play log's NAND write had finished 1 s after HBC started. If it
  comes back, `hbc.py status` right after shows `input_max_us` and
  `slow_loops` for that opening.

### 1.9.6: the agent's safety tools, DEV > Info's hardware pages, `hbc.py hw`

Safety tools (`sdk/hbc_agent/safety.c`, on by default, opt-outs in
`no_safety`):

- Stack guard: the DABR watches the doubleword 1 KiB above the main
  thread's stack bottom (libogc 3's `__ppc_main_sp` less 128 KiB, or
  libogc2's and 1.x's `_thr_main->stack`), with marker words below it
  checked as the agent wakes. A hit is a `stack` crash. The libogc2/1.x
  exception entry clears the DABR first (its frame may cover the
  doubleword); libogc 3's saves to a fixed buffer in real mode. The DABR is
  cleared on every way out (crash, fatal, hang, abort, and a libogc reset
  function for `exit()` and `SYS_ResetSystem()`), so a program started
  after the app does not inherit it. Dolphin does not emulate the DABR, so
  there only the markers apply.
- `assert()` and `abort()` (weak overrides) report `assert` (file:line:
  expression) and `abort`, then return through the stub.
- Reset and Power: taken only from libogc's default handlers. libogc2 and
  1.x restore their default for NULL, so setting NULL twice reveals it;
  libogc 3 stores NULL and its defaults are a bare `blr`, which is how the
  agent recognises them. Reset exits like `HBCX`; Power runs `on_exit` and
  `SYS_ResetSystem(SYS_POWEROFF)`.
- SD and USB are unmounted from a libogc reset function: `exit()` calls
  `SYS_ResetSystem(SYS_SHUTDOWN)` after the app's `atexit` handlers (both
  libogc 3 and libogc2), so their saves are already written.
- Frame pacing from a chained post-retrace callback; the thread list (all
  three libogcs' thread tables, libogc 3's priorities mapped to the LWP
  scale) at the end of the kept log after any stop.
- Opt-in: `guard_reload_stub` (a 6 KiB copy, checked each second, put back
  at exit and on every crash path) and `track_memory`; with
  `-Wl,--wrap=malloc,...`, failed allocations (`alloc_wrap.c`, linked only
  then).

Measured on the bench Wii (`tests/agent_cost.py`, 20 s at 60 fps): the
defaults cost 0.1 us a second for the checks and 0.06 us a frame for the
callback, 0.0004% of the CPU and no memory; with both opt-ins, 5.85 us per
check, 0.0009%, and 10 KiB held (the stub copy and the monitor thread's
stack). Code: safety.o 10.7 KB, info.o 10.8 KB (1.3 KB bss; the 7 KiB
network settings buffer is allocated only while read), alloc_wrap.o 1 KB.
`tests/wii_agent_safety.py` passed all eight checks on the bench Wii: assert,
abort, a deadlock (both threads shown on a mutex), a failed 1.5 GB malloc,
60/30/0 fps, the stub guard (a change past HBC's stub seen and put back,
the app still returning to HBC), Reset, and a runaway recursion stopped by
the breakpoint.

DEV > Info (`sdk/hbc_agent/info.c`): five pages gathered in a thread when one
opens, and the same over `HBCH` (`hbc.py hw`). Found while checking them:

- Dolphin keeps one object behind every handle to a device, so closing a
  handle of our own on `/dev/net/ip/top` stopped the app's network (HBC
  stopped answering). The agent keeps its one handle open; IOS drops it when
  it reloads for the next program.
- IOS's USB devices hold one device-change request each; asking IOS
  directly and closing could take the app's, and on a real Wii the close
  blocked. The USB page reads libogc's lists, starting libogc's USB in an
  app that had not.
- `config.dat`'s layout is Dolphin's `WiiNetConfig.h`: flags 1 wired, 2 DNS
  by DHCP, 4 IP by DHCP, 128 selected; SSID at 0x7c4 (WiiBrew's summary put
  it at 0x292). Interface option 0xb003 ends its DNS list with
  255.255.255.255, and buffers are cleared before each query (Dolphin reads
  an offset from the routing-table buffer).
- An app that never started ISFS read no `setting.txt`; the gatherer starts
  ISFS.

Checked on the bench Wii (`tests/wii_info.py`): `hw` from HBC and from
agent_app agree, and the overlay's five pages render. The overlay test runs
in agent_app only and checks the focus (HBCS `overlay_ui`) before every A
press: an earlier version pressed keys blind in HBC's own HOME menu, landed
on Launch BootMii, and sent the shared Wii to BootMii.

### 1.9.5: popups from the network close themselves

- An upload HBC could not use (not a Wii app, a broken transfer, a bad ZIP,
  no memory) left an error popup up until someone pressed A, and a ZIP sent
  over Wiiload waited on its Yes/No question: either stalled automated
  tests, the bench queue's included. Every popup an upload or the update
  check can raise now closes after 10 s (`show_message_timed`,
  `NET_POPUP_TIMEOUT_S`), counting down on its last button; a question
  closes as No. Any button press stops the count. Popups for what the user
  did on the Wii itself (delete) still wait.
- HBC records each upload's outcome for HBCS `upload` (protocol 6:
  `launched`, `installed`, `theme`, `declined`, or `error` with a code and
  the message shown). `hbc.py send` and `run` read it and fail at once with
  HBC's reason; HBCA's new flags let `send --yes` install a ZIP unasked.
- HBC did not answer the network while a popup was up, or during a long
  receive: its network thread only accepts when the menu loop wakes it.
  Both loops now wake it too.
- Checked on the bench Wii (`tests/wii_upload_popups.py`, this tree's DOL):
  a file that is no app failed `send` in 0.9 s with "This is not a valid
  Wii application" while the popup counted down; an unanswered ZIP was
  declined 23.9 s later (the first popup's 10 s, then its own 10 s); `--yes`
  installed it in 2.1 s without a question.

### 1.9.4: B backs out of every menu

- The message dialogs (OK, OK/Cancel, Yes/No: delete, install, update)
  ignored B. B now picks the last button, which every caller reads as
  "no": OK, Cancel or No.
- B is also the button that drags a dialog's text, and the app dialog only
  closed on B when the pointer was not on the text: pointing at the middle
  of the dialog and pressing B did nothing. B now backs out when it is let
  go without the pointer moving more than 16 px (`view_back`); a press that
  moved was a scroll. The options dialog and About already closed on B; the
  HOME overlay goes back one level on B (its button Test page, which tests
  B, leaves with + and - together).
- Checked in the Wii Menu Dolphin profile, offline (Wii64's Dolphin held
  TCP 4299), with a scripted remote (`dsu_pad.py` gained `down`/`up` for a
  held drag) and Dolphin's frame dump: B with the pointer on the text
  closed the app dialog; a B drag scrolled it and left it open; B on the
  delete confirmation, with the pointer on its text and with it off
  screen, answered No and kept the app.

#### The bench monitor in 1.9.4

Tool work only, in 1.9.4 with no version of its own.

- **`tools/wii-bench/monitor.sh`:** a live dashboard with four pages.
  - **queue:** who has the Wii, what's running and queued here, every
    workstation waiting, and the last jobs here.
  - **errors:** every problem in the window.
  - **history:** per workstation, plus the longest waits.
  - **log:** the end of `dispatcher.log`.
- **It never touches the Wii.** It reads only this workstation's queue files and
  `dispatcher.log`, and the lease server's status and history. One
  `wiibench.py snapshot` per refresh collects, sorts and filters, so no OS needs
  `jq`.
- **Problems it finds:**
  - failed, timed-out and not-started jobs, with their log's last line;
  - a Wii left out of HBC, both now and in the history;
  - an expired holder or waiter, `lease lost`, and the lease server unreachable;
  - a dispatcher traceback, queued jobs with no dispatcher, a Wii busy outside
    the queue for over 5 minutes;
  - a workstation taking turns without sending job records, meaning its
    dispatcher predates 1.9.1.
- **Drawing:** `lib/monitor_lib.sh` is the AI server's library, copied unchanged
  (`686fcbb`). It provides the paging, clickable tabs, click-to-sort headers,
  click-to-copy cells, in-place redraw and `--once`.
- **Server status now reports durations:** `held_s`, `waited_s`, and `ago_s` for a
  Wii left out of HBC. The container's clock is UTC, so its own time strings
  misled readers in other zones.
- **Found on first use** (2026-10-02, the last 24 h): four Wii64 jobs that failed,
  three with a syntax error in `.dev/hardware_run.sh` and one with
  `python: command not found`; and the MacBook taking turns with a pre-1.9.1
  dispatcher.
- **Fixed along the way:**
  - `dispatcher.state` keeps naming the dispatcher's last job, so "running" stayed
    on screen after the job ended. A state whose job is gone now reads as idle.
  - "Recent jobs" is ordered by when each job finished, not by file modification
    time.
- **Checked:**
  - Snapshot tests: each kind of problem, sorting and filtering, no empty fields,
    the lease server's facts, and no connection to a fake HBC.
  - `monitor.sh --once` on every page, in Git Bash on Windows and in WSL Ubuntu.
    macOS's bash 3.2 gets a clear message instead.
  - Live in a Windows console: it refreshed and quit cleanly.
  - One test harness trap: under GNU `timeout` without `--foreground`, a
    dashboard can't read the terminal and stops at its first frame.
- **Too slow at first: about 4 s a frame on Git Bash.** Measured, then fixed:
  | Where | Before | After |
  | --- | --- | --- |
  | Drawing: the library's `tput cup` and `tput el` for every line, 23 ms a fork | about 2.1 s | 8 ms: one write of escape codes (`monitor_draw_frame` redefined in `monitor.sh`; the library stays verbatim) |
  | Building: a `$(...)` for every value, trimmed cell, heading and row, 14 ms a fork | about 1.1 s | 15 ms: `printf -v` and fork-free twins of the library's helpers |
  | Snapshot: two lookups of `homeserver.local` (220-250 ms each: Windows does not cache mDNS) and a new connection per call | about 0.7 s | 86 ms: one `snapshot --serve` coprocess, one lookup, a kept connection, only new history and changed job files |
  | Reading the snapshot: `read -t` on every line (select() on a Git Bash pipe is polled, about 1.6 ms a line) | about 0.4 s | the timeout on the first line only |
  On Linux a frame takes about 10 ms.
- **For every client, the dispatchers too:** `lease_call` looks the server up once per
  process (again after a failure) and keeps one connection per thread. A call took about
  250 ms from this PC and now takes 15 ms. The server speaks HTTP/1.1 and closes a kept
  connection after 60 s idle. A client that goes away is no longer a traceback in its log.
  An HTTP/1.0 server, before this, still works: the client sees it close and reconnects.
- **Checked end to end, and fixed:**
  - **The HBC check always read 0.0 s.** 51 of the first 52 checks said so. `hbc_back()`
    took the time before the probe that got the answer, so a first answer was 0.0 however
    long it took. It now measures to the answer and records the version that answered
    (`hbc_version`). The column reads `ok`, `Ns`, `LEFT` or `-`, and the status line says
    "HBC answered as JOB ended (2m ago)" instead of "back in HBC 0.0 s".
  - **The dispatcher crashed after a job** (13:22:45, found through the monitor's errors
    page). Windows refused to replace the job's record (`PermissionError [WinError 5]`)
    because another process had it open: a `wait` polling for it, or the monitor's snapshot.
    The job lost its HBC check and its history record, and the next `add` started a new
    dispatcher. Every rename, replace and unlink of a queue file now retries for up to 2 s
    on that error (`fs_retry`; a missing file still fails at once). A record that still
    can't be replaced is logged instead of killing the dispatcher. Reproduced with a reader
    holding the file open for 0.3 s.
  - A failed job's detail is the last line of its log that reads like an error.
    WiiStation's `exit 1` read "results: perf.log, ..."; it now reads "results incomplete:
    TimeoutError after 10 files".
  - A lease server that takes connections but doesn't answer stalled each refresh for
    15 s. Now it gets 2 s, isn't asked for history after a failure, and is left alone
    for 15 s.
  - Line wrap is off while the dashboard is up, so a line wider than the terminal is cut
    at the edge and doesn't push the rest down. The first status line fits its "last:"
    part to the width that's left.
  - Hand-overs are shown in milliseconds; "0.0 s" hid them. `--hours` and the refresh are
    validated, and only the last 512 KB of `dispatcher.log` is read.
  - **New test** (Linux and macOS): `monitor.sh` live in a pseudo-terminal. It checks
    pages by key and by a click on a tab, click-to-sort ascending and descending, a click
    on a job id copying it over OSC 52, a resize to 100 columns, a killed snapshot process
    replaced on the next refresh, and q putting the terminal back with no snapshot process
    left behind.

### 1.9.3: the play log test in CI's container

- 1.9.2's CI failed in the devkitPPC job's Tests step: the container's gcc 12
  rejected `strncpy` into the play log record's 6-byte ID in the host driver
  (`tests/cdblog_host/main.c`, `-Werror=stringop-truncation`). The ID is
  NUL-padded, not NUL-terminated, so the driver now copies it with a bounded
  `memcpy`. Reproduced and checked in `devkitpro/devkitppc:latest`: all 71
  tests pass. The host jobs (Windows, macOS, Linux) had passed.

### 1.9.2: bench jobs in a minimized window

- On Windows the bench dispatcher has no console, so each console job
  (python.exe) opened a window of its own over whatever the user was doing.
  Jobs now start minimized and without taking the focus (SW_SHOWMINNOACTIVE).

### 1.9.1: Wiiload uploads named for the play log by their sender

- A Wiiload upload's play log name was its first argument (1.8.9), then the
  agent's name or a `.dol`/`.elf` file name (1.9.0). An app without the agent
  sent as `boot.dol` was still "boot", and one whose first argument is a ROM
  was "Wiiload".
- **New request `HBCA` (protocol 5):** a sender names the next upload. HBC
  keeps the name for 2 minutes, for the upload that follows. `hbc.py
  send`/`run` sends the folder of an `apps/NAME/boot.dol`, else the file's
  name, or `--name NAME`. An HBC before 1.9.1 refuses `HBCA`, and `hbc.py`
  carries on without it.
- **The order:** the app's agent's own name, then the sender's (`HBCA`), then
  a `.dol`/`.elf` file name, then "Wiiload". An app's arguments are never its
  name, so the Message Board gets app names, not ROMs and options.
- **Checked:**
  - `test_hbc_tool.py`: `boot.dol` in `apps/Wii64` is "Wii64", `netblock.elf`
    is "netblock", and `--name` wins.
  - Dolphin (Wii Menu profile): `rtc_shift.dol` sent as `MyTool/boot.dol`
    with argument `0` logged as "MyTool".
- **`tools/msgboard`'s flow scripts** refuse to start while another Dolphin on
  this PC holds TCP 4299. One run had reached WiiStation's Dolphin, running a
  1.8.6 agent app, instead of its own (it stopped before sending anything).

#### The bench queue in 1.9.1: fair turns, no contact during runs, a history

Tool work only, in 1.9.1 with no version of its own.

- **Problem:** on 2026-10-01 the MacBook waited 20.5 min for the Wii, 10:37:17 to
  10:57:49. This PC's WiiStation agent chained three already-queued jobs while it
  waited, because 1.8.8's 10-minute chain cap is checked before a job starts. The
  last job (an 800 s `hprof` run) started at minute 6.6 and ended at minute 20.
  - Over 12 h this PC waited behind the Mac 8 times: a median of 6.3 min, 55 min
    in all. That was the Mac's own job lengths, so turns were fair otherwise.
  - Nobody could see the whole picture: the server kept no history, and SSH to
    the homeserver is refused.
- **No chaining while someone waits:** with another workstation in line, the
  turn ends after every job. A hand-over costs about 2 s since 1.8.8 (long poll,
  short idle wait). With nobody waiting, the 15 s hold for the agent's next `add`
  now lets go within about a second of someone joining. `CHAIN_MAX` is gone. On
  that morning's timeline the Mac would have waited 4.3 min.
- **The queue never contacts the Wii during a run.** Probes happen only between
  jobs, while the dispatcher holds the Wii.
  - `status` and `setup` used to send `HBCV`, and `HBCS` to an agent app, even
    during another workstation's run. Now they report the job or lease holder
    instead (`in use: HOST has it for NAME (not probed)`).
  - A Wii that's busy between jobs (an app outside the queue) is probed every
    15 s instead of 5, and its app name is asked for once.
- **HBC's return checked after each job:** while still holding the Wii, the
  dispatcher waits up to 120 s for HBC.
  - It records `hbc_back_s`, or `wii_left` (`busy: <app> is running` / `busy or off`).
  - The release carries the Wii's state. The server keeps a Wii left out of HBC as
    `left`, shown in every `status` until a clean release, and the next holder does
    the full idle wait.
- **History:** the server logs joins, leaves, grants (time waited), releases
  (time held, the Wii's state), expiries, and each job's record, which its
  dispatcher posts to the new `/event`.
  - Storage: JSON lines in `history/events-YYYY-MM-DD.jsonl`, gzipped after 7 days.
    Each finished month is packed into `history/archive/YYYY-MM.tar`, kept forever
    by default (`KEEP_MONTHS`, `--keep-months`).
  - `GET /history?since=` reads across live, gzipped and archived files.
    `wiibench.py report [--hours|--days|--since] [--local] [--json]` summarizes it
    per workstation.
  - Each dispatcher also keeps its own jobs in `<state dir>/history`.
  - The compose file mounts `./history`, which git ignores.
- **Checked:**
  - 29 bench unit tests on Windows and in WSL Ubuntu, including a real dispatcher
    against a real lease server and a rival workstation: the rival got the Wii
    between two same-agent jobs, the hold let go within 3 s of the rival joining,
    and a Wii left in an agent app was recorded, released as such, and shown in
    `status`.
  - `status` made no connection to the fake HBC during a run, or while another
    workstation held the lease.
  - History rotation over 43 days into two monthly archives, retention, and
    reading back across all of them.
  - The image built and ran in WSL's Docker with `/data` mounted: events landed on
    the volume, `docker stop` took 0.44 s with exit 0, and the history survived a
    restart.
  - **On the bench Wii,** a second dispatcher with its own state folder ran a
    read-only `hbc.py status` job through the homeserver's lease server, which is
    still pre-1.9.1. It waited 5.3 min behind this PC's 1.8.8 dispatcher, which
    chained two queued WiiStation jobs while it waited: the old rule, once more.
    The lease came 1 s after WiiStation's last job and the job started 2 s later;
    it recorded `hbc_back_s` 0.0, and its local history and `report --local` read
    it back.
  - The old server refused the job record (HTTP 400) and has no `/history`; the
    dispatcher logged that and carried on. Both work once the container is rebuilt.

### 1.9.0: fatal reports, a hang watchdog and a kept log for every agent app

WiiStation had all three in its own code: it wrote the agent's crash block
itself under private codes (0x81-0x83, a guest PC and a vblank count in
`dar`/`dsisr`), and ran its own watchdog thread. Each is now general:
- **Crash block version 2** adds `kind` (exception, fatal, hang), `code` and
  `reason`. HBC still reads version 1 blocks, as exceptions; HBC before 1.9.0
  ignores version 2. `hbc.py` prints the app's code as a number and never
  interprets it; an app puts its own details in the reason.
- **`hbc_agent_fatal(code, fmt, ...)`** records the caller's backtrace and the
  reason (also printed, so it ends the kept log), then calls `exit(0)`.
  `hbc_agent_fatal_now()` goes straight to `__reload()`, for state that
  cannot be trusted.
- **`hbc_agent_alive()`** arms a watchdog (`hang_s`, default 60):
  - It is a thread at the highest priority with a 4 KiB stack, created on
    the first call. A 1 Hz alarm alone could not say where the app hung: in
    an interrupt no thread is switched out, so the stuck thread's saved
    registers would be stale.
  - On a hang it records the stuck thread's registers (libogc 3: its
    `KThread` context; libogc2 and 1.x: its `lwp_cntrl`) and calls
    `__reload()`, not `exit()`, which could need a lock the stuck thread holds.
  - It pauses while the HOME overlay is open and while `hbc_agent_hold(true)`
    is in force.
- **Kept log:** the last 4 KiB of the agent's log ring goes to MEM2 at
  `0x91800100` on every way out (`atexit` for a normal one). It is copied
  then, not kept there, since an app's MEM2 may cover that address. HBC
  serves it with `HBCL` (protocol 4); the agent answers the same request
  with a running app's output so far. `hbc.py lastlog`; `hbc.py crash` adds
  the last 12 lines.
- **Cost:** none while the app runs, apart from one 64-bit store per
  `hbc_agent_alive()`, and the watchdog's 4 KiB stack and 1 Hz wake-up once
  armed. HBC keeps a 4 KiB copy of the log.
- **Checked:** the whole `dolphin_smoke.py --agent` suite (new steps: fatal,
  hang caught while the app spins, exit, each with its kept log), and
  `dolphin_ogc_crash.py` on libogc2 with the version 2 block.
- **Play log names:** on the bench Wii, 1.8.9's log listed other
  workstations' Wiiload runs by their first argument: a ROM, `boot`, an
  option. A Wiiload upload now takes the name its agent gave itself (from the
  kept log), else the file it was sent as (`.dol`/`.elf`), else "Wiiload".
- Dolphin asks before installing over a different version of a title, which a
  batch run cannot answer: `tools/msgboard/dolphin_sysmenu.py` removes the
  installed HBC's contents first.
- **Not yet on hardware:** fatal and hang need the installed HBC to be 1.9.0
  (an app's report goes to the installed channel).

### 1.8.9: the Message Board play log, startup timings, the play record off the critical path

- **Play log** (docs/messageboard.md). HBC now logs its own time and each app
  it launches to the Wii Message Board, one line per title a day.
  - The format was worked out in a Dolphin profile running the USA Wii Menu
    4.3U (tools/msgboard/). The profile needed `data/nocopy`, and SYSCONF
    `IPL.CD`/`IPL.CD2` set to 1: with Dolphin's default 0, the Wii Menu
    treats setup as unfinished and wipes `cdb.vff` on every boot.
  - `cdblog.c` matches the Python reference byte for byte (tests/test_cdblog.py).
    The Wii Menu accepted messages written by both, and merged its own records
    into them. The one byte that broke acceptance was the type length at 0x10,
    which is little-endian.
  - **Dolphin:** the installed HBC logged its session and a Wiiload app's,
    named after the file it was sent as ("agent_app").
  - **Bench Wii, read-only:** the permission patch lifted and restored IOS58's
    check at one site, and the Wii Menu's data folder copied in full. This Wii
    has no `cdbwiiid.dat`, so the console ID comes from an existing message;
    a dry run on a copy of its `cdb.vff` gave the right ID and message number.
  - **Not yet on the bench Wii:** a write by the installed 1.8.9.
- **Startup timings:** `HBCS` gains `startup` (each step's end, in ms since
  `main()`) and `zlib_peak`/`zlib_heap`.
  - **Bench Wii, 989 ms to the menu:** the fade-in ~515 ms, the theme's PNGs
    201 ms, the play record 118 ms, video 87 ms, ES 57 ms. The app scan
    (295 ms, 14 apps) runs in its own thread meanwhile.
  - **zlib area:** deflate peaked at 268,000 of 327,680 bytes; inflate used 7 KB.
- **Play record:** written on a thread, started after the last startup IOS call.
  Its 118 ms NAND write had held up the next IOS call (IOS serves one at a
  time), so started earlier it only moved the cost into `home_init()`. Boot to
  menu went from 989 to 922-939 ms.
- **`tests/netblock`:** TCP throughput by IOS call size, through libogc and
  straight to `/dev/net/ip/top` with HBC's own buffers. Past 16 KiB nothing
  changed (sending 0.63-0.69 MB/s, receiving 1.0-1.08 MB/s): the limit is
  IOS's network stack and the radio, not libogc's 16 KiB copy.

### 1.8.8: the bench queue's dead time between jobs

- **Measured first** (163 finished jobs on the bench PC's queue): jobs ran a median
  of 75 s. 39 jobs had waited behind one already queued, with a gap of a median 20 s
  each (the idle wait), 22 minutes in all. 24 jobs ran for 5 s or less and still paid
  the 20 s. A waiting workstation asked for the lease every 5 s, and `wait` and the idle
  dispatcher checked every 5 s too.
- **Idle wait by what's known:** the full 20 s only when the Wii's last use is unknown.
  Up to 60 s after a queue job ended (this workstation's, or the lease's last holder
  released it, which the grant now reports as `released_ago`) it's 2 s. A busy or off
  probe brings back the full wait.
- **Same-agent chains:** a job records its agent (`--agent`, `$WII_BENCH_AGENT`, or the
  Claude Code session). That agent's next job runs in the same turn after one answer
  from HBC. With nobody else in line, the lease is held 15 s for the agent's next `add`.
  With someone waiting, only an already queued job chains, for 10 minutes at most.
- **Long poll:** the server holds `/acquire` open up to 25 s and answers on release.
  Renew replies say how many are waiting. It no longer writes to a socket while
  holding its lock. Clients fall back to a 5 s poll against an older server.
- **Faster local polling:** `wait` checks every 0.5 s, the idle dispatcher every 1 s.
- **`wait` says why:** once a minute on stderr, from the new `dispatcher.state`: the
  place in line and the holder, `Wii busy: <app> is running for N min`, or `running`.
- **Default timeout 1800 s** (was 3600): the slowest 1% took about 2,100 s, and no job
  has timed out. Long jobs already pass their own `--timeout`.
- **Checked:** 20 bench unit tests on Windows and in WSL Ubuntu. They include the long
  poll answering within 2 s of a release, the three idle-wait lengths on a fake clock,
  and a real dispatcher against the fake HBC: a same-agent pair chained, the next agent
  waited about 2 s, a held chain let a later `add` from the agent straight in.
- **On the bench Wii,** through the live queue and the homeserver's lease server
  (still 1.8.6, so the 5 s fallback, not the long poll), three read-only
  `hbc.py status` jobs: a1 (the dispatcher's first job) waited the full 20 s. a2, the
  same agent's, added after a1's `wait` returned, chained and started 1.0 s after a1.
  b1, another agent's, started 3.3 s after a2. Before this change each gap was 20 s or
  more. Not yet checked: the long poll on the homeserver (it needs the container
  rebuilt) and a chain while another workstation waits.

### 1.8.7: the bench queue no longer takes an agent app for HBC

- **Reported by the WiiStation workstation:** twice on 2026-09-30, a WiiStation
  lab chain on the bench Wii ended mid-game with no crash. Its games played,
  then the Wii went back to HBC and the chain's results were never sent.
  - **Cause:** apps that link `sdk/hbc_agent` answer TCP 4299 like HBC's menu.
    The queue's idle check (a 16-byte `PING` that anything on 4299 accepts)
    therefore saw a running agent app as idle HBC, and `status` said "in HBC
    (free)". A dispatcher then started a job whose `hbc.py run` called
    `exit_app()`, which asked the other workstation's app to exit.
  - **Queue:** the probe is now `HBCV`. HBC answers with its version, and an
    agent app with `<version> agent` (as `hbc.py`'s `is_agent()` reads it); an
    agent app counts as busy. The stock HBC answers nothing to `HBCV` and
    rejects it at once, which still means its menu. `status` names the app
    (`busy: agent_app is running`), read from its `HBCS` reply.
  - **`hbc.py`:** each job gets `WII_BENCH_JOB_START`. Inside a job,
    `exit_app()` (`run`, `send`, `exit`, and the test scripts) refuses with "Wii
    busy: <app> is running" when the agent app's uptime is longer than the job
    has run, since that app is someone else's. An app the job started, it
    still exits. By hand, outside the queue, nothing changes.
  - **Tests:** `test_wiibench.py` runs the idle check and `status` against
    `test_hbc_tool.py`'s fake Wii, as HBC and as an agent app
    (`WII_BENCH_PORT` points the queue at it). `test_hbc_tool.py` checks a job
    leaves an older agent app running and exits one it started.
  - **On the bench Wii:** job A started `agent_app` for 90 s and ended. Inside
    it, an `hbc.py send` given a job start later than the app's refused ("Wii
    busy: agent_app is running, started before this bench job; not exiting
    it"), and the app kept running. `status` then said "busy: agent_app is
    running (agent 1.8.6)".
- **Found testing it:** a dispatcher keeps the `wiibench.py` it started with.
  The one that ran job B had started at 14:50, before this fix, and started B
  while the app was still running. B only asked for the version. Noted in
  `tools/wii-bench/README.md`: the next dispatcher runs new code, and the
  running one exits after 10 idle minutes.
- **Rerun with a new dispatcher:** job A started `agent_app` at about 15:30:45
  for 90 s, and the refusal inside it passed again. B, queued at 15:31:00,
  stayed pending while `status` said "busy: agent_app is running". It started
  at 15:32:52, after the app had returned to HBC and HBC had answered for 20 s,
  and it saw HBC 1.8.6, not the agent.

### 1.8.6: one bench Wii for several workstations, on any OS

- **Problem:** the bench queue lived in `C:\tools\wii-bench` on one PC, and its
  dispatcher lock held a local PID, so other workstations couldn't join. They were
  held off only by the HBC-idle check, with no order between them. Job
  `20260930-002306` waited from 00:23 to 08:44 while another workstation had the Wii.
- **Fix:** a lease server, `wiibench.py serve` (stdlib only, TCP 4310, a Dockerfile
  and `compose.yaml` for the homeserver). Each workstation keeps its own queue and
  dispatcher, and takes the lease before each job:
  - first come, first served, so turns alternate between workstations;
  - renewed every 15 s; a lease or a place in line with no renewal for 60 s is dropped;
  - after a server restart, a 60 s grace lets a running job reclaim its lease;
  - an unreachable server blocks the dispatcher instead of being skipped.
- **Setup on every OS:** `wiibench.py setup --server URL` writes the shim and the
  server URL into the state directory (`C:\tools\wii-bench` or `~/.wii-bench`). A
  server file written by Windows PowerShell 5.1 (UTF-16) or with a BOM is read too.
  Job logs are read as UTF-8, and `serve` ends on SIGTERM (`docker stop`).
- **Checked:** 11 unit tests (lease order, expiry, reclaim, an HTTP round trip,
  `setup`, the server file's encodings) on Windows and in WSL Ubuntu. The image was
  built and run in WSL's Docker, and a Windows dispatcher and a Linux dispatcher
  sharing it ran WIN1, LINUX1, WIN2, LINUX2 with no overlap. The README's Windows and
  Linux check commands were run as written.
- **Not checked:** macOS by hand (CI's macOS job runs the unit tests), `docker compose`
  (WSL has no Compose plugin), the homeserver, and a real Wii job under the lease.

### 1.8.5: the libogc2 crash hook on the bench Wii, the agent's stack on libogc2

- **On hardware:** `tests/agent_app` built on libogc2 (devkitPPC r41-2) passed
  the whole `tests/agent_checks.py` run on the bench Wii with HBC 1.8.4:
  - status and files, the overlay, and exit to HBC (8.7 s);
  - the Wiiload upload landing back in HBC;
  - a DSI crash: HBC reported it at `agent_app_crash` (main.c:63), DAR 0x10,
    LR in `main`;
  - a trap: HBC reported a program exception at `agent_app_trap`.

  The Wii went back to HBC each time; nothing froze.
- **Agent stack 100% used on libogc2:** the first bench run stopped at the
  status check, which reported 12288 of 12288 bytes of the agent's stack used
  91 ms after start.
  - **Cause:** libogc2 and libogc 1.x write 0xDEADBABE into the lowest word of
    every thread's stack when the thread starts (`__lwp_thread_loadenv`).
    `stack_used()` counts from the lowest nonzero byte, so it saw the whole
    stack as used. libogc 3 writes no such tag.
  - **Fix:** the scan starts above that word when it holds the tag.
  - **After:** 2992 bytes idle, then 7616 (Dolphin) and 8312 (Wii) of 12288
    after the transfer checks, against about 5000 on libogc 3. Fine for now.
    Raise `AGENT_STACK` if a libogc2 app shows less than 2 KiB left.
- **Dolphin:** the whole `dolphin_smoke.py --agent` suite (installed WAD) also
  passes with the libogc2 app in place of the libogc 3 one.

### 1.8.4: the line-ending test in CI's container

- 1.8.3's CI failed in the devkitPPC job's Tests step. The checkout there
  belongs to another user than the container's root, so git refused the
  repository ("dubious ownership", exit 128), and `tests/test_line_endings.py`
  errored on `git ls-files`. It now runs git with `-c safe.directory=<repo>`,
  which trusts only this checkout for that one read-only command. It was
  reproduced and checked in `devkitpro/devkitppc:latest` with the checkout
  owned by uid 1001. The host jobs (Windows, macOS, Linux) had passed.

### 1.8.3: crash reports on libogc2 and libogc 1.x, LF line endings

- **The agent on libogc2 and libogc 1.x.** Apps still built on those (WiiStation,
  Wii64) could not use the agent's crash reports.
  - **Cause:** tuxedo (libogc 3) calls a C panic function, but the older libogcs'
    `_exceptionhandlertable[]` holds assembly entry points. Their vector code
    saves r0-r5 and the special registers into a frame 728 bytes below the
    interrupted stack pointer and `rfi`s to the entry with r1 not yet moved and
    r3 = the exception number. WiiStation's copy of the agent put a C function
    there. On the first real crash, its prologue wrote over the saved frame, it
    used r3 as a frame pointer, and the bench Wii froze hard (2026-09-30).
    WiiStation had disabled the hook since then.
  - **Fix:** `sdk/hbc_agent/ogc_exc.S` does what libogc's own
    `default_exceptionhandler` does. It claims the frame, saves GQR0-7 and
    r6-r31, then calls `agent_exc(frame_context *)`. That records the crash and
    hands the frame to libogc's `c_default_exceptionhandler` (its crash screen and
    reload). The agent takes only table entries that still hold
    `default_exceptionhandler`; the FPU, interrupt and decrementer handlers stay,
    and so does anything a debugger or the app installed.
  - **Details:** exception numbers are converted to the vector numbers tuxedo
    uses, so `hbc.py crash` reads the same. The installed libogc2 and libogc
    1.8.23 have byte-identical vector code and frame layout (disassembled);
    agent.c checks the frame offsets against `frame_context` at compile time.
    The code that runs inside the exception makes only integer calls (FP is off
    there).
  - **Build:** `make -C sdk/hbc_agent OGC=libogc2` (or `OGC=libogc-1.8.23`). The
    flavor comes from `__has_include(<tuxedo/ppc/exception.h>)`. The overlay's
    Connect asks for the SYNC button there (no `WPAD_StartPairing`).
  - **Checked in Dolphin:** `tests/dolphin_ogc_crash.py` boots
    `agent_app-libogc2-crash.dol` (`make -C tests/agent_app OGC=libogc2 MODE=crash`,
    devkitPPC r41-2) with MMU emulation and the GDB stub. It reads the crash
    block from MEM2 and walks the stack after the crash:
    - crash (DSI): at `agent_app_crash`, DAR 0x10, `main` in the chain, checksum
      good; the CPU was then in libogc's `waitForReload` <- `agent_exc_entry` <-
      `main`.
    - trap (program): also passes.
    - The libogc 1.8.23 library builds and links the same entry, but its test app
      does not build here: there is no libfat for 1.8.23 installed.
  - **Not yet on hardware.**
- **Line endings, for good.**
  - **Before:** Git for Windows sets `core.autocrlf=true`, and `.gitattributes`
    had no general rule. The working tree was a mix: 217 files CRLF, 33 that
    tools had rewritten LF, 17 upstream files really CRLF in the repository, and
    one mixed. Every edit had to preserve each file's endings, and
    `channel/channelapp/banner/icon.ppm` had CRLF injected on this PC by a
    checkout from before it was marked binary. Banners built here had a damaged
    icon; CI's checkout was fine.
  - **Now:** `* text=auto eol=lf` makes every text file LF in the repository and
    in every checkout, whatever `core.autocrlf` says. The 17 upstream CRLF files
    and the mixed `channel/wiiload/main.c` are `-text` (kept byte-for-byte), and
    `.editorconfig` says LF. `tests/test_line_endings.py` (all three CI
    platforms) fails on CRLF committed or checked out.
    `tools/fix_line_endings.py` converts an older checkout and restores only
    binaries whose sole change is CRLF.
- **devfile.c:** a listing entry whose full path does not fit the path buffer is
  now skipped instead of being stat'ed truncated (devkitPPC r41-2's gcc 12 found
  it building the agent for libogc2).
- **`tests/agent_checks.py`:** a return address is matched to its function by
  the call before it. A call to a noreturn function can be `main`'s last
  instruction, which leaves LR just past `main`.
- **`dolphin_smoke.py --agent` needs the installed channel.** It failed at "agent
  exit to HBC": HBC did not answer within 90 s, and about 5 s after the app's
  exit Dolphin's log showed the CPU running off the end of MEM2 ("Unknown
  Pointer 0x14000000 PC 0x94000000 LR 0x939f8000").
  - **No release ever passed this in Dolphin.** With no image argument the
    script booted the DOL. 1.5.0 (the first release with the agent checks), 1.6.0
    and 1.8.2 fail identically, built with the same toolchain and run in the same
    Dolphin executable. The 1.8.2 WAD passes the whole agent suite: exit to HBC in
    3.5 s, the Wiiload upload, and the crash report. README's test table already
    runs `--agent` against the WAD.
  - **Cause:** with cheats off, Dolphin replaces 0x80001800 with its `HBReload`
    hook and writes `STUBHAXX`. A DOL-booted HBC is not `MY_TITLEID`, so since
    1.1.7 it keeps that stub. A real stub would not help either, because a DOL
    boot has no installed title to relaunch. The app's `exit()` enters
    `HBReload`, which asks the host to stop. Dolphin's `RequestStop` sends the
    guest an STM power event and resumes the CPU. The app has already exited and
    never handles the event, so the CPU runs on from the hook into whatever is in
    memory. Dolphin never stops, and HBC never answers again.
  - **Fix:** `--agent` now boots `channel/title/channel_retail.wad` by default
    and refuses a DOL with that explanation. Checked on 1.8.2: a DOL is refused
    at once, and `--agent` with no image passes the whole suite. The bench Wii is
    not affected: there the stub relaunches the installed channel.

### 1.8.2: CI off Node.js 20

- GitHub warned that Node.js 20 is deprecated. `actions/upload-artifact@v4` and
  `actions/setup-python@v5` ran on it; both are now v7 (Node.js 24).
  `actions/checkout@v5` and `msys2/setup-msys2@v2` were already on Node.js 24.
  Nothing we use changed: the artifact is still one zipped upload, and
  setup-python takes only `python-version`.
- **1.8.1 on hardware, completed:** after `tests/rtc_shift` moved the clock 37 s
  ahead, Sync clock logged "Clock set: -36 s (UTC-7:00)"; the RTC counts whole
  seconds. 1.8.1's CI passed on a re-run: the first attempt failed installing
  packages, and the same step passed in the same image locally.

### 1.8.1: DEV > Sync clock, Save and Log that say something, a key-queue fix

- **Sync clock (DEV > Actions):** one SNTP exchange over UDP in a thread, with a
  3 s timeout, corrected by half the round trip. It tries `pool.ntp.org`, then
  `time.google.com`, then `time.cloudflare.com`.
  - **Zone:** the Wii's clock is its RTC plus SYSCONF's counter bias, in local
    time. The difference from UTC is rounded to 15 minutes and kept as the zone;
    only the rest, the drift, goes into the RTC.
  - **Writing:** libogc exports `__SYS_GetRTC` but not its writer, so the RTC is
    written with the same EXI exchange (`0xa0000000`, then the value) and read
    back. `settime` updates the running app's time.
  - **Refusal:** a clock more than UTC-12/+14 off is left alone, with "set it in
    Wii Settings". SYSCONF is never written.
  - **Found on the bench Wii:**
    1. `net_socket(..., SOCK_DGRAM, IPPROTO_UDP)` fails (-12). IOS takes protocol
       0 only; Dolphin's `WiiSockMan::NewSocket` enforces the same.
    2. The counter bias and the RTC must be added as u32; the Wii relies on the
       wrap. The first version added them as doubles, saw a clock 136 years off,
       and, as it then did, set the RTC to UTC. `tests/rtc_shift` (a DOL that
       moves the RTC) put the bench Wii back to UTC-7, and the fixed sync reported
       "The clock is right (UTC-7:00)". That version's "set to UTC" fallback is
       gone.
  - **Log:** the result also goes to DEV > Log.
- **HOME ignored after a stray key (found while testing):**
  `hbc_agent_home_pending()` looked only at the front of the key queue, so any
  other key queued while the overlay was closed blocked every later HOME. It
  now drops keys before a HOME.
- **Save said "Couldn't save" when nothing was wrong.** In HBC,
  `settings_save()` returns false both for a Wiiload DOL (no NAND data folder:
  no installed-channel identity) and when nothing has changed.
  - New `hbc_agent_toast()` lets `on_save` (or a slot item) give its own
    message.
  - HBC now says "Wiiload HBC: save needs the installed channel" or "Settings
    are already saved".
- **Log was empty in HBC.** HBC's `gprintf` compiles to nothing in a release.
  A few `hlog()` lines now go to stdout, which the agent keeps: start-up with the
  IOS, the app scan and its time, the network address, and each Wiiload.
- **Not re-run on hardware:** the drift-correction path of Sync clock (a job was
  queued while another workstation had the Wii), and the Dolphin suite (a
  WiiStation Dolphin with the 1.7.6 agent held port 4299 on this PC).

### 1.8.0: wiispk, a WAV player, the pointer after a reset, a LULZ build

- **1.7.6 on hardware:** a recording of both Test tunes (`audio2.m4a`) measured:
  - notes within 1 Hz of pitch in both formats;
  - on held notes, a worst dip of 0.9 dB (ADPCM) and 3.2 dB (PCM), against
    30 dB before;
  - tone against everything else: 22.2 dB for ADPCM, 14.7 dB for PCM.

  The user: "sounds WAY better". ADPCM at 6 kHz is the format to use.
- **`sdk/wiispk`:** the speaker driver as a library of two files (API, WAV
  loader, windowed-sinc resampler), zlib licence, and a README. Its code is
  written afresh:
  - **Encoder:** a nearest-of-eight search against a model of the speaker's
    shift-and-add decoder, not a port of anyone's encoder. On a two-tone test it
    measures 21.2 dB SNR against both decoder models, the same as libogc's.
  - **Protocol facts** (register values, the set-up order, the 6.67 ms pace, the
    more-than-3-packets limit) come from WiiBrew, Dolphin, public decompilations
    and our recordings. They are facts about the hardware, cited as such.
  - **Host test:** `tests/test_wiispk.py` builds it on the PC against stub
    libogc headers. A 4.6 kHz tone, which would fold onto 1.4 kHz at 6 kHz,
    comes out 64 dB below a 1 kHz tone.
- **Test page:** a third button plays `speaker.wav` from SD or USB, loaded in a
  thread (16 KB stack) with a toast. `hbc.py key w` presses it.
- **The pointer died after DEV > Reset remotes or Settings (reported):**
  - **Reset:** emptying a remote's queue could throw away IR's set-up commands,
    while the remote's status still said IR was on (wiiuse sets the flag from the
    status report), so `wiiuse_set_ir` returned early for good. Reset now clears
    the flag and sets IR up again.
  - **Settings (Disconnect all, Connect remote):** a remote that reconnected while
    the overlay was open had its format read before libogc finished setting it
    up. Its flags said no IR, which was handed back to HBC on close. The format
    is now read once per visit, after the handshake and with an empty queue.
  - **Pointer resolution:** the overlay never gave back the app's pointer
    resolution (`WPAD_SetVRes`); it does now (`wm->ir.vres`).
- **On glow:** On is now a 2-pixel blue ring with no glow; the bloom is for the
  highlight alone. Status strip: player LEDs 8 px apart (was 9), batteries 3 px
  after them (was 6).
- **Auto-off:** HBC set remotes to power off after 120 s idle
  (`controls.c`), while the Settings page showed libogc's 5 min. HBC now uses
  300 s, like libogc and the Wii Menu.
- **`TITLE=LULZ`:** the Makefiles write the title ID to `title_id.h`
  (`config.h`, the stub) and retitle the ticket and TMD
  (`pywii-tools/retitle.py`). The ticket's title key is encrypted with the title
  ID as IV, so it is decrypted and encrypted again.
  - **First version bug:** `brute_sha()` re-parses the ticket body, which
    discarded the new ID; it is now written into the body first.
  - **Checks:** the LULZ WAD's ticket and TMD carry `4c554c5a`, are fakesigned,
    their contents match, and the title key is unchanged.
  - **Dolphin:** it installs, boots, and an app's exit returns to the LULZ
    title. `dolphin_smoke.py` reads the title from the WAD.
- **The Wii's clock:** planned here, built in 1.8.1.

### 1.7.6: a speaker driver after Nintendo's

- **A recording of 1.7.5's Test sounds (ADPCM, then PCM) settled the rate.**
  - **ADPCM:** every note came out an octave high (523 Hz as 1047, 659 as 1310,
    784 as about 1570). The top note also appeared at 3906 Hz, which is 6000 minus 2094,
    a mirror image. So the speaker plays rate `0x07d0` ADPCM at 6 kHz (12,000,000 /
    rate, not WiiBrew's 6,000,000), and 1.7.5's 13.3 ms pace starved it. 1.7.4's
    pace was right.
  - **PCM** (3 kHz) played at the right pitch and length.
  - **Both** had held notes chopped by 10 to 30 dB dips every 10 to 40 ms, and loud
    mirror images from 2.5 to 5 kHz (the speaker holds each sample, and it is loud
    there).
- **Nintendo's SDK** (research from the doldecomp/ogws Wii Sports and Rhae Wii Play
  decompilations: WPAD, WUD, WENC, nw4r::snd::RemoteSpeaker):
  - **Sniff mode:** WUD puts every remote's link in sniff mode at 8 slots (5 ms),
    attempt 1, timeout 0, so the link runs at 200 Hz. libogc never does this: the call
    in `bte.c` is commented out. Dolphin notes that remotes run at about 100 Hz
    without it and drop speaker reports, which desyncs the decoder. **This is the
    chopping.**
  - **Start-up:** enable, mute, `0x01` to 0xa20009, **`0x80`** to 0xa20001 (libogc
    writes `0x08`), the config block `00 00 D0 07 VV 0C 0E`, unmute, and a status
    request. Play (`0x01` to 0xa20008) is sent only after all of that is answered.
  - **Pacing:** NW4R sends 40 samples every 6.67 ms (150 Hz alarm).
  - **Backpressure:** a packet is refused when the controller holds more than 3
    unacknowledged ACL packets, among other checks. A refused block is skipped
    without encoding it, so the encoder and the decoder stay in step. The encoder
    carries on across sounds and silence, and is reset only after a start-up.
  - **Encoder (WENC):** successive approximation, with the same step table and nibble
    order as libogc's `wencdata`. The rounding differs (Nintendo halves with
    truncation), so the result drifts from libogc's by 1 or 2 LSB a sample.
- **lwbt bug, confirmed in the libbte we link (disassembly):** with no controller
  buffer free and nothing queued, `lp_acl_write` queues the packet and then sends it
  anyway. The packet goes out twice (the queued copy again on the next completion),
  and the u16 free count at `hci_dev + 10` wraps to 0xffff.
- **The driver (overlay.c):** does all of the above. It uses sniff mode through
  `hci_sniff_mode`, the SDK's start-up sequence, and a per-report WENC port inside
  the 6.67 ms alarm. It reads the free count at `hci_dev + 10` to skip a block when
  more than 3 packets are in flight. A wrapped count is trusted again every 30 ticks,
  so a count that stays wrong cannot silence the speaker. It restarts the speaker
  when the format or volume changes, and restores libogc's speaker set-up on close if
  the app had it on. `status` shows the driver's counters (`spk`).
- **Still to measure on hardware:** the `spk` counters while a sound plays (skips,
  credits, worst gap, sniff), and whether a recording is still chopped. Sniff mode
  stays on after the overlay closes, as it does under Nintendo's SDK; an app's
  remotes then report at 200 Hz.

### 1.7.5: the speaker's pace, and a PCM/ADPCM test

- **Reported on hardware:** 1.7.4's volume chirp sounded cut off, and Find made only
  noise.
- **Cause: the speaker got data twice as fast as it plays.** By WiiBrew's formula,
  ADPCM at rate `0x07d0` plays 6,000,000 / 2000 = 3,000 samples a second, and
  WiiBrew's PCM example (rate `0x1770`, 20 bytes every 10 ms) checks the same
  formula. libogc, and 1.7.4 after it, sent 40 ADPCM samples every 6.67 ms, 6,000
  a second. Dolphin plays ADPCM at twice the configured rate, so the emulator hid the
  problem. On a remote the buffer overflows. A short sound loses its end, and ADPCM,
  which codes each sample as a step from the last, turns the gaps into noise.
- **Fix:** a sound carries its format, rate value and report period; the alarm runs at
  that period (ADPCM 13.3 ms, PCM 6.67 ms, both 3 kHz). Notes stay under 1.5 kHz,
  half the rate. Each sound starts with 40 ms of silence and ends with 200 ms, so the
  amplifier is awake and the buffer plays out. Find now sends only its chime, no LED
  or rumble commands, and lasts as long as the chime.
- **Test page:** "Sound: ADPCM (1)" and "Sound: PCM (2)" play the same tune in each
  format at the same rate, so they can be compared by ear. They respond to the remote's
  1 and 2, or to A while the pointer is on them (the D-pad and A are under test there,
  so nothing else is highlighted). `hbc.py key` accepts `1` and `2`.
- **Trigger for the next step:** if PCM sounds better, make it the default for the
  chime and chirp, then try PCM at 4 kHz (rate `0x0bb8`, 5 ms reports). If ADPCM is
  better, try ADPCM at 6 kHz (rate `0x03e8`) at the 6.67 ms pace.

### 1.7.4: the remote's speaker, Find's lights, the On glow

- **Sensor bar showed the wrong side.** The display took the Wii's setting as 1 for
  below; `CONF_SENSORBAR_TOP` is 1 (libogc passes `CONF_GetSensorBarPosition() ^ 1` to
  wiiuse, whose `WIIUSE_IR_ABOVE` is 0).
- **Speaker research (WiiBrew Wiimote#Speaker, Dolphin `Speaker.cpp`, libogc
  `speaker.c`/`wpad.c`/lwbt `hci.c`, the InputBridge and trigger-segfault write-ups):**
  - Two formats: 4-bit Yamaha ADPCM (`0x00`) at `6000000 / rate` and signed 8-bit PCM
    (`0x40`) at `12000000 / rate`, 1 to 20 bytes a `0x18` report. libogc's set-up is
    ADPCM, rate `0x07d0`, 40 samples a report every 6.67 ms (Dolphin plays it at
    6 kHz). PCM gets 20 samples a report, so at the same report rate it reaches only
    3 kHz; both write-ups found PCM thin and aliased and chose ADPCM.
  - **The ADPCM volume byte tops out at `0x40`** (WiiBrew); 8-bit PCM's goes to `0xff`.
    1.7.3's steps 6 to 10 wrote `0x4c` to `0x80`, overdriving the speaker.
  - **A report the Bluetooth controller has no buffer for is dropped without an
    error:** `lp_acl_write` returns `ERR_OK` ("Host buffer full. Dropped packet").
    An ADPCM decoder that misses a report keeps a wrong step size until the signal goes
    quiet, which is heard as a burst of noise. Nothing above lwbt can see the drop.
  - The old chime ramped to 27,000 of 32,767 with 10 ms linear edges; loud input to
    4-bit ADPCM overshoots its step, and the amplitude is not the loudness control.
- **What changed:** volume 0 to 10 maps onto `0x00` to `0x40` on a loudness curve (10 is
  libogc's default); sounds are built from notes between 1 and 2 kHz at half scale with
  12 ms raised-cosine fades and a report of silence at each end; the configuration
  block (which restarts the decoder) is written before every sound; the speaker stays
  on while the overlay is open, so a volume change plays a 90 ms chirp at once; Find's
  chime is a three-time ding-dong, each louder.
- **Future work, trigger: the chime still sounds rough on hardware.** Try 8-bit PCM at
  4 kHz (rate `0x0bb8`) with reports every 5 ms, and compare by ear with the ADPCM
  chime; a PCM drop is a 5 ms gap, not a noise burst. Needs a person at the remote.
- **Find's lights drifted.** Each LED change waited its turn in the remote's command
  queue behind the others, so steps arrived late and in bunches. Find now runs a chase
  (1-2-3-4-3-2) on a fixed 83 ms clock and sends a step only into an empty queue,
  skipping it otherwise; its rumble rides in the same report (the rumble bit is set in
  the remote's state, then the LED report carries it), buzzing at each end.
- **The On glow was hidden.** Six stacked boxes at alpha 12 reached 25 % at the edge
  and nothing past 6 pixels. Glows are now drawn per pixel (`ov_glow`, smoothstep
  falloff from the rounded edge): 8 px for the highlight, 16 px at 150 for On, and 16 px
  at 190 for both; an On button is rimmed in the glow's blue; every glow on a layer is
  drawn before that layer's buttons, so a wide glow never tints a neighbour.
- **Rumble-on buzz** is 8 frames (133 ms), 30 % shorter.

### 1.7.3: the remote's command queue

- **libogc's speaker streaming stops a remote's command queue for good.** wiiuse sends
  a remote's commands one at a time, each after the last is acknowledged, and speaker
  data reports are not acknowledged (Dolphin's `HandleSpeakerData` says as much). So
  the first chunk `WPAD_SendStreamData` queued left every later command waiting: no LED
  changes, no rumble anywhere (rumble rides on the LED report), no extension handshake
  (a Classic Controller plugged in then never finished), until the remote reconnected.
  1.7.2's Find did exactly that on a real remote.
- **Fix:** the overlay streams speaker data itself, as raw reports every 6.67 ms outside
  the queue (each with the remote's rumble bit), after the speaker's set-up has been
  answered and its volume written. A guard run each overlay frame drops a speaker chunk
  stuck at a queue's head and re-sends any other command unanswered for 0.5 s; DEV >
  Reset remotes empties every queue. `status` shows each remote's queue (`remotes`:
  state, LEDs, queued, head report, resent, dropped). On the bench Wii, Find's
  nine-command speaker set-up drained in 0.7 s and the LEDs and rumble bit then
  alternated as meant.
- **Also:** a speaker volume stepper per remote (written to the speaker's configuration
  block), a 5 s wait before Calibrate measures, + and - together to leave the Test page
  (which shows every other button), the pointer turned the other way, and a quieter
  bloom.

### 1.7.2: Find, rumble, the pointer on 16:9, the bloom

- **Find sent the speaker the wrong thing.** `WPAD_SendStreamData` takes a whole sound,
  which libogc plays 20 bytes a tick from its own 6.67 ms timer; Find called it every
  6.7 ms with a 20-byte stack buffer, before the speaker was even ready, and the flood
  of commands filled the remote's command queue, which drops what does not fit. On a
  real remote: no sound, LEDs stuck on, no rumble. Find now runs from the overlay's
  frame loop, sends LED and rumble commands only when they change, plays one prebuilt
  3 s chime once the speaker reports ready, and re-sends the resting state after.
- **HBC's hover buzz ran on under the overlay.** HBC starts a rumble when the pointer
  enters a button and stops it from its main loop, which the overlay pauses. The
  overlay now stops every remote's rumble on opening, and `home.c` stops HBC's first.
- **The pointer:** HBC draws its shade 2 pixels right and 4 down under the hand, and
  turns both with the remote (`cursors.c`); the overlay had put the shade straight under
  the hand. On a 16:9 TV the Wii stretches the 640-pixel picture by a third, so the
  overlay now uses fonts and a pointer condensed to three quarters when
  `CONF_GetAspectRatio()` says 16:9.
- **Look:** a blue bloom around the highlighted button and a smaller one around buttons
  that are On, with no black edge on glowing buttons; a short buzz when rumble is
  switched on.

### 1.7.1: the pointer, and the overlay's cost

- **Pointer:** the overlay turns on IR for every connected remote while open (restoring
  each remote's format after), draws HBC's hand cursor for each remote on screen, and
  presses what the remote in use points at.
- **Frame time was over budget.** On the bench Wii a drawn frame took 19.1 ms on average
  (35 ms at most): every blended pixel did three integer divides, and each frame was
  redrawn whether or not it changed. Now opaque runs are 32-bit stores, blending uses a
  shift instead of a divide, the dimmed background comes from lookup tables, and
  unchanged frames are not drawn: 7.9 ms average, 13.9 ms at most, and 65 of 475 frames
  drawn in the same sequence. The PC preview matches the old output within 3 levels.
- **The highlight was invisible on a TV.** A 2-pixel light outline blurred away on an
  interlaced picture, and with the pointer in use a remote aimed at the screen but at no
  button hid the D-pad's highlight. The highlight is now a 3-pixel white frame around a
  brighter body, and it follows whichever of the pointer and the D-pad moved last.
- **Measured cost** (`tests/agent_cost.py`, bench Wii): start-up 13 KB heap and 16 KB
  arena; idle 0.07% CPU (5 wake-ups/s, 140 us each); overlay 1.8 MB while open,
  nothing when closed. README has the table.

### 1.7.0: HBC's HOME menu is the agent's overlay

- **HOME:** `home.c` replaces `m_main.c`. The HBC slot's menu carries the old menu's
  buttons and information; Exit's choices go through HBC's own shutdown path
  (`on_exit_choice`), and Save is `settings_save`. Checked in Dolphin with
  `hbc.py key`/`screen` (`check_home` in `tests/dolphin_smoke.py`).
- **HBC's loader thread accepts only when the main loop signals it** each frame, so a
  modal loop that stops the main loop also stops `hbc.py`. The overlay's `on_frame` hook
  calls `loader_signal_threads()`. Anything else that ever runs its own loop inside HBC
  needs the same.
- **The video interface scans out MEM1 only** (libogc allocates framebuffers from
  arena 1). HBC's heap is in MEM2 once its menu is up, so HBC lends the overlay two
  framebuffers in low MEM1 at `0x80a00000`, which only an app launch writes (after the
  main loop ends). `hbc_agent_home()` in other apps drops heap framebuffers that land
  in MEM2 and draws over the app's own framebuffer instead.
- **A dead Makefile rule built `/home` into `home.o`.** `$(DIR_BUILD)/%.o: $(DIR_INT)/%`
  used a variable that was never set, so any `source/X.c` whose name matched a root
  directory (`/home` under MSYS2) was embedded as binary data instead of compiled.
  Removed; it was unused since the first commit.
- **Not translated yet:** the overlay's font covers ASCII only, so HBC's HOME menu is in
  English for now. Trigger: glyphs for the languages in `i18n/`.

### 1.6.2: small downloads crashed a fresh HBC

- **Symptom (reported from WiiStation's bench jobs):** after a reset, `hbc.py get` of some
  logs timed out and HBC died with a DSI at `devstream_get`, DAR `0xFFFFFFF4`.
- **Cause:** a download's terminator frame takes the next free buffer slot, and its
  header was written at `slot->data - 12`. A file of one to three frames (up to 192 KiB)
  downloaded before any larger transfer left the terminator in a slot that never held
  data, whose `data` was still NULL. Present since framed transfers (1.3.x); the tests
  always moved a 300 KB file first. The agent, which frees its slots after each
  transfer, wrote that header into freed memory instead.
- **Fix:** slots start with `data = raw`, and the terminator's header goes in front of
  `raw`. Checked on the bench Wii with the log that crashed it
  (`sd:/wiisxrx/atrace.log`, 135,816 bytes) as a fresh session's first transfer; the
  Dolphin suite now makes a 3-frame download its first framed transfer.

### 1.6.0: the HOME overlay

- **Overlay:** `hbc_agent_home()` draws a status strip and slide-in menus over the
  app's frozen frame in software, into two framebuffers of its own, so it needs no GX
  state from the app. It passed the full agent suite in Dolphin and on the bench Wii
  (`tests/wii_agent.py`), driven by `hbc.py key` and checked with `hbc.py screen`.
- **`VIDEO_GetCurrentFramebuffer()` returns the VI's physical address.** Reading
  it as a cached or uncached virtual address read unmapped memory (Dolphin: "Invalid
  read from 0x00135460"). The agent masks it to an uncached address.
- **wiiuse handles:** libogc 3.1 keeps `__wpads` static; the agent locates it from
  `WPAD_Rumble`'s instructions and checks each handle's channel before use. A libogc
  that compiles `WPAD_Rumble` differently turns the LED, IR and sensor-bar settings
  off rather than breaking them. Trigger to revisit: libogc exporting a handle getter.
- **`WPAD_Search` disconnects every remote** before searching for guests, so the
  overlay's Connect remote uses `WPAD_StartPairing` instead.
- **Not yet verified by hand:** Find's chime, rumble, and the per-remote settings need a
  remote in someone's hand; Dolphin connects its emulated remote only on input, and the
  bench Wii's remotes are asleep during automated runs.

### 1.5.0: the in-app agent

- **Agent:** `sdk/hbc_agent` builds HBC's `devfile.c` (file requests, split out of
  `devnet.c`), `devstream.c` and `tcp.c` into `libhbcagent.a`, so an app answers the
  developer protocol while it runs. On the bench Wii (`tests/wii_agent.py`) framed
  transfers inside the app ran at 0.92 to 1.09 times HBC's speed with two buffer slots
  instead of four; the agent thread peaked at 3952 of 12288 bytes of stack.
- **Low memory does not survive a title launch.** IOS clears `0x0` to `0x3fff` when it
  boots a title (Dolphin's `IOS.cpp` `SetupMemory` does the same), so the 1.4.0 restore
  of the log target from `0x80002f20` never worked after an app returned to the
  installed channel: a long-running `hbc.py log` went quiet after the first app. HBC now
  keeps a copy in MEM2 at `0x91800000`, and the agent's crash block sits at
  `0x91800020`; HBC reads both first thing in `main()`. On the bench Wii a crash block
  survived the reload stub, IOS58's title launch, a stay in the installed 1.4.1, and a
  Wiiload launch of 1.5.0.
- **libogc's `net_init()` hangs** when it runs while another thread's start-up is in
  progress: `net_init_async()` returns `-EBUSY`, and `net_init()` ignores that and
  sleeps on a queue nobody wakes. The agent starts the network with
  `net_init_async()` and polls `net_get_status()`, as HBC's loader does, and
  `hbc_netlog_init()` now waits while the status is `-EBUSY`. Apps that call
  `net_init()` themselves must do so before `hbc_agent_init()`.
- **Polling through a reboot:** `hbc.py` waited with 15 s connects, and a SYN sent
  while the Wii reboots can go unanswered, so `exit` took 15.6 s on hardware.
  Polls now use 2 s connects (6.1 s).

### 1.4.1 measurements

- **A/B:** three interleaved rounds of 1.3.0 and 1.4.0 on the bench Wii showed no
  regression (ELF download 0.89 against 0.88 MB/s). The apparent downward trend across
  earlier single runs was Wi-Fi variance; one round in the same session dropped to
  0.30 MB/s.
- **End of stream on IOS:** traced over 30 probes. `net_read` gives `-EAGAIN` for an open,
  idle connection and `0` once the peer has closed, so 1.4.1 ends a request on the first
  `0`. IOS sometimes reports a closed socket readable while `net_read` still returns
  `-EAGAIN` until the 2 s header limit; nothing on the Wii can see that close sooner.
  `HBCS` keeps the trace (`tcp_last_failure`).
- **Memory:** `tests/membench` on the Wii measured MEM1 at 3x MEM2 for memcpy, and zlib
  25% (inflate) and 14% (deflate) faster with its state in MEM1; the locked cache ran
  CRC-32 at 244 MB/s from MEM2. 1.4.1 reserves a 320 KiB MEM1 arena for zlib at startup
  (`zlib_mem` in `HBCS`), which cut an ELF download's Wii CPU time from 729 to 653 ms.
  Transfers stay link-bound, frame buffers stay in MEM2, and HBC leaves the locked cache
  alone (docs/devnet.md explains the trade).
- **Bench queue:** `tools/wii-bench` is now git-controlled here, with portable process
  handling and unit tests; its state stays in one shared directory.

The disc and partition tools (`discinfo`, `extract*`, `inject*`, `partsetios`,
`rsapatch`, `getappldr`) run only their usage paths in tests; no disc image
was available.

No measured hot spot emerged from this source review. Measure startup app scan
and theme load time in Dolphin before changing caches, image formats, or draw
paths.

The 1.1.6 DOL was sent to the dev Wii through the existing Homebrew Channel.
The new build answered the local version query on port 4299 with `1.1.6`, which
confirms that it booted and accepted a network connection. This check does not
cover launching another app or installed-WAD behavior.

The 1.1.7 DOL was also sent through the installed channel. Wii64 completed
its two-game diagnostic run and returned to the older Homebrew Channel menu.
A second Wiiload transfer then started HBC 1.1.7 again. Before the fix, the
same Wii64 exit left its screen frozen and needed a power cycle. The older
channel alone returned after the same Wii64 run. The HBC loader had replaced
the older channel's return stub with its own stub, which was linked for
`0x80003f00` but copied to `0x80001800`. The code now keeps a valid launching
channel stub for direct DOL use. Its own stub is linked at the copied address,
and its BSS starts above the Wii low-memory IOS state; `tests/stub_layout.sh`
checks these bounds. A forced test of the own stub from the direct DOL still
froze on the Wii64 screen. It does not establish installed-WAD behavior,
because a direct DOL has no installed OHBC title identity. The 1.1.7 retail
WAD imported into an isolated Dolphin NAND and reached the HBC menu. Its own
app return path still needs a test before installing the WAD on a Wii.

For the installed WAD return test, Dolphin's default `HBReload` hook replaced
the HBC stub and stopped emulation. With that hook disabled, GDB confirmed the
real HBC stub ran. It opened `/dev/es`, tried to reload the already active
IOS58, then waited forever for the second `/dev/es` IPC acknowledgment. The
stub now skips the redundant IOS reload and second open. The rebuilt WAD boots
to the HBC menu and its live stub matches the new binary, but app exit on that
WAD remains unverified: the attempted automated GameCube A input did not
launch the test app. See `WII_DEVELOPMENT.md` for the debugger evidence.

The Dolphin log ends at `Setup Wii Memory...` even when the WAD's HBC menu is
visible. Direct Dolphin executable launches abort in macOS Qt/Cocoa startup;
use `open -n -a /Applications/Dolphin.app` for an isolated profile.

## Future work

1. Add malformed ELF/DOL and XML runtime regression inputs, plus a durable
   forced-short-write Gecko check. They should fail cleanly and keep a normal
   DOL launch working.
2. Port the auxiliary PyWii inspection and disc tools to Python 3 when they
   are needed. The documented retail WAD path already runs under Python 3.
3. Add an isolated Dolphin smoke-test helper for the channel DOL and installed
   WAD. Record or inject two GameCube A presses one second apart to select and
   launch the exit-test app; macOS keyboard injection did not work in the
   current window. Verify app return to the installed WAD before a real Wii
   test.
4. Record supported devkitPro package versions in a repeatable build check.
   Current validation is on Intel macOS and Windows 11 with devkitPPC r50-1 and libogc 3.1.0.
