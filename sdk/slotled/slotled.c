/* slotled: the Wii's blue disc-slot light (slotled.h).
 *
 * The pin. The slot light is Hollywood GPIO 5, active high, and IOS gives
 * the PowerPC that pin (HW_GPIO_OWNER), so the PowerPC's GPIO bank,
 * HW_GPIOB_OUT, switches it. That bank answers at 0x0d0000c0 from the
 * PowerPC, and at 0x0d8000c0 too when AHBPROT is open (Priiloader and
 * wiipax's stub use that one). slotled_init() takes the first whose
 * direction register has the pin set as an output.
 *
 * Brightness. One periodic alarm fires at the start of each 4 ms period:
 * it works out the level the effect shows now, turns the light on and arms
 * a second, one-shot alarm for the moment it goes off again. So a level
 * costs two short interrupts a period, and a steady 0 or 255 costs nothing:
 * the alarms stop until the next change. The on-time is level squared, so
 * levels look evenly spaced to the eye.
 *
 * Time is counted in periods, not read from the clock, so an effect's
 * steps line up exactly with what slotled_effect_level() says on a PC.
 *
 * IOS. /dev/stm/immediate ioctl 0x6002 takes the illumination level
 * (0 off, 1 dim, 2 bright); 0x6001 starts one of 30 flash patterns, with
 * the pattern in bits 0-7 of the first word and the priority in bits 8-15
 * (WiiBrew, /dev/stm/immediate). Bit 16 loads a pattern of the caller's
 * own; its layout is not known, so slotled_ios_flash_raw() sends any
 * buffer for finding out.
 *
 * SYSCONF. Slot illumination is the last byte of IPL.IDL, a small array
 * whose first byte is the standby mode: 2 bytes in libogc's CONF_GetIdleLedMode
 * and Dolphin's default SYSCONF ({0, 1}), 3 by WiiBrew's table. The file is 0x4000 bytes: "SCv0", a
 * big-endian count and offsets, then entries of one byte (type << 5 |
 * name length - 1), the name and the value. slotled_sysconf_illum() finds
 * it the way libogc's CONF_Get does and changes that one byte, so nothing
 * else in the file moves.
 */

#include <string.h>

#include "slotled.h"

#ifndef SLOTLED_NO_OGC
#include <stdio.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/conf.h>
#include <ogc/machine/processor.h>
#include <ogc/ipc.h>
#include <ogc/stm.h>
#endif

#define PIN 0x20u
#define MIN_ON_US 20u               /* shorter is not seen at all */
#define FULL_GAP_US 100u            /* nearer the period's end is full on */

/* ---- Pure parts ------------------------------------------------------- */

static int bad_level(int level)
{
	return level < 0 || level > SLOTLED_MAX;
}

uint32_t slotled_duty_us(int level)
{
	uint32_t us;

	if (level <= 0)
		return 0;
	if (level >= SLOTLED_MAX)
		return SLOTLED_PERIOD_US;
	us = ((uint32_t)level * (uint32_t)level * SLOTLED_PERIOD_US + 65024u) / 65025u;
	if (us < MIN_ON_US)
		us = MIN_ON_US;
	if (us > SLOTLED_PERIOD_US - FULL_GAP_US)
		us = SLOTLED_PERIOD_US;
	return us;
}

int slotled_effect_level(const slotled_effect *e, uint64_t ms, int start, int *done)
{
	uint64_t total = 0, cycle, t;
	uint32_t i;
	int prev;

	if (done)
		*done = 0;
	if (!e || !e->steps || !e->count) {
		if (done)
			*done = 1;
		return start;
	}
	for (i = 0; i < e->count; i++)
		total += e->steps[i].ms;
	if (!total) {
		if (done)
			*done = 1;
		return e->steps[e->count - 1].level;
	}
	cycle = ms / total;
	if (e->repeat && cycle >= e->repeat) {
		if (done)
			*done = 1;
		return e->steps[e->count - 1].level;
	}
	t = ms % total;
	prev = cycle ? e->steps[e->count - 1].level : start;
	for (i = 0; i < e->count; i++) {
		const slotled_step *s = &e->steps[i];

		if (t < s->ms) {
			if (!s->fade)
				return s->level;
			return prev + ((int)s->level - prev) * (int)t / (int)s->ms;
		}
		t -= s->ms;
		prev = s->level;
	}
	return prev;
}

/* Appends a step, joining it to the last one when both hold the same
 * level. */
