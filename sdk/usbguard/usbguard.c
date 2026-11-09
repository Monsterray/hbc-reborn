/* usbguard: survive a stuck USB drive, for libogc programs. See usbguard.h.
 *
 * Public domain: see the end of this file.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "usbguard.h"

/* ---- The platform ------------------------------------------------------ */

#ifdef USBGUARD_NO_OGC

#ifdef _WIN32
#include <windows.h>

typedef HANDLE ug_thread;
static SRWLOCK ug_mx = SRWLOCK_INIT;
static CONDITION_VARIABLE ug_cv = CONDITION_VARIABLE_INIT;

static void ug_init(void) {}
static void ug_lock(void) { AcquireSRWLockExclusive(&ug_mx); }
static void ug_unlock(void) { ReleaseSRWLockExclusive(&ug_mx); }
static void ug_wait(uint32_t ms) { SleepConditionVariableSRW(&ug_cv, &ug_mx, ms, 0); }
static void ug_broadcast(void) { WakeAllConditionVariable(&ug_cv); }
static void ug_sleep(uint32_t ms) { Sleep(ms); }

static uint32_t ug_now(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER t;

	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t);
	return (uint32_t) (t.QuadPart * 1000 / freq.QuadPart);
}

static void *(*ug_entry)(void *);
static DWORD WINAPI ug_trampoline(LPVOID arg)
{
	ug_entry(arg);
	return 0;
}

static int ug_start(ug_thread *t, void *(*entry)(void *), void *arg, void *stack, uint8_t prio)
{
	(void) stack;
	(void) prio;
	ug_entry = entry;
	*t = CreateThread(NULL, USBGUARD_STACK_SIZE, ug_trampoline, arg, 0, NULL);
	return *t ? 0 : -1;
}

static void ug_join(ug_thread t)
{
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}

#else /* POSIX */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef pthread_t ug_thread;
static pthread_mutex_t ug_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ug_cv = PTHREAD_COND_INITIALIZER;

static void ug_init(void) {}
static void ug_lock(void) { pthread_mutex_lock(&ug_mx); }
static void ug_unlock(void) { pthread_mutex_unlock(&ug_mx); }
static void ug_broadcast(void) { pthread_cond_broadcast(&ug_cv); }
static void ug_sleep(uint32_t ms) { usleep(ms * 1000); }

static void ug_wait(uint32_t ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long) (ms % 1000) * 1000000;
	ts.tv_sec += ms / 1000 + ts.tv_nsec / 1000000000;
	ts.tv_nsec %= 1000000000;
	pthread_cond_timedwait(&ug_cv, &ug_mx, &ts);
}

static uint32_t ug_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int ug_start(ug_thread *t, void *(*entry)(void *), void *arg, void *stack, uint8_t prio)
{
	(void) stack;
	(void) prio;
	return pthread_create(t, NULL, entry, arg) ? -1 : 0;
}

static void ug_join(ug_thread t) { pthread_join(t, NULL); }
#endif

/* The host test program (tests/host/usbguard_host) supplies the registers
 * and fatMountSimple, and the stand-in <fat.h>, <ogc/disc_io.h> and
 * "usbguard_test.h" headers. */
#include <fat.h>
#include "usbguard_test.h"

#define ug_read32 usbguard_host_read32
#define ug_write32 usbguard_host_write32
#define UG_NEEDS_STACK 0

#else /* libogc */

#include <gccore.h>
#include <fat.h>
#include <malloc.h>
#include <unistd.h>
#include <ogc/cond.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

typedef lwp_t ug_thread;
static mutex_t ug_mx = LWP_MUTEX_NULL;
static cond_t ug_cv = LWP_COND_NULL;

static void ug_init(void)
{
	u32 level;

	_CPU_ISR_Disable(level);
	if (ug_mx == LWP_MUTEX_NULL) {
		LWP_MutexInit(&ug_mx, false);
		LWP_CondInit(&ug_cv);
	}
	_CPU_ISR_Restore(level);
}

static void ug_lock(void) { LWP_MutexLock(ug_mx); }
static void ug_unlock(void) { LWP_MutexUnlock(ug_mx); }
static void ug_broadcast(void) { LWP_CondBroadcast(ug_cv); }
static void ug_sleep(uint32_t ms) { usleep(ms * 1000); }
static uint32_t ug_now(void) { return (uint32_t) ticks_to_millisecs(gettime()); }

