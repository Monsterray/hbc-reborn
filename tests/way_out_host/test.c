/* sdk/hbc_agent/way_out.c on a PC, against a model of the Wii's IPC registers
 * and of IOS answering, or not: each scenario checks which way out the agent
 * takes and what it notes. tests/test_way_out.py builds and runs it.
 *
 * The model: a request written to PPCMSG with X1 is acked (Y2) unless IOS is
 * "dead"; its reply comes 50 us later (Y1, ARMMSG) unless its fd is "stuck".
 * As on a Wii (Dolphin's WiiIPC::IsReady()), IOS acks or replies only while
 * Y1, Y2 and the PPC's IPC interrupt flag are all clear, so a reply the PPC
 * leaves untaken holds up every ack after it. ES's launch acks twice (it
 * goes ahead) or replies (it refused).
 * Clearing HW_RESETS bit 0, a second launch ack taken, and STM's hot reset
 * end the scenario, as the Wii would reset there. Time is a counter that
 * moves 5 us every other time the code reads it. */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gctypes.h>
#define HBC_AGENT_LAYOUT_ONLY
#include "../../sdk/hbc_agent.h"

#define WAY_OUT_HOST

// Moves on every other read, so two reads in a row can agree, as two reads
// of the time base within a microsecond do on a Wii.
static u64 now, deadline = ~0ull;
static u32 reads;
static u32 now_us(void) {
	if (reads++ & 1)
		now += 5;
	if (now > deadline) {
		printf("  FAIL: still waiting at %llu us\nFAILED\n", (unsigned long long) now);
		exit(1);
	}
	return (u32) now;
}

// Pointers the code hands IOS, as 32-bit "physical" addresses.
static void *ptrs[64];
static int nptrs;

static u32 phys(const void *p) {
	int i;

	for (i = 0; i < nptrs; ++i)
		if (ptrs[i] == p)
			return 0x10000000 + i * 0x100;
	ptrs[nptrs] = (void *) p;
	return 0x10000000 + nptrs++ * 0x100;
}

static void *virt(u32 a) {
	return ptrs[(a - 0x10000000) / 0x100];
}

#define flush(p, n) ((void) (p), (void) (n))
#define invalidate(p, n) ((void) (p), (void) (n))
static vu32 priiloader_word;
#define PRIILOADER_MAGIC (&priiloader_word)

static u32 hw_read(u32 a);
static void hw_write(u32 a, u32 v);

#include "../../sdk/hbc_agent/way_out.c"

// ---- The model ----------------------------------------------------------

enum { END_NONE, END_HW_RESET, END_LAUNCHED, END_STM };

static struct {
	// What this scenario's IOS does.
	int dead_after;       // acks only this many requests (-1: all)
	u32 stuck_mask;       // fds whose close never replies
	u32 late_mask;        // fds whose close replies 1.5 s late
	bool refuse_launch;
	bool ahbprot;
	bool stale_reply;     // a reply libogc never took, pending at the start
	// State.
	u32 ctrl, armmsg, msg, resets, requests, closes, irq, sent;
	u32 queue[16], ready[16];
	int queued, launch_stage;
	jmp_buf end;
} m;

static void reply_later(u32 addr, u32 delay) {
	m.queue[m.queued] = addr;
	m.ready[m.queued++] = (u32) now + delay;
}

static void receive(u32 addr) {
	ipc_req *r = virt(addr);

	++m.requests;
	m.ctrl |= 2;   // ack
	m.irq = 1;
	switch (r->cmd) {
	case 1:   // open
		r->result = !strcmp(virt(r->arg[0]), "/dev/es") ? 7 : 8;
		reply_later(addr, 50);
		break;
	case 2:   // close
		++m.closes;
		r->result = r->fd == 0 || r->fd == 3 ? 0 : -4;
		if (m.stuck_mask & (1u << r->fd))
			break;
		reply_later(addr, m.late_mask & (1u << r->fd) ? 1500000 : 50);
		break;
	case 6:   // ioctl
		if (r->arg[0] == 0x2001)
			longjmp(m.end, END_STM);
		r->result = -4;
		reply_later(addr, 50);
		break;
	case 7: { // ioctlv
		vec *v = virt(r->arg[3]);

		if (r->arg[0] == 0x12) {
			*(u32 *) virt(v[1].data) = 1;
			r->result = 0;
		} else if (r->arg[0] == 0x13) {
			r->result = v[2].len == 0xd8 ? 0 : -1017;
		} else if (r->arg[0] == 0x08) {
			if (m.refuse_launch) {
				r->result = -1017;
				reply_later(addr, 50);
			} else {
				m.launch_stage = 1;
			}
			break;
		}
		reply_later(addr, 50);
		break;
	}
	}
}

