# slotled: what is still to find out

Each item below needs a real Wii, or a document nobody has published yet.
The library works without them, but each one limits or blurs part of it.

**Help wanted.** The items still open are left for anyone with a Wii:
1. Run `tests/slotled_app` (build it with `make -C tests/slotled_app`).
   Send it with `tools/hbc.py run tests/slotled_app/slotled_app.dol demo`,
   or `ios`, and film the slot light with a phone. The TV names each step.
2. Open an issue or a pull request with what you saw, the step names, your
   Wii's model, and the video if you can share it.
3. Each answer goes here, with its date and proof. Then the code, README.md
   and slotled.h are fixed to match.

`demo` and `ios` change only the light. Leave `save` alone unless you mean
to change your Slot illumination setting: it writes SYSCONF.

On the project's own bench, jobs run through the queue on the dev Wii only
(`tests/wii_slotled.py`). The capture card cannot see the slot light, so
"watch" means a person at the Wii.

| # | Question | Why it matters | How to answer | Status |
|---|---|---|---|---|
| 1 | Which GPIO bank reaches the pin: `0xCD0000C0`, `0xCD8000C0`, or both? And without AHBPROT (an app started from a disc or a channel, not HBC)? | `slotled_init` takes the first bank whose direction bit is set. If a bank reads 0 without AHBPROT, the library still works, but we should say so. | `wii_slotled.py check` prints both banks. Run it again from an app without HBC's AHBPROT. | answered (2026-11-04/05, below): both banks work, with and without AHBPROT |
| 2 | Is `IPL.IDL` 2 bytes (libogc, Dolphin) or 3 (WiiBrew)? Which byte is Slot illumination? | `slotled_saved_level` and `slotled_save_level` take the last byte. That is right for 2 bytes; for 3 it relies on WiiBrew. | `check` prints the raw bytes. Then set Slot illumination in Wii Settings to each value and read it again. | answered (2026-11-04/05, below): 2 bytes, shutdown mode then Slot illumination. A check in Wii Settings would confirm it |
| 3 | What do IOS's 30 flash patterns look like? Which one is "new mail"? | `slotled_ios_flash` takes a number with no name. The WiiConnect24 SDK wants the mail pattern (docs/wc24-plan.md, part 4). | Watch `wii_slotled.py ios`: each pattern is named on the TV for 5 s. Write one line per pattern here. | filmed (2026-11-05, below): what each looks like, if the numbering holds. Still open: which is new mail |
| 4 | How does a pattern end? Can a running pattern be stopped, and does it go on after the app exits or IOS reloads? | We cannot offer `slotled_ios_stop()` yet, and the README cannot say whether the light outlives the app. | Watch `ios`: after the last pattern, does the light stop? Exit to HBC, then launch another app, and watch. Try pattern 0 at priority 255. | partly (2026-11-05, below): patterns end by themselves. Still open: stopping one, and outliving the app |
| 5 | The rest of ioctl 0x6001: the user-pattern layout (flag bit 16: a `u16` pattern and a count, where in the 32-byte buffer?), flag bit 17 ("does not start the loop"), and the output words | It is needed for custom IOS patterns, which keep running while the PowerPC does other work, even in standby. | `slotled_ios_flash_raw` with probes, watched. It can also be read from IOS's STM module in a public disassembly or decomp, if one exists. Never from our own Wii's dumps: those are for ironic only. | open |
| 6 | Who wins when IOS flashes and slotled drives the pin at the same time? Does the IOS level (0x6002) change anything for direct effects? | The README says "a direct effect wins". That is the expected behaviour, but it is unproven. | Start `slotled_ios_flash`, then `slotled_pulse`. Watch; then stop the pulse and watch again. | open |
| 7 | What do 4 ms periods cost on hardware? Is 20 µs the shortest visible on-time? Do the levels look even, and does 250 Hz flicker? | Proof of "two short interrupts a period" and of the level curve. The curve may want a stronger power than 2. | `check` gives the busy-loop share. `demo` step 1 shows levels 8, 32, 96 and 255: watch them, and try lower levels. | cost answered (2026-11-04, below). Still open: how the levels look |
| 8 | Does anything else drive the light while an app runs: disc insertion, new mail arriving, IOS reloads, HBC at exit? | It decides whether `slotled_shutdown` must restore the light more carefully, and what an app may assume at start. | Insert a disc and send the Wii a mail (WiiLink, from lrwii) while `demo` runs. Note what the light does at app exit and at HBC start. | open |
| 9 | Can a second `/dev/stm/immediate` handle be opened while libogc holds one? | `slotled_ios_*` open their own handle and fall back to libogc's. If IOS allows only one, the fallback is the only path. | Add a line to `check` that prints the handle result, then run `ios`. | answered (2026-11-05, below): yes |
| 10 | Does `slotled_save_level` work from HBC (NAND permission for `/shared2/sys/SYSCONF`)? Does the Wii Menu show the new value, and use it at the next boot? | The save path is untested on hardware. | `wii_slotled.py save N` on the dev Wii, only on the owner's request, with a SYSCONF backup first (`tests/nandget`). Then check Wii Settings, and the mail glow after a reboot. | answered (2026-11-05, below): Wii Settings shows what was saved, and its own change reads back |
| 11 | In WiiConnect24 standby (yellow light), does IOS use the light for new mail at the saved level, and could an app leave a pattern running into standby? | It is needed for docs/wc24-plan.md part 4: "a light when mail came overnight". | Attended: put the dev Wii in standby with WiiConnect24 on, send it mail from lrwii, and watch. | open |

