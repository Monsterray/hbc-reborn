// The HOME overlay on the Wii: hbc_agent_home() pauses the app's frame loop,
// draws ov_ui.c's strip and menus over the app's last frame in two
// framebuffers of its own, reads the controllers, carries out the actions,
// and gives the app its framebuffer back. Kept apart from agent.c so apps
// that never call hbc_agent_home() do not link libwiiuse through it.

#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <network.h>
#include <ogc/lwp_queue.h>
#include <wiiuse/wpad.h>

#include "../hbc_agent.h"
#include "agent_int.h"
#include "ov_ui.h"

// wiiuse state flags (wiiuse_internal.h, not installed with libogc).
#define WM_STATE_CONNECTED 0x000010
#define WM_STATE_RUMBLE 0x000080
#define WM_STATE_ACC 0x000100
#define WM_STATE_IR 0x000400
#define WM_STATE_SPEAKER 0x000800
#define WIIMOTE_STATE_RUMBLE_FLAG 0x000080

#define TOAST_FRAMES 150
#define CAL_FRAMES 60
#define FIND_MS 3000

extern void __exception_setreload(int t);

// wiiuse internals, exported by libwiiuse but not declared in its headers.
extern int wiiuse_io_write(struct wiimote_t *wm, ubyte *buf, int len);
extern void wiiuse_send_next_command(struct wiimote_t *wm);
extern int wiiuse_sendcmd(struct wiimote_t *wm, ubyte report_type, ubyte *msg, int len, cmd_blk_cb cb);
// lwbt, in libbte: sniff mode for a link, and the host's HCI state, whose
// u16 at offset 10 is how many ACL packets the Bluetooth controller has
// room for (lp_acl_write in hci.c).
extern s8 hci_sniff_mode(struct bd_addr *bdaddr, u16 max_interval, u16 min_interval, u16 attempt,
						 u16 timeout);
extern void *hci_dev;

#define WM_RPT_SPEAKER_DATA 0x18
#define WM_REG_SPEAKER_BLOCK 0x04a20001

// ---- Remote handles ------------------------------------------------------
//
// LEDs, IR sensitivity and the sensor bar position need wiiuse's per-remote
// handle, which libogc keeps in a static array. WPAD_Rumble loads that array
// with one small-data load right before indexing it by channel:
//     lwz rX, d(r13); slwi r3,r3,2; lwzx r3,rX,r3
// Find that load and check every handle it yields names its own channel. If
// anything does not match, these features stay off.

static wiimote **handles;
static bool handles_checked;

static bool ram(const void *p) {
	u32 a = (u32) p;
	return (a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x94000000);
}

static wiimote **find_handles(void) {
	const u32 *code = (const u32 *) WPAD_Rumble;
	register u32 r13 asm("r13");
	u32 sda = r13, i;

	for (i = 0; i < 48; ++i) {
		u32 ins = code[i];

		if ((ins >> 26) == 32 && ((ins >> 16) & 31) == 13 && code[i + 1] == 0x5463103a &&
				(code[i + 2] & 0xfc0007fe) == 0x7c00002e) {
			wiimote **arr = *(wiimote ***) (sda + (s16) (ins & 0xffff));
			int chan;

			if (!ram(arr))
				return NULL;
			for (chan = 0; chan < 4; ++chan)
				if (arr[chan] && (!ram(arr[chan]) || arr[chan]->unid != chan))
					return NULL;
			return arr;
		}
	}
	return NULL;
}

static wiimote *remote(int chan) {
	if (!handles_checked) {
		handles = find_handles();
		handles_checked = true;
	}
	if (!handles || chan < 0 || chan > 3 || !handles[chan])
		return NULL;
	return (handles[chan]->state & WM_STATE_CONNECTED) ? handles[chan] : NULL;
}

// ---- The pointer -----------------------------------------------------------
//
// The overlay turns on IR for every connected remote while it is open (and
// for any that connect meanwhile), with the IR mapped to the framebuffer,
// then gives each remote back the data format the app had it in.

static struct {
	bool set;           // this remote has the overlay's format
	int fmt;            // the app's, to restore; -1 if unknown
} ir[4];

// The data format the app left a remote in, from wiiuse's state flags; -1
// without the remote handles (then IR stays on after the overlay, which
// costs an app that did not use it only a little more data per report).
static int app_format(int chan) {
	wiimote *wm = remote(chan);

	if (!wm)
		return -1;
	return wm->state & WM_STATE_IR ? WPAD_FMT_BTNS_ACC_IR :
		   wm->state & WM_STATE_ACC ? WPAD_FMT_BTNS_ACC : WPAD_FMT_BTNS;
}

static void ir_update(int w, int h) {
	int chan;
	u32 type;

	for (chan = 0; chan < 4; ++chan) {
		bool up = WPAD_Probe(chan, &type) == WPAD_ERR_NONE;

		if (up && !ir[chan].set) {
			ir[chan].fmt = app_format(chan);
			WPAD_SetDataFormat(chan, WPAD_FMT_BTNS_ACC_IR);
			WPAD_SetVRes(chan, w, h);
			ir[chan].set = true;
		} else if (!up) {
			ir[chan].set = false;
		}
	}
}

static void ir_restore(void) {
	int chan;

	for (chan = 0; chan < 4; ++chan)
		if (ir[chan].set) {
			if (ir[chan].fmt >= 0)
				WPAD_SetDataFormat(chan, ir[chan].fmt);
			ir[chan].set = false;
		}
}

// Where each remote points, in framebuffer pixels.
static unsigned ir_read(int x[4], int y[4], float angle[4]) {
	unsigned valid = 0;
	int chan;

	for (chan = 0; chan < 4; ++chan) {
		WPADData *d = WPAD_Data(chan);

		x[chan] = y[chan] = 0;
		if (ir[chan].set && d && d->err == WPAD_ERR_NONE && d->ir.valid) {
			angle[chan] = d->ir.angle;
			x[chan] = (int) d->ir.x;
			y[chan] = (int) d->ir.y;
			valid |= 1u << chan;
		}
	}
	return valid;
}

// ---- Cost, for the status reply --------------------------------------------

static struct {
	u32 frames, avg_us, max_us;
	u32 bytes;          // framebuffer memory it borrowed
	const char *buffers;
} cost;

void agent_overlay_cost(u32 *frames, u32 *avg_us, u32 *max_us, u32 *bytes, const char **buffers) {
	*frames = cost.frames;
	*avg_us = cost.avg_us;
	*max_us = cost.max_us;
	*bytes = cost.bytes;
	*buffers = cost.buffers;
}

int agent_wpad_handles(void) {
	remote(0);
	return handles != NULL;
}

// ---- Session controller settings -----------------------------------------

static struct {
	bool rumble_off[4], rumble_all_off;
	int ir_sens;            // 0: the Wii's own setting
	int sensor_above;       // -1: the Wii's own setting
	int auto_off;           // index into auto_off_s
	bool finding[4];
	wiimote *applied[4];    // handles the settings were last applied to
} ses = { .sensor_above = -1, .auto_off = 1 };

static const u32 auto_off_s[] = { 0xffffffff, 5 * 60, 10 * 60, 15 * 60, 30 * 60 };
static const char *auto_off_names[] = { "Never", "5 min", "10 min", "15 min", "30 min" };

static void apply_remote(int chan) {
	wiimote *wm = remote(chan);
	u32 level;

	if (!wm)
		return;
	_CPU_ISR_Disable(level);
	if (ses.ir_sens)
		wiiuse_set_ir_sensitivity(wm, ses.ir_sens);
	if (ses.sensor_above >= 0)
		wiiuse_set_ir_position(wm, ses.sensor_above ? WIIUSE_IR_ABOVE : WIIUSE_IR_BELOW);
	_CPU_ISR_Restore(level);
	ses.applied[chan] = wm;
}