static int add(slotled_step *steps, size_t max, size_t *n, int level, int fade, uint32_t ms)
{
	if (!ms)
		return 0;
	if (*n && !fade && !steps[*n - 1].fade && steps[*n - 1].level == level &&
	    steps[*n - 1].ms + ms <= 0xFFFF) {
		steps[*n - 1].ms += ms;
		return 0;
	}
	if (ms > 0xFFFF)
		return SLOTLED_E_ARG;
	if (*n >= max)
		return SLOTLED_E_TOO_LONG;
	steps[*n].level = (uint8_t)level;
	steps[*n].fade = (uint8_t)(fade != 0);
	steps[*n].ms = (uint16_t)ms;
	(*n)++;
	return 0;
}

#define ADD(level, fade, ms) do { int r_ = add(steps, max, &n, level, fade, ms); \
	if (r_) return r_; } while (0)

int slotled_build_blink(slotled_step *steps, size_t max, int level, uint32_t on_ms, uint32_t off_ms)
{
	size_t n = 0;

	if (bad_level(level) || !on_ms)
		return SLOTLED_E_ARG;
	ADD(level, 0, on_ms);
	ADD(0, 0, off_ms);
	return (int)n;
}

int slotled_build_pulse(slotled_step *steps, size_t max, int lo, int hi, uint32_t period_ms)
{
	size_t n = 0;

	if (bad_level(lo) || bad_level(hi) || period_ms < 2)
		return SLOTLED_E_ARG;
	ADD(hi, 1, period_ms / 2);
	ADD(lo, 1, period_ms - period_ms / 2);
	return (int)n;
}

int slotled_build_heartbeat(slotled_step *steps, size_t max, int level, uint32_t period_ms)
{
	size_t n = 0;

	if (bad_level(level) || period_ms < 600)
		return SLOTLED_E_ARG;
	ADD(level, 0, 80);
	ADD(0, 1, 140);
	ADD(level, 0, 80);
	ADD(0, 1, 300);
	ADD(0, 0, period_ms - 600);
	return (int)n;
}

/* Dots and dashes for ' ' .. 'Z', 0 where there is no code. */
static const char *const morse[] = {
	/*   */ 0, /* ! */ "-.-.--", /* " */ ".-..-.", /* # */ 0, /* $ */ "...-..-",
	/* % */ 0, /* & */ ".-...", /* ' */ ".----.", /* ( */ "-.--.", /* ) */ "-.--.-",
	/* * */ 0, /* + */ ".-.-.", /* , */ "--..--", /* - */ "-....-", /* . */ ".-.-.-",
	/* / */ "-..-.",
	"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.",
	/* : */ "---...", /* ; */ "-.-.-.", /* < */ 0, /* = */ "-...-", /* > */ 0,
	/* ? */ "..--..", /* @ */ ".--.-.",
	".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..",
	"--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-",
	"-.--", "--.."
};

