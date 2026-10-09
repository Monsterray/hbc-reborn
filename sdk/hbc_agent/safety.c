// The agent's safety tools (../hbc_agent.h, "Safety tools"): what turns a
// silent failure into a report, and what keeps an app's last moments from
// doing damage.
//
//   stack guard    The CPU's data address breakpoint (DABR) watches one
//                  doubleword 1 KiB above the bottom of the main thread's
//                  stack: the first write there is a DSI, reported as a
//                  stack overflow. Below it, marker words catch a frame big
//                  enough to step over the breakpoint; the agent checks them
//                  as it wakes. The 1 KiB is room for the crash handler.
//   assert, abort  Reported like hbc_agent_fatal_now(), with the expression.
//   Reset, Power   Taken only when the app left libogc's default handlers:
//                  Reset exits to HBC (hbc.py exit's path), Power switches off
//                  once the app's on_exit ran and storage is written back.
//   flush          A reset function (libogc calls those from exit(), after
//                  the app's atexit handlers, and from SYS_ResetSystem())
//                  unmounts SD and USB, so their caches reach the card.
//   frames         A post-retrace callback counts framebuffer flips: frames
//                  per second, the longest frame, frames that ran late.
//   thread list    After a crash, fatal, hang or abort, every thread's state
//                  goes to the end of the kept log.
//   opt-in         guard_reload_stub and track_memory, checked once a second
//                  by the monitor thread (agent.c).
//
// Code that runs in an exception (safety_on_death and what it calls) is
// integer only: floating point is off there, so no printf.

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>

#include <ogcsys.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "ogc_flavor.h"
#if AGENT_TUXEDO
#include <tuxedo/thread.h>
#else
#include <ogc/lwp_threads.h>
#include <ogc/lwp_objmgr.h>
#endif

#include "agent_int.h"

// ---- Small, exception-safe text -------------------------------------------

typedef struct {
	char *p, *end;
} text;

static void put(text *t, const char *s) {
	while (*s && t->p < t->end)
		*t->p++ = *s++;
}

static void put_hex(text *t, u32 v) {
	int i;

	for (i = 28; i >= 0 && t->p < t->end; i -= 4)
		*t->p++ = "0123456789abcdef"[(v >> i) & 15];
}

static void put_u(text *t, u32 v) {
	char buf[10];
	int n = 0;

	do
		buf[n++] = '0' + v % 10;
	while ((v /= 10) && n < 10);
	while (n && t->p < t->end)
		*t->p++ = buf[--n];
}

static void put_line(text *t, char *start) {
	put(t, "\n");
	agent_log_raw(start, t->p - start);
	t->p = start;
}

// ---- State ----------------------------------------------------------------

static const hbc_agent_config *cfg;
static u32 off;  // HBC_AGENT_NO_*

static u32 stk_lo, stk_hi;     // the main thread's stack
static u32 guard;              // the DABR's doubleword; 0 when the breakpoint is off
static bool marks;             // marker words under it
#define GUARD_ROOM 1024
#define MARK_WORDS 8
#define MARK_STEP 120
#define MARK 0x5afe57acu
#define DABR_DW 2              // data write
#define DABR_BT 4              // with address translation on, as the app runs

static volatile u32 button;    // 1 Reset, 2 Power, from the interrupt
static bool took_reset, took_power;
static resetcallback prev_reset;  // what the app had, for safety_stop()
static powercallback prev_power;

static VIRetraceCallback prev_retrace;
static bool frames_on;
static void *last_fb;
static volatile u32 retraces, flips, last_flip, worst_gap, late;
static u32 fps, fps_flips, fps_retraces;

#define STUB_ADDR 0x80001800
#define STUB_SIZE 0x1800
static u32 *stub_copy;
static u32 stub_sum, stub_changed_ms;
static bool stub_restored;

static bool mem_on;
static u32 low_mem1 = ~0u, low_mem2 = ~0u, low_heap = ~0u;

// What the tools cost: checks (once a second) and retrace callbacks.
static u32 checks, retrace_calls;
static u64 check_ticks, retrace_ticks;
static u32 bytes_held;

// alloc_wrap.c, linked only with -Wl,--wrap=malloc.
extern agent_alloc_stats agent_alloc __attribute__((weak));

// ---- The DABR -------------------------------------------------------------

static inline u32 dabr_get(void) {
	u32 v;

	__asm__ volatile ("mfspr %0,1013" : "=r" (v));
	return v;
}