static void ug_wait(uint32_t ms)
{
	struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000 };	/* relative */

	LWP_CondTimedWait(ug_cv, ug_mx, &ts);
}

static int ug_start(ug_thread *t, void *(*entry)(void *), void *arg, void *stack, uint8_t prio)
{
	return LWP_CreateThread(t, entry, arg, stack, USBGUARD_STACK_SIZE, prio) < 0 ? -1 : 0;
}

static void ug_join(ug_thread t) { LWP_JoinThread(t, NULL); }

#define ug_read32 read32
#define ug_write32 write32
#define UG_NEEDS_STACK 1

#endif

/* ---- The EHCI registers ------------------------------------------------ */

#define HW_AHBPROT 0x0d800064
#define EHCI_BASE 0x0d040000
#define EHCI_PORTS_MAX 8

/* Capability registers, from EHCI_BASE. */
#define EHCI_CAPLENGTH_MASK 0xff	/* CAPLENGTH: where the operational registers start */
#define EHCI_HCSPARAMS 4
#define EHCI_N_PORTS_MASK 0xf		/* HCSPARAMS bits 3:0 */
/* Operational registers, from EHCI_BASE + CAPLENGTH. */
#define EHCI_PORTSC_OFFSET 0x44		/* PORTSC of port 0; one word per port */

/* Timing, in ms. Tests may override them with -D. */
#ifndef USBGUARD_SETTLE_MS
#define USBGUARD_SETTLE_MS 1500		/* after a replug, before the retry: IOS enumerates */
#endif
#ifndef USBGUARD_D2X_GIVEUP_MS
#define USBGUARD_D2X_GIVEUP_MS 6500	/* d2x gives up after about 7 s; a quicker -101 is a plain answer */
#endif

#define PORTSC_CCS 0x0001	/* a device is connected */
#define PORTSC_CSC 0x0002	/* connect status change (write 1 clears) */
#define PORTSC_PE 0x0004	/* port enabled */
#define PORTSC_PEC 0x0008	/* enable change (write 1 clears) */
#define PORTSC_OCC 0x0020	/* over-current change (write 1 clears) */
#define PORTSC_OWNER 0x2000	/* 1: the companion (USB 1.1) controller has the port */
#define PORTSC_RWC (PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)

static uint32_t ehci_portsc_addr(int i)
{
	return EHCI_BASE + (ug_read32(EHCI_BASE) & EHCI_CAPLENGTH_MASK) + EHCI_PORTSC_OFFSET + 4 * i;
}

static int ehci_ports(void)
{
	int n = (int) (ug_read32(EHCI_BASE + EHCI_HCSPARAMS) & EHCI_N_PORTS_MASK);

	return n > EHCI_PORTS_MAX ? EHCI_PORTS_MAX : n;
}

/* A port with a USB 2.0 device on it (a USB 1.1 one goes to the companion). */
static bool ehci_has_drive(uint32_t v)
{
	return (v & PORTSC_CCS) && !(v & PORTSC_OWNER);
}

bool usbguard_can_free(void)
{
	return ug_read32(HW_AHBPROT) == 0xffffffff;
}

/* The ports with a USB 2.0 device on them now: what a call is about to meet.
 * 0 without AHBPROT. */
static uint32_t ehci_drive_mask(void)
{
	uint32_t m = 0;
	int i, n;

	if (!usbguard_can_free())
		return 0;
	n = ehci_ports();
	for (i = 0; i < n; i++)
		if (ehci_has_drive(ug_read32(ehci_portsc_addr(i))))
			m |= 1u << i;
	return m;
}

/* The devices a call met are still all there: 1 if every port of mask has a
 * USB 2.0 device, 0 if one of them lost its device (or mask is 0), -1 without
 * AHBPROT. Which of them is the drive cannot be told, so the pull of any one
 * counts, and a device that stays on another port does not hide it. */
static int masked_present(uint32_t mask)
{
	int i, n;

	if (!usbguard_can_free())
		return -1;
	if (!mask)
		return 0;
	n = ehci_ports();
	for (i = 0; i < n; i++)
		if ((mask & (1u << i)) && !ehci_has_drive(ug_read32(ehci_portsc_addr(i))))
			return 0;
	return 1;
}

