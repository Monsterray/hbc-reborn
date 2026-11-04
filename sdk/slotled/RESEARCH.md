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
| 1 | Which GPIO bank reaches the pin: `0xCD0000C0`, `0xCD8000C0`, or both? And without AHBPROT (an app started from a disc or a channel, not HBC)? | `slotled_init` takes the first bank whose direction bit is set. If a bank reads 0 without AHBPROT, the library still works, but we should say so. | `wii_slotled.py check` prints both banks. Run it again from an app without HBC's AHBPROT. | open: queued 2026-11-03, the bench lease server was down |
| 2 | Is `IPL.IDL` 2 bytes (libogc, Dolphin) or 3 (WiiBrew)? Which byte is Slot illumination? | `slotled_saved_level` and `slotled_save_level` take the last byte. That is right for 2 bytes; for 3 it relies on WiiBrew. | `check` prints the raw bytes. Then set Slot illumination in Wii Settings to each value and read it again. | open |
| 3 | What do IOS's 30 flash patterns look like? Which one is "new mail"? | `slotled_ios_flash` takes a number with no name. The WiiConnect24 SDK wants the mail pattern (docs/wc24-plan.md, part 4). | Watch `wii_slotled.py ios`: each pattern is named on the TV for 5 s. Write one line per pattern here. | open |
| 4 | How does a pattern end? Can a running pattern be stopped, and does it go on after the app exits or IOS reloads? | We cannot offer `slotled_ios_stop()` yet, and the README cannot say whether the light outlives the app. | Watch `ios`: after the last pattern, does the light stop? Exit to HBC, then launch another app, and watch. Try pattern 0 at priority 255. | open |
| 5 | The rest of ioctl 0x6001: the user-pattern layout (flag bit 16: a `u16` pattern and a count, where in the 32-byte buffer?), flag bit 17 ("does not start the loop"), and the output words | It is needed for custom IOS patterns, which keep running while the PowerPC does other work, even in standby. | `slotled_ios_flash_raw` with probes, watched. It can also be read from IOS's STM module in a public disassembly or decomp, if one exists. Never from our own Wii's dumps: those are for ironic only. | open |
| 6 | Who wins when IOS flashes and slotled drives the pin at the same time? Does the IOS level (0x6002) change anything for direct effects? | The README says "a direct effect wins". That is the expected behaviour, but it is unproven. | Start `slotled_ios_flash`, then `slotled_pulse`. Watch; then stop the pulse and watch again. | open |
| 7 | What do 4 ms periods cost on hardware? Is 20 µs the shortest visible on-time? Do the levels look even, and does 250 Hz flicker? | Proof of "two short interrupts a period" and of the level curve. The curve may want a stronger power than 2. | `check` gives the busy-loop share. `demo` step 1 shows levels 8, 32, 96 and 255: watch them, and try lower levels. | open |
| 8 | Does anything else drive the light while an app runs: disc insertion, new mail arriving, IOS reloads, HBC at exit? | It decides whether `slotled_shutdown` must restore the light more carefully, and what an app may assume at start. | Insert a disc and send the Wii a mail (WiiLink, from lrwii) while `demo` runs. Note what the light does at app exit and at HBC start. | open |
| 9 | Can a second `/dev/stm/immediate` handle be opened while libogc holds one? | `slotled_ios_*` open their own handle and fall back to libogc's. If IOS allows only one, the fallback is the only path. | Add a line to `check` that prints the handle result, then run `ios`. | open |
| 10 | Does `slotled_save_level` work from HBC (NAND permission for `/shared2/sys/SYSCONF`)? Does the Wii Menu show the new value, and use it at the next boot? | The save path is untested on hardware. | `wii_slotled.py save N` on the dev Wii, only on the owner's request, with a SYSCONF backup first (`tests/nandget`). Then check Wii Settings, and the mail glow after a reboot. | open: needs the owner's OK |
| 11 | In WiiConnect24 standby (yellow light), does IOS use the light for new mail at the saved level, and could an app leave a pattern running into standby? | It is needed for docs/wc24-plan.md part 4: "a light when mail came overnight". | Attended: put the dev Wii in standby with WiiConnect24 on, send it mail from lrwii, and watch. | open |

## Answered

(None yet.)