static inline void dabr_set(u32 v) {
	__asm__ volatile ("mtspr 1013,%0 ; isync" : : "r" (v));
}

// Puts the guards down: the breakpoint off, the stub back. Any context.
static void guards_down(void) {
	if (guard && (dabr_get() & ~7u) == guard)
		dabr_set(0);
	guard = 0;
	if (stub_copy && memcmp((void *) STUB_ADDR, stub_copy, STUB_SIZE)) {
		memcpy((void *) STUB_ADDR, stub_copy, STUB_SIZE);
		DCFlushRange((void *) STUB_ADDR, STUB_SIZE);
		ICInvalidateRange((void *) STUB_ADDR, STUB_SIZE);
		stub_restored = true;
	}
}

// ---- The main thread's stack ----------------------------------------------

#if AGENT_TUXEDO
extern void *__ppc_main_sp;
extern KThread *s_firstThread;
#else
extern lwp_objinfo _lwp_thr_objects;
#endif

static u32 current_sp(void) {
	u32 sp;

	__asm__ volatile ("mr %0,1" : "=r" (sp));
	return sp;
}

static bool mark_intact(void) {
	u32 i;

	for (i = 0; i < MARK_WORDS; ++i)
		if (*(volatile u32 *) (stk_lo + 16 + i * MARK_STEP) != (MARK ^ i))
			return false;
	return true;
}

static void stack_guard_init(void) {
	u32 sp = current_sp(), a;

#if AGENT_TUXEDO
	stk_hi = (u32) __ppc_main_sp;
	stk_lo = stk_hi - (cfg->main_stack_size ? cfg->main_stack_size : 0x20000);
#else
	stk_lo = (u32) _thr_main->stack;
	stk_hi = stk_lo + _thr_main->stack_size;
	if (cfg->main_stack_size)
		stk_lo = stk_hi - cfg->main_stack_size;
#endif
	stk_lo = (stk_lo + 7) & ~7u;
	if (stk_hi <= stk_lo + 16 * 1024 || stk_lo < 0x80000000) {
		stk_lo = stk_hi = 0;
		return;
	}
	// Only memory nothing has used: from main() itself, well above it;
	// from another thread, still zero (libogc's stacks are in .bss).
	if (sp >= stk_lo && sp < stk_hi) {
		if (sp < stk_lo + 16 * 1024) {
			stk_lo = stk_hi = 0;
			return;
		}
	} else {
		for (a = stk_lo + 16; a < stk_lo + GUARD_ROOM + 8; a += 4)
			if (*(u32 *) a) {
				stk_lo = stk_hi = 0;
				return;
			}
	}
	for (a = 0; a < MARK_WORDS; ++a)
		*(u32 *) (stk_lo + 16 + a * MARK_STEP) = MARK ^ a;
	marks = true;
	// A debugger may have the breakpoint; then the markers alone.
	if (!dabr_get()) {
		guard = stk_lo + GUARD_ROOM;
		dabr_set(guard | DABR_BT | DABR_DW);
	}
}

bool safety_stack_hit(u32 exid, u32 dsisr, u32 dar) {
	return exid == 3 && (dsisr & 0x00400000) && guard && (dar & ~7u) == guard;
}

// The deepest the main thread has reached: libogc's stacks start zeroed.
static u32 stack_deepest(void) {
	u32 a = stk_lo + GUARD_ROOM + 8;

	while (a < stk_hi && !*(u32 *) a)
		a += 4;
	return stk_hi - a;
}

// ---- The thread list ------------------------------------------------------

#if AGENT_TUXEDO
static const char *tux_state(u32 s) {
	static const char *names[] = { "unused", "finished", "running", "waiting", "on a mutex" };
	return s < 5 ? names[s] : "?";
}
#else
static const char *lwp_state(u32 s) {
	if (!s)
		return "ready";
	if (s & LWP_STATES_WAITING_FOR_MUTEX)
		return "on a mutex";
	if (s & LWP_STATES_WAITING_FOR_SEMAPHORE)
		return "on a semaphore";
	if (s & LWP_STATES_WAITING_FOR_CONDVAR)
		return "on a condition";
	if (s & LWP_STATES_WAITING_FOR_MESSAGE)
		return "on a message";
	if (s & (LWP_STATES_DELAYING | LWP_STATES_WAITING_FOR_TIME))
		return "sleeping";
	if (s & LWP_STATES_WAITING_FOR_JOINATEXIT)
		return "joining";
	if (s & LWP_STATES_SUSPENDED)
		return "suspended";
	if (s & LWP_STATES_DORMANT)
		return "dormant";
	return "waiting";
}
#endif