// libogc re-applies the Wii's settings whenever a remote connects, and apps
// may rumble at any time, so a low-cost thread keeps the session settings in
// force: it runs only while one of them is set.
static lwp_t keeper = LWP_THREAD_NULL;
static u8 keeper_stack[4096] ATTRIBUTE_ALIGN(32);

static bool keeper_needed(void) {
	int i;

	for (i = 0; i < 4; ++i)
		if (ses.rumble_off[i])
			return true;
	return ses.rumble_all_off || ses.ir_sens || ses.sensor_above >= 0;
}

static void *keeper_thread(void *arg) {
	(void) arg;
	while (keeper_needed()) {
		int chan;

		for (chan = 0; chan < 4; ++chan) {
			wiimote *wm = remote(chan);
			u32 level;

			if (!wm) {
				ses.applied[chan] = NULL;
				continue;
			}
			if (ses.applied[chan] != wm)
				apply_remote(chan);
			if ((ses.rumble_all_off || ses.rumble_off[chan]) && !ses.finding[chan] &&
					(wm->state & WM_STATE_RUMBLE)) {
				_CPU_ISR_Disable(level);
				wiiuse_rumble(wm, 0);
				_CPU_ISR_Restore(level);
			}
		}
		usleep(8 * 1000);
	}
	keeper = LWP_THREAD_NULL;
	return NULL;
}

static void keeper_start(void) {
	if (keeper == LWP_THREAD_NULL && keeper_needed())
		LWP_CreateThread(&keeper, keeper_thread, NULL, keeper_stack, sizeof(keeper_stack), 90);
}

// ---- Each remote's command queue ----------------------------------------------
//
// wiiuse sends a remote's commands one at a time and sends the next only
// when the remote acknowledges the last. Speaker data reports are never
// acknowledged, so libogc's own speaker streaming (WPAD_SendStreamData,
// which queues each 20-byte chunk as a command) stops the queue for good:
// no LEDs, no rumble (sent as a bit on the LED command), no extension
// handshake, until the remote reconnects. A lost acknowledgement does the
// same. The guard, run each overlay frame, drops a speaker chunk stuck at
// the head and re-sends any other command left unanswered for half a
// second. "Reset remotes" (DEV) empties every queue.

#define QUEUE_RESEND_FRAMES 30

static struct {
	u32 since;            // frame the head was first seen waiting, or 0
	u32 resent, dropped;
} q[4];
static u32 fx_frame;

static struct cmd_blk_t **head_of(wiimote *wm) {
	return (struct cmd_blk_t **) &wm->cmd_head;
}

// With interrupts off: take the head off the queue, back to the free pool.
static void drop_head(wiimote *wm) {
	struct cmd_blk_t *cmd = *head_of(wm);

	*head_of(wm) = cmd->next;
	if (!cmd->next)
		*(struct cmd_blk_t **) &wm->cmd_tail = NULL;
	cmd->state = CMD_DONE;
	__lwp_queue_append((lwp_queue *) &wm->cmdq, &cmd->node);
}

static void queue_guard(int chan) {
	wiimote *wm = remote(chan);
	struct cmd_blk_t *head;
	u32 level;

	if (!wm)
		return;
	_CPU_ISR_Disable(level);
	head = *head_of(wm);
	if (!head || head->state != CMD_SENT) {
		q[chan].since = 0;
	} else if (head->data[0] == WM_RPT_SPEAKER_DATA) {
		drop_head(wm);
		q[chan].dropped++;
		q[chan].since = 0;
		wiiuse_send_next_command(wm);
	} else if (!q[chan].since) {
		q[chan].since = fx_frame;
	} else if (fx_frame - q[chan].since > QUEUE_RESEND_FRAMES) {
		head->state = CMD_READY;
		wiiuse_send_next_command(wm);
		q[chan].resent++;
		q[chan].since = fx_frame;
	}
	_CPU_ISR_Restore(level);
}

static int queued(wiimote *wm) {
	struct cmd_blk_t *cmd;
	int n = 0;

	for (cmd = *head_of(wm); cmd && n < 99; cmd = cmd->next)
		n++;
	return n;
}

// DEV > Reset remotes: empty every queue and send each remote its resting
// state again (player LED, no rumble).
static void reset_remotes(void) {
	int chan;

	for (chan = 0; chan < 4; ++chan) {
		wiimote *wm = remote(chan);
		u32 level;

		if (!wm)
			continue;
		_CPU_ISR_Disable(level);
		while (*head_of(wm)) {
			drop_head(wm);
			q[chan].dropped++;
		}
		q[chan].since = 0;
		*(int *) &wm->state &= ~WIIMOTE_STATE_RUMBLE_FLAG;   // rumble off in the next report
		wiiuse_set_leds(wm, WIIMOTE_LED_1 << chan, NULL);
		_CPU_ISR_Restore(level);
	}
}

// The speaker driver's counters for `status` (see Sounds below).
static struct {
	u32 sent, skipped, wrapped;      // reports sent; blocks skipped, busy; ticks with a wrapped count
	u32 gap_max;                     // longest time between two ticks, in time-base ticks
	u16 credits_min, credits_peak;   // the controller's free ACL buffers
	bool sniff;
} sst[4] = { [0 ... 3] = { .credits_min = 0xffff } };

int agent_remote_diag(char *buf, int size) {
	int n = 0, chan;

	n += snprintf(buf + n, size - n, "[");
	for (chan = 0; chan < 4; ++chan) {
		wiimote *wm = remote(chan);
		struct cmd_blk_t *head;

		if (!wm)
			continue;
		char rpt[8] = "null";

		head = *head_of(wm);
		if (head)
			snprintf(rpt, sizeof(rpt), "\"%02x\"", head->data[0]);
		// spk: [sent, skipped, wrapped, worst gap us, fewest free ACL buffers,
		// most, sniff]
		n += snprintf(buf + n, size - n, "%s{\"chan\":%d,\"state\":\"%06x\",\"leds\":\"%02x\","
				"\"exp\":%d,\"queued\":%d,\"head\":%s,\"head_sent\":%s,\"resent\":%u,"
				"\"dropped\":%u,\"spk\":[%u,%u,%u,%u,%u,%u,%d]}", n > 1 ? "," : "", chan, wm->state,
				wm->leds, wm->exp.type, queued(wm), rpt, head && head->state == CMD_SENT ? "true" : "false",
				q[chan].resent, q[chan].dropped, sst[chan].sent, sst[chan].skipped, sst[chan].wrapped,
				(u32) ticks_to_microsecs(sst[chan].gap_max), sst[chan].credits_min, sst[chan].credits_peak,
				sst[chan].sniff);
	}
	n += snprintf(buf + n, size - n, "]");
	return n;
}

// ---- The remote's speaker: a driver after Nintendo's --------------------
//
// libogc's speaker streaming sounds broken on a real remote, and a recording
// of 1.7.5 showed why: every held note chopped by 10 to 30 dB dips every
// 10 to 40 ms, both formats. This follows Nintendo's SDK instead (WPAD, WENC
// and NW4R's RemoteSpeaker, from the doldecomp/ogws decompilation of Wii
// Sports; REVIEW.md 1.7.6 has the details):
//  - Sniff mode at 5 ms (8 slots) on the remote's link, as WUD does for
//    every remote. Without it the link runs at about 100 Hz, less than the
//    150 speaker reports a second; libogc never sets it.
//  - Nintendo's start-up: enable, mute, 0x01 to 0xa20009, 0x80 to 0xa20001,
//    the configuration block, unmute, a status request; then, once all of
//    that is answered, 0x01 to 0xa20008 (play).
//  - ADPCM at 6 kHz: 40 samples, 20 bytes, every 6.67 ms. The rate value
//    0x07d0 means 12,000,000 / rate for ADPCM too, whatever WiiBrew says;
//    the recording's notes came out an octave high at 3 kHz pacing.
//  - Each report encoded just before it goes, with Nintendo's encoder.
//    When the Bluetooth controller already holds more than 3 packets, the
//    block is skipped without encoding it, so encoder and the remote's
//    decoder stay in step; a pre-encoded sound loses its place at the
//    first lost report and turns to noise.
//  - The encoder carries on from sound to sound while the speaker is on,
//    and starts fresh only after a start-up.
// PCM (signed 8-bit) runs at 3 kHz, 20 samples a report at the same pace,
// for Test to compare with.