/* The drive is back after a pull: every port of mask has a device again, or
 * the drive went into another port, which is a device on a port outside mask
 * for each port of mask that is still empty. false without AHBPROT. */
static bool replugged(uint32_t mask)
{
	int i, n, missing = 0, extra = 0;

	if (!usbguard_can_free() || !mask)
		return false;
	n = ehci_ports();
	for (i = 0; i < n; i++) {
		bool has = ehci_has_drive(ug_read32(ehci_portsc_addr(i)));

		if (mask & (1u << i))
			missing += !has;
		else
			extra += has;
	}
	return extra >= missing;
}

int usbguard_drive_present(void)
{
	if (!usbguard_can_free())
		return -1;
	return ehci_drive_mask() != 0;
}

usbguard_state usbguard_check(void)
{
	int i, n;

	if (!usbguard_can_free())
		return USBGUARD_NO_ACCESS;
	n = ehci_ports();
	for (i = 0; i < n; i++) {
		uint32_t v = ug_read32(ehci_portsc_addr(i));

		if (ehci_has_drive(v) && !(v & PORTSC_PE))
			return USBGUARD_CUT_OFF;
	}
	return USBGUARD_OK;
}

static uint32_t freed_mask, pulled_mask;
static volatile unsigned free_count;
static usbguard_port_change changes[EHCI_PORTS_MAX];
static int change_count;

bool usbguard_free(void)
{
	bool any = false;
	int i, n;

	if (!usbguard_can_free())
		return false;
	change_count = 0;	/* usbguard_freed_ports describes this free only */
	n = ehci_ports();
	for (i = 0; i < n; i++) {
		uint32_t reg = ehci_portsc_addr(i), v = ug_read32(reg);

		if (!ehci_has_drive(v))
			continue;
		/* PE cleared; PEC and OCC written as 0 (left alone); CSC written
		 * as 1, which clears it, so a new one means the drive came out. */
		ug_write32(reg, (v & ~(PORTSC_RWC | PORTSC_PE)) | PORTSC_CSC);
		if (change_count < EHCI_PORTS_MAX) {
			changes[change_count].port = (uint8_t) i;
			changes[change_count].before = v;
			changes[change_count].after = ug_read32(reg);
			change_count++;
		}
		freed_mask |= 1u << i;
		pulled_mask &= ~(1u << i);
		any = true;
	}
	if (any)
		free_count++;
	return any;
}

int usbguard_freed_ports(usbguard_port_change *out, int max)
{
	int i;

	for (i = 0; i < change_count && i < max; i++)
		out[i] = changes[i];
	return i;
}

bool usbguard_pulled(void)
{
	int i;

	if (!freed_mask || !usbguard_can_free())
		return pulled_mask != 0;
	for (i = 0; i < EHCI_PORTS_MAX; i++) {
		uint32_t bit = 1u << i, v;

		if (!(freed_mask & bit) || (pulled_mask & bit))
			continue;
		v = ug_read32(ehci_portsc_addr(i));
		if ((v & PORTSC_CSC) || !(v & PORTSC_CCS))
			pulled_mask |= bit;
	}
	return pulled_mask != 0;
}

/* ---- The workers ------------------------------------------------------- */

/* A slot is free to reuse in SLOT_IDLE, or in SLOT_DONE when its caller gave
 * up and the worker has since come back (it still has to be joined).
 * SLOT_JOINING is a finished call whose caller is joining the thread: its
 * stack is not free until the join is done. */
enum { SLOT_IDLE, SLOT_RUNNING, SLOT_DONE, SLOT_JOINING };

static struct {
	ug_thread thread;
	bool joinable;		/* a thread was started and not yet joined */
	void *stack;
	usbguard_fn fn;
	void *arg;
	int32_t result;
	int state;		/* under ug_mx */
	volatile bool stuck;	/* past warn_ms, or given up on */
} slots[USBGUARD_WORKERS];

static void *worker_main(void *p)
{
	int s = (int) (intptr_t) p;
	int32_t r = slots[s].fn(slots[s].arg);

	ug_lock();
	slots[s].result = r;
	slots[s].state = SLOT_DONE;
	ug_broadcast();
	ug_unlock();
	return NULL;
}

