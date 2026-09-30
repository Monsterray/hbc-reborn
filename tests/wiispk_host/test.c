// Host test for sdk/wiispk: loads a WAV, checks the resampler's low-pass,
// and measures the ADPCM encoder against two models of the speaker's
// decoder. usage: test IN.wav
//
// Prints key=value lines for tests/test_wiispk.py.

#include "../../sdk/wiispk/wiispk.c"

// Stubs for what the driver half calls; nothing here runs it.
void *hci_dev;
s32 SYS_CreateAlarm(syswd_t *a) { *a = 0; return 0; }
s32 SYS_SetPeriodicAlarm(syswd_t a, const struct timespec *s, const struct timespec *p, alarmcallback cb,
						 void *arg) { (void) a; (void) s; (void) p; (void) cb; (void) arg; return 0; }
s32 SYS_CancelAlarm(syswd_t a) { (void) a; return 0; }
u64 gettime(void) { return 0; }
int wiiuse_io_write(struct wiimote_t *wm, ubyte *b, int n) { (void) wm; (void) b; (void) n; return 0; }
int wiiuse_sendcmd(struct wiimote_t *wm, ubyte t, ubyte *m, int n, cmd_blk_cb cb) {
	(void) wm; (void) t; (void) m; (void) n; (void) cb; return 0;
}
int wiiuse_write_data(struct wiimote_t *wm, u32 a, ubyte *d, u8 n, cmd_blk_cb cb) {
	(void) wm; (void) a; (void) d; (void) n; (void) cb; return 0;
}
void wiiuse_status(struct wiimote_t *wm, cmd_blk_cb cb) { (void) wm; (void) cb; }
s8 hci_sniff_mode(struct bd_addr *b, u16 a, u16 c, u16 d, u16 e) { (void) b; (void) a; (void) c; (void) d; (void) e; return 0; }
s32 WPAD_Rumble(s32 c, int s) { (void) c; (void) s; return 0; }
s32 WPAD_Probe(s32 c, u32 *t) { (void) c; (void) t; return -1; }
s32 WPAD_IsSpeakerEnabled(s32 c) { (void) c; return -1; }

// The level of one frequency, by Goertzel, as RMS.
static double level(const s16 *x, u32 n, double f, double rate) {
	double w = 2 * 3.14159265358979 * f / rate, k = 2 * cos(w), s1 = 0, s2 = 0;
	u32 i;

	for (i = 0; i < n; ++i) {
		double s0 = x[i] + k * s1 - s2;

		s2 = s1;
		s1 = s0;
	}
	return sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / n * sqrt(2.0);
}

// Decoder models: shift-and-add (as the hardware), and the (2m+1)/8 form
// Dolphin and ffmpeg use.
static int decode(int *pred, int *step, u8 nib, bool shift_add) {
	int m = nib & 7, d = shift_add ? step_part(*step, m) : *step * (2 * m + 1) / 8, v;

	v = *pred + (nib & 8 ? -d : d);
	*pred = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
	v = (*step * step_scale[m]) >> 8;
	*step = v < 127 ? 127 : v > 24576 ? 24576 : v;
	return *pred;
}

// libogc's encoder (wiiuse/speaker.c), for comparison.
static u8 libogc_encode(coder *co, int sample) {
	static const int diff[16] = { 1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15 };
	int delta = sample - co->predicted, a = delta < 0 ? -delta : delta;
	int nib = (a * 4 / co->step > 7 ? 7 : a * 4 / co->step) + (delta < 0) * 8, v;

	v = co->predicted + co->step * diff[nib] / 8;
	co->predicted = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
	v = (co->step * step_scale[nib & 7]) >> 8;
	co->step = v < 127 ? 127 : v > 24576 ? 24576 : v;
	return (u8) nib;
}

static double snr(const s16 *x, u32 n, bool ours, bool shift_add) {
	coder co = { 0, 127 };
	int pred = 0, step = 127;
	double sig = 0, err = 0;
	u32 i;

	for (i = 0; i < n; ++i) {
		u8 nib = ours ? encode(&co, x[i]) : libogc_encode(&co, x[i]);
		int y = decode(&pred, &step, nib, shift_add);

		sig += (double) x[i] * x[i];
		err += (double) (x[i] - y) * (x[i] - y);
	}
	return 10 * log10(sig / (err + 1));
}

int main(int argc, char **argv) {
	u32 n = 0, i;
	s16 *pcm;
	int peak = 0;

	if (argc < 2)
		return 2;
	pcm = wiispk_load_wav(argv[1], 60, &n);
	if (!pcm) {
		printf("error=%d\n", errno);
		return 1;
	}
	for (i = 0; i < n; ++i)
		if (abs(pcm[i]) > peak)
			peak = abs(pcm[i]);
	printf("samples=%u\npeak=%d\n", n, peak);
	// Skip the filter's edges.
	printf("tone_1000=%.1f\n", level(pcm + 600, n - 1200, 1000, 6000));
	printf("alias_1400=%.1f\n", level(pcm + 600, n - 1200, 1400, 6000));
	printf("snr_ours_shiftadd=%.1f\nsnr_ours_ffmpeg=%.1f\n", snr(pcm, n, true, true), snr(pcm, n, true, false));
	printf("snr_libogc_shiftadd=%.1f\nsnr_libogc_ffmpeg=%.1f\n", snr(pcm, n, false, true), snr(pcm, n, false, false));
	free(pcm);
	return 0;
}
