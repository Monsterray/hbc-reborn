#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>

#include "../config.h"
#include "bootlog.h"
#include "devnet.h"

// The calls. Those made before the first frame are kept; later ones (the
// drive scan's polls go on all the time) reuse the oldest finished slot.
#define CALLS 96
#define SLOW_MS 50

enum { FREE, IN, OUT };

static struct {
	const char *call;
	lwp_t thread;
	u32 in_ms, out_ms;
	s32 rc;
	u32 seq;
	u8 state;
	bool kept;
} calls[CALLS];
static u32 call_seq, dropped;

static struct {
	lwp_t id;
	const char *name;
} threads[10];
static u32 thread_count;

static volatile bool menu_up;

void bootlog_thread(const char *name) {
	u32 level;

	_CPU_ISR_Disable(level);
	if (thread_count < sizeof(threads) / sizeof(*threads)) {
		threads[thread_count].id = LWP_GetSelf();
		threads[thread_count].name = name;
		thread_count++;
	}
	_CPU_ISR_Restore(level);
}

static const char *thread_name(lwp_t id, char *buf, size_t size) {
	u32 i;

	for (i = 0; i < thread_count; ++i)
		if (threads[i].id == id)
			return threads[i].name;
	snprintf(buf, size, "%08x", (unsigned) id);
	return buf;
}

static int take_slot(const char *call, lwp_t thread) {
	int i, slot = -1;
	u32 level;

	_CPU_ISR_Disable(level);
	for (i = 0; i < CALLS && slot < 0; ++i)
		if (calls[i].state == FREE)
			slot = i;
	// Full: the oldest finished call after the first frame, else any.
	for (i = 0; i < CALLS && slot < 0; ++i)
		if (calls[i].state == OUT && !calls[i].kept &&
				(slot < 0 || calls[i].seq < calls[slot].seq))
			slot = i;
	for (i = 0; i < CALLS && slot < 0; ++i)
		if (calls[i].state == OUT && (slot < 0 || calls[i].seq < calls[slot].seq))
			slot = i;
	if (slot >= 0) {
		if (calls[slot].state != FREE)
			dropped++;
		calls[slot].call = call;
		calls[slot].thread = thread;
		calls[slot].in_ms = devnet_boot_ms();
		calls[slot].out_ms = 0;
		calls[slot].rc = 0;
		calls[slot].seq = ++call_seq;
		calls[slot].state = IN;
		calls[slot].kept = !menu_up;
	} else {
		dropped++;
	}
	_CPU_ISR_Restore(level);
	return slot;
}

int bootlog_in(const char *call) {
	return take_slot(call, LWP_GetSelf());
}

// From a thread, or from the IPC interrupt (the probes, say false).
static void finish(int slot, s32 rc, bool say) {
	const char *call;
	u32 level, in_ms, out_ms;
	lwp_t thread;
	char buf[12];

	if (slot < 0 || slot >= CALLS)
		return;
	_CPU_ISR_Disable(level);
	calls[slot].out_ms = devnet_boot_ms();
	calls[slot].rc = rc;
	calls[slot].state = OUT;
	call = calls[slot].call;
	thread = calls[slot].thread;
	in_ms = calls[slot].in_ms;
	out_ms = calls[slot].out_ms;
	_CPU_ISR_Restore(level);
	if (say && out_ms - in_ms >= SLOW_MS)
		hlog("Start-up: %s (%s) took %u ms (in at %u), rc %d\n", call,
			 thread_name(thread, buf, sizeof(buf)), (unsigned) (out_ms - in_ms),
			 (unsigned) in_ms, (int) rc);
}

void bootlog_out(int slot, s32 rc) {
	finish(slot, rc, true);
}

void bootlog_note(const char *call, s32 rc) {
	finish(take_slot(call, LWP_GetSelf()), rc, false);
}

bool bootlog_next(u32 after, bootlog_row *row) {
	int i, next = -1;
	u32 level;

	_CPU_ISR_Disable(level);
	for (i = 0; i < CALLS; ++i)
		if (calls[i].state != FREE && calls[i].seq > after &&
				(next < 0 || calls[i].seq < calls[next].seq))
			next = i;
	if (next >= 0) {
		char buf[12];

		row->call = calls[next].call;
		snprintf(row->thread, sizeof(row->thread), "%s",
				 thread_name(calls[next].thread, buf, sizeof(buf)));
		row->in_ms = calls[next].in_ms;
		row->out_ms = calls[next].out_ms;
		row->rc = calls[next].rc;
		row->seq = calls[next].seq;
		row->done = calls[next].state == OUT;
	}
	_CPU_ISR_Restore(level);
	return next >= 0;
}

// ---- The probes ---------------------------------------------------------------

// Opened with no wait: an answer, even "no such device", shows that IOS
// module answers. One that never answers stays logged in.
// No USB node: requests waiting in a wedged USB module hold up every other
// module's, so USB gets only the one request of usbstart.h.
static const struct {
	const char *path, *name;
} probes[] = {
	{ "/dev/fs", "probe /dev/fs" },
	{ "/dev/es", "probe /dev/es" },
	{ "/dev/sdio/slot0", "probe /dev/sdio/slot0" },
	{ "/dev/net/ip/top", "probe /dev/net/ip/top" },
};
#define PROBES (sizeof(probes) / sizeof(*probes))

static s32 probe_closed(s32 result, void *arg) {
	(void) result;
	(void) arg;
	return 0;
}

static s32 probe_opened(s32 result, void *arg) {
	finish((int) (intptr_t) arg, result, false);
	if (result >= 0)
		IOS_CloseAsync(result, probe_closed, NULL);
	return 0;
}

static void probe(void) {
	u32 i;

	for (i = 0; i < PROBES; ++i) {
		int slot = take_slot(probes[i].name, LWP_GetSelf());

		if (slot >= 0 && IOS_OpenAsync(probes[i].path, 0, probe_opened, (void *) (intptr_t) slot) < 0)
			finish(slot, -1, false);
	}
}

void bootlog_start(void) {
	bootlog_thread("main");
	probe();
}

void bootlog_menu_up(void) {
	menu_up = true;
	devnet_boot_mark("first_frame");
}

// ---- HBCS ---------------------------------------------------------------------

int bootlog_json(char *buf, int size) {
	bootlog_row r;
	u32 after = 0;
	int n, add;

	n = snprintf(buf, size, ",\"calls\":{\"total\":%u,\"dropped\":%u,\"list\":[",
				 (unsigned) call_seq, (unsigned) dropped);
	if (n < 0 || n >= size)
		return 0;
	// In the order they began.
	while (bootlog_next(after, &r)) {
		// Room for the longest entry and the end, so the reply stays JSON.
		if (size - n < 160)
			break;
		after = r.seq;
		if (r.done)
			add = snprintf(buf + n, size - n, "%s{\"call\":\"%s\",\"thread\":\"%s\",\"in\":%u,"
						   "\"out\":%u,\"rc\":%d}", buf[n - 1] == '[' ? "" : ",", r.call,
						   r.thread, (unsigned) r.in_ms, (unsigned) r.out_ms, (int) r.rc);
		else
			add = snprintf(buf + n, size - n, "%s{\"call\":\"%s\",\"thread\":\"%s\",\"in\":%u,"
						   "\"out\":null}", buf[n - 1] == '[' ? "" : ",", r.call, r.thread,
						   (unsigned) r.in_ms);
		n += add > 0 && add < size - n ? add : 0;
	}
	if (n < size)
		n += snprintf(buf + n, size - n, "]}");
	return n < size ? n : size - 1;
}