#define SND_ADPCM 0x00
#define SND_PCM 0x40
#define ADPCM_RATE 6000
#define PCM_RATE 3000

typedef struct {
	s16 *pcm;                                // at the format's rate
	u32 count;
	u8 format;
} snd;

typedef struct {
	float freq, start, len;                  // Hz, seconds, seconds
	float amp;                               // 0 to 1
} snd_note;

static float note_value(const snd_note *notes, int count, float t) {
	float v = 0;
	int k;

	for (k = 0; k < count; ++k) {
		const snd_note *m = &notes[k];
		float u = t - m->start, fade = 0.012f, env;

		if (u < 0 || u >= m->len)
			continue;
		env = u < fade ? 0.5f - 0.5f * cosf(3.14159265f * u / fade) :
			  m->len - u < fade ? 0.5f - 0.5f * cosf(3.14159265f * (m->len - u) / fade) : 1.f;
		v += m->amp * env * sinf(2.f * 3.14159265f * m->freq * u);
	}
	return v > 1.f ? 1.f : v < -1.f ? -1.f : v;
}

static void snd_make(snd *out, const snd_note *notes, int count, u8 format) {
	int rate = format == SND_PCM ? PCM_RATE : ADPCM_RATE;
	float end = 0;
	u32 i;
	int k;

	for (k = 0; k < count; ++k)
		if (notes[k].start + notes[k].len > end)
			end = notes[k].start + notes[k].len;
	out->format = format;
	out->count = (u32) (end * rate) + 1;
	out->pcm = malloc(out->count * sizeof(s16));
	if (!out->pcm) {
		out->count = 0;
		return;
	}
	for (i = 0; i < out->count; ++i)
		out->pcm[i] = (s16) (note_value(notes, count, (float) i / rate) * 16000.f);
}

// Find's chime: a ding-dong (E6, C6), three times, each louder.
static const snd_note chime_notes[] = {
	{ 1319, 0.00f, 0.22f, 0.45f }, { 1047, 0.25f, 0.40f, 0.45f },
	{ 1319, 0.95f, 0.22f, 0.70f }, { 1047, 1.20f, 0.40f, 0.70f },
	{ 1319, 1.90f, 0.22f, 1.00f }, { 1047, 2.15f, 0.40f, 1.00f },
};
// The volume chirp: one short E6 blip, at the new volume.
static const snd_note chirp_notes[] = { { 1319, 0.00f, 0.12f, 1.00f } };
// Test's tune, the same in both formats (under PCM's 1.5 kHz limit):
// C5 E5 G5 C6 up, then a held G5.
static const snd_note test_notes[] = {
	{ 523, 0.00f, 0.18f, 1.00f }, { 659, 0.20f, 0.18f, 1.00f }, { 784, 0.40f, 0.18f, 1.00f },
	{ 1047, 0.60f, 0.18f, 1.00f }, { 784, 0.85f, 0.60f, 1.00f },
};

#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

// Made the first time a sound is wanted, kept for the life of the app (the
// speaker's alarm reads them).
static snd chime, chirp, test_adpcm, test_pcm;

static void snd_init(void) {
	if (chime.pcm)
		return;
	snd_make(&chime, chime_notes, COUNT(chime_notes), SND_ADPCM);
	snd_make(&chirp, chirp_notes, COUNT(chirp_notes), SND_ADPCM);
	snd_make(&test_adpcm, test_notes, COUNT(test_notes), SND_ADPCM);
	snd_make(&test_pcm, test_notes, COUNT(test_notes), SND_PCM);
}

// Nintendo's 4-bit ADPCM encoder (WENCGetEncodeData in the RVL SDK):
// successive approximation against the step and its halves, the step then
// scaled by k/256 (230 ... 614) and kept within 127 to 0x6000.
typedef struct {
	int xn, dl;
} wenc;

static const u16 wenc_scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };

static u8 wenc_nibble(wenc *e, int x) {
	int l3 = x < e->xn, dn = l3 ? e->xn - x : x - e->xn;
	int dl = e->dl, dlh = dl / 2, dlq = dlh / 2, l2, l1, l0, qn;

	l2 = dn >= dl;
	if (l2)
		dn -= dl;
	l1 = dn >= dlh;
	if (l1)
		dn -= dlh;
	l0 = dn >= dlq;
	qn = dl * l2 + dlh * l1 + dlq * l0 + dlq / 2;
	if (l3)
		qn = -qn;
	qn += e->xn;
	e->xn = qn > 32767 ? 32767 : qn < -32768 ? -32768 : qn;
	dl = (dl * wenc_scale[l2 << 2 | l1 << 1 | l0]) >> 8;
	e->dl = dl < 127 ? 127 : dl > 0x6000 ? 0x6000 : dl;
	return l3 << 3 | l2 << 2 | l1 << 1 | l0;
}

// Speaker volume per remote, 0 to 10. 10 is 0x40, libogc's and the
// configuration block's default (WiiBrew gives 0x40 as ADPCM's top; the SDK
// allows 127); the steps below fall off as the ear hears loudness. PCM gets
// twice the byte.
static const u8 volume_byte[11] = { 0x00, 0x03, 0x05, 0x08, 0x0b, 0x10, 0x16, 0x1e, 0x28, 0x33, 0x40 };
static int volume[4] = { 10, 10, 10, 10 };

enum { SPK_OFF, SPK_STARTING, SPK_PLAY_SENT, SPK_READY };

static struct {
	int st;
	bool was_on;                         // libogc's speaker was on before ours
	u8 format, vol;                      // as configured
	const snd *want;                     // to play once ready
	const snd *sound;                    // playing (read by the alarm)
	u32 pos;
	bool playing;
	wenc enc;
	u8 report[22] ATTRIBUTE_ALIGN(32);
} sp[4];

static syswd_t spk_alarm;
static bool spk_alarm_made, spk_alarm_running;
static u64 spk_last_tick;

// Whether the controller can take a speaker report now. Nintendo refuses a
// packet with more than 3 un-acknowledged ones in the controller. lwbt's
// count can wrap below 0 (lp_acl_write sends even when it has no room),
// which is counted and then trusted again after 30 ticks.
static bool acl_busy(int chan) {
	u16 free = hci_dev ? *(volatile u16 *) ((u8 *) hci_dev + 10) : 0;

	if (free >= 0x8000) {
		return ++sst[chan].wrapped % 30 != 0;
	}
	if (free > sst[chan].credits_peak && free < 0x100)
		sst[chan].credits_peak = free;
	if (free < sst[chan].credits_min)
		sst[chan].credits_min = free;
	return !free || sst[chan].credits_peak - free > 3;
}

