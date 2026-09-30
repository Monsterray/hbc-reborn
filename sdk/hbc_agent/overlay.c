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
#include <wiiuse/wpad.h>

#include "../hbc_agent.h"
#include "agent_int.h"
#include "ov_ui.h"

// wiiuse state flags (wiiuse_internal.h, not installed with libogc).
#define WM_STATE_CONNECTED 0x000010
#define WM_STATE_RUMBLE 0x000080
#define WM_STATE_ACC 0x000100
#define WM_STATE_IR 0x000400

#define TOAST_FRAMES 150
#define CAL_FRAMES 60
#define FIND_MS 3000
#define SPEAKER_RATE 6000   // libogc's speaker set-up: 4-bit ADPCM at 6 kHz

extern void __exception_setreload(int t);

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

// ---- Find: blink the LEDs, pulse the rumble, and chime, getting louder ----

static lwp_t finder = LWP_THREAD_NULL;
static u8 finder_stack[8192] ATTRIBUTE_ALIGN(32);
static int find_chan;

static void *find_thread(void *arg) {
	int chan = find_chan, n = 0;
	u64 start = gettime();
	WPADEncStatus enc;
	bool speaker = WPAD_ControlSpeaker(chan, 1) == WPAD_ERR_NONE;
	s16 pcm[40];
	u8 data[20];
	u32 level;
	(void) arg;

	memset(&enc, 0, sizeof(enc));
	ses.finding[chan] = true;
	while (true) {
		u32 ms = ticks_to_millisecs(diff_ticks(start, gettime()));
		wiimote *wm = remote(chan);
		int i;

		if (ms >= FIND_MS || !wm)
			break;
		_CPU_ISR_Disable(level);
		if ((ms / 200) != (u32) n) {
			n = ms / 200;
			wiiuse_set_leds(wm, n & 1 ? 0xf0 : WIIMOTE_LED_1 << chan, NULL);
			wiiuse_rumble(wm, (ms % 600) < 150);
		}
		_CPU_ISR_Restore(level);

		if (!speaker) {
			usleep(20 * 1000);
			continue;
		}
		// Two alternating tones, their volume ramping up over the 3 s.
		for (i = 0; i < 40; ++i) {
			u32 s = (ms * SPEAKER_RATE) / 1000 + i;
			float f = (s / (SPEAKER_RATE / 4)) & 1 ? 1320.f : 880.f;
			float amp = 2000.f + 18000.f * ms / FIND_MS;

			pcm[i] = (s16) (amp * sinf(2.f * (float) M_PI * f * s / SPEAKER_RATE));
		}
		WPAD_EncodeData(&enc, ms ? WPAD_ENC_CONT : 0, pcm, 40, data);
		WPAD_SendStreamData(chan, data, sizeof(data));
		usleep(1000000 * 40 / SPEAKER_RATE);
	}
	if (speaker)
		WPAD_ControlSpeaker(chan, 0);
	{
		wiimote *wm = remote(chan);

		_CPU_ISR_Disable(level);
		if (wm) {
			wiiuse_rumble(wm, 0);
			wiiuse_set_leds(wm, WIIMOTE_LED_1 << chan, NULL);
		}
		_CPU_ISR_Restore(level);
	}
	if (!remote(chan))
		WPAD_Rumble(chan, 0);
	ses.finding[chan] = false;
	finder = LWP_THREAD_NULL;
	return NULL;
}

static void find_start(int chan) {
	if (finder != LWP_THREAD_NULL)
		return;
	find_chan = chan;
	if (!remote(chan)) {
		// Without handles: rumble only, from this thread's frame loop.
		ses.finding[chan] = true;
		return;
	}
	LWP_CreateThread(&finder, find_thread, NULL, finder_stack, sizeof(finder_stack), 70);
}

// ---- Calibration --------------------------------------------------------

static hbc_agent_cal cal[4];
static struct {
	int chan, frames;
	s64 sum[8];
} cal_run = { -1 };

static void cal_sample(void) {
	WPADData *d;
	int c = cal_run.chan;

	if (c < 0 || cal_run.frames >= CAL_FRAMES)
		return;
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
		rm->finding = ses.finding[chan];
	}

	e->log_pc = agent_log_to_pc;
	e->crash_stay = agent_crash_stay;
	e->hbcpy = agent_listen_enabled;
	e->has_save = cfg->on_save != NULL;
	e->has_restart = cfg->on_restart != NULL;
	remote(0);
	e->can_leds = handles != NULL;
	e->sensor_above = ses.sensor_above < 0 ? !CONF_GetSensorBarPosition() : ses.sensor_above;
	e->ir_sens = ses.ir_sens ? ses.ir_sens : CONF_GetIRSensitivity();
	snprintf(e->auto_off, sizeof(e->auto_off), "%s", auto_off_names[ses.auto_off]);
	e->rumble_all = !ses.rumble_all_off;
	snprintf(e->slot[0], sizeof(e->slot[0]), "%s", agent_slot_label(0));
	snprintf(e->slot[1], sizeof(e->slot[1]), "%s", agent_slot_label(1));
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