static const char *morse_code(char c)
{
	if (c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');
	if (c == '_')
		return "..--.-";
	if (c < ' ' || c > 'Z')
		return 0;
	return morse[c - ' '];
}

int slotled_build_morse(slotled_step *steps, size_t max, const char *text, int level,
                        uint32_t unit_ms, int *chars)
{
	size_t n = 0;
	int sent = 0;
	int gap = 0;            /* units of dark owed before the next mark */

	if (chars)
		*chars = 0;
	if (!text || bad_level(level) || !unit_ms || unit_ms * 7 > 0xFFFF)
		return SLOTLED_E_ARG;
	for (; *text; text++) {
		const char *code = morse_code(*text);

		if (!code) {
			if (n)
				gap = 7;
			sent++;
			continue;
		}
		if (gap)
			ADD(0, 0, gap * unit_ms);
		for (; *code; code++) {
			ADD(level, 0, (*code == '-' ? 3 : 1) * unit_ms);
			if (code[1])
				ADD(0, 0, unit_ms);
		}
		gap = 3;
		sent++;
		if (chars)
			*chars = sent;
	}
	/* A word's gap after the end, so a repeat reads as a new word. */
	if (n)
		ADD(0, 0, 7 * unit_ms);
	if (chars)
		*chars = sent;
	return (int)n;
}

static uint32_t be16(const uint8_t *p)
{
	return (uint32_t)p[0] << 8 | p[1];
}

int slotled_sysconf_illum(uint8_t *sc, size_t size, int set)
{
	static const char name[] = "IPL.IDL";
	const size_t nlen = sizeof name - 1;
	uint32_t count, i;

	if (!sc || size < 8 || memcmp(sc, "SCv0", 4))
		return SLOTLED_E_FORMAT;
	count = be16(sc + 4);
	if (6 + 2 * (size_t)count > size)
		return SLOTLED_E_FORMAT;
	for (i = 0; i < count; i++) {
		size_t off = be16(sc + 6 + 2 * i), val;
		int old;

		if (off >= size || (size_t)(sc[off] & 0x1F) + 1 != nlen ||
		    off + 1 + nlen + 2 > size || memcmp(sc + off + 1, name, nlen))
			continue;
		/* A small array (type 2) of 2 or 3 bytes: a length byte of 1 or 2. */
		if (sc[off] >> 5 != 2 || sc[off + 1 + nlen] < 1 || sc[off + 1 + nlen] > 2)
			return SLOTLED_E_FORMAT;
		val = off + 1 + nlen + 1 + sc[off + 1 + nlen];
		if (val >= size)
			return SLOTLED_E_FORMAT;
		old = sc[val];
		if (old > SLOTLED_ILLUM_BRIGHT)
			return SLOTLED_E_FORMAT;
		if (set >= SLOTLED_ILLUM_OFF && set <= SLOTLED_ILLUM_BRIGHT)
			sc[val] = (uint8_t)set;
		return old;
	}
	return SLOTLED_E_FORMAT;
}

#ifndef SLOTLED_NO_OGC

/* ---- Directly --------------------------------------------------------- */

static volatile u32 *gpio;          /* HW_GPIOB_OUT; [1] is HW_GPIOB_DIR */
static int was_on;
static syswd_t period_alarm, off_alarm;
static int alarms_made, ticking;

static slotled_step own[SLOTLED_MAX_STEPS];
static slotled_effect cur;          /* what plays */
static u32 periods;                 /* periods since it started */
static int start_level, shown, ended;
static int flash_level;
static u32 flash_left;              /* periods of flash still to show */

static void pin(int on)
{
	if (on)
		gpio[0] |= PIN;
	else
		gpio[0] &= ~PIN;
}

static void off_edge(syswd_t alarm, void *arg)
{
	(void)alarm;
	(void)arg;
	pin(0);
}

static void stop_ticking(void)
{
	if (ticking) {
		SYS_CancelAlarm(period_alarm);
		SYS_CancelAlarm(off_alarm);
		ticking = 0;
	}
}

/* At the start of each period, with interrupts off. */
static void period_start(syswd_t alarm, void *arg)
{
	int level, done = 0, flashing = 0;
	u32 us;

	(void)alarm;
	(void)arg;
	level = slotled_effect_level(&cur, (u64)periods * (SLOTLED_PERIOD_US / 1000), start_level, &done);
	periods++;
	if (done && !ended) {
		ended = 1;
		cur.count = 0;          /* held from here on */
		start_level = level;
	}
	if (flash_left) {
		flash_left--;
		level = flash_level;
		flashing = 1;
	}
	shown = level;
	us = slotled_duty_us(level);
	pin(us != 0);
	if (us && us < SLOTLED_PERIOD_US) {
		struct timespec t = { 0, (long)us * 1000 };
		SYS_SetAlarm(off_alarm, &t, off_edge, NULL);
	}
	/* Stop only on the held level itself: a flash's last period may be
	 * 0 or full while the level beneath it is not. */
	if (ended && !flashing && (us == 0 || us == SLOTLED_PERIOD_US)) {
		SYS_CancelAlarm(period_alarm);
		ticking = 0;
	}
}

static void start_ticking(void)
{
	struct timespec p = { 0, SLOTLED_PERIOD_US * 1000 };
	struct timespec now = { 0, 1000 };

	if (ticking)
		return;
	ticking = 1;
	SYS_SetPeriodicAlarm(period_alarm, &now, &p, period_start, NULL);
}

int slotled_init(void)
{
	static volatile u32 *const bases[] = { (volatile u32 *)0xCD0000C0, (volatile u32 *)0xCD8000C0 };
	unsigned i;

	if (gpio)
		return SLOTLED_OK;
	for (i = 0; i < sizeof bases / sizeof bases[0]; i++)
		if (bases[i][1] & PIN) {
			gpio = bases[i];
			break;
		}
	if (!gpio)
		return SLOTLED_E_NO_PIN;
	if (!alarms_made) {
		if (SYS_CreateAlarm(&period_alarm) < 0)
			return gpio = NULL, SLOTLED_E_NO_PIN;
		if (SYS_CreateAlarm(&off_alarm) < 0) {
			SYS_RemoveAlarm(period_alarm);
			return gpio = NULL, SLOTLED_E_NO_PIN;
		}
		alarms_made = 1;
	}
	was_on = (gpio[0] & PIN) != 0;
	shown = was_on ? SLOTLED_MAX : 0;
	start_level = shown;
	cur.count = 0;
	ended = 1;
	flash_left = 0;
	return SLOTLED_OK;
}

void slotled_shutdown(void)
{
	u32 isr;

	if (!gpio)
		return;
	_CPU_ISR_Disable(isr);
	stop_ticking();
	pin(was_on);
	_CPU_ISR_Restore(isr);
	if (alarms_made) {
		SYS_RemoveAlarm(period_alarm);
		SYS_RemoveAlarm(off_alarm);
		alarms_made = 0;
	}
	gpio = NULL;
}

/* Starts `n` steps already in own[] (or the caller's), from the level
 * shown now. */
static int run(const slotled_step *steps, u32 n, u32 repeat)
{
	u32 isr;

	if (!gpio)
		return SLOTLED_E_NOT_INIT;
	_CPU_ISR_Disable(isr);
	cur.steps = steps;
	cur.count = n;
	cur.repeat = repeat;
	periods = 0;
	start_level = shown;
	ended = 0;
	start_ticking();
	_CPU_ISR_Restore(isr);
	return SLOTLED_OK;
}

int slotled_set(int level)
{
	if (bad_level(level))
		return SLOTLED_E_ARG;
	if (!gpio)
		return SLOTLED_E_NOT_INIT;
	{
		u32 isr;

		_CPU_ISR_Disable(isr);
		stop_ticking();
		own[0].level = (u8)level;
		own[0].fade = 0;
		own[0].ms = SLOTLED_PERIOD_US / 1000;
		_CPU_ISR_Restore(isr);
	}
	return run(own, 1, 1);
}

int slotled_get(void)
{
	return shown;
}

/* Builds into own[] with interrupts held off the old steps first. */
#define BUILD(call, repeat) do { int n_; u32 isr_; \
	if (!gpio) return SLOTLED_E_NOT_INIT; \
	_CPU_ISR_Disable(isr_); stop_ticking(); cur.count = 0; _CPU_ISR_Restore(isr_); \
	n_ = call; if (n_ < 0) return n_; \
	return run(own, (u32)n_, repeat); } while (0)

