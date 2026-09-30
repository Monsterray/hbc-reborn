# wiispk: clean Wii Remote speaker audio for libogc

`wiispk` plays sound on the Wii Remote's speaker without the stutter and noise
of libogc's `WPAD_SendStreamData`. It is two files, [wiispk.h](wiispk.h) and
[wiispk.c](wiispk.c). They need only libogc (libwiiuse, libbte) and use the
zlib licence, so you can copy them into any project.

## Use

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

- **Samples:** mono signed 16-bit at 6 kHz, `WIISPK_ADPCM_RATE`. Keep the buffer
  until `wiispk_busy()` returns false.
- **Converting:** `wiispk_resample()` converts audio you already have in memory.
  `wiispk_load_wav()` reads a WAV file, resamples it and sets its level.
- **Volume:** `wiispk_set_volume(chan, 0..127)` takes effect from the next
  sound. 0x40 is the Wii's usual level.
- **PCM:** `WIISPK_PCM`, 8-bit at 3 kHz, is there for comparison. ADPCM at 6 kHz
  measures about 7 dB cleaner on a real remote.
- **Diagnostics:** `wiispk_get_stats()` reports reports sent and skipped, timer
  gaps, and Bluetooth buffer use.

Build a library instead with `make -C sdk/wiispk`, then link
`-lwiispk -lwiiuse -lbte -logc -lm`.

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
