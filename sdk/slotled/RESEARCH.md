# slotled: what is still to find out

Each item below needs a real Wii, or a document nobody has published yet.
The library works without them, but each one limits or blurs part of it.
When one is answered, record the answer, the date and the proof here. Then
fix the code, README.md and slotled.h to match.

Jobs run through the bench queue on the dev Wii only (`tests/wii_slotled.py`).
The capture card cannot see the slot light, so "watch" means a person at the
Wii.

| # | Question | Why it matters | How to answer | Status |
|---|---|---|---|---|
| 1 | Which GPIO bank reaches the pin: `0xCD0000C0`, `0xCD8000C0`, or both? And without AHBPROT (an app started from a disc or a channel, not HBC)? | `slotled_init` takes the first bank whose direction bit is set. If a bank reads 0 without AHBPROT, the library still works, but we should say so. | `wii_slotled.py check` prints both banks. Run it again from an app without HBC's AHBPROT. | partly answered (2026-11-04, below): from HBC, both banks work. Still open: an app without AHBPROT |
| 2 | Is `IPL.IDL` 2 bytes (libogc, Dolphin) or 3 (WiiBrew)? Which byte is Slot illumination? | `slotled_saved_level` and `slotled_save_level` take the last byte. That is right for 2 bytes; for 3 it relies on WiiBrew. | `check` prints the raw bytes. Then set Slot illumination in Wii Settings to each value and read it again. | answered (2026-11-04/05, below): 2 bytes, shutdown mode then Slot illumination. A check in Wii Settings would confirm it |
| 3 | What do IOS's 30 flash patterns look like? Which one is "new mail"? | `slotled_ios_flash` takes a number with no name. The WiiConnect24 SDK wants the mail pattern (docs/wc24-plan.md, part 4). | Watch `wii_slotled.py ios`: each pattern is named on the TV for 5 s. Write one line per pattern here. | open |
| 4 | How does a pattern end? Can a running pattern be stopped, and does it go on after the app exits or IOS reloads? | We cannot offer `slotled_ios_stop()` yet, and the README cannot say whether the light outlives the app. | Watch `ios`: after the last pattern, does the light stop? Exit to HBC, then launch another app, and watch. Try pattern 0 at priority 255. | open |
| 5 | The rest of ioctl 0x6001: the user-pattern layout (flag bit 16: a `u16` pattern and a count, where in the 32-byte buffer?), flag bit 17 ("does not start the loop"), and the output words | It is needed for custom IOS patterns, which keep running while the PowerPC does other work, even in standby. | `slotled_ios_flash_raw` with probes, watched. It can also be read from IOS's STM module in a public disassembly or decomp, if one exists. Never from our own Wii's dumps: those are for ironic only. | open |
| 6 | Who wins when IOS flashes and slotled drives the pin at the same time? Does the IOS level (0x6002) change anything for direct effects? | The README says "a direct effect wins". That is the expected behaviour, but it is unproven. | Start `slotled_ios_flash`, then `slotled_pulse`. Watch; then stop the pulse and watch again. | open |
| 7 | What do 4 ms periods cost on hardware? Is 20 µs the shortest visible on-time? Do the levels look even, and does 250 Hz flicker? | Proof of "two short interrupts a period" and of the level curve. The curve may want a stronger power than 2. | `check` gives the busy-loop share. `demo` step 1 shows levels 8, 32, 96 and 255: watch them, and try lower levels. | cost answered (2026-11-04, below). Still open: how the levels look |
| 8 | Does anything else drive the light while an app runs: disc insertion, new mail arriving, IOS reloads, HBC at exit? | It decides whether `slotled_shutdown` must restore the light more carefully, and what an app may assume at start. | Insert a disc and send the Wii a mail (WiiLink, from lrwii) while `demo` runs. Note what the light does at app exit and at HBC start. | open |
| 9 | Can a second `/dev/stm/immediate` handle be opened while libogc holds one? | `slotled_ios_*` open their own handle and fall back to libogc's. If IOS allows only one, the fallback is the only path. | Add a line to `check` that prints the handle result, then run `ios`. | open |
| 10 | Does `slotled_save_level` work from HBC (NAND permission for `/shared2/sys/SYSCONF`)? Does the Wii Menu show the new value, and use it at the next boot? | The save path is untested on hardware. | `wii_slotled.py save N` on the dev Wii, only on the owner's request, with a SYSCONF backup first (`tests/nandget`). Then check Wii Settings, and the mail glow after a reboot. | the write is answered (2026-11-05, below). Still open: Wii Settings and the next boot, attended |
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