bool usbguard_blocked(void)
{
	bool b = false;
	int i;

	ug_init();
	ug_lock();
	for (i = 0; i < USBGUARD_WORKERS; i++)
		if (slots[i].state == SLOT_RUNNING && slots[i].stuck)
			b = true;
	ug_unlock();
	return b;
}

/* A worker running fn(arg): its slot, or USBGUARD_STUCK (all are blocked)
 * or USBGUARD_NO_WORKER. A slot is reused only once its thread is done and
 * joined. */
static int start_worker(usbguard_fn fn, void *arg, uint8_t prio)
{
	bool join;
	int i, s = -1;

	ug_lock();
	for (i = 0; i < USBGUARD_WORKERS && s < 0; i++)
		if (slots[i].state == SLOT_IDLE || slots[i].state == SLOT_DONE)
			s = i;
	if (s < 0) {
		ug_unlock();
		return USBGUARD_STUCK;
	}
	join = slots[s].joinable;	/* a worker given up on that came back */
	slots[s].state = SLOT_RUNNING;
	slots[s].stuck = false;
	slots[s].joinable = false;
	ug_unlock();
	if (join)
		ug_join(slots[s].thread);
#if UG_NEEDS_STACK
	if (!slots[s].stack)
		slots[s].stack = memalign(32, USBGUARD_STACK_SIZE);
	if (!slots[s].stack) {
		ug_lock();
		slots[s].state = SLOT_IDLE;
		ug_unlock();
		return USBGUARD_NO_WORKER;
	}
#endif
	slots[s].fn = fn;
	slots[s].arg = arg;
	if (ug_start(&slots[s].thread, worker_main, (void *) (intptr_t) s, slots[s].stack, prio) < 0) {
		ug_lock();
		slots[s].state = SLOT_IDLE;
		ug_unlock();
		return USBGUARD_NO_WORKER;
	}
	slots[s].joinable = true;
	return s;
}

/* Counts attempts, so that usbguard_default_ui can tell a new one from the
 * same one going on, whatever phase it starts in. */
static volatile unsigned attempt_serial;	/* read only by usbguard_default_ui */

/* One attempt: fn on a worker, on_wait while it runs. */
static int32_t run(usbguard_fn fn, void *arg, const usbguard_opts *o, bool *stuck_seen,
		   uint32_t *took)
{
	bool was_blocked = usbguard_blocked();
	uint32_t t0 = ug_now(), ms;
	int32_t r;
	int s;

	attempt_serial++;
	s = start_worker(fn, arg, o->priority);
	*took = 0;
	if (s < 0)
		return s;
	ug_lock();
	for (;;) {
		if (slots[s].state == SLOT_DONE)
			break;
		ug_wait(o->poll_ms);
		if (slots[s].state == SLOT_DONE)
			break;
		ug_unlock();
		ms = ug_now() - t0;
		usbguard_phase phase = was_blocked || ms >= o->warn_ms ?
			USBGUARD_PHASE_STUCK : USBGUARD_PHASE_WAITING;
		if (phase == USBGUARD_PHASE_STUCK) {
			slots[s].stuck = true;
			*stuck_seen = true;
		}
		if (o->on_wait && !o->on_wait(ms, phase, o->ctx)) {
			/* The worker keeps its slot and stack until it comes back. */
			slots[s].stuck = true;
			*took = ms;
			return USBGUARD_STUCK;
		}
		ug_lock();
	}
	r = slots[s].result;
	slots[s].state = SLOT_JOINING;
	ug_unlock();
	ug_join(slots[s].thread);
	ug_lock();
	slots[s].joinable = false;
	slots[s].state = SLOT_IDLE;
	ug_unlock();
	*took = ug_now() - t0;
	return r;
}

/* d2x gave up (-101) and the drive is still connected: wait for the pull.
 * 0: pulled. 1: usbguard_free cut it off. -1: on_wait gave up, or, with no
 * on_wait, replug_ms passed. */