static void act(int action, int arg, void *user) {
	const hbc_agent_config *cfg = agent_cfg();
	char path[64];
	(void) user;

	switch (action) {
	case OVA_HBC:
	case OVA_SYSMENU:
	case OVA_RESTART_WII:
	case OVA_POWEROFF:
		if (cfg->on_exit)
			cfg->on_exit(cfg->user);
		VIDEO_SetBlack(true);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		if (action == OVA_HBC)
			exit(0);
		SYS_ResetSystem(action == OVA_SYSMENU ? SYS_RETURNTOMENU :
						action == OVA_RESTART_WII ? SYS_RESTART : SYS_POWEROFF, 0, 0);
		break;
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
		break;
	case OVA_DISCONNECT:
		WPAD_Disconnect(arg);
		break;
	case OVA_TEST_START: {
		wiimote *wm = remote(arg);

		run->test_chan = arg;
		run->test_fmt = !wm ? WPAD_FMT_BTNS :
				wm->state & WM_STATE_IR ? WPAD_FMT_BTNS_ACC_IR :
				wm->state & WM_STATE_ACC ? WPAD_FMT_BTNS_ACC : WPAD_FMT_BTNS;
		WPAD_SetDataFormat(arg, WPAD_FMT_BTNS_ACC_IR);
		WPAD_SetVRes(arg, run->w, run->h);
		break;
	}
	case OVA_TEST_STOP:
		WPAD_SetDataFormat(arg, run->test_fmt);
		run->test_chan = -1;
		break;
	case OVA_CAL_START:
		memset(&cal_run, 0, sizeof(cal_run));
		cal_run.chan = arg;
		cal[arg].valid = false;
		WPAD_SetDataFormat(arg, WPAD_FMT_BTNS_ACC_IR);
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
	case OVA_RUMBLE_ALL:
		ses.rumble_all_off = !arg;
		keeper_start();
		break;
	}
}

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
		if (b)
			out |= OV_ANY;
	}
	return out;
}

s32 hbc_agent_home(const GXRModeObj *rmode) {
	static bool inside;
	static ov_ui ui;
	void *app_fb = VIDEO_GetCurrentFramebuffer();
	u8 *fb[2] = { NULL, NULL };
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

	// The frozen frame, and two framebuffers to draw into without tearing.
	r.frozen = memalign(32, size);
	fb[0] = memalign(32, size);
	fb[1] = memalign(32, size);
	if (!r.frozen || !fb[0]) {
		free(r.frozen);
		free(fb[0]);
		free(fb[1]);
		return -ENOMEM;
	}
	inside = true;
	run = &r;
	app_fb = agent_uncached(app_fb);
	memcpy(r.frozen, app_fb, size);
	LWP_CreateThread(&sd, sd_thread, &r, NULL, 16 * 1024, 30);

	ov_init(&ui, r.w, r.h);
	while (running) {
		ov_canvas c;
		unsigned pressed;
		u8 *back = fb[fb[1] ? cur : 0];

		poll(&r);
		pressed = read_input();
		cal_sample();
		// Find without handles: pulse the rumble from here.
		for (i = 0; i < 4; ++i)
			if (ses.finding[i] && finder == LWP_THREAD_NULL) {
				static u32 f;

				WPAD_Rumble(i, (++f % 36) < 9);
				if (f > 180) {
					WPAD_Rumble(i, 0);
					ses.finding[i] = false;
					f = 0;
				}
			}
		running = ov_step(&ui, &r.ext, pressed, act, NULL);

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
		VIDEO_SetNextFramebuffer(back);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		cur ^= 1;
	}

	VIDEO_SetNextFramebuffer(app_fb);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (r.test_chan >= 0)
		WPAD_SetDataFormat(r.test_chan, r.test_fmt);
	if (sd != LWP_THREAD_NULL)
		LWP_JoinThread(sd, NULL);
	free(fb[1]);
	free(fb[0]);
	free(r.frozen);
	run = NULL;
	inside = false;

	if (ui.after == OVA_RESTART_APP && agent_cfg()->on_restart)
		agent_cfg()->on_restart(agent_cfg()->user);
	else if (ui.after == OVA_SLOT)
		agent_slot_press(ui.after_arg);
	return 0;
}
