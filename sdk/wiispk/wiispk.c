// wiispk: Wii Remote speaker audio for libogc programs. See wiispk.h.
//
// How the speaker works (sources: WiiBrew's Wiimote page, Dolphin's
// WiimoteEmu/Speaker.cpp, and the behaviour of commercial Wii software as
// public decompilations show it; measured on a real remote with recordings):
//  - Output report 0x18 carries 1 to 20 bytes of sound; byte 1 is the length
//    times 8, plus the rumble bit every output report carries.
//  - Registers 0xa20001 to 0xa20009 set it up. The 7-byte block at 0xa20001 is
//    00, format (0x00 4-bit Yamaha ADPCM, 0x40 signed 8-bit PCM), the rate
//    value (little-endian), volume, 0x0c, 0x0e. Either format plays
//    12,000,000 / rate samples a second (0x07d0: 6 kHz; recordings show ADPCM
//    an octave high at half pace, so not the 6,000,000 often quoted).
//  - At 6 kHz ADPCM a 20-byte report holds 40 samples, so the speaker needs
//    one every 6.67 ms. The remote's link only carries that in sniff mode.

#include <errno.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <wiiuse/wpad.h>

#include "wiispk.h"

// wiiuse state flags (wiiuse_internal.h, not installed with libogc).
#define ST_CONNECTED 0x000010
#define ST_RUMBLE 0x000080

// Exported by libwiiuse and libbte, not declared in their installed headers.
extern int wiiuse_io_write(struct wiimote_t *wm, ubyte *buf, int len);
extern int wiiuse_sendcmd(struct wiimote_t *wm, ubyte report_type, ubyte *msg, int len, cmd_blk_cb cb);
extern s8 hci_sniff_mode(struct bd_addr *bdaddr, u16 max_interval, u16 min_interval, u16 attempt,
						 u16 timeout);
// lwbt's host state; the u16 at offset 10 counts the ACL packets the
// Bluetooth controller still has room for.
extern void *hci_dev;

#define REG_SPK_CONF 0x04a20001
#define REG_SPK_PLAY 0x04a20008
#define REG_SPK_POWER 0x04a20009
#define TICK_NS 6666667

// ---- Finding wiiuse's remote handles ----------------------------------------
//
// libogc keeps them in a static array. WPAD_Rumble indexes it by channel
// right after loading its address from the small-data area:
//     lwz rA,off(r13) ; slwi r3,r3,2 ; lwzx r3,rA,r3
// That pattern gives the array; every entry must then be a plausible pointer
// to a remote that knows its own channel, or nothing is used.

static struct wiimote_t **remotes;
static bool looked;

static bool plausible(const void *p) {
	uintptr_t a = (uintptr_t) p;

	return (a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x94000000);
}

static struct wiimote_t **locate_remotes(void) {
#ifndef GEKKO
	return NULL;    // a host build (tests/wiispk_host) has no remotes
#else
	const u32 *op = (const u32 *) WPAD_Rumble;
	register u32 sda asm("r13");
	int i, chan;

	for (i = 0; i < 48; ++i) {
		bool load_sda = (op[i] >> 26) == 32 && ((op[i] >> 16) & 31) == 13;
		bool shift = op[i + 1] == 0x5463103a;
		bool indexed = (op[i + 2] & 0xfc0007fe) == 0x7c00002e;
		struct wiimote_t **table;

		if (!load_sda || !shift || !indexed)
			continue;
		table = *(struct wiimote_t ***) (sda + (s16) (op[i] & 0xffff));
		if (!plausible(table))
			return NULL;
		for (chan = 0; chan < 4; ++chan)
			if (table[chan] && (!plausible(table[chan]) || table[chan]->unid != chan))
				return NULL;
		return table;
	}
	return NULL;
#endif
}

struct wiimote_t *wiispk_wiimote(int chan) {
	if (!looked) {
		remotes = locate_remotes();
		looked = true;
	}
	if (!remotes || chan < 0 || chan > 3 || !remotes[chan])
		return NULL;
	return (remotes[chan]->state & ST_CONNECTED) ? remotes[chan] : NULL;
}