static int wait_pull(const usbguard_opts *o, uint32_t took, unsigned frees, uint32_t mask)
{
	uint32_t t0 = ug_now();

	for (;;) {
		if (free_count != frees)
			return 1;
		if (masked_present(mask) == 0)
			return 0;
		if (!o->on_wait && ug_now() - t0 >= o->replug_ms)
			return -1;
		if (o->on_wait && !o->on_wait(took + ug_now() - t0, USBGUARD_PHASE_STUCK, o->ctx))
			return -1;
		ug_sleep(o->poll_ms);
	}
}

/* After a pull: until the drive is back and settled (with AHBPROT), or for
 * replug_ms (without). With no on_wait, replug_ms also ends the wait for a
 * drive that does not come back. false: on_wait gave up, or that. */
static bool wait_replug(const usbguard_opts *o, uint32_t took, uint32_t mask)
{
	bool see = usbguard_can_free(), is_back = false;
	uint32_t t0 = ug_now(), back_at = 0, now;

	for (;;) {
		now = ug_now();
		if (see) {
			if (!is_back && replugged(mask)) {
				is_back = true;
				back_at = now;
			}
			if (is_back && now - back_at >= USBGUARD_SETTLE_MS)
				return true;
			if (!is_back && !o->on_wait && now - t0 >= o->replug_ms)
				return false;
		} else if (now - t0 >= o->replug_ms) {
			return true;
		}
		if (o->on_wait && !o->on_wait(took + now - t0, USBGUARD_PHASE_REPLUG, o->ctx))
			return false;
		ug_sleep(o->poll_ms);
	}
}

static void fill(usbguard_opts *o, const usbguard_opts *in)
{
	static const usbguard_opts none = USBGUARD_OPTS_INIT;

	*o = in ? *in : none;
	if (!o->warn_ms)
		o->warn_ms = 5000;
	if (!o->poll_ms)
		o->poll_ms = 50;
	if (!o->replug_ms)
		o->replug_ms = 10000;
	if (!o->priority)
		o->priority = 64;
}

/* keep_ok: a non-negative result passes even when usbguard_free cut the
 * stuck request off (a reload that finished is done, whoever freed it). */
static int32_t call(usbguard_fn fn, void *arg, const usbguard_opts *opts, bool keep_ok)
{
	usbguard_opts o;
	int attempt;

	ug_init();
	fill(&o, opts);
	for (attempt = 0;; attempt++) {
		unsigned frees = free_count;
		/* The ports with a device now: only they count as "the drive". */
		uint32_t mask = ehci_drive_mask();
		bool stuck = false, was_in = mask != 0;
		uint32_t took;
		int32_t r = run(fn, arg, &o, &stuck, &took);

		if (r == USBGUARD_STUCK || r == USBGUARD_NO_WORKER)
			return r;
		/* The app cut the drive off while this call was stuck. A stuck
		 * request can still answer after that (IOS_Open gives a handle),
		 * but the drive is gone, so the answer is not trusted. */
		if (stuck && free_count != frees && !(keep_ok && r >= 0))
			return USBGUARD_FREED;
		if (r == USBGUARD_D2X_GAVE_UP) {
			int w;

			/* A quicker answer than d2x's give-up, or no drive: as it is. */
			if (took < USBGUARD_D2X_GIVEUP_MS ||
			    (usbguard_can_free() && masked_present(mask) != 1))
				return r;
			/* The drive is in but d2x gave up on it: as stuck. */
			if (!usbguard_can_free())
				return USBGUARD_STUCK;
			w = wait_pull(&o, took, frees, mask);

			if (w < 0)
				return USBGUARD_STUCK;
			if (w > 0)
				return USBGUARD_FREED;
		} else if (stuck && r < 0) {
			/* The stuck request came back with an error: cut off or pulled. */
			if (free_count != frees)
				return USBGUARD_FREED;
		} else {
			/* A stuck request can answer successfully as the drive is
			 * pulled (IOS_Open of /dev/usb/ven gives a handle). The next
			 * call needs the drive back, so with AHBPROT wait for the
			 * replug before returning the result. */
			if (stuck && was_in && free_count == frees && !o.no_retry &&
			    masked_present(mask) == 0)
				wait_replug(&o, took, mask);
			return r;
		}
		if (attempt > 0 || o.no_retry || !wait_replug(&o, took, mask))
			return USBGUARD_FREED;
	}
}

