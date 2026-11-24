# slotled

*The Wii's blue disc-slot light, every way the Wii allows.*

| | |
| --- | --- |
| **Files** | [`slotled.h`](slotled.h) and [`slotled.c`](slotled.c) |
| **Needs** | libogc |
| **Licence** | zlib (not GPL) |
| **Changes on the Wii** | Only the light, except `slotled_save_level`, which writes one byte of SYSCONF (see [below](#what-it-changes-on-the-wii)) |
| **Since** | HBC 1.14.28 |

**Contents:** [What it does](#what-it-does) · [Quick start](#quick-start) ·
[Add it to your app](#add-it-to-your-app) · [API](#api) ·
[Effects and levels](#effects-and-levels) · [IOS patterns](#ios-patterns) ·
[What it changes on the Wii](#what-it-changes-on-the-wii) · [Limits](#limits) ·
[Things to know](#things-to-know) · [Licence](#licence)

## What it does

Three ways to drive the light:

- **Directly:** 256 levels, blink, pulse, heartbeat, Morse and flashes for
  activity, from a timer, with no thread.
- **Through IOS:** its off/dim/bright level and its own flash patterns.
- **The saved setting:** the Wii Menu's Slot illumination.

## Quick start

```c
#include "slotled.h"

slotled_init();                        // -1: the light's pin is not reachable
slotled_pulse(0, 255, 2000, 0);        // breathe every 2 s until replaced

while (running) {
    ...
    if (read_a_block())
        slotled_flash(255, 40);        // a blink over the pulse, for activity
}
slotled_shutdown();                    // before exit: the light as it was found
```

Every direct call returns at once, except `slotled_wait`. The effect runs
from a timer interrupt, with no thread. A new effect replaces the one
running, and a call that fails leaves it running. `slotled_save_level`
writes the NAND, so it takes a moment.

## Add it to your app

Copy `slotled.h` and `slotled.c` into your project, or build the library with
`make -C sdk/slotled` and link `-lslotled -logc`.

## API

Calls return 0 or more on success and a negative `SLOTLED_E_*` code on
failure: `E_NO_PIN` (-1, the PowerPC cannot reach the pin), `E_ARG` (-2, a
level, pattern or count out of range), `E_NOT_INIT` (-3, `slotled_init()` was
not called or failed), `E_IOS` (-4, IOS refused; its error is not kept),
`E_FORMAT` (-5, SYSCONF is not as the Wii makes it), `E_TOO_LONG` (-6, more
than `SLOTLED_MAX_STEPS` steps).

**Directly.** The PowerPC switches the light's pin itself (Hollywood GPIO 5).

| Name | What it does |
| --- | --- |
| `slotled_init()` | Finds the light's pin and sets up the timer. Turns nothing on. |
| `slotled_shutdown()` | Stops the effect and leaves the light as `slotled_init()` found it. |
| `slotled_set(level)` | Steady at a level, 0..255. |
| `slotled_get()` | The level shown now. |
| `slotled_blink(level, on_ms, off_ms, count)` | On and off at `level`, `count` times. |
| `slotled_pulse(lo, hi, period_ms, count)` | Breathing: from `lo` up to `hi` and back down, once every `period_ms`. |
| `slotled_heartbeat(level, period_ms, count)` | Two quick beats, then rest, once every `period_ms` (at least 600). |
| `slotled_morse(text, level, unit_ms, count)` | Text in Morse code, `unit_ms` a dot (WPM = 1200 / `unit_ms`). Letters, digits and `.,?'!/()&:;=+-_"$@`; other characters are a word gap. Returns the characters sent. |
| `slotled_play(&effect)` | Plays your own steps (at most `SLOTLED_MAX_PLAY`, 4096). They are read while it plays: keep them until `slotled_busy()` is false or another effect replaces them. |
| `slotled_flash(level, ms)` | `level` for `ms` over whatever is running, which carries on beneath it (its time keeps going). Call it again to extend it: for disc or network activity, call it on each transfer. |
| `slotled_busy()` | Whether an effect or a flash is still running. An effect with count 0 is always busy. |
| `slotled_wait()` | Waits until `slotled_busy()` is false: never, while a count-0 effect runs. |

**Through IOS** (`/dev/stm/immediate`). These calls need no `slotled_init()`.

| Name | What it does |
| --- | --- |
| `slotled_ios_level(SLOTLED_ILLUM_OFF / DIM / BRIGHT)` | The slot illumination level: how bright IOS's own patterns are. Not saved. |
| `slotled_ios_flash(pattern, priority)` | One of IOS's 30 built-in patterns (0..29, `SLOTLED_IOS_PATTERNS`), the kind the Wii uses for new mail. A pattern runs only if its priority (0..255) is higher than the one running. |
| `slotled_ios_flash_raw(in, out)` | The raw ioctl (STM 0x6001), for flags and the user pattern format, which are not documented yet. `in` is 32 bytes, `out` gets 32 bytes (may be NULL). Returns IOS's result. |

**The saved setting.** The Wii Menu's *Slot illumination*.

| Name | What it does |
| --- | --- |
| `slotled_saved_level()` | 0..2 from SYSCONF (`IPL.IDL`). |
| `slotled_save_level(illum)` | Writes it to SYSCONF and sets the IOS level to match; the Wii Menu uses it from the next boot. It rewrites the file in place, as libogc's `CONF_SaveChanges` does, so the file keeps its owner and permissions; only that one byte changes. Then it reads the file back and returns `SLOTLED_E_IOS` if it differs. This writes the NAND, so call it only when the user asks. |

**On a PC.** These are plain C and build with `-DSLOTLED_NO_OGC`;
`tests/host/test_slotled.py` tests them.

| Name | What it does |
| --- | --- |
| `slotled_effect_level(&effect, ms, start, &done)` | The level an effect shows `ms` into it. `*done` (may be NULL) is set once a finite effect has ended. `start` is the level it fades from at first. |
| `slotled_duty_us(level)` | The on-time, in microseconds, for a level in one `SLOTLED_PERIOD_US` (4000). |
| `slotled_build_blink`, `_pulse`, `_heartbeat`, `_morse` | Fill a `slotled_step` array (room for `max`, at most `SLOTLED_MAX_STEPS`, 512) and return how many steps, or an error. The direct calls use these. |
| `slotled_sysconf_illum(sysconf, size, set)` | Finds IPL.IDL in a SYSCONF image and reads or sets its Slot illumination byte, in place. Returns the old value (0..2), or `SLOTLED_E_FORMAT`. |

A step is `slotled_step { level, fade, ms }`: go to `level` and stay for
`ms`. With `fade`, the level moves there evenly over the `ms` instead of
jumping. A `slotled_effect { steps, count, repeat }` plays its steps in order,
`repeat` times (0: until replaced), and holds the last step's level when it
ends.

## Effects and levels

- **`count`:** 0 repeats an effect until it is replaced. A finite one holds
  its last level when it ends.
- **Levels:** levels are perceived brightness. The on-time is the level
  squared, so 128 looks about half as bright as 255.
- **Step length:** steps last whole 4 ms periods; a step shorter than that
  may not show.
- **How brightness works:** the light is switched in 4 ms periods (250 Hz),
  with two short interrupts per period. A steady 0 or 255 costs nothing:
  the timer stops until the next change. `tests/apps/slotled_app check` measures
  the cost.

## IOS patterns

IOS starts the pattern and runs it on the Wii's ARM processor, not in your
app. Use it when you want the Wii's own look. The direct calls and IOS drive
the same pin; while a direct effect runs, it wins. What the 30 patterns look
like, and the user pattern format, are open questions in
[RESEARCH.md](RESEARCH.md).

## What it changes on the Wii

| Way | What changes | How long it lasts | Risk |
| --- | --- | --- | --- |
| **Directly** (`slotled_init`, `set`, `blink`, `pulse` and the rest) | Only the light, by switching its pin. Nothing is written anywhere. | While your app runs. `slotled_shutdown()` puts the light back as it was. If the app crashes or skips `slotled_shutdown()`, the light stays as it was last left until the next app, HBC or a reboot changes it. | None to the Wii. While the level is between off and full, the timer takes two short interrupts every 4 ms. A steady off or full costs nothing. |
| **IOS level** (`slotled_ios_level`) | IOS's slot illumination setting in memory: how bright IOS's own patterns, such as the new-mail glow, are. | Until the Wii restarts or returns to the Wii Menu, which sets the saved value again. Nothing is written. | None. |
| **IOS patterns** (`slotled_ios_flash`, `_raw`) | IOS itself starts flashing the light. It runs on the Wii's ARM processor, not in your app. | Until the pattern ends or a higher-priority one replaces it. It is not yet known whether it outlives your app ([RESEARCH.md](RESEARCH.md), item 4). Nothing is written. | None known. `_raw` sends any bytes to IOS; use it only for research. |
| **Read the setting** (`slotled_saved_level`) | Nothing. It reads SYSCONF. | — | None. |
| **Save the setting** (`slotled_save_level`) | One byte of the Wii's settings file, `/shared2/sys/SYSCONF` on the NAND. It is the same byte as **Wii Settings → Slot illumination**. | For good, until it is changed again here or in Wii Settings. | The same as any settings write: a power cut during the write could damage SYSCONF. The library changes one byte in place and writes only when the value differs. Call it only when the user asks. |
| **On a PC** (`slotled_effect_level`, `slotled_build_*`, the SYSCONF patcher) | Nothing on a Wii: plain C for tools and tests. | — | None. |

Nothing in slotled installs anything, touches saves or games, or uses the
network.

## Limits

- Dolphin does not show the slot light. The calls run there but nothing
  lights up.
- The direct calls and IOS drive the same pin. While a direct effect runs, it
  wins.
- After `slotled_save_level`, libogc's `CONF_Get` still returns the copy it
  read at start. A later `CONF_SaveChanges` from your program writes that old
  copy back.
- `slotled_play` steps must stay valid while the effect plays.
- Whether an IOS pattern outlives your app, what the patterns look like and
  the setting's exact size are not yet known; see
  [RESEARCH.md](RESEARCH.md).

## Things to know

- **Tested on a real Wii:** every call, with and without AHBPROT. The saved
  setting shows up in Wii Settings, and a change made there reads back. See
  [RESEARCH.md](RESEARCH.md).
- **Open questions:** [RESEARCH.md](RESEARCH.md) lists what is still to find
  out on a real Wii.
- **Bench tests:** [tests/wii/wii_slotled.py](../../tests/wii/wii_slotled.py) runs
  `tests/apps/slotled_app` on a Wii.
  - `check` is read-only and runs unattended.
  - `demo` and `ios` need a person to watch the light. The capture card
    cannot see it.

## Licence

zlib: use it in anything, including closed apps. Don't claim you wrote the
original, mark changed copies as changed, and keep the licence notice at the
end of `slotled.c`. Credit in your app's documentation is welcome but not
required. The other SDKs and their licences are listed in the
[SDK catalog](../README.md).
