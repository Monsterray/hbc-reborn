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