int slotled_blink(int level, uint32_t on_ms, uint32_t off_ms, uint32_t count)
{
	BUILD(slotled_build_blink(own, SLOTLED_MAX_STEPS, level, on_ms, off_ms), count);
}

int slotled_pulse(int lo, int hi, uint32_t period_ms, uint32_t count)
{
	BUILD(slotled_build_pulse(own, SLOTLED_MAX_STEPS, lo, hi, period_ms), count);
}

int slotled_heartbeat(int level, uint32_t period_ms, uint32_t count)
{
	BUILD(slotled_build_heartbeat(own, SLOTLED_MAX_STEPS, level, period_ms), count);
}

int slotled_morse(const char *text, int level, uint32_t unit_ms, uint32_t count)
{
	int chars, n, r;
	u32 isr;

	if (!gpio)
		return SLOTLED_E_NOT_INIT;
	_CPU_ISR_Disable(isr);
	stop_ticking();
	cur.count = 0;
	_CPU_ISR_Restore(isr);
	n = slotled_build_morse(own, SLOTLED_MAX_STEPS, text, level, unit_ms, &chars);
	if (n < 0)
		return n;
	r = run(own, (u32)n, count);
	return r < 0 ? r : chars;
}

int slotled_play(const slotled_effect *e)
{
	u32 i;

	if (!e || (e->count && !e->steps))
		return SLOTLED_E_ARG;
	for (i = 0; i < e->count; i++)
		if (e->steps[i].fade > 1)
			return SLOTLED_E_ARG;
	return run(e->steps, e->count, e->repeat);
}

int slotled_flash(int level, uint32_t ms)
{
	u32 isr, n;

	if (bad_level(level))
		return SLOTLED_E_ARG;
	if (!gpio)
		return SLOTLED_E_NOT_INIT;
	n = (ms * 1000 + SLOTLED_PERIOD_US - 1) / SLOTLED_PERIOD_US;
	_CPU_ISR_Disable(isr);
	flash_level = level;
	if (n > flash_left)
		flash_left = n;
	start_ticking();
	_CPU_ISR_Restore(isr);
	return SLOTLED_OK;
}

int slotled_busy(void)
{
	return gpio && (!ended || flash_left);
}

void slotled_wait(void)
{
	while (slotled_busy())
		usleep(SLOTLED_PERIOD_US);
}

/* ---- Through IOS ------------------------------------------------------- */

