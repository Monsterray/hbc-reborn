# Project review

How hbc-reborn is reviewed, what is still open, and where each release's evidence is.

- **Open items** are here: work noted for later, with what triggered it.
- **Findings and hardware results** for each release are in [history/](history/), one file
  per minor version, newest first.
- **A short list of changes** per version is in [CHANGELOG.md](../CHANGELOG.md).

Record a finding as trigger, effect, smallest safe fix, and the check that shows the fix
works, in the history file of the version that fixes it.

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

## Open items

### A USB keyboard library that works

The USB keyboard (1.14.6, PR 15) works since 1.14.8 only because HBC stopped
using libogc's libwiikeyboard, which failed on the bench Wii:

- It opens only the first boot keyboard interface. The bench's Chicony
  (04f2:0841) lists its silent one first, so it never typed.
- `USBKeyboard_Scan` blocks in IOS until a key report, so a shutdown that waits
  for the reader hangs until a key is pressed.
- The caller must have run `USB_Initialize`, and nothing says so.
- No SET_IDLE, no hot-plug notice, only polling.

`channel/channelapp/source/keyboard.c` works around all four: it reads every
boot keyboard interface with async interrupt requests and cancels them at
shutdown. Either turn that into a full keyboard SDK in `sdk/` (like
`sdk/mboard`: key events with modifiers, repeat and layouts, a host-tested
core), or send libogc a fix PR that makes libwiikeyboard as robust. Then HBC
and other apps can use it.

### The keyboard host test fails since the USB worker thread

`tests/host/test_keyboard.py` stopped compiling at d37bbd5 (2026-11-06), which
moved USB setup and the device list onto a thread: its stub headers have no
`lwp_t`, `LWP_CreateThread` or `LWP_JoinThread`, and its checks expect
`keyboard_scan()` to look for devices itself, which the worker does now. Give
the stub a real thread (or run `kbd_worker`'s look from a stub hook) and wait
for the look in the test. Found 2026-11-07 while moving the tests; the move
did not cause it.

### More keyboard keys

The keyboard works but needs a lot more bindings to feel right (Monty,
2026-11-02). The HOME menu inside apps does not read it yet.

### At the next minor release (1.16.0)

- Delete the forwarding headers `sdk/hbc_agent.h` and `sdk/hbc_netlog.h`
  (the real ones are in `sdk/hbc_agent/` and `sdk/hbc_netlog/` since November
  2026). First check that no project on the bench still includes them: build
  each one and look for the forwarders' `#pragma message` note.

### Data to collect

Measurements a doc quotes that are out of date. Each needs a run, not a guess.

- **The agent's cost.** "What it costs" in
  [sdk/hbc_agent/README.md](../sdk/hbc_agent/README.md#what-it-costs) was
  measured on HBC 1.7.1. The HOME overlay has gained pages and features since.
  Re-run `tests/wii/agent_cost.py` on the bench Wii (queued) and in the build, and
  update the README's figures and the 1.8 MB overlay figure for
  `hbc_agent_home()` against `hbc_agent_home_fb()`.

### From the first review (1.1.x)

Carried over from the 1.1.x review and not re-checked since; some may be done.

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

## Findings by release

| Version | Sections | Range |
| --- | --- | --- |
| [1.15](history/1.15.md) | 13 | 1.15.0 to 1.15.12 |
| [1.14](history/1.14.md) | 17 | 1.14.0 to 1.14.31 |
| [1.13](history/1.13.md) | 2 | 1.13.0 |
| [1.12](history/1.12.md) | 3 | 1.12.0 to 1.12.2 |
| [1.11](history/1.11.md) | 10 | 1.11.0 to 1.11.10 |
| [1.10](history/1.10.md) | 23 | 1.10.0 to 1.10.26 |
| [1.9](history/1.9.md) | 10 | 1.9.0 to 1.9.9 |
| [1.8](history/1.8.md) | 10 | 1.8.0 to 1.8.9 |
| [1.7](history/1.7.md) | 7 | 1.7.0 to 1.7.6 |
| [1.6](history/1.6.md) | 2 | 1.6.0 to 1.6.2 |
| [1.5](history/1.5.md) | 1 | 1.5.0 |
| [1.4](history/1.4.md) | 2 | 1.4.0 to 1.4.1 |
| [1.3](history/1.3.md) | 2 | 1.3.0 to 1.3.1 |
| [1.2](history/1.2.md) | 2 | 1.2.0 to 1.2.2 |
| [1.1](history/1.1.md) | 3 | 1.1.7 to 1.1.9 |