static void spk_tick(syswd_t alarm, void *arg) {
	u64 now = gettime();
	bool any = false;
	int chan;
	(void) arg;

	for (chan = 0; chan < 4; ++chan) {
		wiimote *wm = handles ? handles[chan] : NULL;
		const snd *so = sp[chan].sound;
		u32 per, n, i, bytes;
		u8 *d = sp[chan].report;

		if (!sp[chan].playing)
			continue;
		if (!wm || !(wm->state & WM_STATE_CONNECTED) || !so || sp[chan].pos >= so->count) {
			sp[chan].playing = false;
			continue;
		}
		any = true;
		if (spk_last_tick && now - spk_last_tick > sst[chan].gap_max)
			sst[chan].gap_max = now - spk_last_tick;
		per = so->format == SND_PCM ? 20 : 40;
		n = so->count - sp[chan].pos < per ? so->count - sp[chan].pos : per;
		if (acl_busy(chan)) {
			// Skip this block unencoded: the remote hears a 6.67 ms gap,
			// and its decoder and ours stay in step.
			sp[chan].pos += n;
			sst[chan].skipped++;
			continue;
		}
		memset(d, 0, sizeof(sp[chan].report));
		if (so->format == SND_PCM) {
			for (i = 0; i < n; ++i)
				d[2 + i] = (u8) (s8) (so->pcm[sp[chan].pos + i] >> 8);
			bytes = n;
		} else {
			for (i = 0; i < n; ++i) {
				u8 nib = wenc_nibble(&sp[chan].enc, so->pcm[sp[chan].pos + i]);

				d[2 + i / 2] |= i & 1 ? nib : nib << 4;
			}
			bytes = (n + 1) / 2;
		}
		d[0] = WM_RPT_SPEAKER_DATA;
		d[1] = (bytes << 3) | (wm->state & WM_STATE_RUMBLE ? 1 : 0);
		wiiuse_io_write(wm, d, 22);
		sp[chan].pos += n;
		sst[chan].sent++;
	}
	spk_last_tick = now;
	if (!any) {
		SYS_CancelAlarm(alarm);
		spk_alarm_running = false;
		spk_last_tick = 0;
	}
}

static void spk_stream(int chan, const snd *so) {
	struct timespec tb = { 0, 6666667 };
	u32 level;

	_CPU_ISR_Disable(level);
	sp[chan].sound = so;
	sp[chan].pos = 0;
	sp[chan].playing = true;
	_CPU_ISR_Restore(level);
	if (!spk_alarm_made) {
		SYS_CreateAlarm(&spk_alarm);
		spk_alarm_made = true;
	}
	if (!spk_alarm_running) {
		spk_alarm_running = true;
		SYS_SetPeriodicAlarm(spk_alarm, &tb, &tb, spk_tick, NULL);
	}
}

static void spk_halt(int chan) {
	sp[chan].playing = false;
}

// Queue Nintendo's start-up for a format and volume (WPADControlSpeaker,
// WPAD_SPEAKER_ON); play follows once it is answered (spk_frame).
static void spk_start(int chan, u8 format, u8 vol) {
	wiimote *wm = remote(chan);
	u8 conf[7] = { 0x00, format, 0, 0, vol, 0x0c, 0x0e };
	u16 rate = 12000000 / (format == SND_PCM ? PCM_RATE : ADPCM_RATE);
	u8 b;
	u32 level;

	if (!wm)
		return;
	conf[2] = rate & 0xff;
	conf[3] = rate >> 8;
	spk_halt(chan);
	_CPU_ISR_Disable(level);
	if (hci_sniff_mode((struct bd_addr *) &wm->bdaddr, 8, 8, 1, 0) == 0)
		sst[chan].sniff = true;
	b = 0x04;
	wiiuse_sendcmd(wm, 0x14, &b, 1, NULL);                // speaker enable
	b = 0x04;
	wiiuse_sendcmd(wm, 0x19, &b, 1, NULL);                // mute
	b = 0x01;
	wiiuse_write_data(wm, 0x04a20009, &b, 1, NULL);
	b = 0x80;
	wiiuse_write_data(wm, 0x04a20001, &b, 1, NULL);
	wiiuse_write_data(wm, WM_REG_SPEAKER_BLOCK, conf, sizeof(conf), NULL);
	b = 0x00;
	wiiuse_sendcmd(wm, 0x19, &b, 1, NULL);                // unmute
	wiiuse_status(wm, NULL);
	_CPU_ISR_Restore(level);
	sp[chan].format = format;
	sp[chan].vol = vol;
	sp[chan].st = SPK_STARTING;
}

static void spk_stop_remote(int chan) {
	wiimote *wm = remote(chan);
	u8 b;
	u32 level;

	spk_halt(chan);
	sp[chan].want = NULL;
	if (sp[chan].st == SPK_OFF)
		return;
	sp[chan].st = SPK_OFF;
	if (!wm)
		return;
	if (sp[chan].was_on) {
		// Leave it as libogc set it up: ADPCM, its volume, playing.
		spk_start(chan, SND_ADPCM, 0x40);
		sp[chan].st = SPK_OFF;
		b = 0x01;
		_CPU_ISR_Disable(level);
		wiiuse_write_data(wm, 0x04a20008, &b, 1, NULL);
		_CPU_ISR_Restore(level);
		return;
	}
	_CPU_ISR_Disable(level);
	b = 0x04;
	wiiuse_sendcmd(wm, 0x19, &b, 1, NULL);                // mute
	b = 0x01;
	wiiuse_write_data(wm, 0x04a20001, &b, 1, NULL);
	b = 0x00;
	wiiuse_write_data(wm, 0x04a20009, &b, 1, NULL);
	b = 0x00;
	wiiuse_sendcmd(wm, 0x14, &b, 1, NULL);                // speaker off
	wiiuse_status(wm, NULL);
	_CPU_ISR_Restore(level);
}

static u8 vol_for(int chan, u8 format) {
	return volume_byte[volume[chan]] * (format == SND_PCM ? 2 : 1);
}

// Play a sound on a remote, starting or re-starting the speaker when its
// format or volume differs from what it is set up for.
static void spk_play_sound(int chan, const snd *so) {
	if (!so->pcm || !remote(chan))
		return;
	spk_halt(chan);
	sp[chan].want = so;
	if (sp[chan].st == SPK_OFF) {
		if (!sp[chan].was_on)
			sp[chan].was_on = WPAD_IsSpeakerEnabled(chan) == WPAD_ERR_NONE;
		spk_start(chan, so->format, vol_for(chan, so->format));
	} else if (sp[chan].format != so->format || sp[chan].vol != vol_for(chan, so->format)) {
		spk_start(chan, so->format, vol_for(chan, so->format));
	}
}

// From the frame loop: play after the start-up is answered, then the sound.
static void spk_frame(int chan) {
	wiimote *wm = remote(chan);
	u32 type;
	u8 b = 0x01;
	u32 level;

	if (sp[chan].st == SPK_OFF)
		return;
	if (!wm || WPAD_Probe(chan, &type) != WPAD_ERR_NONE) {
		spk_halt(chan);
		sp[chan].st = SPK_OFF;
		sp[chan].want = NULL;
		return;
	}
	if (*head_of(wm))
		return;
	if (sp[chan].st == SPK_STARTING) {
		_CPU_ISR_Disable(level);
		wiiuse_write_data(wm, 0x04a20008, &b, 1, NULL);   // play
		_CPU_ISR_Restore(level);
		sp[chan].st = SPK_PLAY_SENT;
	} else if (sp[chan].st == SPK_PLAY_SENT) {
		// A fresh start-up: the remote's decoder starts from rest.
		sp[chan].enc.xn = 0;
		sp[chan].enc.dl = 127;
		sp[chan].st = SPK_READY;
	}
	if (sp[chan].st == SPK_READY && sp[chan].want) {
		spk_stream(chan, sp[chan].want);
		sp[chan].want = NULL;
	}
}

// ---- Find, sounds, and rumble feedback, run from the overlay's frame loop ----
//
// Find only plays its chime: every LED or rumble change is another command
// for the remote, and the speaker needs the link.