static bool queue_empty(struct wiimote_t *wm) {
	return *(void *volatile *) &wm->cmd_head == NULL;
}

// ---- The encoder ----------------------------------------------------------
//
// The speaker decodes Yamaha ADPCM with shift-and-add hardware: a nibble is
// a sign and a 3-bit magnitude m, the step's 1/8 plus a whole, a half and a
// quarter step for m's three bits, and the step is then scaled by m (the
// standard 230 ... 614 / 256 table) and kept within 127 to 24576.
// This encoder models that decoder exactly and, for every sample, picks
// whichever of the 8 magnitudes lands closest to the input.

typedef struct {
	int predicted, step;
} coder;

static const u16 step_scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };

static int step_part(int step, int m) {
	return (step >> 3) + (m & 4 ? step : 0) + (m & 2 ? step >> 1 : 0) + (m & 1 ? step >> 2 : 0);
}

static u8 encode(coder *co, int sample) {
	int want = sample - co->predicted, sign = want < 0, best = 0, best_err = 0x7fffffff, m, v;

	if (sign)
		want = -want;
	for (m = 0; m < 8; ++m) {
		int err = abs(want - step_part(co->step, m));

		if (err < best_err) {
			best_err = err;
			best = m;
		}
	}
	v = co->predicted + (sign ? -step_part(co->step, best) : step_part(co->step, best));
	co->predicted = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
	v = (co->step * step_scale[best]) >> 8;
	co->step = v < 127 ? 127 : v > 24576 ? 24576 : v;
	return (u8) (sign << 3 | best);
}

// ---- Per-remote state ----------------------------------------------------------

enum { OFF, STARTING, PLAY_SENT, READY };

static struct {
	int phase;
	bool libogc_had_it;      // libogc's speaker was on before ours
	u8 format, volume;       // as set up
	u8 want_volume;
	const s16 *next;         // to play once ready
	u32 next_count;
	u8 next_format;
	const s16 *pcm;          // playing, read by the timer
	u32 count, pos;
	bool playing;
	coder co;
	wiispk_stats stats;
	u8 report[22] ATTRIBUTE_ALIGN(32);
} spk[4];

static syswd_t timer;
static bool timer_made, timer_on;
static u64 last_tick;

int wiispk_init(void) {
	int chan;

	for (chan = 0; chan < 4; ++chan) {
		spk[chan].want_volume = WIISPK_VOLUME_DEFAULT;
		spk[chan].stats.free_min = 0xffff;
	}
	wiispk_wiimote(0);
	return remotes ? 0 : -1;
}

// Whether the Bluetooth controller is too full for a speaker report. The
// limit, more than 3 packets waiting, is what commercial software uses. lwbt
// can count below zero (lp_acl_write sends a packet even with no room, and
// sends it again later), so a wrapped count is trusted again every 30 ticks
// rather than silencing the speaker for good.
static bool controller_full(int chan) {
	wiispk_stats *st = &spk[chan].stats;
	u16 free = hci_dev ? *(volatile u16 *) ((u8 *) hci_dev + 10) : 1;

	if (free >= 0x8000)
		return ++st->wrapped % 30 != 0;
	if (free > st->free_max && free < 0x100)
		st->free_max = free;
	if (free < st->free_min)
		st->free_min = free;
	return !free || st->free_max - free > 3;
}