// IOS's side, whenever the PPC looks: the request sent, else the second
// launch ack, else the first reply that is due (IOS answers each request
// when it is done), and only while Y1, Y2 and the interrupt are clear.
static void step(void) {
	int i, j;

	if ((m.ctrl & 6) || m.irq)
		return;
	if (m.sent) {
		m.sent = 0;
		if (m.dead_after < 0 || (int) m.requests < m.dead_after)
			receive(m.msg);
		return;
	}
	if (m.launch_stage == 2) {   // the second ack: going ahead
		m.launch_stage = 3;
		m.ctrl |= 2;
		m.irq = 1;
		return;
	}
	for (i = 0; i < m.queued && (u32) now < m.ready[i]; ++i)
		;
	if (i == m.queued)
		return;
	m.armmsg = m.queue[i];
	m.ctrl |= 4;
	m.irq = 1;
	for (j = i + 1; j < m.queued; ++j) {
		m.queue[j - 1] = m.queue[j];
		m.ready[j - 1] = m.ready[j];
	}
	--m.queued;
}

static u32 hw_read(u32 a) {
	switch (a) {
	case IPC_PPCCTRL:
		step();
		return m.ctrl | 0x30;
	case IPC_ARMMSG:
		return m.armmsg;
	case HW_AHBPROT:
		return m.ahbprot ? 0xffffffff : 0;
	case HW_RESETS:
		return m.resets;
	}
	return 0;
}

static void hw_write(u32 a, u32 v) {
	switch (a) {
	case IPC_PPCMSG:
		m.msg = v;
		break;
	case IPC_PPCCTRL:
		if (v & 2) {
			m.ctrl &= ~2u;
			if (m.launch_stage == 1)
				m.launch_stage = 2;
		}
		if (v & 4)
			m.ctrl &= ~4u;
		if ((v & 8) && m.launch_stage == 3)
			longjmp(m.end, END_LAUNCHED);
		if (v & 1)
			m.sent = 1;
		break;
	case IPC_IRQFLAG:
		m.irq = 0;
		break;
	case HW_RESETS:
		if (!(v & 1))
			longjmp(m.end, END_HW_RESET);
		m.resets = v;
		break;
	}
}

// ---- Scenarios --------------------------------------------------------------

static u32 notes[8], nnotes;
static char last_text[200];

static void note(u32 way, const char *text) {
	if (nnotes < 8)
		notes[nnotes++] = way;
	snprintf(last_text, sizeof(last_text), "%s", text);
	printf("    %s", text);
}

static int failures;

static void scenario(const char *name, bool stub, int want_end, u32 want_ret,
					 const u32 *want_notes, u32 want_closes) {
	volatile int end;
	volatile u32 ret = 0;
	u32 i;

	printf("%s\n", name);
	nnotes = 0;
	priiloader_word = 0;
	m.ctrl = m.armmsg = m.msg = m.requests = m.closes = 0;
	m.resets = 0xffffffff;
	m.queued = m.launch_stage = 0;
	nptrs = 0;
	if (m.stale_reply) {
		static ipc_req stale;

		m.armmsg = phys(&stale);
		m.ctrl = 4;
	}
	deadline = now + 200000000ull;   // the longest way out takes about 30 s
	end = setjmp(m.end);
	if (!end)
		ret = way_out_run(0x000100014f484243ull, stub, note);
	bool ok = end == want_end && (end != END_NONE || ret == want_ret) &&
			  (!want_closes || m.closes == want_closes);
	for (i = 0; want_notes[i] != ~0u; ++i)
		if (i >= nnotes || want_notes[i] != notes[i])
			ok = false;
	if (i != nnotes)
		ok = false;
	if ((end == END_HW_RESET || end == END_STM) && priiloader_word != 0x41627261)
		ok = false;
	printf("  %s: end %d, returned %u, %u notes, %u closes\n", ok ? "PASS" : "FAIL", end,
		   (unsigned) ret, (unsigned) nnotes, (unsigned) m.closes);
	if (!ok)
		++failures;
	memset(&m, 0, sizeof(m) - sizeof(m.end));
	m.dead_after = -1;
	hbc_agent_way_out_test = 0;
}

#define NOTES(...) ((const u32[]) { __VA_ARGS__, ~0u })

// A reply wait with no time left gives up at once, whichever way the clock's
// reads fall (a second read a microsecond later once made the time left
// wrap to 71 minutes).
static void no_time_left(void) {
	u32 parity;

	for (parity = 0; parity < 2; ++parity) {
		u64 start;
		ipc_status st;

		reads = parity;
		start = now;
		deadline = now + 1000000;
		used = 0;
		st = recv_reply(&slot[0], 0);
		bool ok = st == POLL_NO_REPLY && now - start < 1000;
		printf("no time left (clock parity %u)\n  %s: %llu us\n", (unsigned) parity,
			   ok ? "PASS" : "FAIL", (unsigned long long) (now - start));
		if (!ok)
			++failures;
	}
}

