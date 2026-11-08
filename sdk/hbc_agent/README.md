# hbc_agent

*HBC's developer tools, kept working inside your running app.*

| | |
| --- | --- |
| **Files** | [`hbc_agent.h`](hbc_agent.h) and `libhbcagent.a` (build it with `make -C sdk/hbc_agent`) |
| **Needs** | libogc 3.x (or libogc2, or libogc 1.x), libfat, zlib, a Wii on the same network as the PC; `-lwiiuse -lbte` for the HOME overlay |
| **Licence** | GPL v2 or later (see [below](#licence)) |
| **Changes on the Wii** | Nothing lasting, but it opens TCP port 4299 (see [below](#what-it-changes-on-the-wii)) |
| **Since** | HBC 1.5.0 |

**Contents:** [What it does](#what-it-does) · [Quick start](#quick-start) ·
[Add it to your app](#add-it-to-your-app) · [API](#api) ·
[Safety tools](#safety-tools) · [The HOME overlay](#the-home-overlay) ·
[What it costs](#what-it-costs) ·
[What it changes on the Wii](#what-it-changes-on-the-wii) · [Limits](#limits) ·
[Licence](#licence)

## What it does

Link the agent into a development build and `tools/hbc.py` keeps working
while your app runs:

- `status`, the file commands and `log` reach your app instead of HBC.
- `run` and `send` ask the app to exit to HBC before sending the next build.
- A crash comes back to the PC as a report, not a frozen screen.
- It guards the app against a stack overflow, `assert()` and `abort()`, and a
  hang, and writes the SD and USB caches back at exit.
- It can show a HOME overlay in HBC's style over your game's last frame.

## Quick start

```sh
make -C sdk/hbc_agent            # builds sdk/hbc_agent/libhbcagent.a
```

```c
#include <fat.h>
#include "hbc_netlog.h"          // optional: stdout to the PC
#include "hbc_agent.h"

int main(int argc, char **argv) {
    // ... video setup ...
    fatInitDefault();            // mount first: the agent serves what the app mounted
    hbc_netlog_init();
    hbc_agent_init(NULL);        // one low-priority thread, about 20 KiB while idle
    while (!hbc_agent_exit_requested()) {
        // ... one frame ...
    }
    return 0;                    // back to HBC
}
```

Then, on the PC:

```sh
python3 tools/hbc.py run myapp.dol                 # again and again: each run replaces the last
python3 tools/hbc.py status                        # the app's name, uptime, memory, agent stack
python3 tools/hbc.py get sd:/apps/myapp/save.dat   # while the app runs
python3 tools/hbc.py crash --elf myapp.elf         # after a crash: where it happened
```

[`tests/agent_app`](../../tests/agent_app) is a complete example, and HBC's own
HOME menu ([`channel/channelapp/source/home.c`](../../channel/channelapp/source/home.c))
is a commented one.

## Add it to your app

Compile with `-I<hbc-reborn>/sdk/hbc_agent -I<hbc-reborn>/sdk/hbc_netlog` and
link with:

```
-L<hbc-reborn>/sdk/hbc_agent -lhbcagent -lfat -lz -logc
```

The old `-I<hbc-reborn>/sdk` still works for now, through a forwarding header
at `sdk/hbc_agent.h` (and `sdk/hbc_netlog.h`). HBC 1.16.0 deletes the forwarders; until then a build that uses them prints a note.

- **Other libogcs.** The command above is for devkitPro's current libogc
  (3.x). For an app on libogc2 (Extrems' fork) or libogc 1.x, build the agent
  against that libogc, with the devkitPPC the app uses, and link the copy it
  makes:

  ```sh
  make -C sdk/hbc_agent OGC=libogc2        # sdk/hbc_agent/libogc2/libhbcagent.a
  make -C sdk/hbc_agent OGC=libogc-1.8.23  # sdk/hbc_agent/libogc-1.8.23/libhbcagent.a
  ```

  Everything works the same there, crash reports included (the agent adds its
  own exception entry, `sdk/hbc_agent/ogc_exc.S`, to the older libogc's
  exception table). The one difference: those libogcs' WPAD has no pairing
  call, so the overlay's Connect asks for the Wii's SYNC button.
- **The network.** If your app calls `net_init()` itself, call it before
  `hbc_agent_init()`, or call `hbc_agent_net_wait()` first. libogc's
  `net_init()` hangs if it runs while another thread is starting the network.
  `hbc_netlog_init()` already waits for a start-up in progress.
- **The HOME overlay.** Link `-lwiiuse -lbte` (which WPAD apps already use).
- **Leaving another way.** An app that leaves without `exit()` (a loader
  jumping to a game) calls `hbc_agent_stop()` first.
- **The PC.** It must reach the Wii on TCP port 4299 (Wiiload's port).

## API

All in [`hbc_agent.h`](hbc_agent.h). Define `HBC_AGENT_LAYOUT_ONLY` before the
include to get only the crash block and kept-log layouts, as HBC itself does.

### Functions

| Name | What it does |
| --- | --- |
| `s32 hbc_agent_init(const hbc_agent_config *cfg)` | Starts the agent. `cfg` may be NULL for the defaults. Returns 0, or a negative error when the thread cannot start (then nothing of it stays); `-EALREADY` while it runs. After `hbc_agent_stop()` it starts again. |
| `s32 hbc_agent_stop(void)` | Undoes `hbc_agent_init()` completely, before the app hands the console to something else without `exit()`: threads ended, listening socket and connection closed (a transfer in progress fails for its client), hang watchdog off, hooks restored (vertical blank callback, stack breakpoint, Reset and Power, reset function, stdout and stderr, crash handler; a hook the app replaced since keeps the app's), overlay sounds and threads gone. It does not unmount, flush or shut down the network, write a crash report or kept log, or return to HBC. Call it from any app thread with interrupts on, never from the overlay's callbacks. Returns within about a second: 0, or `-ETIMEDOUT` when an agent thread was blocked in IOS (everything else is still undone). Calling it twice, or before `hbc_agent_init()`, returns 0. Afterwards `hbc_agent_alive()` and `hbc_agent_hold()` do nothing, `hbc_agent_exit_requested()` and `hbc_agent_home_pending()` are false and `hbc_agent_home()` returns `-ENODEV`, until `hbc_agent_init()` again. |
| `HBC_AGENT_HAS_STOP` | Defined when this header has `hbc_agent_stop()` and `hbc_agent_listen()`. Use `#ifdef` for apps that build against older copies. |
| `s32 hbc_agent_listen(bool on)` | The DEV > "hbc.py connection" switch, for the app. `false` closes the listening socket on TCP 4299 (ending a transfer in progress with an error for its client) and leaves the network as the app has it, until `true`. Returns once the port is free: 0, or `-ETIMEDOUT` after about a second. `true` listens again on the agent's next loop and returns at once. Does nothing and returns 0 before `hbc_agent_init()`, after `hbc_agent_stop()`, or with `no_network`. |
| `bool hbc_agent_exit_requested(void)` | True once `hbc.py exit` or `hbc.py run` asked the app to exit. |
| `s32 hbc_agent_net_wait(u32 ms)` | Waits up to `ms` for the network: 0 when it is up, else a negative error. |
| `void hbc_agent_fatal(u32 code, const char *fmt, ...)` | Stops the app on a failure that is not a CPU exception and reports it like one (`hbc.py crash`): the caller's backtrace, your `code` (never interpreted) and a printf-style reason cut to 63 characters, also printed so it ends the kept log. Then calls `exit(0)`, so `atexit` handlers and the SD and USB caches still run; if `exit()` has not reloaded HBC 15 s later, the monitor thread takes [the way out](#the-way-out). Does not return. Works before `hbc_agent_init()`, but without the monitor. |
| `void hbc_agent_fatal_now(u32 code, const char *fmt, ...)` | The same, straight to the way out, for when the app's state cannot be trusted (a lock may be held, or IOS may be stuck). Does not return. |
| `void hbc_agent_alive(void)` | Arms the hang watchdog on the first call; after that, a gap of more than `hang_s` seconds between two calls is a hang. Call it wherever the app makes progress; once a frame is fine. |
| `void hbc_agent_hold(bool hold)` | Pauses the watchdog for a long wait the app expects (a load, a network listing). Holds nest. |
| `s32 hbc_agent_home(const struct _gx_rmodeobj *rmode)` | Opens the HOME overlay, runs its own loop until the user closes it, and returns 0, or a negative error when there is no memory. `rmode` may be NULL for the preferred video mode. Needs `WPAD_Init()`. |
| `s32 hbc_agent_home_fb(rmode, void *fb0, void *fb1)` | The same, drawing into framebuffers the app lends it: each at least `fbWidth * xfbHeight * 2` bytes, 32-byte aligned, in MEM1 or MEM2, and not the one on screen. `fb1` may be NULL (one buffer; the menus may tear while they slide). The overlay reads the app's frame in place and needs no memory of its own. |
| `bool hbc_agent_home_pending(void)` | True once when `hbc.py key h` asked for the overlay. |
| `void hbc_agent_push_keys(const char *keys)` | Presses overlay keys as `hbc.py key` does: each of `udlrabh12w` is up, down, left, right, A, B, HOME, 1, 2 and WAV, one every 12 frames; other characters are skipped. For another input device, such as a USB keyboard read from `on_frame`. `HBC_AGENT_HAS_PUSH_KEYS` is defined when it exists. |
| `void hbc_agent_toast(const char *msg)` | Shows a short message at the top of the overlay for a few seconds. From `on_save` or a slot item it replaces the overlay's own "Saved" or "Couldn't save". |
| `void hbc_agent_set_slot(int slot, const char *label, void (*press)(void *user), void *user)` | Sets one of the two bar buttons beside Exit: slot 0 on its left (blank by default), slot 1 on its right (Shot by default). `press` runs in the app's thread after the overlay closes. A NULL `label` restores the default. |
| `void hbc_agent_set_slot_menu(int slot, const char *label, const char *title, const hbc_agent_item *items, int count)` | Gives a slot a menu instead of one action. Up to 12 items, shown in order; info rows (`value` set) take a whole row, buttons pair up two to a row. The overlay reads `items[]` and each `value` every frame, so keep them alive and update the strings in place. |
| `bool hbc_agent_handle(s32 s, const u8 *hdr)` | For hosts with their own server (`no_network`): answers HBCK (`hbc.py key`) and HBCP (`hbc.py screen`) on an accepted connection whose 16-byte header is `hdr`. Returns false for any other request. The caller closes `s`. |
| `bool hbc_agent_calibration(int chan, hbc_agent_cal *out)` | Reads the calibration measured by WiiMote > More > Calibrate this session. |
| `void hbc_agent_remotes_scanned(void)` | Call after each `WPAD_ScanPads()`, for the sideways setting. |
| `u32 hbc_agent_remote_buttons(int chan, u32 buttons)` | Passes a remote's buttons (down, held or up) through the sideways turn. The remote's data format needs the accelerometer (`WPAD_FMT_BTNS_ACC` or `_ACC_IR`). |
| `bool hbc_agent_sideways_remotes(void)` | Whether the setting is on; read it to keep it longer than the session. |
| `bool hbc_agent_remote_sideways(int chan)` | Whether remote `chan`'s D-pad is being turned now (HBC shows it). |

### hbc_agent_config

Leave any field 0 (or NULL, false) for its default.

| Field | What it does |
| --- | --- |
| `const char *name`, `version` | Shown by `hbc.py status`. Name defaults to "app". |
| `s32 priority` | The agent thread's priority, 0 to 127 (higher runs first). Keep it below the app's threads. Default 40 (libogc's main thread runs at 64). |
| `s32 crash_reload_s` | Seconds libogc's crash screen stays up before returning to HBC. Default 3. Negative keeps libogc's own setting, which without `__exception_setreload()` leaves the screen up for good. |
| `bool app_polls_exit` | Set when the app calls `hbc_agent_exit_requested()` in its main loop: an exit request then waits up to `exit_grace_ms` for the app to exit by itself before the agent calls `exit(0)`. Without it, the agent calls `on_exit` and `exit(0)` at once. |
| `u32 exit_grace_ms` | The wait above. Default 5000. |
| `void (*on_exit)(void *user)`, `void *user` | Runs in the agent's thread before it calls `exit(0)`; save state here. `user` is passed to every callback. |
| `bool no_crash_handler` | Leaves the crash handler out. |
| `bool (*on_save)(void *user)` | Enables DEV > Save (greyed out when NULL). Returns whether it saved. |
| `void (*on_restart)(void *user)` | Enables DEV > Restart app (greyed out when NULL). Runs in the app's thread once the overlay has closed. |
| `bool no_network` | For a host with its own network server, such as HBC: no listener thread and no network start-up. DEV then leaves out the `hbc.py` connection and Log to PC switches. Hand HBCK and HBCP requests to `hbc_agent_handle()` to keep `hbc.py key` and `screen` working. |
| `bool gc_pads` | Also reads GameCube controllers in the overlay (START closes it, as HOME does). Set it only if the app called `PAD_Init()`. |
| `bool sideways_remotes` | Starts with Controller settings > Sideways remote on. |
| `unsigned hide_exit_choices` | A mask of `(1 << choice)` for Exit choices (`HBC_AGENT_EXIT_*`) to leave out. |
| `bool (*on_exit_choice)(int choice, void *user)` | Runs in the app's thread once the overlay has closed. Returning true means the app took care of it (for example through its own shutdown); false lets the agent do it. |
| `void (*on_frame)(void *user)` | Runs once per overlay frame in the app's thread, for work the app's loop would do each frame (HBC keeps its network server accepting). Keep it short. |
| `u32 hang_s` | The hang watchdog's limit, once the app has called `hbc_agent_alive()`. Default 60. |
| `bool guard_reload_stub` | Opt-in. See [Safety tools](#safety-tools). |
| `bool track_memory` | Opt-in. See [Safety tools](#safety-tools). |
| `u32 no_safety` | `HBC_AGENT_NO_*` bits for default safety tools to leave out. |
| `u32 main_stack_size` | The main thread's stack, for its guard: libogc's 128 KiB unless the app gives `main()` its own (`__ppc_main_sp` with libogc 3); then its size here, or `HBC_AGENT_NO_STACK_GUARD`. |

### Constants

| Name | Value and meaning |
| --- | --- |
| `HBC_AGENT_NO_STACK_GUARD` | 1. The breakpoint and marker under main's stack. |
| `HBC_AGENT_NO_BUTTONS` | 2. Reset and Power stay libogc's. |
| `HBC_AGENT_NO_FLUSH` | 4. Leave SD and USB mounted at exit. |
| `HBC_AGENT_NO_FRAMES` | 8. No frame pacing (a vertical blank callback). |
| `HBC_AGENT_NO_THREADS` | 16. No thread list in the kept log. |
| `HBC_AGENT_NO_IOS_CHECK` | 32. Leave through the reload stub unchecked, as agents before 1.10.18 did: no descriptor closes, no ES launch, no reset. |
| `HBC_AGENT_EXIT_HBC` | Exit's choice: `exit(0)`, back through the reload stub. |
| `HBC_AGENT_EXIT_SYSTEM_MENU` | Exit's choice: the System Menu. |
| `HBC_AGENT_EXIT_RESTART` | Exit's choice: restart the Wii. |
| `HBC_AGENT_EXIT_POWER_OFF` | Exit's choice: power off. |
| `HBC_AGENT_ITEM_CLOSE` | 1. Slot menu item: close the overlay first, then run `press`. |
| `HBC_AGENT_ITEM_DISABLED` | 2. Slot menu item: greyed out. |

### Slot menu items and calibration

| Type | Fields |
| --- | --- |
| `hbc_agent_item` | `label`; `value` (non-NULL makes an info row showing this text); `press` (buttons; runs in the app's thread); `user`; `flags` (`HBC_AGENT_ITEM_*`). |
| `hbc_agent_cal` | `valid`, `motionplus`; `accel[3]` (raw accelerometer at rest); `gyro[3]` (raw MotionPlus rates at rest, its zero offsets); `stick[2]` (raw Nunchuk stick centre). |

### Crash report and kept log

These are for HBC and tools that read what the agent leaves in MEM2. Both
blocks are written only as the app stops, so an app's heap may cover them.
Each has a check word (`hbc_crash_check()`, `hbc_crash_way_check()`,
`hbc_lastlog_check()`) computed with `hbc_check_words()`.

| Name | Value and meaning |
| --- | --- |
| `HBC_CRASH_ADDR` | `0x91800020`. The crash block (`hbc_crash_block`), in MEM2 next to `hbc_netlog.h`'s kept copy: IOS clears low memory when the reload stub boots the installed channel, and HBC reads this before its own allocations could reach it. |
| `HBC_CRASH_MAGIC`, `HBC_CRASH_VERSION` | `0x48424343` ('HBCC'), 2. |
| `HBC_CRASH_FRAMES`, `HBC_CRASH_REASON` | 12 return addresses, 64 bytes of reason. |
| `HBC_CRASH_V1_WORDS`, `HBC_CRASH_V2_WORDS` | 29 and 47 words before a version 1 and a version 2 block's check. |
| `HBC_CRASH_WAY_MAGIC` | `0x57415931` ('WAY1'). The way-out fields follow version 2's check, so an older HBC still reads the block as version 2. They are valid when `way_magic` and `way_check` match. |
| `HBC_LASTLOG_ADDR` | `0x91800100`. The app's last output (`hbc_lastlog_block`), for `hbc.py lastlog`. |
| `HBC_LASTLOG_MAGIC`, `HBC_LASTLOG_VERSION`, `HBC_LASTLOG_SIZE` | `0x4842434c` ('HBCL'), 1, 4096 bytes of text. |

What stopped the app (`hbc_crash_block.kind`):

| Name | Meaning |
| --- | --- |
| `HBC_CRASH_EXCEPTION` (0) | A CPU exception; `exception` says which. |
| `HBC_CRASH_FATAL` (1) | `hbc_agent_fatal()`: the app's code and reason. |
| `HBC_CRASH_HANG` (2) | No `hbc_agent_alive()` for `hang_s` seconds. |
| `HBC_CRASH_ASSERT` (3) | `assert()` failed; the reason is `file:line: expression`. |
| `HBC_CRASH_ABORT` (4) | `abort()`. |
| `HBC_CRASH_STACK` (5) | The main thread overran its stack. |

Why the log was kept (`hbc_lastlog_block.why`): `HBC_LASTLOG_EXIT` (0, `exit()`
or a return from main), `_EXCEPTION` (1), `_FATAL` (2), `_HANG` (3), `_ABORT`
(4, `assert()` or `abort()`), `_STACK` (5), `_POWER` (6, the Power button).

How the agent left the app (`hbc_crash_block.way_out`):

| Name | Meaning |
| --- | --- |
| `HBC_WAY_NOT_CHECKED` (0) | The reload stub, unchecked: an exception, a check turned off, or an agent before 1.10.18. |
| `HBC_WAY_STUB` (1) | IOS closed every descriptor and ES refused every HBC title at once: the reload stub. |
| `HBC_WAY_ES_LAUNCH` (2) | IOS acked every close: HBC launched through ES directly, with time limits (1.10.24 on; before, only when a close got no reply). |
| `HBC_WAY_RESET` (3) | A system reset through `HW_RESETS` (AHBPROT; needs no IOS). |
| `HBC_WAY_STM_RESET` (4) | A system reset through `/dev/stm/immediate` (while IOS still answers). |
| `HBC_WAY_STUB_ANYWAY` (5) | No launch and no reset worked, or a reset came in the last 10 minutes: the reload stub anyway, which may wait for good. |

### The way out

`hbc_agent_fatal_now()`, the hang watchdog, `assert()`, `abort()` and the stack
guard's marker check all leave the same way. HBC's reload stub (0x80001800)
closes every IOS descriptor and launches HBC through ES, waiting on each step
for good, so an IOS that stopped answering used to leave the Wii frozen until
someone pulled the power. The agent now does the stub's closes itself first,
with interrupts off and a time limit on each (0.5 s for IOS's ack, 1 s for
the reply), and then, in this order:

1. IOS acked every close, whether or not each got its reply: HBC launched
   through ES here, with time limits, by the stub's title, then OHBC's, then
   LULZ's. IOS reloads and the Wii is back in HBC, with the crash block and
   kept log (`HBC_WAY_ES_LAUNCH`). Not the stub: its own launch has no time
   limit, and a real wedge (a 1 MiB DIP read through d2x cIOS, 2026-10-25)
   answered every close and then froze it.
2. Every close answered and ES refused every title at once (no HBC
   installed): IOS is well, so the reload stub (`HBC_WAY_STUB`).
3. No launch went ahead, while IOS still acks: `/dev/stm/immediate`'s hot
   reset, which IOS takes, so it can end a NAND write (`HBC_WAY_STM_RESET`).
4. Still here, or IOS acked nothing: with AHBPROT open (the app's `meta.xml`
   has `<ahb_access/>`, and any IOS it reloaded kept it), and not on a vWii, a
   system reset through `HW_RESETS` bit 0, which needs no IOS and cuts off any
   NAND write (`HBC_WAY_RESET`).
5. The reload stub anyway, which may wait for good (`HBC_WAY_STUB_ANYWAY`).

A reset reboots the whole console as a power-on does: boot2, then the System
Menu, or Priiloader and what it is set to boot (the agent asks it for
nothing). Without Priiloader the Wii stops in the System Menu: the resets are
the last resort, for a Wii that would otherwise stay frozen. No reset is
tried within 10 minutes of the last one (the crash block's `reset_s`, read at
`hbc_agent_init()`), so a Wii whose autoboot wedges at once stops instead of
looping. The kept log outlived an `HW_RESETS` reset on the bench Wii
(2026-10-24); the crash block across a reset is not shown yet. Where no stub
was found and nothing reset the Wii, the agent stops, its kept log written.

Each step is written to the crash block's `way_out` and to the kept log before
it is taken, and drawn on the TV in a black panel at the top left of the
picture the VI shows, which a power cycle does not lose (1.10.24 on). A
healthy IOS answers the 32 closes in about 25 ms. An exception still leaves
through libogc's crash screen and the stub, unchecked: interrupts are off
there. `HBC_AGENT_NO_IOS_CHECK` turns the check off.

## Safety tools

The agent also guards the app, at no cost until something goes wrong. They are
on unless the app turns them off with `no_safety`.

- **Stack overflow.** The CPU's data breakpoint watches the main thread's
  stack 1 KiB above its bottom; the first write there stops the app with a
  `stack` report at the exact instruction. Markers below it catch a frame big
  enough to step over it.
- **`assert()` and `abort()`** come back as reports (`assert` with
  `file:line: expression`) instead of a frozen screen.
- **Reset and Power**, when the app left libogc's defaults: Reset exits to
  HBC, Power switches off after the app's `on_exit`.
- **SD and USB are written back** when the app exits, after its own `atexit`
  handlers, so a quick exit does not lose a save.
- **Frame pacing** in `hbc.py status` (`safety.frames`): frames a second, the
  longest frame, frames that ran late.
- **Every thread's state** at the end of the kept log after a crash, fatal,
  hang or abort: priority, state, stack left and where it is waiting.

Two cost a little time each second, so an app asks for them. A 4 KiB thread at
the highest priority (the hang watchdog's) checks each once a second:

- `guard_reload_stub` keeps a 6 KiB copy of the loader's reload stub
  (0x80001800, how `exit()` gets back to HBC) and puts it back before the app
  exits or crashes, so an app that overwrites low memory still gets back to
  HBC. `hbc.py` HBCS "safety" says when it was first found changed. Leave it
  off for an app that installs a stub of its own on purpose.
- `track_memory` records the lowest free MEM1, MEM2 and heap, for HBCS and
  the end of the kept log. Linking with
  `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign` also counts
  failed allocations with their size and caller.

`no_safety` leaves out any of the defaults (the `HBC_AGENT_NO_*` bits above).
[docs/devnet.md](../../docs/devnet.md) has the details and `tests/agent_cost.py`
measures what they cost.

The hang watchdog is a thread at the highest priority, so a spinning app
thread cannot hold it off; a hang with interrupts disabled stops it too. It
pauses while the HOME overlay is open and while `hbc_agent_hold(true)` is in
force. After a crash the agent shows libogc's crash screen for 3 s
(`crash_reload_s`) and returns to HBC, which keeps the report until you read
or clear it; `run` prints it and exits with status 3.

## The HOME overlay

Call `hbc_agent_home()` when HOME is pressed, and the agent shows its overlay
over your game's last frame, in the Homebrew Channel's style:

```c
WPAD_ScanPads();
if ((WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) || hbc_agent_home_pending())
    hbc_agent_home(rmode);            // returns when the user closes it
```

On a GameCube controller, HBC opens HOME with Z and START together, so START
alone stays a game's pause button; do the same with
`(PAD_ButtonsDown(0) & PAD_BUTTON_START) && (PAD_ButtonsHeld(0) & PAD_TRIGGER_Z)`
and set `gc_pads` so the overlay reads the controller too.

A status strip shows the app, the clock, and the four remotes' player LEDs and
batteries, over five buttons:

| Button | What it does |
| --- | --- |
| DEV | Actions: Restart app, Pause, Save, Log (the app's recent output), Reset remotes, Sync clock (sets the Wii's clock from an NTP server, keeping its time zone), Log to PC, the crash screen's time, and the `hbc.py` connection. Info: time, play time, network, SD space, the app, and MEM1 and MEM2 free, used and total in KB, then five pages read when one opens: System (console and model, region, serial, console ID, Wii Menu, IOS and AHBPROT, boot2, CPU, Hollywood revision, language), Video (mode, cable, TV type, 16:9, 480p, PAL 60, sound, sensor bar, paired remotes), Storage (SD and USB size and free space, NAND free, GameCube memory cards), USB (each device on the ports, by vendor and product ID) and Network (IP and netmask, gateway, DNS, MAC, the Wii Menu connection in use). |
| (app slot) | Whatever the app puts there with `hbc_agent_set_slot(0, ...)`; blank otherwise. |
| Exit | The Homebrew Channel, System Menu, Restart Wii, Power off. |
| Shot | Saves the game's frame to `sd:/screenshots/<app>-NNN.bmp`. An app can replace it with `hbc_agent_set_slot(1, ...)`. |
| WiiMote | A card per remote (Find: a chime that gets louder; More: battery, extension, MotionPlus, rumble on or off, speaker volume with a chirp, Test with an ADPCM and a PCM tune and a WAV player for `speaker.wav` on SD or USB, Calibrate, Disconnect; Test and Calibrate switch a MotionPlus on to show and measure its gyro, and switch it back off as they close, so the app gets the remote back as it left it), and Settings: connect a remote, disconnect all, sensor bar, IR sensitivity, auto power-off, rumble for all. |

Set `on_save` and `on_restart` in `hbc_agent_config` to enable Save and
Restart app.

An app can give a slot a menu of its own instead of one action
(`hbc_agent_set_slot_menu`), with buttons and live info rows. HBC's own HOME
menu is this overlay, set up in
[`channel/channelapp/source/home.c`](../../channel/channelapp/source/home.c): a
commented example of every step, from the config to lending the overlay its
framebuffers and handling Exit through HBC's own shutdown. Its HBC slot holds
what the old HOME menu had: HBC's and IOS's versions, the Wii's IP, About,
Launch BootMii, Exit to System Menu, and Shutdown, plus Restart network, which
shuts the network down and joins it again (for a Wi-Fi connection that went
stale), and Save Mii faces, which copies the Wii Menu's Mii face models
(`RFL_Res.dat`) and the Mii Channel's male and female bodies with their
animations (`maleBody.brres`, `femaleBody.brres`) and effects
(`effect_nigaoe.breff`, `.breft`) to `sd:/mii` for apps that draw Miis
([docs/mii/README.md](../../docs/mii/README.md#the-face-models-rfl_resdat)).

When the network fails to start, HBC tries again by itself. Each start has 30
seconds in all, and Restart network or launching an app cancels one that IOS
never finishes. Shutting the network down can itself hang inside IOS (libogc's
`net_deinit` waits for a reply that never comes); if it has not returned after
5 seconds, HBC says so for 10 seconds and then starts the installed channel
again with a fresh IOS.

### Pointer and D-pad

Point a Wii Remote at the screen and HBC's hand cursor follows it, turning as
you twist the remote: the button under it lights up and A presses it, and
pointing at a bar button while a menu is open switches straight to that menu.
The overlay turns on IR for every connected remote while it is open and gives
each back its previous mode when it closes. The D-pad moves the highlight when
no remote points at the screen, A chooses, B goes back one level, and HOME
closes everything. A GameCube controller's START closes the overlay, like HOME.

### Sideways remote

Controller settings > Sideways remote (off by default; `sideways_remotes` in
the config turns it on to start) is for a Wii Remote held sideways with its
D-pad at the left: tilt the buttons towards you and the D-pad turns to match,
Up acting as Left, Right as Up, and so on. A remote pointing at the screen, or
held upright, reads as usual; lying flat keeps the last way it was held. It
turns after about a sixth of a second held the new way, and back after about
half a second, so a brief twist or a glimpse of the sensor bar does not flip
it. The overlay always follows it; an app's own menus can too, by calling
`hbc_agent_remotes_scanned()` after `WPAD_ScanPads()` and passing a remote's
buttons through `hbc_agent_remote_buttons(chan, buttons)`, as HBC's app list
does, with a data format that includes the accelerometer. Classic and
GameCube controllers are never turned. HBC keeps this one setting in its
settings.xml.

### Controller settings

Controller settings last until the app exits; nothing is written to the Wii's
own settings, except that Connect remote may save the new remote in the Wii's
pairing list as the Sync button would, and Reconnect in player order saves the
new order. Reconnect works like the Wii Menu's: every remote disconnects, then
each one you press 1 and 2 on (any button, for a paired one) becomes the next
player, 1 to 4; Done, a fourth remote, or closing the overlay ends it (libogc
3 builds only).

While open, the overlay borrows three framebuffers, about 1.8 MB at 640x480
(none with `hbc_agent_home_fb()`).

## What it costs

Measured on a real Wii with `tests/agent_cost.py` (HBC 1.7.1, 640x480):

| | Cost |
| --- | --- |
| Code | 25.6 KB for the agent (network, files, crash reports), 77 KB more for the overlay (of which about 30 KB are its fonts and pointer) |
| Static data | 17 KB for the agent (half of it the Log page's 8 KB of recent output), 21 KB for the overlay (its threads' stacks and state) |
| Start-up | `hbc_agent_init()`: 13 KB of heap and 16 KB of MEM1 arena (its thread's 12 KB stack, of which it uses 3 to 4.5 KB) |
| Idle | 5 wake-ups a second to check for a connection, 140 µs each: 0.07% of the CPU |
| File transfers | about 28 KB of heap kept after the first (the rest is freed after each); zlib and CRC work of 9 to 130 ms per upload and 50 to 620 ms per download of 2 to 5 MB, on the agent's thread |
| HOME overlay, while open | 1.8 MB: a copy of the frame and two framebuffers (HBC lends the framebuffers from low MEM1); 7.9 ms average and 14 ms at most to draw a frame that changed, nothing for frames that did not, so it keeps 60 fps |
| HOME overlay, closed | nothing: no thread, no memory |

`hbc.py status` shows the live figures: `agent_idle_wakes`, `agent_idle_us`,
`agent_request_ms`, `agent_stack_used`, `heap_arena`, and `overlay` (frames
drawn, average and worst frame time, memory borrowed) after the overlay was
open.

The agent's thread runs below your main thread, so a loop that waits for each
frame never loses time to it; an app that never blocks starves it and the
tools time out. It holds no transfer buffers while idle and frees them after
each transfer (about 280 KiB, plus up to 260 KiB of zlib state for a
compressed download). A file transfer borrows up to about 560 KiB (less
without zlib) through `malloc()`, and so does the overlay while open.

The agent's lasting memory (its threads' stacks, and the reload stub's copy if
`guard_reload_stub` is on: 22 KiB) comes from the top of the MEM2 arena at
`hbc_agent_init()`, the way libogc takes WPAD's and the network's buffers, so
an app that builds its heap from the arena keeps it in one piece; leave room
for it at the top as for libogc's (HBCS `agent_mem` says where it went).

## What it changes on the Wii

| Part | What changes | How long it lasts |
| --- | --- | --- |
| The agent thread | One thread below your main thread, and about 20 KiB at idle, taken from the top of the MEM2 arena. A file transfer borrows up to about 560 KiB and gives it back. | While the app runs. |
| The network | The Wii listens on TCP port 4299. **Any PC on your network** can then list, read, write and delete files on the devices your app mounted, and stop the app, just as with HBC itself. | While the app runs. Keep it to development builds on a home network. |
| Files | Only what the PC asks for, on the devices the app mounted: SD, USB, or the NAND only if your app mounts it. The agent writes nothing by itself. | For good, like any file write. |
| Crash reports | A small record and the app's last output, in MEM2 at `0x91800000`, written only as the app stops (the records span `0x91800000` to `0x91801140`). HBC reads them back. | Until HBC reads or clears them, or the Wii powers off. |
| Safety tools | They watch the stack, `assert`/`abort`, Reset and Power, and write back the SD and USB caches at exit. Nothing is saved. | While the app runs. |

MEM2 is the app's: while it runs the agent writes nothing into it. It
installs nothing, and changes no settings and no NAND files of its own.

## Limits

- The agent is for development builds: anything on your network can reach
  port 4299 while it runs.
- An app that never blocks starves the agent thread, and the tools time out.
- A plain `wiiload` to a running agent app fails once and leaves the Wii in
  HBC, so the retry works.
- `hbc_agent_fatal()` before `hbc_agent_init()` has no monitor; its `exit()`
  runs alone.
- On libogc2 and libogc 1.x, the overlay's Connect asks for the Wii's SYNC
  button, and Reconnect in player order is libogc 3 only.
- The overlay's Controller settings are for the session only (the sideways
  setting aside, which HBC keeps in its settings.xml).

## Licence

The agent is built from HBC's own sources, so it is **GPL v2 or later**, like
HBC. An app that ships with it linked in must be GPL-compatible. Otherwise
keep the agent in development builds only, for example behind an `#ifdef`.
`hbc_netlog.h` is public domain and can go into any app.