static void tick(syswd_t alarm, void *arg) {
	u64 now = gettime();
	bool any = false;
	int chan;
	(void) arg;

	for (chan = 0; chan < 4; ++chan) {
		struct wiimote_t *wm = remotes ? remotes[chan] : NULL;
		u8 *r = spk[chan].report;
		u32 block, n, i, bytes;

		if (!spk[chan].playing)
			continue;
		if (!wm || !(wm->state & ST_CONNECTED) || spk[chan].pos >= spk[chan].count) {
			spk[chan].playing = false;
			continue;
		}
		any = true;
		if (last_tick) {
			u32 gap = ticks_to_microsecs(now - last_tick);

			if (gap > spk[chan].stats.gap_max_us)
				spk[chan].stats.gap_max_us = gap;
		}
		block = spk[chan].format == WIISPK_PCM ? 20 : 40;
		n = spk[chan].count - spk[chan].pos < block ? spk[chan].count - spk[chan].pos : block;
		if (controller_full(chan)) {
			// Drop this block without encoding it: a 6.67 ms gap, but the
			// decoder and the encoder stay in step.
			spk[chan].pos += n;
			spk[chan].stats.skipped++;
			continue;
		}
		memset(r, 0, sizeof(spk[chan].report));
		if (spk[chan].format == WIISPK_PCM) {
			for (i = 0; i < n; ++i)
				r[2 + i] = (u8) (s8) (spk[chan].pcm[spk[chan].pos + i] >> 8);
			bytes = n;
		} else {
			for (i = 0; i < n; ++i) {
				u8 nib = encode(&spk[chan].co, spk[chan].pcm[spk[chan].pos + i]);

				r[2 + i / 2] |= (i & 1) ? nib : nib << 4;
			}
			bytes = (n + 1) / 2;
		}
		r[0] = 0x18;
		r[1] = (u8) (bytes << 3) | (wm->state & ST_RUMBLE ? 1 : 0);
		wiiuse_io_write(wm, r, 22);
		spk[chan].pos += n;
		spk[chan].stats.sent++;
	}
	last_tick = now;
	if (!any) {
		SYS_CancelAlarm(alarm);
		timer_on = false;
		last_tick = 0;
	}
}

static void stream(int chan) {
	struct timespec period = { 0, TICK_NS };
	u32 level;

	_CPU_ISR_Disable(level);
	spk[chan].pcm = spk[chan].next;
	spk[chan].count = spk[chan].next_count;
	spk[chan].pos = 0;
	spk[chan].playing = true;
	spk[chan].next = NULL;
	_CPU_ISR_Restore(level);
	if (!timer_made) {
		SYS_CreateAlarm(&timer);
		timer_made = true;
	}
	if (!timer_on) {
		timer_on = true;
		SYS_SetPeriodicAlarm(timer, &period, &period, tick, NULL);
	}
}

static void halt(int chan) {
	spk[chan].playing = false;
}

// The set-up commercial software sends: power the speaker, mute it, enable
// its registers (0x01 to 0xa20009, 0x80 to 0xa20001), write the block,
// unmute, and ask for a status report. The play command follows once all
// of it is acknowledged (wiispk_update).
static void power_up(int chan, u8 format, u8 volume) {
	struct wiimote_t *wm = wiispk_wiimote(chan);
	u16 rate = 12000000 / (format == WIISPK_PCM ? WIISPK_PCM_RATE : WIISPK_ADPCM_RATE);
	u8 block[7] = { 0x00, format, rate & 0xff, rate >> 8, volume, 0x0c, 0x0e };
	u8 b;
	u32 level;

	if (!wm)
		return;
	halt(chan);
	_CPU_ISR_Disable(level);
	// A 5 ms sniff interval (8 slots) lets the link carry 200 reports a
	// second. lwbt allows sniff mode on every link but never asks for it.
	if (hci_sniff_mode((struct bd_addr *) &wm->bdaddr, 8, 8, 1, 0) == 0)
		spk[chan].stats.sniff = true;
	b = 0x04;
	wiiuse_sendcmd(wm, 0x14, &b, 1, NULL);
	b = 0x04;
	wiiuse_sendcmd(wm, 0x19, &b, 1, NULL);
	b = 0x01;
	wiiuse_write_data(wm, REG_SPK_POWER, &b, 1, NULL);
	b = 0x80;
	wiiuse_write_data(wm, REG_SPK_CONF, &b, 1, NULL);
	wiiuse_write_data(wm, REG_SPK_CONF, block, sizeof(block), NULL);
	b = 0x00;
	wiiuse_sendcmd(wm, 0x19, &b, 1, NULL);
	wiiuse_status(wm, NULL);
	_CPU_ISR_Restore(level);
	spk[chan].format = format;
	spk[chan].volume = volume;
	spk[chan].phase = STARTING;
}

