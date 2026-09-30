"""Build sdk/wiispk on the host and check its WAV reader, resampler and ADPCM
encoder: a 44.1 kHz stereo WAV with a 1 kHz tone and a 4.6 kHz tone (which
would fold onto 1.4 kHz at 6 kHz without the low-pass) must come out as
6 kHz mono with the 1 kHz tone kept, the 4.6 kHz one gone, and the encoder
at least as good as libogc's."""

import math
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave

root = pathlib.Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


def write_wav(path, seconds=1.5, rate=44100):
    frames = bytearray()
    for i in range(int(seconds * rate)):
        t = i / rate
        v = 0.4 * math.sin(2 * math.pi * 1000 * t) + 0.4 * math.sin(2 * math.pi * 4600 * t)
        s = int(v * 32767)
        frames += struct.pack("<hh", s, s)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


@unittest.skipUnless(CC, "no C compiler")
class WiiSpkTest(unittest.TestCase):
    def test_wav_resample_encode(self):
        host = root / "tests/wiispk_host"
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp, "wiispk_test.exe" if os.name == "nt" else "wiispk_test")
            wav = pathlib.Path(tmp, "in.wav")
            write_wav(wav)
            subprocess.run([CC, "-O2", "-Wall", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                            "-Wno-deprecated-declarations", "-Wno-unused-function", "-I", str(host / "include"),
                            str(host / "test.c"), "-o", str(exe)] + ([] if os.name == "nt" else ["-lm"]),
                           check=True, env=dict(os.environ, TMP=tmp, TEMP=tmp, TMPDIR=tmp))
            out = subprocess.run([str(exe), str(wav)], check=True, capture_output=True, text=True).stdout
        v = {k: float(x) for k, x in (line.split("=") for line in out.split())}
        self.assertAlmostEqual(v["samples"], 9000, delta=60)          # 1.5 s at 6 kHz
        self.assertAlmostEqual(v["peak"], 24576, delta=300)           # scaled to 3/4
        self.assertGreater(v["tone_1000"], 8000)
        # The 4.6 kHz tone, folded to 1.4 kHz, is at least 40 dB under the 1 kHz one.
        self.assertLess(20 * math.log10(v["alias_1400"] / v["tone_1000"]), -40)
        self.assertGreater(v["snr_ours_shiftadd"], 15)
        self.assertGreater(v["snr_ours_ffmpeg"], 15)
        self.assertGreaterEqual(v["snr_ours_shiftadd"], v["snr_libogc_shiftadd"])


if __name__ == "__main__":
    unittest.main()