int32_t usbguard_call(usbguard_fn fn, void *arg, const usbguard_opts *opts)
{
	return call(fn, arg, opts, false);
}

/* A call with no retry, whose non-negative result passes even if
 * usbguard_free cut the request off: what usbguard_reload_ios runs. */
static int32_t reload_call(usbguard_fn fn, void *arg, const usbguard_opts *opts)
{
	usbguard_opts o;

	fill(&o, opts);
	o.no_retry = true;
	return call(fn, arg, &o, true);
}

#ifdef USBGUARD_NO_OGC
int32_t usbguard_test_reload_call(usbguard_fn fn, void *arg, const usbguard_opts *opts)
{
	return reload_call(fn, arg, opts);
}
#endif

/* ---- The wrappers ------------------------------------------------------ */

struct mount_args {
	const char *name;
	const DISC_INTERFACE *iface;
	uint32_t warn_ms;
};

static int32_t mount_fn(void *p)
{
	const struct mount_args *a = p;
	uint32_t t0 = ug_now();

	if (!a->iface->startup())
		return ug_now() - t0 >= a->warn_ms ? USBGUARD_D2X_GAVE_UP : USBGUARD_NO_DRIVE;
	if (!a->iface->isInserted())
		return USBGUARD_NO_DRIVE;
	return fatMountSimple(a->name, a->iface) ? 0 : USBGUARD_NOT_MOUNTED;
}

int32_t usbguard_mount(const char *name, const DISC_INTERFACE *iface, const usbguard_opts *opts)
{
	usbguard_opts o;
	struct mount_args a;
	int32_t r;

	fill(&o, opts);
	a.name = name;
	a.iface = iface;
	a.warn_ms = o.warn_ms;
	r = usbguard_call(mount_fn, &a, &o);
	/* A slow startup failure with no drive connected, or a quick one. */
	return r == USBGUARD_D2X_GAVE_UP ? USBGUARD_NO_DRIVE : r;
}

#ifndef USBGUARD_NO_OGC
static int32_t reload_fn(void *p)
{
	return IOS_ReloadIOS((int) (intptr_t) p);
}

int32_t usbguard_reload_ios(int ios, const usbguard_opts *opts)
{
	return reload_call(reload_fn, (void *) (intptr_t) ios, opts);
}

bool usbguard_default_ui(uint32_t ms, usbguard_phase phase, void *ctx)
{
	static bool pad_ready, cut;
	static int shown = -1;
	static unsigned serial;
	u32 down = 0;
	int i;

	(void) ms;
	(void) ctx;
	if (serial != attempt_serial) {	/* a new attempt: say it all again */
		serial = attempt_serial;
		shown = -1;
		cut = false;
	}
	if (phase == USBGUARD_PHASE_WAITING)
		return true;
	if (!pad_ready) {
		PAD_Init();
		pad_ready = true;
	}
	PAD_ScanPads();
	for (i = 0; i < 4; i++)
		down |= PAD_ButtonsDown(i);
	if ((int) phase != shown) {
		shown = phase;
		if (phase == USBGUARD_PHASE_STUCK) {
			printf("\n%s\n", USBGUARD_MSG_STUCK);
			printf("Wii Remotes do not work until it is unplugged.\n");
			if (usbguard_can_free())
				printf("On a GameCube controller: A = continue without USB.\n");
			else
				printf("No button works until it is unplugged.\n");
		} else {
			printf("\nThe USB drive is out. Plug it back in to try again.\n");
			printf("On a GameCube controller: B = go on without it.\n");
		}
	}
	if (phase == USBGUARD_PHASE_STUCK && !cut && (down & PAD_BUTTON_A) && usbguard_can_free()) {
		cut = true;
		printf(usbguard_free() ? "USB is cut off: going on without it.\n" :
		       "No USB drive to cut off.\n");
	}
	if (phase == USBGUARD_PHASE_REPLUG && (down & PAD_BUTTON_B))
		return false;
	return true;
}
#endif

/* This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors of
 * this software dedicate any and all copyright interest in the software to
 * the public domain. We make this dedication for the benefit of the public
 * at large and to the detriment of our heirs and successors. We intend this
 * dedication to be an overt act of relinquishment in perpetuity of all
 * present and future rights to this software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * For more information, please refer to <https://unlicense.org/>
 */