static bool ram(u32 a) {
	return (a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x94000000);
}

// One thread: its priority (libogc's LWP_* scale, 127 highest; -1 idle),
// state, stack pointer and room left, then where it is: the return
// addresses up its stack chain, nearest first. A thread that is not running
// was switched out inside libogc, so the first few are libogc's own wait;
// the app's code follows (hbc.py crash --elf or addr2line names them).
static void thread_line(text *t, char *start, bool self, int prio, const char *state, u32 sp,
						u32 lo) {
	u32 i, back = sp;

	put(t, self ? "  * " : "    ");
	if (prio < 0) {
		put(t, "idle");
	} else {
		put(t, "prio ");
		put_u(t, prio);
	}
	put(t, ", ");
	put(t, self ? "stopped here" : state);
	put(t, ", sp ");
	put_hex(t, sp);
	if (lo && sp > lo) {
		put(t, " (");
		put_u(t, sp - lo);
		put(t, " bytes left)");
	}
	put(t, ", calls");
	for (i = 0; i < 6 && ram(back) && !(back & 3); ++i) {
		u32 next = *(u32 *) back;

		if (!ram(next) || next <= back || (next & 3))
			break;
		put(t, " ");
		put_hex(t, *(u32 *) (next + 4));
		back = next;
	}
	put_line(t, start);
}

static void thread_list(void) {
	char line[112];
	text t = { line, line + sizeof(line) - 1 };
	int n = 0;

	put(&t, "-- threads when the app stopped (* the one that stopped) --");
	put_line(&t, line);
#if AGENT_TUXEDO
	{
		KThread *self = KThreadGetSelf(), *k;

		for (k = s_firstThread; k && ram((u32) k) && n < 24; k = k->next, ++n) {
			u32 sp = k->ctx.gpr[1];

			// libogc 3 counts 0 highest and puts the idle thread at 128.
			thread_line(&t, line, k == self, k->prio > 127 ? -1 : 127 - k->prio,
						tux_state(k->state), sp, sp >= stk_lo && sp < stk_hi ? stk_lo : 0);
		}
	}
#else
	{
		u32 i;

		for (i = 0; i < _lwp_thr_objects.max_nodes && n < 24; ++i) {
			lwp_cntrl *k = (lwp_cntrl *) _lwp_thr_objects.local_table[i];
			u32 core;

			if (!k || !ram((u32) k))
				continue;
			core = k->cur_prio;
			thread_line(&t, line, k == _thr_executing, core >= 128 ? 255 ^ core : -1,
						lwp_state(k->cur_state), k->context.FC_GPR[1], (u32) k->stack);
			++n;
		}
	}
#endif
}

// ---- assert() and abort() -------------------------------------------------

static void __attribute__((noreturn)) stop(u32 kind, u32 why, const char *reason, u32 from) {
	agent_stop_record(kind, 0, reason, from, from, current_sp());
	safety_on_death();
	agent_lastlog(why);
	__reload();
}

// newlib's assert() calls this. Weak, so an app's own still wins.
void __attribute__((weak, noreturn)) __assert_func(const char *file, int line,
												   const char *func, const char *expr) {
	char reason[HBC_CRASH_REASON];
	const char *base = file ? strrchr(file, '/') : NULL;

	base = base ? base + 1 : file ? file : "?";
	snprintf(reason, sizeof(reason), "%s:%d: %s", base, line, expr ? expr : "?");
	fprintf(stderr, "%s: assertion \"%s\" failed: file \"%s\", line %d%s%s\n",
			cfg && cfg->name ? cfg->name : "app", expr ? expr : "?", file ? file : "?", line,
			func ? ", function: " : "", func ? func : "");
	stop(HBC_CRASH_ASSERT, HBC_LASTLOG_ABORT, reason, (u32) __builtin_return_address(0));
}

void __attribute__((weak, noreturn)) abort(void) {
	fprintf(stderr, "%s: abort()\n", cfg && cfg->name ? cfg->name : "app");
	stop(HBC_CRASH_ABORT, HBC_LASTLOG_ABORT, "abort() called", (u32) __builtin_return_address(0));
}

// ---- Reset and Power ------------------------------------------------------

