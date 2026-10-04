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