int wiispk_play(int chan, const s16 *samples, u32 count, int format) {
	u8 fmt = format == WIISPK_PCM ? WIISPK_PCM : WIISPK_ADPCM;

	if (chan < 0 || chan > 3 || !samples || !count || !wiispk_wiimote(chan))
		return -1;
	halt(chan);
	spk[chan].next = samples;
	spk[chan].next_count = count;
	spk[chan].next_format = fmt;
	if (spk[chan].phase == OFF) {
		spk[chan].libogc_had_it = WPAD_IsSpeakerEnabled(chan) == WPAD_ERR_NONE;
		power_up(chan, fmt, spk[chan].want_volume);
	} else if (spk[chan].format != fmt || spk[chan].volume != spk[chan].want_volume) {
		// A new format or volume takes a fresh set-up, as the Wii applies one.
		power_up(chan, fmt, spk[chan].want_volume);
	}
	return 0;
}

void wiispk_update(void) {
	int chan;

	for (chan = 0; chan < 4; ++chan) {
		struct wiimote_t *wm;
		u32 type, level;
		u8 b = 0x01;

		if (spk[chan].phase == OFF)
			continue;
		wm = wiispk_wiimote(chan);
		if (!wm || WPAD_Probe(chan, &type) != WPAD_ERR_NONE) {
			halt(chan);
			spk[chan].phase = OFF;
			spk[chan].next = NULL;
			continue;
		}
		if (!queue_empty(wm))
			continue;
		if (spk[chan].phase == STARTING) {
			_CPU_ISR_Disable(level);
			wiiuse_write_data(wm, REG_SPK_PLAY, &b, 1, NULL);
			_CPU_ISR_Restore(level);
			spk[chan].phase = PLAY_SENT;
		} else if (spk[chan].phase == PLAY_SENT) {
			// A fresh set-up: the speaker's decoder starts from rest, so
			// the encoder does too. After this it carries on from sound to
			// sound.
			spk[chan].co.predicted = 0;
			spk[chan].co.step = 127;
			spk[chan].phase = READY;
		}
		if (spk[chan].phase == READY && spk[chan].next)
			stream(chan);
	}
}

void wiispk_stop(int chan) {
	if (chan < 0 || chan > 3)
		return;
	halt(chan);
	spk[chan].next = NULL;
}

bool wiispk_busy(int chan) {
	if (chan < 0 || chan > 3)
		return false;
	return spk[chan].playing || spk[chan].next != NULL;
}

void wiispk_set_volume(int chan, int volume) {
	if (chan < 0 || chan > 3)
		return;
	spk[chan].want_volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
}

void wiispk_off(int chan) {
	struct wiimote_t *wm;
	u32 level;
	u8 b;

	if (chan < 0 || chan > 3)
		return;
	wiispk_stop(chan);
	if (spk[chan].phase == OFF)
		return;
	spk[chan].phase = OFF;
	wm = wiispk_wiimote(chan);
	if (!wm)
		return;
	if (spk[chan].libogc_had_it) {
		// Hand it back as libogc set it up: ADPCM at its volume, playing.
		power_up(chan, WIISPK_ADPCM, 0x40);
		spk[chan].phase = OFF;
		b = 0x01;
		_CPU_ISR_Disable(level);
		wiiuse_write_data(wm, REG_SPK_PLAY, &b, 1, NULL);
		_CPU_ISR_Restore(level);
		return;
	}
	_CPU_ISR_Disable(level);
	b = 0x04;
	wiiuse_sendcmd(wm, 0x19, &b, 1, NULL);
	b = 0x01;
	wiiuse_write_data(wm, REG_SPK_CONF, &b, 1, NULL);
	b = 0x00;
	wiiuse_write_data(wm, REG_SPK_POWER, &b, 1, NULL);
	b = 0x00;
	wiiuse_sendcmd(wm, 0x14, &b, 1, NULL);
	wiiuse_status(wm, NULL);
	_CPU_ISR_Restore(level);
}

void wiispk_shutdown(void) {
	int chan;

	for (chan = 0; chan < 4; ++chan)
		wiispk_off(chan);
	if (timer_on) {
		SYS_CancelAlarm(timer);
		timer_on = false;
	}
}

void wiispk_get_stats(int chan, wiispk_stats *out) {
	if (chan >= 0 && chan < 4)
		*out = spk[chan].stats;
	else
		memset(out, 0, sizeof(*out));
}

