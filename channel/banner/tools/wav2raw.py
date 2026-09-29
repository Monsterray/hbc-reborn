#!/usr/bin/env python3
"""Strip a banner WAV to the raw PCM that mkbns reads.

usage: wav2raw.py input.wav output.raw

The banner sound must already be 32 kHz, stereo, signed 16-bit; this tool
does not resample. The output is little-endian PCM, as SoX wrote it on x86.
"""

import sys
import wave

if len(sys.argv) != 3:
    raise SystemExit(__doc__.strip().splitlines()[2])

with wave.open(sys.argv[1], "rb") as wav:
    shape = (wav.getframerate(), wav.getnchannels(), wav.getsampwidth())
    if shape != (32000, 2, 2) or wav.getcomptype() != "NONE":
        raise SystemExit(f"{sys.argv[1]}: need 32 kHz stereo 16-bit PCM, got "
                         f"{shape[0]} Hz, {shape[1]} channel(s), {shape[2] * 8}-bit")
    pcm = wav.readframes(wav.getnframes())

with open(sys.argv[2], "wb") as out:
    out.write(pcm)
