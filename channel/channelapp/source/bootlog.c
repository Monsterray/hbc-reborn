#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>

#include "../config.h"
#include "bootlog.h"
#include "devnet.h"

// The calls. Up to KEPT of those made before the first frame are kept; later
// ones (the drive scan's polls go on all the time) reuse the oldest finished
// slot. A fast call that comes back like the last one of its name on its
// thread is folded into that one's row (repeats), so a poll takes one row.
#define CALLS 96
#define KEPT (CALLS / 2)
#define SLOW_MS 50

enum { FREE, IN, OUT };

static struct {
	const char *call;
	lwp_t thread;
	const char *thread_name;	// its name when the call began; NULL: none
	u32 in_ms, out_ms;		// the first time
	u32 last_out_ms;		// the latest repeat
	u32 repeats;
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

// A thread that ends leaves its id to the next one: the newest name for an id
// wins, and each call keeps the name its thread had when it began.
void bootlog_thread(const char *name) {
	lwp_t self = LWP_GetSelf();
	u32 i, level;

	_CPU_ISR_Disable(level);
	for (i = 0; i < thread_count && threads[i].id != self; ++i)
		;
	if (i < sizeof(threads) / sizeof(*threads)) {
		threads[i].id = self;
		threads[i].name = name;
		if (i == thread_count)
			thread_count++;
	}
	_CPU_ISR_Restore(level);
}

static const char *thread_lookup(lwp_t id) {
	u32 i;

	for (i = 0; i < thread_count; ++i)
		if (threads[i].id == id)
			return threads[i].name;
	return NULL;
}

static const char *thread_name(int slot, char *buf, size_t size) {
	if (calls[slot].thread_name)
		return calls[slot].thread_name;
	snprintf(buf, size, "%08x", (unsigned) calls[slot].thread);
	return buf;
}

static int take_slot(const char *call, lwp_t thread) {
	int i, slot = -1, kept = 0;
	u32 level;

	_CPU_ISR_Disable(level);
	for (i = 0; i < CALLS; ++i)
		kept += calls[i].state != FREE && calls[i].kept;
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
		calls[slot].thread_name = thread_lookup(thread);
		calls[slot].in_ms = devnet_boot_ms();
		calls[slot].out_ms = 0;
		calls[slot].repeats = 0;
		calls[slot].rc = 0;
		calls[slot].seq = ++call_seq;
		calls[slot].state = IN;
		calls[slot].kept = !menu_up && kept < KEPT;
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
	const char *call, *name;
	u32 level, in_ms, out_ms;
	int i, last = -1;
	char buf[12];

	if (slot < 0 || slot >= CALLS)
		return;
	_CPU_ISR_Disable(level);
	calls[slot].out_ms = devnet_boot_ms();
	calls[slot].rc = rc;
	calls[slot].state = OUT;
	call = calls[slot].call;
	in_ms = calls[slot].in_ms;
	out_ms = calls[slot].out_ms;
	name = thread_name(slot, buf, sizeof(buf));
	// A fast call that ends like the latest fast one of its name on this
	// thread: one row. Slow ones keep rows of their own, between.
	for (i = 0; out_ms - in_ms < SLOW_MS && i < CALLS; ++i)
		if (i != slot && calls[i].state == OUT && calls[i].call == call &&
				calls[i].thread == calls[slot].thread && calls[i].rc == rc &&
				calls[i].out_ms - calls[i].in_ms < SLOW_MS &&
				(last < 0 || calls[i].seq > calls[last].seq))
			last = i;
	if (last >= 0) {
		calls[last].repeats++;
		calls[last].last_out_ms = out_ms;
		calls[slot].state = FREE;
	}
	_CPU_ISR_Restore(level);
	if (say && out_ms - in_ms >= SLOW_MS)
		hlog("Start-up: %s (%s) took %u ms (in at %u), rc %d\n", call, name,
			 (unsigned) (out_ms - in_ms), (unsigned) in_ms, (int) rc);
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
		snprintf(row->thread, sizeof(row->thread), "%s", thread_name(next, buf, sizeof(buf)));
		row->in_ms = calls[next].in_ms;
		row->out_ms = calls[next].out_ms;
		row->repeats = calls[next].repeats;
		row->last_out_ms = calls[next].repeats ? calls[next].last_out_ms : calls[next].out_ms;
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
		if (size - n < 200)
			break;
		after = r.seq;
		if (r.done && r.repeats)
			add = snprintf(buf + n, size - n, "%s{\"call\":\"%s\",\"thread\":\"%s\",\"in\":%u,"
						   "\"out\":%u,\"rc\":%d,\"repeats\":%u,\"last_out\":%u}",
						   buf[n - 1] == '[' ? "" : ",", r.call, r.thread, (unsigned) r.in_ms,
						   (unsigned) r.out_ms, (int) r.rc, (unsigned) r.repeats,
						   (unsigned) r.last_out_ms);
		else if (r.done)
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
