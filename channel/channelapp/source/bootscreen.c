#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ogcsys.h>

#include "../config.h"
#include "../build_tag.h"
#include "bootlog.h"
#include "bootscreen.h"
#include "devnet.h"
#include "gfx.h"
#include "usbguard.h"
#include "usbstart.h"

// The list shows when the menu is not up this long after the start.
#define SCREEN_AFTER_MS 5000
#define COLS 76
#define MAX_ROWS 64

extern unsigned char console_font_8x16[];

static volatile bool stopped;
static mutex_t draw_lock = LWP_MUTEX_NULL;
static lwp_t screen_thread = LWP_THREAD_NULL;
static u8 screen_stack[8 * 1024] ATTRIBUTE_ALIGN(32);

// YUY2: Y0 Cb Y1 Cr, white on black, or on dark red for a call still in.
static u32 pixel_pair(int on0, int on1, u32 bg) {
	u32 y0 = on0 ? 235 : bg >> 24, y1 = on1 ? 235 : (bg >> 8) & 0xff;
	u32 cb = on0 || on1 ? 128 : (bg >> 16) & 0xff, cr = on0 || on1 ? 128 : bg & 0xff;

	return (y0 << 24) | (cb << 16) | (y1 << 8) | cr;
}

static void draw_row(u32 *xfb, u32 stride, u32 y0, const char *s, u32 bg) {
	u32 c, y, x;

	for (c = 0; c < COLS; c++) {
		unsigned char ch = *s ? (unsigned char) *s++ : ' ';
		const unsigned char *p = &console_font_8x16[16 * ch];

		for (y = 0; y < 16; y++) {
			unsigned char v = *p++;

			for (x = 0; x < 4; x++, v <<= 2)
				xfb[(y0 + y) * stride + 8 + c * 4 + x] = pixel_pair(v & 0x80, v & 0x40, bg);
		}
	}
}

static void call_row(char *row, size_t size, const bootlog_row *c, u32 now) {
	if (!c->done)
		snprintf(row, size, "%7u %-6.6s %-38.38s NO ANSWER %u ms", (unsigned) c->in_ms, c->thread,
				 c->call, (unsigned) (now - c->in_ms));
	else
		snprintf(row, size, "%7u %-6.6s %-38.38s %6u ms rc %d", (unsigned) c->in_ms, c->thread,
				 c->call, (unsigned) (c->out_ms - c->in_ms), (int) c->rc);
}

static void draw_list(bool first) {
	GXRModeObj *rm = gfx_video_mode();
	u32 *xfb = gfx_xfb();
	u32 now = devnet_boot_ms(), stride, top, rows, r = 0, mark_ms = 0;
	// The calls in the order they began, copied so the log can go on changing.
	static bootlog_row list[MAX_ROWS];
	bootlog_row c;
	u32 after = 0;
	int n = 0, i, open = 0, start;
	const char *mark;
	char row[COLS + 1];

	if (!rm || !xfb)
		return;
	stride = rm->fbWidth / 2;
	top = rm->xfbHeight > 500 ? 48 : 24;
	rows = (rm->xfbHeight - 2 * top) / 16;
	if (rows > MAX_ROWS)
		rows = MAX_ROWS;
	if (first) {
		VIDEO_ClearFrameBuffer(rm, xfb, COLOR_BLACK);
		VIDEO_SetBlack(FALSE);
		VIDEO_Flush();
	}

	// A full list gives up its oldest finished call for each new one: the TV
	// has room for fewer rows than that.
	while (bootlog_next(after, &c)) {
		after = c.seq;
		open += !c.done;
		if (n == MAX_ROWS) {
			for (i = 0; i < n && list[i].done; ++i)
				;
			if (i == n)
				continue;
			memmove(&list[i], &list[i + 1], (n - i - 1) * sizeof(*list));
			n--;
		}
		list[n++] = c;
	}

	mark = devnet_last_boot_mark(&mark_ms);
	snprintf(row, sizeof(row), "HBC %s: no menu %u.%u s after the start", CHANNEL_BUILD_STR,
			 (unsigned) (now / 1000), (unsigned) (now % 1000 / 100));
	draw_row(xfb, stride, top + 16 * r++, row, COLOR_BLACK);
	if (usbstart_stuck() && usbguard_blocked())
		draw_row(xfb, stride, top + 16 * r++, USBGUARD_MSG_STUCK, COLOR_MAROON);
	snprintf(row, sizeof(row), "Last start-up step: %s at %u ms. %d call%s with no answer:",
			 mark ? mark : "none", (unsigned) mark_ms, open, open == 1 ? "" : "s");
	draw_row(xfb, stride, top + 16 * r++, row, COLOR_BLACK);
	draw_row(xfb, stride, top + 16 * r++, "     in thread  call into IOS", COLOR_BLACK);

	// Calls still in are all shown, then as many of the latest finished ones as
	// fit: the finished calls to skip are the oldest.
	start = n - open - ((int) rows - (int) r - open);
	for (i = 0; i < n && r < rows; ++i) {
		if (list[i].done && start > 0) {
			start--;
			continue;
		}
		call_row(row, sizeof(row), &list[i], now);
		draw_row(xfb, stride, top + 16 * r++, row, list[i].done ? COLOR_BLACK : COLOR_MAROON);
	}
	while (r < rows)
		draw_row(xfb, stride, top + 16 * r++, "", COLOR_BLACK);
}

static void *screen_func(void *arg) {
	bool drawn = false;

	(void) arg;
	while (!stopped) {
		if (devnet_boot_ms() >= SCREEN_AFTER_MS) {
			LWP_MutexLock(draw_lock);
			if (!stopped) {
				draw_list(!drawn);
				drawn = true;
			}
			LWP_MutexUnlock(draw_lock);
			usleep(500 * 1000);
		} else {
			usleep(100 * 1000);
		}
	}
	return NULL;
}

void bootscreen_start(void) {
	if (LWP_MutexInit(&draw_lock, false) < 0)
		return;
	// Above the main thread: it shows up even when start-up is busy, not
	// only when it waits on IOS. It sleeps between the draws.
	LWP_CreateThread(&screen_thread, screen_func, NULL, screen_stack, sizeof(screen_stack), 80);
}

void bootscreen_stop(void) {
	stopped = true;
	if (draw_lock != LWP_MUTEX_NULL) {
		LWP_MutexLock(draw_lock);
		LWP_MutexUnlock(draw_lock);
	}
}