// ---- Resampling ---------------------------------------------------------------
//
// A windowed-sinc low-pass (Blackman window) with its cut-off at 45 % of the
// lower rate, read from a table in 1/32-sample steps. Input comes from a
// callback so a file can be converted without holding all of it.

typedef int (*reader)(void *ctx, float *dst, int max);   // mono samples, 0 at the end

#define PHASES 32

static s16 *convert(reader read, void *ctx, int in_rate, int out_rate, u32 max_out, u32 *out_count) {
	double ratio = (double) in_rate / out_rate;
	float fc = 0.45f * (in_rate < out_rate ? in_rate : out_rate) / in_rate;   // cycles a source sample
	int half = (int) ceilf(4.f / fc), taps, t, win_cap = 8192 + 4 * half;
	float *kern, *win;
	s16 *out;
	long win_start = 0, win_len = 0;
	bool done = false;
	u32 i, n = 0, cap = 6000;

	if (in_rate <= 0 || out_rate <= 0)
		return NULL;
	taps = half * PHASES;
	kern = malloc((taps + 1) * sizeof(float));
	win = malloc(win_cap * sizeof(float));
	out = malloc(cap * sizeof(s16));
	if (!kern || !win || !out) {
		free(kern);
		free(win);
		free(out);
		errno = ENOMEM;
		return NULL;
	}
	for (t = 0; t <= taps; ++t) {
		float x = (float) t / PHASES, a = 3.14159265f * x / half;
		float s = x == 0 ? 1.f : sinf(2.f * 3.14159265f * fc * x) / (2.f * 3.14159265f * fc * x);

		kern[t] = 2.f * fc * s * (0.42f + 0.5f * cosf(a) + 0.08f * cosf(2.f * a));
	}
	for (i = 0; n < max_out; ++i) {
		double centre = i * ratio;
		long first = (long) floor(centre) - half + 1, last = (long) floor(centre) + half;
		float acc = 0;
		long k;

		// Keep the window covering [first, last], reading more as needed.
		while (!done && last >= win_start + win_len) {
			long drop = first - win_start;
			int got;

			if (drop > 0) {
				if (drop > win_len)
					drop = win_len;
				memmove(win, win + drop, (win_len - drop) * sizeof(float));
				win_start += drop;
				win_len -= drop;
			}
			got = read(ctx, win + win_len, win_cap - win_len);
			if (got <= 0)
				done = true;
			else
				win_len += got;
		}
		if (done && first >= win_start + win_len)
			break;
		for (k = first; k <= last; ++k) {
			int idx = (int) floor(fabs(centre - k) * PHASES + 0.5);

			if (k >= win_start && k < win_start + win_len && idx <= taps)
				acc += win[k - win_start] * kern[idx];
		}
		if (n == cap) {
			s16 *more = realloc(out, cap * 2 * sizeof(s16));

			if (!more)
				break;
			out = more;
			cap *= 2;
		}
		acc = acc > 32767.f ? 32767.f : acc < -32768.f ? -32768.f : acc;
		out[n++] = (s16) acc;
	}
	free(kern);
	free(win);
	*out_count = n;
	return out;
}

typedef struct {
	const s16 *p;
	u32 left;
} mem_ctx;

static int mem_read(void *ctx, float *dst, int max) {
	mem_ctx *m = ctx;
	int i, n = m->left < (u32) max ? (int) m->left : max;

	for (i = 0; i < n; ++i)
		dst[i] = m->p[i];
	m->p += n;
	m->left -= n;
	return n;
}

s16 *wiispk_resample(const s16 *in, u32 count, int in_rate, int out_rate, u32 *out_count) {
	mem_ctx m = { in, count };

	return convert(mem_read, &m, in_rate, out_rate, 0xffffffff, out_count);
}

// ---- WAV files -------------------------------------------------------------------

typedef struct {
	FILE *f;
	u32 left;              // data bytes left
	int channels, bytes;   // per sample
	bool fp;               // 32-bit float
	u8 buf[4096];
} wav_ctx;

static u32 le32(const u8 *p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (u32) p[3] << 24;
}