#define FIND_TIMEOUT 600                 // frames: give up if the speaker never starts

static struct {
	u32 find_end, pulse_end;             // frame numbers; 0 when idle
	int rumble;                          // what was last sent; -1 unknown
} fx[4];

static void fx_rumble(int chan, int on) {
	if (fx[chan].rumble == on)
		return;
	WPAD_Rumble(chan, on);
	fx[chan].rumble = on;
}

static void fx_sound(int chan, const snd *so) {
	spk_play_sound(chan, so);
}

static void find_start(int chan) {
	if (fx[chan].find_end)
		return;
	snd_init();
	fx[chan].find_end = fx_frame + FIND_TIMEOUT;
	ses.finding[chan] = true;
	fx_sound(chan, &chime);
}

static void find_stop(int chan) {
	if (sp[chan].sound == &chime || sp[chan].want == &chime) {
		spk_halt(chan);
		if (sp[chan].want == &chime)
			sp[chan].want = NULL;
	}
	fx[chan].find_end = 0;
	ses.finding[chan] = false;
}

// A short buzz when rumble is switched on.
static void pulse(int chan) {
	u32 type;

	if (WPAD_Probe(chan, &type) != WPAD_ERR_NONE)
		return;
	fx[chan].rumble = -1;
	fx_rumble(chan, 1);
	fx[chan].pulse_end = fx_frame + 8;
}

static void fx_tick(void) {
	int chan;

	fx_frame++;
	for (chan = 0; chan < 4; ++chan) {
		queue_guard(chan);
		spk_frame(chan);

		if (fx[chan].pulse_end && fx_frame >= fx[chan].pulse_end) {
			fx_rumble(chan, 0);
			fx[chan].pulse_end = 0;
		}
		// Find lasts as long as its chime.
		if (fx[chan].find_end) {
			bool queued_chime = sp[chan].want == &chime;
			bool playing_chime = sp[chan].sound == &chime && sp[chan].playing;

			if (fx_frame >= fx[chan].find_end || sp[chan].st == SPK_OFF ||
					(!queued_chime && !playing_chime))
				find_stop(chan);
		}
	}
}

static void fx_stop_all(void) {
	int chan;

	for (chan = 0; chan < 4; ++chan) {
		if (fx[chan].find_end)
			find_stop(chan);
		spk_stop_remote(chan);
		if (fx[chan].pulse_end) {
			fx_rumble(chan, 0);
			fx[chan].pulse_end = 0;
		}
	}
}

// ---- Calibration --------------------------------------------------------

static hbc_agent_cal cal[4];
#define CAL_WAIT_FRAMES 300          // 5 s to put the remote down

static struct {
	int chan, frames, wait;
	s64 sum[8];
} cal_run = { -1 };

static void cal_sample(void) {
	WPADData *d;
	int c = cal_run.chan;

	if (c < 0 || cal_run.frames >= CAL_FRAMES)
		return;
	if (cal_run.wait > 0) {
		cal_run.wait--;
		return;
	}
	d = WPAD_Data(c);
	if (!d || d->err != WPAD_ERR_NONE)
		return;
	cal_run.sum[0] += d->accel.x;
	cal_run.sum[1] += d->accel.y;
	cal_run.sum[2] += d->accel.z;
	if (d->exp.type == EXP_MOTION_PLUS) {
		cal_run.sum[3] += d->exp.mp.rx;
		cal_run.sum[4] += d->exp.mp.ry;
		cal_run.sum[5] += d->exp.mp.rz;
	}
	if (d->exp.type == EXP_NUNCHUK) {
		cal_run.sum[6] += d->exp.nunchuk.js.pos.x;
		cal_run.sum[7] += d->exp.nunchuk.js.pos.y;
	}
	if (++cal_run.frames == CAL_FRAMES) {
		hbc_agent_cal *r = &cal[c];
		int i;

		r->valid = true;
		for (i = 0; i < 3; ++i)
			r->accel[i] = cal_run.sum[i] / CAL_FRAMES;
		r->motionplus = d->exp.type == EXP_MOTION_PLUS;
		for (i = 0; i < 3; ++i)
			r->gyro[i] = cal_run.sum[3 + i] / CAL_FRAMES;
		r->stick[0] = cal_run.sum[6] / CAL_FRAMES;
		r->stick[1] = cal_run.sum[7] / CAL_FRAMES;
	}
}

bool hbc_agent_calibration(int chan, hbc_agent_cal *out) {
	if (chan < 0 || chan > 3 || !cal[chan].valid)
		return false;
	*out = cal[chan];
	return true;
}

// ---- Screenshot ---------------------------------------------------------

// Writes a 24-bit BMP of a YUYV frame to sd:/screenshots/<app>-NNN.bmp.
static bool screenshot(const u8 *yuyv, int w, int h, char *path, size_t size) {
	u8 hdr[54] = { 'B', 'M' };
	u32 row = w * 3, img = row * h, i;
	u8 *line;
	FILE *f = NULL;
	int y, x, n;

	mkdir("sd:/screenshots", 0777);
	for (n = 1; n < 1000; ++n) {
		snprintf(path, size, "sd:/screenshots/%s-%03d.bmp", agent_cfg()->name, n);
		if (access(path, F_OK))
			break;
	}
	f = fopen(path, "wb");
	line = malloc(row);
	if (!f || !line) {
		if (f)
			fclose(f);
		free(line);
		return false;
	}
	*(u32 *) (hdr + 2) = __builtin_bswap32(54 + img);
	hdr[10] = 54;
	hdr[14] = 40;
	*(u32 *) (hdr + 18) = __builtin_bswap32(w);
	*(u32 *) (hdr + 22) = __builtin_bswap32(h);
	hdr[26] = 1;
	hdr[28] = 24;
	*(u32 *) (hdr + 34) = __builtin_bswap32(img);
	fwrite(hdr, 1, sizeof(hdr), f);
	for (y = h - 1; y >= 0; --y) {
		const u8 *s = yuyv + y * w * 2;

		for (x = 0; x < w; ++x) {
			int c = s[(x & ~1) * 2 + (x & 1) * 2] - 16, d = s[(x & ~1) * 2 + 1] - 128;
			int e = s[(x & ~1) * 2 + 3] - 128;
			int r = (298 * c + 409 * e + 128) >> 8;
			int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
			int b = (298 * c + 516 * d + 128) >> 8;

			line[x * 3] = b < 0 ? 0 : b > 255 ? 255 : b;
			line[x * 3 + 1] = g < 0 ? 0 : g > 255 ? 255 : g;
			line[x * 3 + 2] = r < 0 ? 0 : r > 255 ? 255 : r;
		}
		fwrite(line, 1, row, f);
	}
	free(line);
	i = fclose(f);
	return i == 0;
}

// ---- The overlay ---------------------------------------------------------

typedef struct {
	ov_ext ext;
	u8 *frozen;               // the app's last frame, undimmed
	int w, h, toast_frames;
	int test_chan;
	int test_fmt;             // data format to restore after the test page
	int exit_choice;          // an Exit choice the app handles, + 1; 0 for none
	char sd[24];
	volatile bool sd_done;
} ov_run;

static ov_run *run;

static void toast(const char *msg) {
	snprintf(run->ext.toast, sizeof(run->ext.toast), "%s", msg);
	run->toast_frames = TOAST_FRAMES;
}

static void *sd_thread(void *arg) {
	struct statvfs st;
	ov_run *r = arg;

	if (!statvfs("sd:/", &st) && st.f_frsize) {
		double gb = (double) st.f_bavail * st.f_frsize / 1e9;

		snprintf(r->sd, sizeof(r->sd), gb >= 10 ? "%.0f GB free" : "%.1f GB free", gb);
	} else {
		snprintf(r->sd, sizeof(r->sd), "No card");
	}
	r->sd_done = true;
	return NULL;
}

static const char *ext_name(const WPADData *d) {
	switch (d->exp.type) {
	case EXP_NUNCHUK: return "Nunchuk";
	case EXP_CLASSIC: return "Classic Controller";
	case EXP_GUITAR_HERO_3: return "Guitar";
	case EXP_WII_BOARD: return "Balance Board";
	case EXP_MOTION_PLUS: return "";
	default: return "";
	}
}

static void button_names(u32 b, char *out, size_t size) {
	static const struct { u32 bit; const char *name; } names[] = {
		{ WPAD_BUTTON_A, "A" }, { WPAD_BUTTON_B, "B" }, { WPAD_BUTTON_1, "1" },
		{ WPAD_BUTTON_2, "2" }, { WPAD_BUTTON_MINUS, "-" }, { WPAD_BUTTON_PLUS, "+" },
		{ WPAD_BUTTON_UP, "Up" }, { WPAD_BUTTON_DOWN, "Down" }, { WPAD_BUTTON_LEFT, "Left" },
		{ WPAD_BUTTON_RIGHT, "Right" }, { WPAD_NUNCHUK_BUTTON_C, "C" },
		{ WPAD_NUNCHUK_BUTTON_Z, "Z" },
	};
	size_t n = 0, i;

	out[0] = 0;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		if (b & names[i].bit)
			n += snprintf(out + n, size - n, "%s%s", n ? ", " : "", names[i].name);
	if (!n)
		snprintf(out, size, "none held");
}

static void poll(ov_run *r) {
	ov_ext *e = &r->ext;
	const hbc_agent_config *cfg = agent_cfg();
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	u32 up = agent_uptime_ms() / 60000, ip = net_gethostip();
	int chan;

	snprintf(e->app, sizeof(e->app), "%s%s%s", cfg->name, cfg->version[0] ? " " : "", cfg->version);
	strftime(e->clock, sizeof(e->clock), "%H:%M", tm);
	strftime(e->date, sizeof(e->date), "%H:%M, %a %d %b", tm);
	if (up < 60)
		snprintf(e->playing, sizeof(e->playing), "%u min", up);
	else
		snprintf(e->playing, sizeof(e->playing), "%u h %u min", up / 60, up % 60);
	if (ip)
		snprintf(e->network, sizeof(e->network), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff,
				 (ip >> 8) & 0xff, ip & 0xff);
	else
		snprintf(e->network, sizeof(e->network), "Not connected");
	snprintf(e->sd, sizeof(e->sd), "%s", r->sd_done ? r->sd : "Checking...");
	e->mem_free_kb[0] = SYS_GetArena1Size() / 1024;
	e->mem_free_kb[1] = SYS_GetArena2Size() / 1024;
	e->mem_total_kb[0] = 24 * 1024;
	e->mem_total_kb[1] = 64 * 1024;

	for (chan = 0; chan < OV_REMOTES; ++chan) {
		ov_remote *rm = &e->remote[chan];
		u32 type;
		WPADData *d = WPAD_Data(chan);

		memset(rm, 0, sizeof(*rm));
		if (WPAD_Probe(chan, &type) != WPAD_ERR_NONE || !d)
			continue;
		rm->connected = true;
		rm->battery_pct = WPAD_BatteryLevel(chan) * 100 / 208;
		if (rm->battery_pct > 100)
			rm->battery_pct = 100;
		rm->battery = rm->battery_pct ? (rm->battery_pct + 24) / 25 : 0;
		if (rm->battery > 4)
			rm->battery = 4;
		snprintf(rm->ext, sizeof(rm->ext), "%s", ext_name(d));
		rm->motionplus = d->exp.type == EXP_MOTION_PLUS ||
				(remote(chan) && remote(chan)->state & 0x100000);
		rm->rumble = !ses.rumble_off[chan];
		rm->volume = volume[chan];
		rm->finding = ses.finding[chan];
	}

	e->log_pc = agent_log_to_pc;
	e->crash_stay = agent_crash_stay;
	e->hbcpy = agent_listen_enabled;
	e->has_save = cfg->on_save != NULL;
	e->has_restart = cfg->on_restart != NULL;
	remote(0);
	e->can_leds = handles != NULL;
	e->sensor_above = ses.sensor_above < 0 ?
		CONF_GetSensorBarPosition() == CONF_SENSORBAR_TOP : ses.sensor_above;
	e->ir_sens = ses.ir_sens ? ses.ir_sens : CONF_GetIRSensitivity();
	snprintf(e->auto_off, sizeof(e->auto_off), "%s", auto_off_names[ses.auto_off]);
	e->rumble_all = !ses.rumble_all_off;
	snprintf(e->slot[0], sizeof(e->slot[0]), "%s", agent_slot_label(0));
	snprintf(e->slot[1], sizeof(e->slot[1]), "%s", agent_slot_label(1));
	for (chan = 0; chan < 2; ++chan) {
		const char *title;
		int count, i;
		const hbc_agent_item *items = agent_slot_menu(chan, &title, &count);
		ov_menu *m = &e->menu[chan];

		snprintf(m->title, sizeof(m->title), "%s", title);
		m->count = count;
		for (i = 0; i < count; ++i) {
			snprintf(m->item[i].label, sizeof(m->item[i].label), "%s",
					 items[i].label ? items[i].label : "");
			snprintf(m->item[i].value, sizeof(m->item[i].value), "%s",
					 items[i].value ? items[i].value : "");
			m->item[i].flags = (items[i].value ? OV_ITEM_INFO : 0) |
					(items[i].flags & HBC_AGENT_ITEM_CLOSE ? OV_ITEM_CLOSE : 0) |
					(items[i].flags & HBC_AGENT_ITEM_DISABLED ? OV_ITEM_DISABLED : 0);
		}
	}
	e->show_net = e->show_log_pc = !cfg->no_network;
	e->show_crash = !cfg->no_crash_handler;
	e->exit_mask = 15 & ~cfg->hide_exit_choices;
	e->log_text = agent_log_text();

	if (r->test_chan >= 0) {
		WPADData *d = WPAD_Data(r->test_chan);
		char held[40];

		memset(e->test, 0, sizeof(e->test));
		if (d && d->err == WPAD_ERR_NONE) {
			button_names(d->btns_h, held, sizeof(held));
			snprintf(e->test[0], sizeof(e->test[0]), "Buttons\t%s", held);
			if (d->ir.valid)
				snprintf(e->test[1], sizeof(e->test[1]), "Pointer\tx %d, y %d", (int) d->ir.x,
						 (int) d->ir.y);
			else
				snprintf(e->test[1], sizeof(e->test[1]), "Pointer\tnot on screen");
			snprintf(e->test[2], sizeof(e->test[2]), "Tilt\tpitch %d, roll %d",
					 (int) d->orient.pitch, (int) d->orient.roll);
			snprintf(e->test[3], sizeof(e->test[3]), "Accel\t%d, %d, %d", d->accel.x, d->accel.y,
					 d->accel.z);
			if (d->exp.type == EXP_MOTION_PLUS)
				snprintf(e->test[4], sizeof(e->test[4]), "MotionPlus\t%d, %d, %d", d->exp.mp.rx,
						 d->exp.mp.ry, d->exp.mp.rz);
			else if (d->exp.type == EXP_NUNCHUK)
				snprintf(e->test[4], sizeof(e->test[4]), "Nunchuk\tstick %d, %d",
						 d->exp.nunchuk.js.pos.x, d->exp.nunchuk.js.pos.y);
		}
	}

	if (cal_run.chan >= 0) {
		const hbc_agent_cal *c = &cal[cal_run.chan];

		e->cal_progress = cal_run.frames * 100 / CAL_FRAMES;
		e->cal_wait_s = (cal_run.wait + 59) / 60;
		if (c->valid && c->motionplus)
			snprintf(e->cal_result, sizeof(e->cal_result), "Gyro at rest: %d, %d, %d",
					 c->gyro[0], c->gyro[1], c->gyro[2]);
		else if (c->valid)
			snprintf(e->cal_result, sizeof(e->cal_result), "Level: %d, %d, %d", c->accel[0],
					 c->accel[1], c->accel[2]);
	}

	if (r->toast_frames && !--r->toast_frames)
		e->toast[0] = 0;
}

static void set_all(void (*fn)(int chan)) {
	int chan;

	for (chan = 0; chan < 4; ++chan)
		fn(chan);
}

static void disconnect(int chan) {
	WPAD_Disconnect(chan);
}

// Exit's choices, HBC_AGENT_EXIT_*.
static void do_exit(int choice) {
	const hbc_agent_config *cfg = agent_cfg();

	if (cfg->on_exit)
		cfg->on_exit(cfg->user);
	VIDEO_SetBlack(true);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (choice == HBC_AGENT_EXIT_HBC)
		exit(0);
	SYS_ResetSystem(choice == HBC_AGENT_EXIT_SYSTEM_MENU ? SYS_RETURNTOMENU :
					choice == HBC_AGENT_EXIT_RESTART ? SYS_RESTART : SYS_POWEROFF, 0, 0);
}

static void act(int action, int arg, void *user) {
	const hbc_agent_config *cfg = agent_cfg();
	char path[64];
	(void) user;

	switch (action) {
	case OVA_HBC:
	case OVA_SYSMENU:
	case OVA_RESTART_WII:
	case OVA_POWEROFF:
		// The app asked to handle Exit itself: close first (the loop feeds
		// HOME), then call it from hbc_agent_home's caller's thread.
		if (cfg->on_exit_choice) {
			run->exit_choice = action - OVA_HBC + 1;
			break;
		}
		do_exit(action - OVA_HBC);
		break;
	case OVA_SLOT_ITEM: {
		const char *title;
		int count;
		const hbc_agent_item *items = agent_slot_menu(arg / 16, &title, &count);

		if (arg % 16 < count && items[arg % 16].press)
			items[arg % 16].press(items[arg % 16].user);
		break;
	}
	case OVA_SHOT:
		toast(screenshot(run->frozen, run->w, run->h, path, sizeof(path)) ? path :
			  "Couldn't save the screenshot. Is the SD card in?");
		break;
	case OVA_SAVE:
		toast(cfg->on_save && cfg->on_save(cfg->user) ? "Saved" : "Couldn't save");
		break;
	case OVA_LOG_PC:
		hbc_agent_log_muted = !arg;
		break;
	case OVA_CRASH_STAY:
		agent_crash_stay = arg;
		__exception_setreload(arg ? 0 : (cfg->crash_reload_s > 0 ? cfg->crash_reload_s : 3));
		break;
	case OVA_HBCPY:
		agent_listen_enabled = arg;
		break;
	case OVA_FIND:
		find_start(arg);
		break;
	case OVA_RUMBLE:
		ses.rumble_off[arg] = !ses.rumble_off[arg];
		keeper_start();
		// Feel it: a short buzz when rumble comes on.
		if (!ses.rumble_off[arg] && !ses.rumble_all_off)
			pulse(arg);
		break;
	case OVA_DISCONNECT:
		WPAD_Disconnect(arg);
		break;
	case OVA_TEST_START:
		run->test_chan = arg;
		break;
	case OVA_TEST_STOP:
		run->test_chan = -1;
		break;
	case OVA_CAL_START:
		memset(&cal_run, 0, sizeof(cal_run));
		cal_run.chan = arg;
		cal_run.wait = CAL_WAIT_FRAMES;
		cal[arg].valid = false;
		break;
	case OVA_CONNECT:
		WPAD_StartPairing();
		toast("Press 1 and 2 on the remote");
		break;
	case OVA_DISCONNECT_ALL:
		set_all(disconnect);
		break;
	case OVA_SENSOR_ABOVE:
		ses.sensor_above = arg;
		set_all(apply_remote);
		keeper_start();
		break;
	case OVA_IR_SENS:
		ses.ir_sens = arg;
		set_all(apply_remote);
		keeper_start();
		break;
	case OVA_AUTO_OFF:
		ses.auto_off += arg;
		if (ses.auto_off < 0)
			ses.auto_off = 0;
		if (ses.auto_off > 4)
			ses.auto_off = 4;
		WPAD_SetIdleTimeout(auto_off_s[ses.auto_off]);
		break;
	case OVA_VOLUME: {
		int chan = arg & 15;

		volume[chan] += arg & 16 ? 1 : -1;
		if (volume[chan] < 0)
			volume[chan] = 0;
		if (volume[chan] > 10)
			volume[chan] = 10;
		// Let them hear it: a blip at the new level (re-starting the speaker
		// with the new volume, as the SDK applies one).
		snd_init();
		fx_sound(chan, &chirp);
		break;
	}
	case OVA_SOUND_TEST:
		snd_init();
		fx_sound(arg & 15, arg & 16 ? &test_pcm : &test_adpcm);
		break;
	case OVA_RESET_REMOTES:
		reset_remotes();
		fx_stop_all();
		toast("Remotes reset: queues emptied");
		break;
	case OVA_RUMBLE_ALL:
		ses.rumble_all_off = !arg;
		keeper_start();
		if (arg) {
			int chan;

			for (chan = 0; chan < 4; ++chan)
				if (!ses.rumble_off[chan])
					pulse(chan);
		}
		break;
	}
}

static int last_chan = -1;

static unsigned read_input(void) {
	static u32 key_wait;
	unsigned out = 0;
	int chan;

	// Keys from the PC, one every 12 frames so animations can finish.
	if (key_wait)
		key_wait--;
	else {
		switch (agent_key_pop()) {
		case 'u': out = OV_UP; break;
		case 'd': out = OV_DOWN; break;
		case 'l': out = OV_LEFT; break;
		case 'r': out = OV_RIGHT; break;
		case 'a': out = OV_A; break;
		case 'b': out = OV_B; break;
		case 'h': out = OV_HOME; break;
		case '1': out = OV_1; break;
		case '2': out = OV_2; break;
		}
		if (out) {
			out |= OV_ANY;
			key_wait = 12;
		}
	}

	// GameCube controllers, if the app set them up.
	if (agent_cfg()->gc_pads) {
		PAD_ScanPads();
		for (chan = 0; chan < 4; ++chan) {
			u16 b = PAD_ButtonsDown(chan);

			out |= (b & PAD_BUTTON_UP ? OV_UP : 0) | (b & PAD_BUTTON_DOWN ? OV_DOWN : 0) |
				   (b & PAD_BUTTON_LEFT ? OV_LEFT : 0) | (b & PAD_BUTTON_RIGHT ? OV_RIGHT : 0) |
				   (b & PAD_BUTTON_A ? OV_A : 0) | (b & PAD_BUTTON_B ? OV_B : 0) |
				   (b & PAD_BUTTON_START ? OV_HOME : 0) | (b ? OV_ANY : 0);
		}
	}

	WPAD_ScanPads();
	for (chan = 0; chan < 4; ++chan) {
		u32 b = WPAD_ButtonsDown(chan);

		if (b & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP))
			out |= OV_UP;
		if (b & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN))
			out |= OV_DOWN;
		if (b & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT))
			out |= OV_LEFT;
		if (b & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT))
			out |= OV_RIGHT;
		if (b & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A))
			out |= OV_A;
		if (b & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B))
			out |= OV_B;
		if (b & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME))
			out |= OV_HOME;
		if (b & WPAD_BUTTON_1)
			out |= OV_1;
		if (b & WPAD_BUTTON_2)
			out |= OV_2;
		// + and - together: leave the Test page (which shows every other button).
		if ((WPAD_ButtonsHeld(chan) & (WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS)) ==
				(WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS) && (b & (WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS)))
			out |= OV_TEST_EXIT;
		if (b) {
			out |= OV_ANY;
			last_chan = chan;
		}
	}
	return out;
}

static bool in_mem1(const void *p) {
	return p && ((u32) p & 0x1fffffff) < 0x01800000;
}

// fb0 and fb1 are the app's (lent) or NULL (allocate our own).
static s32 home(const GXRModeObj *rmode, void *fb0, void *fb1) {
	static bool inside;
	static ov_ui ui;
	void *app_fb = VIDEO_GetCurrentFramebuffer();
	u8 *fb[2] = { fb0, fb1 };
	bool own = !fb0;
	ov_run r;
	u32 size;
	lwp_t sd = LWP_THREAD_NULL;
	int cur = 0, i;
	bool running = true;

	if (inside || !app_fb)
		return -EBUSY;
	if (!rmode)
		rmode = VIDEO_GetPreferredMode(NULL);
	memset(&r, 0, sizeof(r));
	r.w = rmode->fbWidth;
	r.h = rmode->xfbHeight;
	r.test_chan = -1;
	size = r.w * r.h * 2;
	agent_set_screen_size(r.w, r.h);
	app_fb = agent_uncached(app_fb);

	// The frozen frame (any memory: only the CPU reads it), and framebuffers
	// to draw into without tearing. The video interface reads only MEM1, so
	// buffers the heap gives from MEM2 are no use; then draw over the app's
	// own framebuffer, putting its picture back on the way out.
	r.frozen = memalign(32, size);
	if (own) {
		fb[0] = memalign(32, size);
		fb[1] = memalign(32, size);
		for (i = 1; i >= 0; --i)
			if (fb[i] && !in_mem1(fb[i])) {
				free(fb[i]);
				fb[i] = NULL;
			}
		if (!fb[0]) {
			fb[0] = fb[1];
			fb[1] = NULL;
		}
	}
	if (!r.frozen) {
		if (own) {
			free(fb[0]);
			free(fb[1]);
		}
		return -ENOMEM;
	}
	if (!fb[0])
		fb[0] = (u8 *) ((u32) app_fb & ~0x40000000);   // cached, like ours
	inside = true;
	run = &r;
	memcpy(r.frozen, app_fb, size);
	LWP_CreateThread(&sd, sd_thread, &r, NULL, 16 * 1024, 30);

	// Like the Wii's HOME Menu, pausing stops every remote's rumble; an app
	// (HBC's hover buzz, for one) may have started one its loop would stop.
	WPAD_Rumble(WPAD_CHAN_ALL, 0);
	// A 16:9 TV stretches the 640-pixel picture: use condensed text and
	// pointer so they look right.
	ov_set_widescreen(CONF_GetAspectRatio() == CONF_ASPECT_16_9);
	ov_init(&ui, r.w, r.h);
	last_chan = -1;
	memset(&cost, 0, sizeof(cost));
	cost.buffers = !own ? "lent" : fb[0] == (u8 *) ((u32) app_fb & ~0x40000000) ? "app's" : "own";
	cost.bytes = !own ? (fb[1] ? 2 : 1) * size : (fb[1] ? size : 0) +
			(cost.buffers[0] == 'o' ? size : 0);
	cost.bytes += size;   // the frozen frame
	while (running) {
		ov_canvas c;
		unsigned pressed, pointing;
		int px[4], py[4];
		float pa[4] = { 0, 0, 0, 0 };
		u8 *back = fb[fb[1] ? cur : 0];
		u64 t0 = gettime();
		u32 us;

		if (agent_cfg()->on_frame)
			agent_cfg()->on_frame(agent_cfg()->user);
		poll(&r);
		ir_update(r.w, r.h);
		pressed = read_input();
		pointing = ir_read(px, py, pa);
		ov_point(&ui, px, py, pa, pointing, last_chan);
		cal_sample();
		fx_tick();
		// An Exit choice the app handles: close everything first.
		if (r.exit_choice && !ui.closing)
			pressed = OV_HOME;
		running = ov_step(&ui, &r.ext, pressed, act, NULL);

		// Nothing on screen changed (a menu just sitting there): keep the
		// picture the VI already shows, and spend no time drawing.
		if (!ov_changed(&ui, &r.ext)) {
			VIDEO_WaitVSync();
			continue;
		}
		c.fb = back;
		c.w = r.w;
		c.h = r.h;
		c.stride = r.w * 2;
		ov_noclip(&c);
		if (ui.paused) {
			memcpy(back, r.frozen, size);
		} else {
			ov_dim_copy(&c, r.frozen, 96 + (ui.menu ? 0 : 60) + (256 - ui.open_t) * 100 / 256);
			ov_draw(&ui, &r.ext, &c);
		}
		DCFlushRange(back, size);
		// The overlay's own work this frame, before it waits for the VI.
		us = ticks_to_microsecs(diff_ticks(t0, gettime()));
		cost.avg_us = (cost.avg_us * cost.frames + us) / (cost.frames + 1);
		if (us > cost.max_us)
			cost.max_us = us;
		cost.frames++;
		VIDEO_SetNextFramebuffer(back);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		cur ^= 1;
	}

	// Give the app its picture back: if we drew over its framebuffer,
	// restore what was there first.
	if (((u32) fb[0] & 0x1fffffff) == ((u32) app_fb & 0x1fffffff)) {
		memcpy(app_fb, r.frozen, size);
		DCFlushRange(fb[0], size);
	}
	VIDEO_SetNextFramebuffer(app_fb);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	fx_stop_all();
	ir_restore();
	if (sd != LWP_THREAD_NULL)
		LWP_JoinThread(sd, NULL);
	if (own) {
		if (in_mem1(fb[1]))
			free(fb[1]);
		if (((u32) fb[0] & 0x1fffffff) != ((u32) app_fb & 0x1fffffff))
			free(fb[0]);
	}
	free(r.frozen);
	run = NULL;
	inside = false;

	if (ui.after == OVA_RESTART_APP && agent_cfg()->on_restart)
		agent_cfg()->on_restart(agent_cfg()->user);
	else if (ui.after == OVA_SLOT)
		agent_slot_press(ui.after_arg);
	else if (ui.after == OVA_SLOT_ITEM) {
		const char *title;
		int count;
		const hbc_agent_item *items = agent_slot_menu(ui.after_arg / 16, &title, &count);

		if (ui.after_arg % 16 < count && items[ui.after_arg % 16].press)
			items[ui.after_arg % 16].press(items[ui.after_arg % 16].user);
	}
	if (r.exit_choice) {
		const hbc_agent_config *cfg = agent_cfg();

		if (!cfg->on_exit_choice(r.exit_choice - 1, cfg->user))
			do_exit(r.exit_choice - 1);
	}
	return 0;
}

s32 hbc_agent_home(const GXRModeObj *rmode) {
	return home(rmode, NULL, NULL);
}

s32 hbc_agent_home_fb(const GXRModeObj *rmode, void *fb0, void *fb1) {
	if (!fb0 || !in_mem1(fb0) || (fb1 && !in_mem1(fb1)))
		return -EINVAL;
	return home(rmode, (void *) ((u32) fb0 & ~0x40000000),
				fb1 ? (void *) ((u32) fb1 & ~0x40000000) : NULL);
}
