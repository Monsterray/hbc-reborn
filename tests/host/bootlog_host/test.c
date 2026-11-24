// HBC's start-up call log (channel/channelapp/source/bootlog.c) on the host:
// a poll repeated before the first frame takes one row, the kept calls leave
// room for later ones, and a thread id that a new thread reuses names each
// call by the thread that made it. Built by tests/host/test_bootlog.py, which
// copies bootlog.c next to stub config.h and devnet.h.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootlog.h"

static u32 now_ms, self = 0x10000;
static int fails;

u32 devnet_boot_ms(void) { return now_ms; }
void devnet_boot_mark(const char *mark) { (void) mark; }
void hlog(const char *fmt, ...) { (void) fmt; }
lwp_t LWP_GetSelf(void) { return self; }
s32 IOS_OpenAsync(const char *p, u32 m, ipccallback cb, void *arg) { (void) m; cb(p[5] == 'n' ? -6 : 3, arg); return 0; }
s32 IOS_CloseAsync(s32 fd, ipccallback cb, void *arg) { (void) fd; (void) cb; (void) arg; return 0; }

#define CHECK(cond, ...) do { if (cond) printf("  PASS: " __VA_ARGS__); \
	else { printf("  FAIL: " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int rows(const char *call, bootlog_row *found) {
	bootlog_row r;
	u32 after = 0;
	int n = 0;

	while (bootlog_next(after, &r)) {
		after = r.seq;
		if (!call || !strcmp(r.call, call)) {
			if (found)
				*found = r;
			n++;
		}
	}
	return n;
}

static void timed(const char *call, u32 ms, s32 rc) {
	int slot = bootlog_in(call);

	now_ms += ms;
	bootlog_out(slot, rc);
	now_ms += 1;
}

int main(void) {
	static char json[16384];
	bootlog_row r;
	int i;

	bootlog_start();	// main, four probes
	CHECK(rows(NULL, NULL) == 4, "four probes logged (%d)", rows(NULL, NULL));

	// The start-up's SD polls: 200 fast "sd inserted" calls, all rc 0.
	for (i = 0; i < 200; ++i)
		timed("sd inserted", 2, 0);
	CHECK(rows("sd inserted", &r) == 1 && r.repeats == 199,
		  "200 fast polls take one row (%d rows, %u repeats)", rows("sd inserted", NULL),
		  (unsigned) r.repeats);
	CHECK(r.last_out_ms > r.out_ms, "the row says when the last poll came back (%u > %u)",
		  (unsigned) r.last_out_ms, (unsigned) r.out_ms);

	// A slow one, or one with another answer, gets a row of its own.
	timed("sd inserted", 80, 0);
	timed("sd inserted", 2, -1);
	CHECK(rows("sd inserted", NULL) == 3, "slow and different answers stay apart (%d rows)",
		  rows("sd inserted", NULL));
	// Fast polls after them still go on the first row (seen on a Wii: a slow
	// poll during the network's start split one poll into five rows).
	for (i = 0; i < 10; ++i)
		timed("sd inserted", 2, 0);
	{
		bootlog_row first;
		u32 after = 0;

		while (bootlog_next(after, &first) && strcmp(first.call, "sd inserted"))
			after = first.seq;
		CHECK(rows("sd inserted", NULL) == 3 && first.repeats == 209,
			  "later fast polls join the first row past a slow one (%d rows, %u repeats)",
			  rows("sd inserted", NULL), (unsigned) first.repeats);
	}

	// A thread names itself, ends, and a new unnamed thread gets its id.
	self = 0x20001;
	bootlog_thread("usbguard");
	timed("probe /dev/usb/ven", 3, 3);
	self = 0x20001;	// the same id, a new thread that names itself "scan"
	bootlog_thread("scan");
	timed("usb inserted", 90, 0);
	rows("probe /dev/usb/ven", &r);
	CHECK(!strcmp(r.thread, "usbguard"), "the earlier call keeps its thread's name (%s)", r.thread);
	rows("usb inserted", &r);
	CHECK(!strcmp(r.thread, "scan"), "the reused id gives the new name (%s)", r.thread);

	// Fill the log before the first frame with distinct slow calls.
	self = 0x10000;
	static char names[200][16];
	for (i = 0; i < 200; ++i) {
		snprintf(names[i], sizeof(names[i]), "early %d", i);
		timed(names[i], 60, i);
	}
	bootlog_menu_up();
	for (i = 0; i < 40; ++i) {
		snprintf(names[i], sizeof(names[i]), "late %d", i);
		timed(names[i], 60, i);
	}
	{
		bootlog_row first;
		u32 after = 0;
		int late = 0, probes = 0;

		while (bootlog_next(after, &first)) {
			after = first.seq;
			late += !strncmp(first.call, "late ", 5);
			probes += !strncmp(first.call, "probe /dev/", 11);
		}
		CHECK(late == 40, "after the first frame, 40 later calls are all kept (%d)", late);
		CHECK(probes >= 4, "the first calls of the start-up are still there (%d probes)", probes);
	}

	i = bootlog_json(json, sizeof(json));
	CHECK(i > 0 && json[i - 1] == '}' && strstr(json, "\"repeats\":209"),
		  "status JSON ends whole and carries the repeats (%d bytes)", i);

	if (fails)
		return 1;
	printf("all bootlog checks passed\n");
	return 0;
}