static int wav_read(void *ctx, float *dst, int max) {
	wav_ctx *w = ctx;
	int frame = w->channels * w->bytes, per = sizeof(w->buf) / frame, n = 0;

	while (n < max && w->left >= (u32) frame) {
		int want = max - n < per ? max - n : per, got, i, c;

		if ((u32) (want * frame) > w->left)
			want = w->left / frame;
		got = fread(w->buf, frame, want, w->f);
		if (got <= 0)
			break;
		w->left -= got * frame;
		for (i = 0; i < got; ++i) {
			float sum = 0;

			for (c = 0; c < w->channels; ++c) {
				const u8 *s = w->buf + i * frame + c * w->bytes;
				float v;

				if (w->fp) {
					u32 u = le32(s);
					float fv;

					memcpy(&fv, &u, 4);
					v = fv * 32768.f;
				} else if (w->bytes == 1) {
					v = (s[0] - 128) * 256.f;
				} else if (w->bytes == 2) {
					v = (s16) (s[0] | s[1] << 8);
				} else if (w->bytes == 3) {
					v = (float) ((s32) ((u32) s[0] << 8 | (u32) s[1] << 16 | (u32) s[2] << 24) >> 8) / 256.f;
				} else {
					v = (float) (s32) le32(s) / 65536.f;
				}
				sum += v;
			}
			dst[n++] = sum / w->channels;
		}
	}
	return n;
}

s16 *wiispk_load_wav(const char *path, int max_s, u32 *out_count) {
	wav_ctx *w = calloc(1, sizeof(*w));
	u8 hdr[12], ch[8], fmt[40];
	int rate = 0, bits = 0, tag = 0;
	bool have_fmt = false;
	s16 *out = NULL;
	u32 i, peak = 0;

	if (!w) {
		errno = ENOMEM;
		return NULL;
	}
	w->f = fopen(path, "rb");
	if (!w->f)
		goto fail;
	if (fread(hdr, 1, 12, w->f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
		goto bad;
	// Walk the chunks to "fmt " and "data" (chunks are padded to even sizes).
	while (fread(ch, 1, 8, w->f) == 8) {
		u32 size = le32(ch + 4);

		if (!memcmp(ch, "fmt ", 4)) {
			u32 take = size < sizeof(fmt) ? size : sizeof(fmt);

			if (take < 16 || fread(fmt, 1, take, w->f) != take)
				goto bad;
			if (size > take)
				fseek(w->f, size - take, SEEK_CUR);
			tag = fmt[0] | fmt[1] << 8;
			w->channels = fmt[2] | fmt[3] << 8;
			rate = (int) le32(fmt + 4);
			bits = fmt[14] | fmt[15] << 8;
			if (tag == 0xfffe && take >= 26)      // WAVE_FORMAT_EXTENSIBLE: the sub-format
				tag = fmt[24] | fmt[25] << 8;
			have_fmt = true;
		} else if (!memcmp(ch, "data", 4)) {
			w->left = size;
			break;
		} else {
			fseek(w->f, size + (size & 1), SEEK_CUR);
		}
	}
	if (!have_fmt || !w->left || w->channels < 1 || rate < 1000)
		goto bad;
	if (tag == 3 && bits == 32)
		w->fp = true;
	else if (tag != 1 || (bits != 8 && bits != 16 && bits != 24 && bits != 32))
		goto bad;
	w->bytes = bits / 8;
	out = convert(wav_read, w, rate, WIISPK_ADPCM_RATE, (u32) max_s * WIISPK_ADPCM_RATE, out_count);
	fclose(w->f);
	free(w);
	if (!out)
		return NULL;
	// Loud enough to hear, never clipping: peak at three quarters of full scale.
	for (i = 0; i < *out_count; ++i)
		if ((u32) abs(out[i]) > peak)
			peak = abs(out[i]);
	if (peak)
		for (i = 0; i < *out_count; ++i)
			out[i] = (s16) (out[i] * 24576 / (s32) peak);
	return out;
bad:
	errno = EINVAL;
fail:
	if (w->f)
		fclose(w->f);
	free(w);
	return NULL;
}

/*
 * Copyright (C) 2026 the hbc-reborn authors
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */
