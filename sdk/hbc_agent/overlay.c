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
#define SPEAKER_RATE 6000   // libogc's speaker set-up: 4-bit ADPCM at 6 kHz

extern void __exception_setreload(int t);

// wiiuse internals, exported by libwiiuse but not declared in its headers.
extern int wiiuse_io_write(struct wiimote_t *wm, ubyte *buf, int len);
extern void wiiuse_send_next_command(struct wiimote_t *wm);

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
static void reset_leds(int chan) {
	wiimote *wm = remote(chan);
	u32 level;

	if (!wm)
		return;
	_CPU_ISR_Disable(level);
	*(int *) &wm->state &= ~WIIMOTE_STATE_RUMBLE_FLAG;
	wiiuse_set_leds(wm, WIIMOTE_LED_1 << chan, NULL);
	_CPU_ISR_Restore(level);
}

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
		n += snprintf(buf + n, size - n, "%s{\"chan\":%d,\"state\":\"%06x\",\"leds\":\"%02x\","
				"\"exp\":%d,\"queued\":%d,\"head\":%s,\"head_sent\":%s,\"resent\":%u,"
				"\"dropped\":%u}", n > 1 ? "," : "", chan, wm->state, wm->leds, wm->exp.type,
				queued(wm), rpt, head && head->state == CMD_SENT ? "true" : "false",
				q[chan].resent, q[chan].dropped);
	}
	n += snprintf(buf + n, size - n, "]");
	return n;
}

// Speaker data, sent the way games do: one raw report every 6.67 ms, not
// through the command queue. Each carries the remote's rumble bit, as every
// output report must, or it would switch the rumble off.
static struct {
	const u8 *data;
	u32 len, off;
	bool on;
	u8 report[22] ATTRIBUTE_ALIGN(32);
} spk[4];
static syswd_t spk_alarm;
static bool spk_alarm_made, spk_alarm_running;

static void spk_tick(syswd_t alarm, void *arg) {
	bool any = false;
	int chan;
	(void) arg;

	for (chan = 0; chan < 4; ++chan) {
		wiimote *wm = handles ? handles[chan] : NULL;
		u32 n;

		if (!spk[chan].on)
			continue;
		if (!wm || !(wm->state & WM_STATE_CONNECTED) || spk[chan].off >= spk[chan].len) {
			spk[chan].on = false;
			continue;
		}
		n = spk[chan].len - spk[chan].off > 20 ? 20 : spk[chan].len - spk[chan].off;
		memset(spk[chan].report, 0, sizeof(spk[chan].report));
		spk[chan].report[0] = WM_RPT_SPEAKER_DATA;
		spk[chan].report[1] = (n << 3) | (wm->state & WM_STATE_RUMBLE ? 1 : 0);
		memcpy(spk[chan].report + 2, spk[chan].data + spk[chan].off, n);
		wiiuse_io_write(wm, spk[chan].report, 22);
		spk[chan].off += n;
		any = true;
	}
	if (!any) {
		SYS_CancelAlarm(alarm);
		spk_alarm_running = false;
	}
}