int main(void) {
	m.dead_after = -1;
	no_time_left();

	scenario("healthy IOS: the reload stub", true, END_NONE, HBC_WAY_STUB,
			 NOTES(HBC_WAY_STUB), 32);

	m.stale_reply = true;
	scenario("a reply libogc left pending: drained, then the stub", true, END_NONE, HBC_WAY_STUB,
			 NOTES(HBC_WAY_STUB), 32);

	scenario("no reload stub: HBC through ES", false, END_LAUNCHED, 0,
			 NOTES(HBC_WAY_ES_LAUNCH), 32);

	m.stuck_mask = 1u << 3;
	scenario("fd 3's close never replies: HBC through ES", true, END_LAUNCHED, 0,
			 NOTES(HBC_WAY_ES_LAUNCH), 32);

	m.stuck_mask = 1u << 3 | 1u << 9 | 1u << 12 | 1u << 20;
	scenario("four stuck closes: stop at the third, HBC through ES", true, END_LAUNCHED, 0,
			 NOTES(HBC_WAY_ES_LAUNCH), 13);

	m.late_mask = 1u << 3;
	scenario("fd 3 replies 1.5 s late, among later replies: HBC through ES", true, END_LAUNCHED,
			 0, NOTES(HBC_WAY_ES_LAUNCH), 32);

	m.stuck_mask = 1u << 3;
	m.refuse_launch = true;
	m.ahbprot = true;
	scenario("stuck, ES refuses, AHBPROT open: HW_RESETS", true, END_HW_RESET, 0,
			 NOTES(HBC_WAY_ES_LAUNCH, HBC_WAY_ES_LAUNCH, HBC_WAY_RESET), 32);

	m.stuck_mask = 1u << 3;
	m.refuse_launch = true;
	scenario("stuck, ES refuses, AHBPROT closed: STM hot reset", true, END_STM, 0,
			 NOTES(HBC_WAY_ES_LAUNCH, HBC_WAY_ES_LAUNCH, HBC_WAY_STM_RESET), 32);

	m.dead_after = 0;
	m.ahbprot = true;
	scenario("IOS acks nothing, AHBPROT open: HW_RESETS", true, END_HW_RESET, 0,
			 NOTES(HBC_WAY_RESET, HBC_WAY_RESET), 0);

	m.dead_after = 5;
	m.ahbprot = true;
	scenario("IOS stops after 5 closes, AHBPROT open: HW_RESETS", true, END_HW_RESET, 0,
			 NOTES(HBC_WAY_RESET, HBC_WAY_RESET), 5);

	m.dead_after = 0;
	scenario("IOS acks nothing, AHBPROT closed: the stub anyway", true, END_NONE,
			 HBC_WAY_STUB_ANYWAY, NOTES(HBC_WAY_STUB_ANYWAY, HBC_WAY_STUB_ANYWAY), 0);

	hbc_agent_way_out_test = WAY_TEST_STUCK_FD;
	scenario("test bit: fd 3 stuck", true, END_LAUNCHED, 0, NOTES(HBC_WAY_ES_LAUNCH), 32);

	hbc_agent_way_out_test = WAY_TEST_NO_ACK;
	m.ahbprot = true;
	scenario("test bit: no ack", true, END_HW_RESET, 0, NOTES(HBC_WAY_RESET, HBC_WAY_RESET), 0);

	hbc_agent_way_out_test = WAY_TEST_STUCK_FD | WAY_TEST_NO_LAUNCH | WAY_TEST_AHBPROT;
	m.ahbprot = true;
	scenario("test bits: stuck, no launch, AHBPROT closed: STM", true, END_STM, 0,
			 NOTES(HBC_WAY_ES_LAUNCH, HBC_WAY_ES_LAUNCH, HBC_WAY_STM_RESET), 32);

	hbc_agent_way_out_test = WAY_TEST_STUCK_FD | WAY_TEST_NO_LAUNCH | WAY_TEST_NO_RESET;
	m.ahbprot = true;
	scenario("test bits: stuck, no launch, no reset: the stub anyway", true, END_NONE,
			 HBC_WAY_STUB_ANYWAY, NOTES(HBC_WAY_ES_LAUNCH, HBC_WAY_ES_LAUNCH, HBC_WAY_STUB_ANYWAY), 32);

	printf("%s\n", failures ? "FAILED" : "all way-out scenarios passed");
	return failures != 0;
}
