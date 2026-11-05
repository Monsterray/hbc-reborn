# slotled: the Wii's disc-slot light for libogc

`slotled` drives the blue light in the Wii's disc slot every way the Wii
allows. It is two files, [slotled.h](slotled.h) and [slotled.c](slotled.c).
They need only libogc and use the zlib licence, so you can copy them into any
project.

## Use

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

Every call returns at once. The effect runs from a timer interrupt, with no
thread. A new effect replaces the one running.

## What it does

**Directly**: the PowerPC switches the light's pin itself (Hollywood GPIO 5).

| Call | Effect |
|---|---|
| `slotled_set(level)` | steady, 0..255 |
| `slotled_blink(level, on_ms, off_ms, count)` | on and off |
| `slotled_pulse(lo, hi, period_ms, count)` | breathing, fading up and down |
| `slotled_heartbeat(level, period_ms, count)` | two quick beats, then rest |
| `slotled_morse(text, level, unit_ms, count)` | text in Morse code |
| `slotled_play(&effect)` | your own steps: a level, a time, jump or fade |
| `slotled_flash(level, ms)` | a flash over whatever runs, which carries on beneath it |
| `slotled_busy()`, `slotled_wait()` | whether a finite effect or a flash is still running |

- **`count`:** 0 repeats an effect until it is replaced. A finite one holds
  its last level when it ends.
- **Levels:** levels are perceived brightness. The on-time is the level
  squared, so 128 looks about half as bright as 255.
- **How brightness works:** the light is switched in 4 ms periods (250 Hz),
  with two short interrupts per period. A steady 0 or 255 costs nothing:
  the timer stops until the next change. `tests/slotled_app check` measures
  the cost.

**Through IOS** (`/dev/stm/immediate`): these calls need no `slotled_init()`.

| Call | Effect |
|---|---|
| `slotled_ios_level(SLOTLED_ILLUM_OFF / DIM / BRIGHT)` | the slot illumination level: how bright IOS's own patterns are. Not saved. |
| `slotled_ios_flash(pattern, priority)` | one of IOS's 30 built-in patterns (0..29), the kind the Wii uses for new mail. A pattern runs only if its priority is higher than the one running. |
| `slotled_ios_flash_raw(in, out)` | the raw ioctl, for flags and the user pattern format, which are not documented yet |

**The saved setting**: the Wii Menu's *Slot illumination*.

| Call | Effect |
|---|---|
| `slotled_saved_level()` | 0..2 from SYSCONF (`IPL.IDL`) |
| `slotled_save_level(illum)` | writes it, changing only that byte. This writes the NAND, so call it only when the user asks. |

**On a PC**: the step engine (`slotled_effect_level`), the effect builders
(`slotled_build_*`), `slotled_duty_us` and the SYSCONF patcher build with
`-DSLOTLED_NO_OGC`. `tests/test_slotled.py` tests them.

## What each way does to your Wii

| Way | What changes | How long it lasts | Risk |
|---|---|---|---|
| **Directly** (`slotled_init`, `set`, `blink`, `pulse` and the rest) | Only the light, by switching its pin. Nothing is written anywhere. | While your app runs. `slotled_shutdown()` puts the light back as it was. If the app crashes or skips `slotled_shutdown()`, the light stays as it was last left until the next app, HBC or a reboot changes it. | None to the Wii. While the level is between off and full, the timer takes two short interrupts every 4 ms. A steady off or full costs nothing. |
| **IOS level** (`slotled_ios_level`) | IOS's slot illumination setting in memory: how bright IOS's own patterns, such as the new-mail glow, are. | Until the Wii restarts or returns to the Wii Menu, which sets the saved value again. Nothing is written. | None. |
| **IOS patterns** (`slotled_ios_flash`, `_raw`) | IOS itself starts flashing the light. It runs on the Wii's ARM processor, not in your app. | Until the pattern ends or a higher-priority one replaces it. It is not yet known whether it outlives your app ([RESEARCH.md](RESEARCH.md), item 4). Nothing is written. | None known. `_raw` sends any bytes to IOS; use it only for research. |
| **Read the setting** (`slotled_saved_level`) | Nothing. It reads SYSCONF. | — | None. |
| **Save the setting** (`slotled_save_level`) | One byte of the Wii's settings file, `/shared2/sys/SYSCONF` on the NAND. It is the same byte as **Wii Settings → Slot illumination**. | For good, until it is changed again here or in Wii Settings. | The same as any settings write: a power cut during the write could damage SYSCONF. The library changes one byte in place and writes only when the value differs. Call it only when the user asks. |
| **On a PC** (`slotled_effect_level`, `slotled_build_*`, the SYSCONF patcher) | Nothing on a Wii: plain C for tools and tests. | — | None. |

Nothing in slotled installs anything, touches saves or games, or uses the
network.

## Things to know

- **Tested on a real Wii:** every call, with and without AHBPROT. The saved
  setting shows up in Wii Settings, and a change made there reads back. See
  [RESEARCH.md](RESEARCH.md).
- **Who drives the light:** the direct calls and IOS drive the same pin. While
  a direct effect runs, it wins.
- **The saved setting:** after `slotled_save_level`, libogc's `CONF_Get`
  still returns the copy it read at start. A later `CONF_SaveChanges` from
  your program writes that old copy back.
- **Dolphin:** Dolphin does not show the slot light. The calls run there but
  nothing lights up.
- **Open questions:** [RESEARCH.md](RESEARCH.md) lists what is still to find
  out on a real Wii: what IOS's patterns look like, the setting's exact
  size, and more.
- **Bench tests:** [tests/wii_slotled.py](../../tests/wii_slotled.py) runs
  `tests/slotled_app` on a Wii.
  - `check` is read-only and runs unattended.
  - `demo` and `ios` need a person to watch the light. The capture card
    cannot see it.

## Build

```
make -C sdk/slotled          # libslotled.a
```

Link with `-lslotled -logc`, or compile `slotled.c` into your program.
