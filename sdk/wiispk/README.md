# wiispk

*Clean sound on the Wii Remote's speaker.*

| | |
| --- | --- |
| **Files** | [`wiispk.h`](wiispk.h) and [`wiispk.c`](wiispk.c) |
| **Needs** | libogc (libwiiuse, libbte) |
| **Licence** | zlib (not GPL) |
| **Changes on the Wii** | Nothing written; the remote's link stays in sniff mode (see [below](#what-it-changes-on-the-wii)) |
| **Since** | HBC 1.8.0 |

**Contents:** [What it does](#what-it-does) · [Quick start](#quick-start) ·
[Add it to your app](#add-it-to-your-app) · [API](#api) ·
[What it changes on the Wii](#what-it-changes-on-the-wii) ·
[Why libogc's streaming sounds bad](#why-libogcs-streaming-sounds-bad-and-what-this-does-instead) ·
[Limits](#limits) · [Licence](#licence)

## What it does

- Plays sound on a Wii Remote's speaker without the stutter and noise of
  libogc's `WPAD_SendStreamData`.
- Loads any PCM WAV file and converts it to what the speaker plays.
- Is the speaker driver HBC's HOME menu uses, as a library of its own.
- Is two files that need only libogc, so any app can copy them in.

## Quick start

```c
#include <wiiuse/wpad.h>
#include "wiispk.h"

WPAD_Init();
wiispk_init();                            // -1: this libogc's layout is unknown

u32 n;
s16 *sound = wiispk_load_wav("sd:/beep.wav", 10, &n);   // any PCM WAV, to 6 kHz mono
wiispk_play(0, sound, n, WIISPK_ADPCM);   // remote 1

while (running) {
    WPAD_ScanPads();
    wiispk_update();                      // once a frame
    ...
}
wiispk_shutdown();                        // before exiting
free(sound);                              // once wiispk_busy(0) is false
```

- **Samples:** mono signed 16-bit at 6 kHz, `WIISPK_ADPCM_RATE`. Keep the
  buffer until `wiispk_busy()` returns false.
- **Converting:** `wiispk_resample()` converts audio you already have in
  memory. `wiispk_load_wav()` reads a WAV file, resamples it and sets its
  level.
- **Volume:** `wiispk_set_volume(chan, 0..127)` takes effect from the next
  sound. 0x40 is the Wii's usual level.
- **PCM:** `WIISPK_PCM`, 8-bit at 3 kHz, is there for comparison. ADPCM at
  6 kHz measures about 7 dB cleaner on a real remote.
- **Diagnostics:** `wiispk_get_stats()` reports reports sent and skipped,
  timer gaps, and Bluetooth buffer use.

## Add it to your app

Copy `wiispk.h` and `wiispk.c` into your project, or build a library with
`make -C sdk/wiispk` and link `-lwiispk -lwiiuse -lbte -logc -lm`.

## API

Remotes are numbered 0 to 3 (`chan`). Functions that return `int` give 0 on
success and -1 on failure unless noted.

| Name | What it does |
| --- | --- |
| `int wiispk_init(void)` | Finds wiiuse's per-remote handles. Returns -1 if this libogc's layout is unknown; then every call returns an error and nothing plays. |
| `void wiispk_update(void)` | Advances each remote's speaker start-up. Call once a frame. |
| `int wiispk_play(chan, samples, count, format)` | Plays a sound on a remote, starting its speaker if needed. Replaces whatever that remote was playing. `format` is `WIISPK_ADPCM` (0x00, 4-bit Yamaha ADPCM, 6 kHz, the clean one) or `WIISPK_PCM` (0x40, signed 8-bit, 3 kHz). The samples are read while the sound plays. |
| `void wiispk_stop(chan)` | Stops the sound. The speaker stays on for the next one. |
| `bool wiispk_busy(chan)` | Whether a sound is starting or playing. |
| `void wiispk_set_volume(chan, volume)` | The speaker's volume byte, 0 to 127, applied from the next sound. `WIISPK_VOLUME_DEFAULT` is 0x40. |
| `void wiispk_off(chan)` | Turns one remote's speaker off. |
| `void wiispk_shutdown(void)` | Turns every speaker off and stops the timer. |
| `void wiispk_get_stats(chan, wiispk_stats *out)` | Fills `sent` (reports sent), `skipped` (blocks skipped because the controller was full), `wrapped` (timer ticks where lwbt's buffer count had wrapped), `gap_max_us` (longest time between two timer ticks), `free_min` and `free_max` (the controller's free ACL buffers seen), and `sniff` (the link was put in sniff mode). |
| `struct wiimote_t *wiispk_wiimote(chan)` | wiiuse's handle for a connected remote, or NULL. Useful for LEDs and IR settings libogc's WPAD calls do not reach. |
| `s16 *wiispk_resample(in, count, in_rate, out_rate, &out_count)` | Resamples mono signed 16-bit audio with a windowed-sinc low-pass. Returns a malloc'd buffer (free it), or NULL. |
| `s16 *wiispk_load_wav(path, max_s, &out_count)` | Reads a PCM WAV file (8, 16, 24 or 32-bit integer, or 32-bit float; any rate; mono or more channels, which are mixed), keeping at most `max_s` seconds. Returns 6 kHz mono scaled so its peak is about three quarters of full scale, as a malloc'd buffer (free it), or NULL with `errno` set. Slow for a long file: call it from a thread. |

Constants: `WIISPK_ADPCM_RATE` 6000, `WIISPK_PCM_RATE` 3000.

## What it changes on the Wii

| Part | What changes | How long it lasts |
| --- | --- | --- |
| The remote's Bluetooth link | Put in sniff mode with a 5 ms interval. | Stays on once set, as it does on the Wii itself. |
| The remote's speaker | Switched on, set up and fed sound. | Until `wiispk_off()` or `wiispk_shutdown()`. |
| A timer | A 6.67 ms timer sends the speaker reports. | Until `wiispk_shutdown()`. |

It writes no files, uses no network, and installs nothing.

## Why libogc's streaming sounds bad, and what this does instead

| libogc | wiispk |
| --- | --- |
| The remote's Bluetooth link is left in active mode (about 100 reports a second). The speaker needs 150 a second, so reports are late or lost and every held note is chopped. | Puts the link in sniff mode with a 5 ms interval, as the Wii's own software does. |
| Each 20-byte chunk is queued as a command that waits for an acknowledgement. Speaker reports are never acknowledged, so the queue stalls: no LEDs, no rumble, no extension handshake. | Sends raw 0x18 reports from its own 6.67 ms timer, outside the command queue, each with the remote's rumble bit. |
| The whole sound is encoded in advance. One lost report leaves the speaker's ADPCM decoder at a different step size from the encoder, and the rest of the sound turns to noise. | Encodes each report just before sending it. If the Bluetooth controller already holds more than 3 packets, the block is skipped without encoding it, so the decoder and encoder stay in step and the listener hears a 6.67 ms gap. |
| Set-up writes 0x08 to 0xa20001 and sends "play" at once. | Writes 0x80, then sends "play" only after the rest of the set-up is acknowledged. |

**Rates:** both formats play at 12,000,000 / rate samples a second. Value 0x07d0
is 6 kHz for ADPCM; recordings put ADPCM an octave high when paced for 3 kHz.

**Encoder:** it models the speaker's shift-and-add Yamaha ADPCM decoder, and for
each sample chooses the magnitude whose result lands closest to the input.

**Measured on a real remote:** notes on pitch to within 1 Hz. On a held tone,
the worst dip is 0.9 dB with wiispk against 30 dB with libogc. ADPCM carries
22 dB more tone than everything else; PCM carries 15 dB.

## Limits

- It finds wiiuse's per-remote handles by reading `WPAD_Rumble`'s code. If a
  future libogc changes that code, `wiispk_init()` returns -1 and nothing plays.
- It reads lwbt's count of free controller buffers at `hci_dev + 10`, which
  depends on that struct's layout.
- Sniff mode stays on once set, as it does on the Wii itself.
- Sounds are mono signed 16-bit. Keep the samples until `wiispk_busy()` says
  the sound is done.

## Licence

zlib: use it in anything, including closed apps. Don't claim you wrote the
original, mark changed copies as changed, and keep the licence notice at the
end of `wiispk.c`. Credit in your app's documentation is welcome but not
required. The other SDKs and their licences are listed in the
[SDK catalog](../README.md).