// libogc 3 and 1.8.23 pass the interrupt and a context, libogc2 nothing; the
// arguments are registers this ignores, so one function serves all three.
static void on_reset(u32 irq, void *ctx) {
	(void) irq;
	(void) ctx;
	button = 1;
}

static void on_power(void) {
	button = 2;
}

// A handler that does nothing: libogc 3's defaults are a bare blr.
static bool does_nothing(void *fn) {
	return !fn || *(u32 *) fn == 0x4e800020;
}

// Takes a button only when the app left libogc's default. libogc2 and 1.x
// put their default back for NULL, so setting NULL twice shows it; libogc
// 3 stores NULL, and its default does nothing.
static void take_buttons(void) {
	resetcallback old_r = SYS_SetResetCallback(NULL), def_r = SYS_SetResetCallback(NULL);
	powercallback old_p = SYS_SetPowerCallback(NULL), def_p = SYS_SetPowerCallback(NULL);

	prev_reset = old_r;
	prev_power = old_p;
	took_reset = old_r == def_r || does_nothing(old_r);
	SYS_SetResetCallback(took_reset ? (resetcallback) on_reset : old_r);
	took_power = old_p == def_p || does_nothing(old_p);
	SYS_SetPowerCallback(took_power ? on_power : old_p);
}

// ---- Storage at exit, and the guards, from libogc's reset functions -------

#if AGENT_TUXEDO
extern void dvmUnmountVolume(const char *name) __attribute__((weak));
#define UNMOUNT dvmUnmountVolume
#else
extern void fatUnmount(const char *name) __attribute__((weak));
#define UNMOUNT fatUnmount
#endif

static void flush_storage(void) {
	static const char *names[] = { "sd:", "usb:", "usb2:" };
	u64 t = gettime();
	unsigned i, n = 0;

	if (!UNMOUNT)
		return;
	for (i = 0; i < 3; ++i)
		if (FindDevice(names[i]) >= 0) {
			UNMOUNT(names[i]);
			++n;
		}
	if (n)
		printf("agent: wrote back and unmounted %u device%s in %u ms\n", n, n > 1 ? "s" : "",
			   (unsigned) ticks_to_millisecs(diff_ticks(t, gettime())));
}

static s32 on_system_reset(s32 final) {
	static bool flushed;

	if (!final) {
		if (!flushed && !(off & HBC_AGENT_NO_FLUSH)) {
			flushed = true;
			flush_storage();
		}
		return 1;
	}
	if (frames_on) {
		VIDEO_SetPostRetraceCallback(prev_retrace);
		frames_on = false;
	}
	guards_down();
	return 1;
}

static sys_resetinfo reset_info = { {}, on_system_reset, 1 };

// exit(): the kept log is written after this (atexit runs in reverse), so it
// says when the stub was put back. The reset function does it again for
// the other ways out.
static void at_exit(void) {
	if (agent_stopped())
		return;
	if (stub_copy && memcmp((void *) STUB_ADDR, stub_copy, STUB_SIZE)) {
		printf("agent: putting back the reload stub (overwritten at %u ms)\n",
			   (unsigned) stub_changed_ms);
		guards_down();
	}
}

// ---- Frames ---------------------------------------------------------------

static void on_retrace(u32 count) {
	u32 t0 = gettick(), n = ++retraces;
	void *fb = VIDEO_GetCurrentFramebuffer();

	if (fb != last_fb) {
		u32 f = ++flips, gap = n - last_flip;

		// A frame the overlay or a hold (a load) paused is no frame time.
		if (f > 2 && !agent_paused()) {
			if (gap > worst_gap)
				worst_gap = gap;
			// Late: half again as long as the average frame so far.
			if (2 * gap * f > 3 * n)
				++late;
		}
		last_fb = fb;
		last_flip = n;
	}
	if (prev_retrace)
		prev_retrace(count);
	retrace_calls++;
	retrace_ticks += (u32) (gettick() - t0);
}

// ---- Periodic checks ------------------------------------------------------

static void __attribute__((noreturn)) marks_broken(void) {
	u32 pc = 0, lr = 0, sp = 0;

#if AGENT_TUXEDO
	KThread *k;

	for (k = s_firstThread; k && ram((u32) k); k = k->next)
		if (k->ctx.gpr[1] >= stk_lo && k->ctx.gpr[1] < stk_hi) {
			pc = k->ctx.pc;
			lr = k->ctx.lr;
			sp = k->ctx.gpr[1];
			break;
		}
#else
	pc = _thr_main->context.FC_LR;
	lr = pc;
	sp = _thr_main->context.FC_GPR[1];
#endif
	printf("%s: stack overflow: the main thread wrote below its stack's last 1 KiB\n",
		   cfg->name ? cfg->name : "app");
	agent_stop_record(HBC_CRASH_STACK, 0, "main thread stack overflow (marker)", pc, lr, sp);
	safety_on_death();
	agent_lastlog(HBC_LASTLOG_STACK);
	__reload();
}

static u32 sum_words(const u32 *p, u32 n) {
	u32 x = 0;

	while (n--)
		x = ((x << 5) | (x >> 27)) ^ *p++;
	return x;
}

static void check_once_a_second(void) {
	u64 t = gettime();

	if (marks && !mark_intact())
		marks_broken();
	if (stub_copy && !stub_changed_ms &&
			sum_words((const u32 *) STUB_ADDR, STUB_SIZE / 4) != stub_sum) {
		stub_changed_ms = agent_uptime_ms();
		printf("agent: the reload stub at 0x80001800 was overwritten; it is put back at exit\n");
	}
	if (mem_on) {
		struct mallinfo m = mallinfo();
		u32 a1 = SYS_GetArena1Size(), a2 = SYS_GetArena2Size();

		if (a1 < low_mem1)
			low_mem1 = a1;
		if (a2 < low_mem2)
			low_mem2 = a2;
		if ((u32) m.fordblks < low_heap)
			low_heap = m.fordblks;
	}
	fps = flips - fps_flips;
	fps_flips = flips;
	fps_retraces = retraces;
	checks++;
	check_ticks += diff_ticks(t, gettime());
}

void safety_poll(bool from_monitor) {
	static u64 last;
	u32 b = button;

	if (b) {
		button = 0;
		if (b == 1) {
			printf("%s: Reset pressed: exiting to HBC\n", cfg->name ? cfg->name : "app");
			agent_request_exit();
		}
		printf("%s: Power pressed: switching off\n", cfg->name ? cfg->name : "app");
		agent_power_off();
	}
	// Once a second, from the monitor when it runs (it outranks the app).
	if (from_monitor != agent_monitor_running())
		return;
	if (last && diff_ticks(last, gettime()) < secs_to_ticks(1))
		return;
	last = gettime();
	check_once_a_second();
}

// ---- When the app stops -----------------------------------------------------

void safety_on_death(void) {
	char line[112];
	text t = { line, line + sizeof(line) - 1 };

	guards_down();
	if (marks && !mark_intact()) {
		put(&t, "-- the main thread's stack overflowed: its last 1 KiB was written --");
		put_line(&t, line);
	}
	if (mem_on) {
		put(&t, "-- lowest free memory: MEM1 ");
		put_u(&t, low_mem1);
		put(&t, ", MEM2 ");
		put_u(&t, low_mem2);
		put(&t, ", heap ");
		put_u(&t, low_heap);
		put(&t, " bytes --");
		put_line(&t, line);
	}
	if (stub_changed_ms) {
		put(&t, "-- the reload stub was overwritten at ");
		put_u(&t, stub_changed_ms);
		put(&t, " ms and put back --");
		put_line(&t, line);
	}
	if (!(off & HBC_AGENT_NO_THREADS))
		thread_list();
}

// ---- Start-up and status --------------------------------------------------

static bool at_exit_set;

void safety_init(const hbc_agent_config *config) {
	cfg = config;
	off = cfg->no_safety;

	if (!(off & HBC_AGENT_NO_STACK_GUARD))
		stack_guard_init();
	if (!(off & HBC_AGENT_NO_BUTTONS))
		take_buttons();
	SYS_RegisterResetFunc(&reset_info);
	if (!at_exit_set)   // once: atexit() has no undo, and a stop makes it a no-op
		atexit(at_exit);
	at_exit_set = true;
	if (!(off & HBC_AGENT_NO_FRAMES)) {
		last_fb = VIDEO_GetCurrentFramebuffer();
		prev_retrace = VIDEO_SetPostRetraceCallback(on_retrace);
		frames_on = true;
	}
	if (cfg->guard_reload_stub && *(u32 *) (STUB_ADDR + 4) == 0x53545542 &&
			*(u32 *) (STUB_ADDR + 8) == 0x48415858) {   // "STUBHAXX"
		stub_copy = memalign(32, STUB_SIZE);
		if (stub_copy) {
			memcpy(stub_copy, (void *) STUB_ADDR, STUB_SIZE);
			stub_sum = sum_words(stub_copy, STUB_SIZE / 4);
			bytes_held += STUB_SIZE;
		}
	}
	mem_on = cfg->track_memory;
	// The opt-in checks run in the monitor, which outranks the app; so do
	// the buttons when there is no agent thread to see them.
	if (stub_copy || mem_on || ((took_reset || took_power) && cfg->no_network))
		bytes_held += agent_monitor_start();
}

// Each hook goes back to what it replaced, unless the app has put its own in
// since: then the app's stays.
void safety_stop(void) {
	if (frames_on) {
		VIRetraceCallback cur = VIDEO_SetPostRetraceCallback(prev_retrace);

		if (cur != on_retrace)
			VIDEO_SetPostRetraceCallback(cur);
		frames_on = false;
	}
	guards_down();
	marks = false;
	stk_lo = stk_hi = 0;
	if (took_reset) {
		resetcallback cur = SYS_SetResetCallback(prev_reset);

		if (cur != (resetcallback) on_reset)
			SYS_SetResetCallback(cur);
	}
	if (took_power) {
		powercallback cur = SYS_SetPowerCallback(prev_power);

		if (cur != on_power)
			SYS_SetPowerCallback(cur);
	}
	took_reset = took_power = false;
	button = 0;
	SYS_UnregisterResetFunc(&reset_info);
	free(stub_copy);
	stub_copy = NULL;
	stub_changed_ms = 0;
	mem_on = false;
	bytes_held = 0;
}

s32 safety_json(char *buf, size_t size) {
	s32 n;

	n = snprintf(buf, size, ",\"safety\":{\"stack_guard\":\"%s\"",
				 guard ? "breakpoint" : marks ? "marker" : "off");
	if (stk_hi)
		n += snprintf(buf + n, size - n, ",\"main_stack\":{\"size\":%u,\"deepest\":%u}",
					  (unsigned) (stk_hi - stk_lo), (unsigned) stack_deepest());
	n += snprintf(buf + n, size - n, ",\"reset\":%s,\"power\":%s,\"flush_at_exit\":%s",
				  took_reset ? "true" : "false", took_power ? "true" : "false",
				  off & HBC_AGENT_NO_FLUSH || !UNMOUNT ? "false" : "true");
	if (frames_on) {
		u32 r = retraces, f = flips;

		n += snprintf(buf + n, size - n, ",\"frames\":{\"fps\":%u,\"flips\":%u,\"retraces\":%u,"
					  "\"worst_ms\":%u,\"late\":%u,\"single_buffer\":%s}",
					  (unsigned) (checks ? fps : r ? f * 60 / r : 0), (unsigned) f,
					  (unsigned) r, (unsigned) (worst_gap * 1000 / 60), (unsigned) late,
					  r > 120 && f < 2 ? "true" : "false");
	} else {
		n += snprintf(buf + n, size - n, ",\"frames\":null");
	}
	if (stub_copy)
		n += snprintf(buf + n, size - n, ",\"stub\":{\"changed_at_ms\":%u}",
					  (unsigned) stub_changed_ms);
	else
		n += snprintf(buf + n, size - n, ",\"stub\":null");
	if (mem_on && checks)
		n += snprintf(buf + n, size - n, ",\"mem_low\":{\"mem1\":%u,\"mem2\":%u,\"heap\":%u}",
					  (unsigned) low_mem1, (unsigned) low_mem2, (unsigned) low_heap);
	else
		n += snprintf(buf + n, size - n, ",\"mem_low\":null");
	if (&agent_alloc)
		n += snprintf(buf + n, size - n, ",\"alloc_failures\":{\"count\":%u,\"last_size\":%u,"
					  "\"last_from\":\"%08x\"}", (unsigned) agent_alloc.count,
					  (unsigned) agent_alloc.last_size, (unsigned) agent_alloc.last_from);
	else
		n += snprintf(buf + n, size - n, ",\"alloc_failures\":null");
	n += snprintf(buf + n, size - n, ",\"cost\":{\"checks\":%u,\"check_us\":%u,"
				  "\"retraces\":%u,\"retrace_us\":%u,\"bytes\":%u}}",
				  (unsigned) checks, (unsigned) ticks_to_microsecs(check_ticks),
				  (unsigned) retrace_calls, (unsigned) ticks_to_microsecs(retrace_ticks),
				  (unsigned) bytes_held);
	return n;
}