static int stm(u32 ioctl, const u32 in[8], u32 out[8])
{
	static u32 bin[8] ATTRIBUTE_ALIGN(32), bout[8] ATTRIBUTE_ALIGN(32);
	static const char dev[] ATTRIBUTE_ALIGN(32) = "/dev/stm/immediate";
	s32 fd, r;

	fd = IOS_Open(dev, 0);
	if (fd < 0)
		return fd;
	memcpy(bin, in, sizeof bin);
	memset(bout, 0, sizeof bout);
	r = IOS_Ioctl(fd, ioctl, bin, sizeof bin, bout, sizeof bout);
	IOS_Close(fd);
	if (out)
		memcpy(out, bout, sizeof bout);
	return r;
}

int slotled_ios_level(int illum)
{
	u32 in[8] = { 0 };

	if (illum < SLOTLED_ILLUM_OFF || illum > SLOTLED_ILLUM_BRIGHT)
		return SLOTLED_E_ARG;
	in[0] = (u32)illum;
	if (stm(0x6002, in, NULL) >= 0)
		return SLOTLED_OK;
	/* If IOS keeps to one handle, libogc's (when it opened one). */
	return STM_SetLedMode((u32)illum) < 0 ? SLOTLED_E_IOS : SLOTLED_OK;
}

int slotled_ios_flash(int pattern, int priority)
{
	u32 in[8] = { 0 };

	if (pattern < 0 || pattern >= SLOTLED_IOS_PATTERNS || priority < 0 || priority > 255)
		return SLOTLED_E_ARG;
	in[0] = (u32)pattern | (u32)priority << 8;
	return stm(0x6001, in, NULL) < 0 ? SLOTLED_E_IOS : SLOTLED_OK;
}

int slotled_ios_flash_raw(const uint32_t in[8], uint32_t out[8])
{
	if (!in)
		return SLOTLED_E_ARG;
	return stm(0x6001, in, out);
}

/* ---- The saved setting ------------------------------------------------- */

int slotled_saved_level(void)
{
	u8 idl[3];
	s32 r = CONF_Init();

	if (r < 0)
		return SLOTLED_E_IOS;
	r = CONF_Get("IPL.IDL", idl, sizeof idl);
	if (r < 2)
		return SLOTLED_E_FORMAT;
	return idl[r - 1] > SLOTLED_ILLUM_BRIGHT ? SLOTLED_E_FORMAT : idl[r - 1];
}

/* Read the file back after closing it, in blocks, and compare. */
static int sysconf_matches(const char *path, const u8 *want, u32 size)
{
	static u8 blk[0x200] ATTRIBUTE_ALIGN(32);
	s32 fd = IOS_Open(path, 1);
	u32 at;

	if (fd < 0)
		return 0;
	for (at = 0; at < size; at += sizeof blk)
		if (IOS_Read(fd, blk, sizeof blk) != (s32)sizeof blk || memcmp(blk, want + at, sizeof blk))
			break;
	IOS_Close(fd);
	return at >= size;
}

int slotled_save_level(int illum)
{
	static u8 sc[0x4000] ATTRIBUTE_ALIGN(32);
	static const char path[] ATTRIBUTE_ALIGN(32) = "/shared2/sys/SYSCONF";
	s32 fd, r;
	int old;

	if (illum < SLOTLED_ILLUM_OFF || illum > SLOTLED_ILLUM_BRIGHT)
		return SLOTLED_E_ARG;
	fd = IOS_Open(path, 3);
	if (fd < 0)
		return SLOTLED_E_IOS;
	r = IOS_Read(fd, sc, sizeof sc);
	if (r != (s32)sizeof sc) {
		IOS_Close(fd);
		return SLOTLED_E_IOS;
	}
	old = slotled_sysconf_illum(sc, sizeof sc, -1);
	if (old < 0 || memcmp(sc + sizeof sc - 4, "SCed", 4)) {
		IOS_Close(fd);
		return SLOTLED_E_FORMAT;
	}
	if (old != illum) {
		slotled_sysconf_illum(sc, sizeof sc, illum);
		if (IOS_Seek(fd, 0, SEEK_SET) < 0 || IOS_Write(fd, sc, sizeof sc) != (s32)sizeof sc) {
			IOS_Close(fd);
			return SLOTLED_E_IOS;
		}
	}
	IOS_Close(fd);
	if (old != illum && !sysconf_matches(path, sc, sizeof sc))
		return SLOTLED_E_IOS;
	return slotled_ios_level(illum);
}

#endif /* SLOTLED_NO_OGC */

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
