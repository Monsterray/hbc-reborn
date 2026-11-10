# Contributing to HBC reborn

How to build the channel, which test answers which question, and the rules
every change follows. Agents: [AGENTS.md](AGENTS.md) and the
[project skill](.agents/skills/hbc-build-and-review/SKILL.md) have the
detailed procedures.

**Contents:** [Building](#building) · [Testing](#testing) · [Rules for changes](#rules-for-changes)

## Building

Every platform uses devkitPro (devkitPPC r50-1, libogc 3.1.0), GNU make, and a
POSIX shell. The tree has no symlinks and keeps LF endings on shell scripts,
so a plain `git clone` works everywhere.

### 1. Toolchain and libraries

Install devkitPro with [its installer or pacman](https://devkitpro.org/wiki/Getting_Started),
then the PowerPC libraries:

```sh
dkp-pacman -S wii-dev ppc-zlib ppc-libpng ppc-mxml ppc-freetype ppc-bzip2 ppc-brotli
```

- **Windows:** run everything from the devkitPro MSYS2 shell
  (`C:\devkitPro\msys2\msys2_shell.bat`), where the command is `pacman` and
  `DEVKITPRO`/`DEVKITPPC` are set for you.
- **Linux and macOS:** use `sudo dkp-pacman` and export
  `DEVKITPRO=/opt/devkitpro` and `DEVKITPPC=$DEVKITPRO/devkitPPC`.

### 2. Host tools

| Tool | Needed for | Windows (devkitPro MSYS2) | Debian/Ubuntu | macOS (Homebrew) |
| --- | --- | --- | --- | --- |
| C compiler, zlib | WiiPAX, banner tools, host `wiiload`, tests | `pacman -S gcc zlib-devel` | `apt install build-essential zlib1g-dev` | Xcode command-line tools |
| `xxd` | WiiPAX stub embedding | `pacman -S vim` | `apt install xxd` | included |
| `msgfmt` | translations | included | `apt install gettext` | `brew install gettext` |
| Python 3.10+ | banner, WAD packaging, tools, tests | [python.org](https://www.python.org/) | `apt install python3-venv` | `brew install python` |

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt     # Windows: .venv/Scripts/python
```

### 3. Channel DOL

```sh
make -C wiipax
make -C channel/channelapp channel
```

This produces `channel/channelapp/channelapp-channel.dol`, the file you send
over Wiiload. It needs no Wii keys.

### 4. Retail WAD

```sh
make -C channel PYTHON="$(pwd)/.venv/bin/python"
make -C channel/title PYTHON="$(pwd)/.venv/bin/python" check
```

The build reads a 16-byte Wii common key from `~/.wii/common-key` (or from
`$WII_KEYS_DIR`). Take it from your own Wii's BootMii `keys.bin`, where
[BackupMii documents](https://wiibrew.org/wiki/BackupMii) it at offset `0x114`.
Keep `keys.bin` and the key private: put your copies in `keys/`, which Git
ignores along with loose key files anywhere in the tree. `wadpack.py` refuses
any content whose size or SHA-1 does not match the TMD, and a failed build
leaves no partial WAD. NAND saves and themes need the installed channel's
title identity, so they do not work from a direct DOL launch.

`TITLE=LULZ` (four characters, default `OHBC`) sets the title ID everywhere
it is used: see [Replacing the original Homebrew Channel](README.md#replacing-the-original-homebrew-channel).

The displayed version and the WAD title version share one `major.minor.patch`
value; the 16-bit TMD field packs it as `major << 11 | minor << 5 | patch`.

## Testing

The tests are in folders named for where they run; [tests/README.md](tests/README.md)
lists every one with what it covers.

| Folder | Runs on | Run it with |
| --- | --- | --- |
| `tests/host/` | this PC, no Wii | `python3 -m unittest discover -s tests/host -p 'test_*.py'` (CI runs this), or `python3 tests/host/run_parallel.py`: the same, all at once, about 20 s |
| `tests/dolphin/` | Dolphin | `python3 tests/dolphin/<test>.py` |
| `tests/wii/` | a real Wii, through the bench queue | queue `python tests/wii/<test>.py` |
| `tests/apps/` | the test DOLs the others send | `make -C tests/apps/<app>` |
| `tests/lib/` | shared helpers | imported: `sys.path.insert(0, str(root / "tests" / "lib")); import testenv` puts `tools/` and every folder on the path |

| Check | Command | Needs |
| --- | --- | --- |
| Unit tests (PyWii, `hbc.py` protocol, the SDKs, channel code on the PC) | `python3 -m unittest discover -s tests/host -p 'test_*.py'` | Python, a C compiler |
| Release version fields agree | `python3 tests/host/release_version.py` | Python |
| Reload stub memory layout | `tests/host/stub_layout.sh` | built DOL |
| Host tool bounds | `tests/host/host_bounds.sh` | C compiler |
| Boot in Dolphin | `python3 tests/dolphin/dolphin_smoke.py` | Dolphin |
| Developer network in Dolphin | `make -C tests/apps/netlog_app` then `python3 tests/dolphin/dolphin_smoke.py --devnet` | Dolphin |
| Installed WAD in Dolphin, including app exit back to it | `python3 tests/dolphin/dolphin_smoke.py --devnet channel/title/channel_retail.wad 120` | Dolphin, WAD |
| What the agent costs on a real Wii (memory, CPU, frame time) | `python3 tests/wii/agent_cost.py WII-IP` | Wii in any HBC |
| Overlay layout on the PC, every page | `python3 tests/host/overlay_preview/preview.py` (needs a C compiler and Pillow; writes PNGs) | C compiler |
| In-app agent in Dolphin: status, files, overlay, exit, Wiiload, crash report | `make -C tests/apps/agent_app` then `python3 tests/dolphin/dolphin_smoke.py --agent channel/title/channel_retail.wad 120` | Dolphin, WAD |
| The agent's way out past a stuck IOS ([docs](docs/devnet.md#the-way-out)): each fallback, from test bits | `make -C tests/apps/agent_app` then `python3 tests/dolphin/dolphin_way_out.py` | Dolphin, WAD |
| Developer network on a real Wii | `python3 tests/wii/wii_devnet.py WII-IP` | Wii in any HBC |
| The overlay with a MotionPlus remote: Test switches it on (the gyro, live), closing switches it back off | `python3 tests/wii/wii_mplus.py WII-IP [--chan N] [--hbc]` (`--hbc`: HBC's own HOME menu) | Wii in any HBC, a MotionPlus remote |
| What a Wii's DNS answers from a fresh network start (WiiLink's user agreement server, Pi-hole), read-only | queue `python tests/wii/wii_dns.py [NAME...]` (build `tests/apps/dnscheck` first) | the bench queue |
| Why User Agreements will not open: the hidden EULA channel (HAKx), the IOS it needs, and IOS56, read-only | queue `python tests/wii/wii_eulacheck.py` (build `tests/apps/eulacheck` first) | the bench queue |
| Message Board library (`sdk/mboard`) on the PC: posts, the full message, sending, play history, byte for byte | `python3 tests/host/test_mboard.py`, `python3 tests/host/test_cdblog.py` | C compiler |
| Message Board library with the real Wii Menu 4.3U filing the post (`--full` for the Mii, photo and board data) | `python3 tests/dolphin/dolphin_mboard.py --restore` (build `tests/apps/mboard_app` first) | Dolphin, the Wii Menu profile |
| Message Board stationery (a letter's border, board icon, music): every encoder read back, the five example themes | `python3 tests/host/test_stationery.py` | Pillow |
| Message Board library on the bench Wii: `backup`, `id`, `post`, `full`, `send WII`, `board`, `friends`, `restore` | queue `python tests/wii/wii_mboard.py STEP` (it refuses any other Wii) | the bench queue |
| Mii library (`sdk/mii`) on the PC: every field and limit, IDs, CRCs, the database, the hidden database and a Remote's Miis byte for byte (`MII_REAL_DB` adds a Wii's own `RFL_DB.dat`) | `python3 tests/host/test_mii.py` | C compiler |
| Mii library in Dolphin: a database on its NAND (add, hidden, remove) and the emulated Wii Remote's Miis, each written and read back | `python3 tests/dolphin/dolphin_mii.py` (build `tests/apps/mii_app` first) | Dolphin |
| Mii drawing (`sdk/mii/miidraw.c`) on the PC: a synthetic face resource builds a Mii with every part and each expression's face where RFL puts it, a damaged one is refused (`MII_REAL_RES` and `MII_REAL_DB` add your own files: every Mii builds) | `python3 tests/host/test_miidraw.py` | C compiler |
| Mii effects (`sdk/mii/miifx.c`) on the PC: synthetic REFF and REFT files play each feature (forms, rate, curves, fields, the flip-book, child emitters, the pools), damaged or cut files are refused (`MII_REAL_EFFECTS`, a folder with your own `effect_nigaoe.breff` and `.breft`, adds the Mii Channel's) | `python3 tests/host/test_miifx.py` | C compiler |
| HBC's avatar on the bench Wii: ready, 60 frames a second for a minute, ten TV pictures | queue `python tests/wii/wii_avatar.py` | the bench queue |
| Mii drawing on a Wii: `mii_icon` (normal, smiling) and a full-body `mii_capture`, saved to `sd:/mii_test` and checked | `mii_app.dol draw [SLOT]` (Dolphin: with `Graphics.Hacks.EFBToTextureEnable=False`) | Wii or Dolphin |
| Mii library on the bench Wii: `backup`, `check`, `add`, `remove ID`, `edit ID FIELD=N...`, `hidden`, `remote-add ID`, `remote-remove ID`, `remote-rewrite`, `restore` | queue `python tests/wii/wii_mii.py STEP` (build `tests/apps/mii_app`, `tests/apps/nandget` and `tests/apps/nandput` first; it refuses any other Wii) | the bench queue |
| Upload popups close themselves and report back (bad file, unanswered ZIP, `--yes`) | `python3 tests/wii/wii_upload_popups.py WII-IP` | Wii in any HBC |
| The agent and MEM2: nothing written into HBC's records while an app runs, the log target kept as it stops, the agent's memory at the top of the MEM2 arena | `python3 tests/wii/wii_agent_mem2.py WII-IP` (`--dol` for another build) | Wii in any HBC |
| `hbc_agent_stop()` (then an IOS reload, a re-init, a crash) and `hbc_agent_listen()` (the app's own server on 4299) on a real Wii | `make -C tests/apps/agent_app`, then `python3 tests/wii/wii_agent_stop.py --wii WII-IP` (`--dol` for another build) | Wii in any HBC |
| File times and flags from HBC and an app's agent (libogc 3, libogc2), and a re-sync that reads nothing | `python3 tests/wii/wii_file_meta.py WII-IP` | Wii in any HBC |
| HOME > HBC > Restart network (keys checked against `overlay_ui` before each A) | `python3 tests/wii/wii_net_restart.py WII-IP` | Wii in any HBC |
| Network start budget and cancellation, and cleanup supervision, on the PC | `python3 tests/host/test_netinit.py`, `python3 tests/host/test_netrecovery.py` | C compiler |
| A failed start cleaned up and retried with no input and no reload | build with `EXTRA_CFLAGS=-DHBC_TEST_INIT_FAILURE`, then `python3 tests/dolphin/dolphin_network_retry.py IMAGE` | Dolphin; never on a Wii |
| A network cleanup that hangs: HBC carries on without a reload; with `--reload` (`net-hang reload`), a fresh IOS and the installed channel again, no loop | build the WAD with `EXTRA_CFLAGS=-DHBC_TEST_STUCK_NET_CLEANUP`, then `python3 tests/dolphin/dolphin_network_recovery.py [--reload] WAD` | Dolphin; never on a Wii |
| File errors from HBC and an app's agent: a missing folder, a missing local file, a 400 MB upload to a card without room (`ENOSPC` at once) | `python3 tests/wii/wii_file_errors.py WII-IP` (SD with under 400 MB free), or `--dolphin` | Wii in any HBC, or Dolphin |
| The agent's safety tools on a real Wii (assert, abort, deadlock, failed malloc, frame pacing, stub guard, Reset, stack overflow) | `make -C tests/apps/agent_app`, then `python3 tests/wii/wii_agent_safety.py --wii WII-IP` | Wii in any HBC |
| DEV > Info and `hbc.py hw` on a real Wii | `python3 tests/wii/wii_info.py WII-IP` | Wii in any HBC |
| Installed channel on a real Wii | `python3 tests/wii/wii_devnet.py --installed --expect 1.10.7 WII-IP` | installed channel running |
| Every `meta.xml` option through the menu, driven by a scripted Wii Remote: arguments, `ahb_access`, `no_ios_reload`, author/coder, version, descriptions, icon, the Name, Date and Custom sorts, Load as the dialog's default | `python3 tests/dolphin/dolphin_meta.py [OUTDIR]` | Dolphin (no other test Dolphin running) |
| App list while files arrive (one in-place reload, no page slides) and `hbc.py order` (file requests only) | `python3 tests/wii/wii_app_list.py WII-IP` | Wii in any HBC |
| Both USB drives through HBC: list, write and read back 1 MiB on each (file requests only) | `python3 tests/wii/wii_usb2.py WII-IP` | Wii in any HBC, two USB drives |
| HOME overlay frame time in HBC, first opening and later (HOME presses only) | `python3 tests/wii/wii_overlay_speed.py WII-IP`; right after an app returns: `python3 tests/wii/wii_overlay_return.py WII-IP` | Wii in any HBC |
| In-app agent on a real Wii, with its speed next to HBC's | `python3 tests/wii/wii_agent.py WII-IP` | Wii in any HBC |
| Throughput on a real Wii | `python3 tests/wii/wii_netbench.py WII-IP` | Wii in any HBC |
| MEM1, MEM2 and locked-cache speed on a real Wii | `make -C tests/apps/membench`, then `python3 tools/hbc.py run tests/apps/membench/membench.dol sd:/path/to/sample` | Wii in any HBC |
| Start an installed title from HBC | `python3 tools/hbc.py send tests/apps/launch_title/launch_title.dol 000100014f484243` | Wii in any HBC |

The Dolphin tests use a throwaway profile, pass every setting on the command
line, and delete the profile when they pass. They find Dolphin with
`tools/wii_paths.py`: on PATH or in the standard install, or name it once in
`DOLPHIN` or `~/.wii-paths.json` (`{"DOLPHIN": "D:/emu/Dolphin/Dolphin.exe"}`).
`python tools/wii_paths.py --all` shows what this workstation has. Dolphin uses
the PC's own sockets, so it cannot show the Wii's network quirks; measure speed
and network reliability on hardware.

The real-Wii tests send the DOL over Wiiload, clean up after themselves on
the SD card (`sd:/hbctest`, `sd:/hbcbench`), and leave the Wii in HBC. They
need inbound TCP on the log port (`--log-port`) allowed on the PC.

When several people or agents share one Wii, queue hardware jobs through the
bench queue: one queue and one dispatcher, which starts a job only once HBC has
been idle for 20 s. Workstations on Windows, Linux and macOS take turns through a
lease server in Docker. The queue lives in the wii-bench repository, cloned next
to this one; its `bench/README.md` has the steps for the server and for each
workstation, and `wiibench.py setup` writes the workstation's shim that every
project calls:

```sh
bench=$(python3 tools/wii_paths.py wii_bench)
python3 "$bench" add --name "devnet" --cwd . -- python3 tests/wii/wii_devnet.py --log-port 4300
python3 "$bench" wait <id>
```

## Rules for changes

- [CI](.github/workflows/ci.yml) builds the DOL, banner, and TMD in the
  `devkitpro/devkitppc` container with warnings as errors. It also builds the
  host tools and runs the Python tests on Windows, Linux, and macOS. Pass
  `EXTRA_CFLAGS=-Werror` to any `make` to check locally.
- Each push bumps both version fields once, `CHANNEL_VERSION_STR` in
  `channel/channelapp/config.h` and `CHANNEL_VERSION` in
  `channel/title/Makefile`, to the same release. Between bumps HBC reports
  the release plus `+N.gHASH` (commits since the bump and git's hash, from
  `build_tag.h`), so each test build still has its own version.
- Text is LF everywhere, on every platform (`.gitattributes`). A few upstream
  files kept exactly as they came are marked `-text` there and keep CRLF. A
  checkout made before 1.8.3 on Windows still has CRLF files; run
  `python tools/fix_line_endings.py` once (it keeps uncommitted changes).
- Never commit Wii keys. Turn on the pre-push check once per clone:
  `git config core.hooksPath tools/hooks`. For each pushed ref it runs
  [`tools/check_secrets.py`](tools/check_secrets.py), which reads every file
  added or changed in the pushed commits (so a key added and later deleted is
  still caught) and stops the push on: anything under `keys/` or named like a
  Wii key or console dump (`*.pem`, `*.key`, `nand*.bin`, `otp*.bin`,
  `seeprom.bin`, ...), a private key header, a GitHub or other well-known
  token, and the bytes of any file in your local `keys/` folder (raw or hex;
  it never prints a value, only the file's name). `--staged` checks before a
  commit, `--all` the whole tree. A public key or test vector that only looks
  secret goes in `tools/check_secrets_allow.txt` as `path  # reason`.
- [`AGENTS.md`](AGENTS.md) and the
  [project skill](.agents/skills/hbc-build-and-review/SKILL.md) hold the
  detailed build, Dolphin, and hardware procedures;
  [`docs/research/return-path.md`](docs/research/return-path.md) covers the reload stub and return
  path.