## Answered

**2026-11-04, dev Wii, `wii_slotled.py check` from HBC 1.14.0** (bench job
20261103-175103-b60d6a, exit 0, all 8 checks ok):

- **Item 1:** both banks read the same: `OUT 00008000 DIR 0000c320 IN 00008000`.
  The direction bit for the light (0x20) is set, and `slotled_init` returned 0.
  Setting 255 made `OUT` read `00008020`, and 0 made it `00008000` again. This
  is with HBC's AHBPROT open; an app without it is still to test.
- **Item 2:** `CONF_Get("IPL.IDL")` returned 2 bytes, `01 01`, so libogc and
  Dolphin are right about the size. The last byte, 1 (dim), is what
  `slotled_saved_level` returned. Dolphin's own SYSCONF has `00 01`. The
  first byte is the shutdown mode: libogc's `CONF_GetShutdownMode` returns
  it, and `CONF_GetIdleLedMode` returns the second. The dev Wii's 1 is
  WiiConnect24 standby; Dolphin's 0 is a full power-off.
- **Item 7, the cost:** a busy loop counted 28,037,185 with the light off and
  28,028,374 while pulsing: 0.03% of the CPU to the timer (worked out with
  Python). `slotled_wait` after three 200 ms blinks took 600 ms exactly, and
  the light was off at the end, as it was found.