static void spk_play(int chan, const u8 *data, u32 len) {
	struct timespec tb = { 0, 6666667 };
	u32 level;

	_CPU_ISR_Disable(level);
	spk[chan].data = data;
	spk[chan].len = len;
	spk[chan].off = 0;
	spk[chan].on = true;
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

static void spk_stop(int chan) {
	spk[chan].on = false;
}

// Speaker volume per remote, 0 to 10. In 4-bit ADPCM the speaker's volume
// byte tops out at 0x40 (WiiBrew, Wiimote#Speaker; libogc's default), and a
// larger value overdrives it, so 10 is 0x40 and the steps below it fall
// off roughly as the ear hears loudness.
static const u8 volume_byte[11] = { 0x00, 0x03, 0x05, 0x08, 0x0b, 0x10, 0x16, 0x1e, 0x28, 0x33, 0x40 };
static int volume[4] = { 10, 10, 10, 10 };

// Writing the configuration block also restarts the speaker's ADPCM
// decoder, which every sound here is encoded from the start for.
static void write_volume(int chan) {
	wiimote *wm = remote(chan);
	u8 conf[7] = { 0x00, 0x00, 0xd0, 0x07, 0x00, 0x0c, 0x0e };
	u32 level;

	if (!wm || !(wm->state & WM_STATE_SPEAKER))
		return;
	conf[4] = volume_byte[volume[chan]];
	_CPU_ISR_Disable(level);
	wiiuse_write_data(wm, WM_REG_SPEAKER_BLOCK, conf, sizeof(conf), NULL);
	_CPU_ISR_Restore(level);
}

// ---- Sounds for the remote's speaker -------------------------------------
//
// What the hardware gives: a 21 mm speaker behind a Yamaha ADPCM decoder,
// 4 bits a sample at 6 kHz, 40 samples a report. 8-bit PCM is the other
// format, but at the same report rate it only reaches 3 kHz, and its tones
// alias badly; ADPCM is what games use. So the sounds are made to suit it:
//  - pure tones between 1 and 2 kHz, where the speaker is loudest and the
//    6 kHz rate still has 3 or more samples a cycle;
//  - a peak of about half full scale, so ADPCM's step never overshoots and
//    the speaker's amplifier never clips (a louder signal only distorts:
//    the volume byte is the loudness control);
//  - raised-cosine fades, so no note starts or stops with a click;
//  - a report of silence first and last, so the decoder starts from rest
//    and ends there.
// A report the Bluetooth controller has no room for is dropped silently
// (lwbt's lp_acl_write), and ADPCM can't recover its step size after a
// gap until the signal goes quiet, so the sounds are short and the remotes'
// other traffic (LEDs, rumble) is kept low while one plays.

#define SND_BLOCK 40                         // samples a report

typedef struct {
	float freq, start, len;                  // Hz, seconds, seconds
	float amp;                               // 0 to 1
} snd_note;

static u32 snd_samples(const snd_note *notes, int count) {
	float end = 0;
	int i;

	for (i = 0; i < count; ++i)
		if (notes[i].start + notes[i].len > end)
			end = notes[i].start + notes[i].len;
	// A report of silence before and after; whole reports.
	return ((u32) (end * SPEAKER_RATE) / SND_BLOCK + 3) * SND_BLOCK;
}

// Returns the ADPCM data, snd_samples() / 2 bytes; free() it.
static u8 *snd_make(const snd_note *notes, int count) {
	u32 n = snd_samples(notes, count), i, j;
	u8 *out = memalign(32, n / 2);
	WPADEncStatus enc;
	s16 pcm[SND_BLOCK];
	int k;

	if (!out)
		return NULL;
	memset(&enc, 0, sizeof(enc));
	for (i = 0; i < n; i += SND_BLOCK) {
		for (j = 0; j < SND_BLOCK; ++j) {
			float t = (float) ((int) (i + j) - SND_BLOCK) / SPEAKER_RATE, v = 0;

			for (k = 0; k < count; ++k) {
				const snd_note *m = &notes[k];
				float u = t - m->start, fade = 0.012f, env;

				if (u < 0 || u >= m->len)
					continue;
				// 12 ms raised-cosine in and out.
				env = u < fade ? 0.5f - 0.5f * cosf(3.14159265f * u / fade) :
					  m->len - u < fade ? 0.5f - 0.5f * cosf(3.14159265f * (m->len - u) / fade) : 1.f;
				v += m->amp * env * sinf(2.f * 3.14159265f * m->freq * u);
			}
			if (v > 1.f)
				v = 1.f;
			if (v < -1.f)
				v = -1.f;
			pcm[j] = (s16) (v * 16000.f);
		}
		WPAD_EncodeData(&enc, i ? WPAD_ENC_CONT : 0, pcm, SND_BLOCK, out + i / 2);
	}
	return out;
}

// Find's chime: a two-note ding-dong (E6, C6), three times, each louder.
static const snd_note chime_notes[] = {
	{ 1319, 0.00f, 0.22f, 0.45f }, { 1047, 0.25f, 0.40f, 0.45f },
	{ 1319, 0.95f, 0.22f, 0.70f }, { 1047, 1.20f, 0.40f, 0.70f },
	{ 1319, 1.90f, 0.22f, 1.00f }, { 1047, 2.15f, 0.40f, 1.00f },
};
// The volume chirp: one short A6 blip, at the new volume.
static const snd_note chirp_notes[] = { { 1760, 0.00f, 0.09f, 1.00f } };

#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

// Made the first time they are used; spk_tick reads them from its alarm,
// so they stay for the life of the app.
static u8 *chime, *chirp;
static u32 chime_len, chirp_len;

static void snd_init(void) {
	if (!chime) {
		chime = snd_make(chime_notes, COUNT(chime_notes));
		chime_len = snd_samples(chime_notes, COUNT(chime_notes)) / 2;
	}
	if (!chirp) {
		chirp = snd_make(chirp_notes, COUNT(chirp_notes));
		chirp_len = snd_samples(chirp_notes, COUNT(chirp_notes)) / 2;
	}
}

// ---- Find, and rumble feedback, run from the overlay's frame loop ------------
//
// A remote takes commands (LEDs, rumble, speaker set-up) one at a time from
// a queue, each waiting for the remote's answer. Anything sent while the
// queue is busy arrives late, which is what made the old Find blink out of
// step. So Find's light show moves on a fixed clock and sends a step only
// when the remote's queue is empty; a step that can't go is skipped, not
// sent late. Its rumble rides in the same report as its LEDs: wiiuse puts
// the remote's rumble bit into every report it sends, so setting the bit
// and then sending the LEDs changes both at once.

#define FIND_FRAMES 180                  // 3 s at 60 Hz
#define FIND_STEP 5                      // frames a step of the light chase

static struct {
	u32 find_end, pulse_end;             // frame numbers; 0 when idle
	u32 restore_end;                     // re-sending the resting state until
	u32 find_start;
	int led, rumble;                     // what was last sent; -1 unknown
	int step;                            // the chase step last shown
	bool speaker_was_on;
	int speaker;                         // 0 off, 1 starting, 2 volume sent, 3 playing, 4 on and quiet
	const u8 *sound;                     // what to play once the speaker is ready
	u32 sound_len;
} fx[4];

static void fx_rumble(int chan, int on) {
	if (fx[chan].rumble == on)
		return;
	WPAD_Rumble(chan, on);
	fx[chan].rumble = on;
}

// LEDs and rumble in one report, and only into an empty queue.
static bool fx_show(int chan, int leds, int rumble) {
	wiimote *wm = remote(chan);
	u32 level;

	if (!wm)
		return false;
	_CPU_ISR_Disable(level);
	if (*head_of(wm)) {
		_CPU_ISR_Restore(level);
		return false;
	}
	if (rumble)
		*(int *) &wm->state |= WIIMOTE_STATE_RUMBLE_FLAG;
	else
		*(int *) &wm->state &= ~WIIMOTE_STATE_RUMBLE_FLAG;
	wiiuse_set_leds(wm, leds, NULL);
	_CPU_ISR_Restore(level);
	fx[chan].led = leds;
	fx[chan].rumble = rumble;
	return true;
}

// Turns the speaker on if it needs to, then plays the sound once the
// remote has answered the set-up (fx_tick).
static void fx_sound(int chan, const u8 *data, u32 len) {
	if (!data || !remote(chan))
		return;
	spk_stop(chan);
	fx[chan].sound = data;
	fx[chan].sound_len = len;
	if (!fx[chan].speaker) {
		fx[chan].speaker_was_on = WPAD_IsSpeakerEnabled(chan) == WPAD_ERR_NONE;
		if (!fx[chan].speaker_was_on)
			WPAD_ControlSpeaker(chan, 1);
	}
	fx[chan].speaker = 1;
}

static void find_start(int chan) {
	if (fx[chan].find_end)
		return;
	snd_init();
	fx[chan].find_start = fx_frame;
	fx[chan].find_end = fx_frame + FIND_FRAMES;
	fx[chan].led = fx[chan].rumble = fx[chan].step = -1;
	fx[chan].restore_end = 0;
	ses.finding[chan] = true;
	fx_sound(chan, chime, chime_len);
}

static void find_stop(int chan) {
	fx[chan].find_end = 0;
	ses.finding[chan] = false;
	// The player's LED again, rumble off: sent by fx_tick into an empty
	// queue, and a few more times in case one is lost.
	fx[chan].led = fx[chan].rumble = -1;
	fx[chan].restore_end = fx_frame + 30;
}

// Once on, the speaker stays on until the overlay closes, so the next
// sound needs no set-up (a volume chirp comes at once).
static void sound_done(int chan) {
	if (fx[chan].speaker == 3 && !spk[chan].on)
		fx[chan].speaker = 4;
}

// A short buzz when rumble is switched on.
static void pulse(int chan) {
	u32 type;

	if (WPAD_Probe(chan, &type) != WPAD_ERR_NONE || fx[chan].find_end)
		return;
	fx[chan].rumble = -1;
	fx_rumble(chan, 1);
	fx[chan].pulse_end = fx_frame + 8;
}

// Find's light show: one LED running 1-2-3-4-3-2-..., the remote buzzing
// as it turns at each end.
static const u8 chase[6] = { 0x10, 0x20, 0x40, 0x80, 0x40, 0x20 };

static void fx_tick(void) {
	int chan;

	fx_frame++;
	for (chan = 0; chan < 4; ++chan) {
		u32 type;

		queue_guard(chan);

		if (fx[chan].pulse_end && fx_frame >= fx[chan].pulse_end) {
			fx_rumble(chan, 0);
			fx[chan].pulse_end = 0;
		}
		if (fx[chan].restore_end) {
			if (fx_frame >= fx[chan].restore_end)
				fx[chan].restore_end = 0;
			else if (fx[chan].led < 0 || fx_frame % 10 == 0)
				fx_show(chan, WIIMOTE_LED_1 << chan, fx[chan].pulse_end != 0);
		}
		if (fx[chan].find_end) {
			if (WPAD_Probe(chan, &type) != WPAD_ERR_NONE || fx_frame >= fx[chan].find_end) {
				find_stop(chan);
			} else {
				int step = (fx_frame - fx[chan].find_start) / FIND_STEP;

				if (step != fx[chan].step) {
					int i = step % 6;

					// Skipped when the queue is busy; the next step
					// comes on time.
					fx_show(chan, chase[i], i == 0 || i == 3);
					fx[chan].step = step;
				}
			}
		}
		// The speaker's set-up is a run of queued commands: wait for the
		// remote to answer them all, set the volume, then stream.
		if (fx[chan].speaker == 1 || fx[chan].speaker == 2) {
			wiimote *wm = remote(chan);

			if (!wm || WPAD_Probe(chan, &type) != WPAD_ERR_NONE) {
				fx[chan].speaker = 0;
			} else if (!*head_of(wm) && (wm->state & WM_STATE_SPEAKER)) {
				if (fx[chan].speaker == 1) {
					write_volume(chan);
					fx[chan].speaker = 2;
				} else {
					spk_play(chan, fx[chan].sound, fx[chan].sound_len);
					fx[chan].speaker = 3;
				}
			}
		}
		sound_done(chan);
	}
}

static void fx_stop_all(void) {
	int chan;

	for (chan = 0; chan < 4; ++chan) {
		spk_stop(chan);
		if (fx[chan].speaker) {
			if (!fx[chan].speaker_was_on)
				WPAD_ControlSpeaker(chan, 0);
			fx[chan].speaker = 0;
		}
		if (fx[chan].find_end || fx[chan].restore_end) {
			fx[chan].find_end = fx[chan].restore_end = 0;
			ses.finding[chan] = false;
			reset_leds(chan);
		}
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
		// Let them hear it: a blip at the new level.
		snd_init();
		if (volume[chan])
			fx_sound(chan, chirp, chirp_len);
		else
			write_volume(chan);
		break;
	}
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