**2026-11-05, dev Wii, `wii_slotled.py save 2` then `save 1`, from HBC 1.14.0**
(bench jobs 20261105-003532-9d8881 and 20261105-004143-dee651, both exit 0,
with the owner's OK):

- **Item 10, the write:** `slotled_save_level` works from HBC. SYSCONF is
  owned by the System Menu (`owner 00001000 group 0001`) with read and write
  for everyone (`perms 3 3 3`), so an in-place write needs no NAND patch;
  owner and permissions were the same after both saves. Each save changed
  exactly one byte, 0x17ff: the `IPL.IDL` entry starts at 0x17f5
  (`46 'IPL.IDL' 01`, values at 0x17fe and 0x17ff). After `save 1` the file
  was byte for byte the backup taken before `save 2`.
- **How to write it:** in place, as libogc's `CONF_SaveChanges` does.
  Dolphin's way, a new `/tmp/SYSCONF` renamed over the old one, would leave
  the file owned by the app's own user ID when done from homebrew.

**2026-11-05, dev Wii, `wii_slotled.py check`** (bench job
20261105-004739-573792, exit 0, all checks ok):

- **Item 9:** a second `/dev/stm/immediate` handle opens (fd 6) while
  libogc holds its own, and `slotled_ios_level` through slotled's handle
  returned 0. The fallback to libogc's `STM_SetLedMode` is only for an IOS
  that refuses; Dolphin behaves the same.

**2026-11-05, dev Wii, filmed by the owner** (phone videos, not in git;
bench jobs 20261105-095048-c03368 `demo` and 20261105-095048-e57440 `ios`).
Read frame by frame: the blue of the slot's rim, per frame, at 30 fps. The
phone's exposure changes, so only *off*, *dim* and *bright* and the timings
count, not the levels.

- **`demo`:** blink (200/200 ms), pulse (2 s), heartbeat, Morse SOS (dots
  80 ms, dashes 240 ms), the wave and the fade-out all looked as written.
  - **A bug, fixed in 1.15.13:** in step 6 the first `slotled_flash` left
    the light fully on for 4.3 s instead of 40 ms flashes over level 24.
    The timer stopped on the flash's last period, because that period was
    full on. `check` now tests a flash over level 24. In Dolphin it fails on
    the old code (pin on in every sample) and passes on the new one (on 0.9%
    of the time).
  - Step 1 (levels 8, 32, 96, 255) is not on the video, so item 7's look is
    still open.
- **`ios`:**
  - The three IOS levels alone lit nothing: 0x6002 sets how bright
    patterns are, and does not turn the light on.
  - All 30 patterns returned 0, with output words 0.
  - The numbering below assumes the first light (14.2 s into the video) is
    pattern 0. Each window is 5 s, the last one cut short.

| Pattern | What it showed |
|---|---|
| 0 | quick flickers, dim 1.5 s, then bright to the end |
| 1 | bright, steady |
| 2 | bright 1.1 s, then off |
| 3 | off 0.4 s, bright 1.2 s, then off |
| 4 | a 0.2 s blip, then off |
| 5 | bright 2.1 s, then off |
| 6 | bright 1 s, then off |
| 7 | two blinks (0.5 s on, 0.23 s off), then off |
| 8 | a 0.2 s double blip, then off |
| 9 | bright 2.2 s, then off |
| 10 | bright 2.4 s, then off |
| 11 | bright 1 s, off 3.8 s, a blip at the end |
| 12 | bright 1.9 s, then off |
| 13 | two blinks (0.5 s on, 0.23 s off), then off |
| 14 | a 0.2 s blip, off 2.6 s, then bright 2.1 s |
| 15 | bright 0.3 s, off 2.6 s, then bright 1.9 s |
| 16 | two blinks, off 1 s, bright 1.9 s, then off |
| 17 | blips, dim 2.6 s, then bright 2.1 s |
| 18 | a blip, dim 2.6 s, then bright 1.9 s |
| 19 | two blinks, dim 1.6 s, fast flicker, then bright |
| 20 | dim, steady |
| 21 | off |
| 22 | off |
| 23 | two blinks, then dim |
| 24 | flicks, dim 1.4 s, then bright 2.6 s |
| 25 | flicks, dim 3.1 s, then flicks |
| 26 | flicks, dim 1.3 s, then bright 2.5 s |
| 27 | flicks, dim 1.5 s, then bright 2.5 s |
| 28 | off after 0.2 s |
| 29 | off (cut short) |

  - Most patterns end by themselves within a few seconds. After the last,
    the light was off until the video ended.
  - None looks like a slow endless pulse in a 5 s window, so which one is
    new mail is still open. Next: hold each candidate for 30 s, or film the
    Wii Menu's own mail glow and compare.

**2026-11-05, dev Wii, attended by the owner:**

- **Item 1, without AHBPROT:** `wii_slotled.py install` put the app on the
  SD card with a meta.xml without `<ahb_access/>`, and the owner started it
  from HBC's menu. It reported `IOS 58, AHBPROT 00000000`. Both banks still
  read `OUT 00008000 DIR 0000c320`, `slotled_init` returned 0, all 9 checks
  passed (bench job 20261105-112544-2ff01e fetched it), and the owner saw
  the flashes. slotled needs no AHBPROT.
- **Item 10, the Wii Menu:** after `save 2` (job 20261105-105212-8011ca,
  one byte changed), Wii Settings showed Slot illumination *Bright*. The
  owner set it back to *Dim* there and reset the Wii.
  `wii_slotled.py sysconf --compare` (job 20261105-112544-77632c) then read
  Slot illumination 1, and 0 bytes different from the backup taken before
  the first save. The Wii Menu's own write changes only that byte, like
  slotled's.
