/* sdk/hbc_agent/way_out.c on a PC, against a model of the Wii's IPC registers
 * and of IOS answering, or not: each scenario checks which way out the agent
 * takes and what it notes. tests/test_way_out.py builds and runs it.
 *
 * The model, after Dolphin's WiiIPC (Source/Core/Core/HW/WII_IPC.cpp):
 *   - A request written to PPCMSG with X1 is acked (Y2) unless IOS is "dead";
 *     its reply comes 50 us later (Y1, ARMMSG) unless its fd is "stuck".
 *   - IOS acks or replies only while Y1, Y2 and the PPC's IPC interrupt flag
 *     are all clear (WiiIPC::IsReady()), so a reply the PPC leaves untaken
 *     holds up every ack after it.
 *   - Writing Y1 or Y2 with its interrupt enabled raises the IPC interrupt,
 *     even when the write clears the bit, so the flag must be cleared after.
 *   - ES's launch acks twice (it goes ahead) or replies (it refused), and
 *     each ES ioctlv's vector counts and lengths are checked.
 *   - Clearing HW_RESETS bit 0, X2 after a launch's second ack, and STM's
 *     hot reset end the scenario, as the Wii would reset there, unless the
 *     scenario says that one does nothing.
 *   - Writing any other register, or 0x817FEFF0 (Priiloader's word), fails.
 * Time is a counter that moves 5 us every other time the code reads it. */

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

static u32 hw_read(u32 a);
static void hw_write(u32 a, u32 v);

#include "../../sdk/hbc_agent/way_out.c"

// ---- The model ----------------------------------------------------------

enum { END_NONE, END_HW_RESET, END_LAUNCHED, END_STM, END_BAD };

#define OHBC 0x000100014f484243ull
#define LULZ 0x000100014c554c5aull
#define OTHER 0x0001000148424c57ull   // a title whose ticket is not there

static struct {
	// What this scenario's IOS does.
	int dead_after;       // acks only this many requests (-1: all)
	u32 stuck_mask;       // fds whose close never replies
	u32 late_mask;        // fds whose close replies 1.5 s late
	u64 installed[2];     // titles ES has (OHBC by default)
	bool refuse_launch;   // ES refuses every launch
	bool launch_hangs;    // the launch acks twice, and no reset follows
	bool launch_slow;     // the launch answers only after the agent's next request, refused
	bool stm_refused;     // /dev/stm/immediate does not open
	bool hw_reset_dud;    // HW_RESETS does nothing
	bool ahbprot, vwii;
	bool stale_reply;     // a reply libogc never took, pending at the start
	bool trailing_reply;  // a reply arrives after the last close's
	// State.
	u32 ctrl, armmsg, msg, resets, requests, closes, irq, sent, bad;
	u32 queue[16], ready[16], held;
	s32 result[16];       // written into the request as its reply comes
	int queued, launch_stage;
	u64 launched;
	jmp_buf end;
} m;

// IOS writes a request's result back as it replies, not before: a request
// slot the PPC reuses while a reply to it is still due gets that reply's
// result.
static void reply_later(u32 addr, u32 delay, s32 result) {
	m.queue[m.queued] = addr;
	m.result[m.queued] = result;
	m.ready[m.queued++] = (u32) now + delay;
}

static bool has(u64 title) {
	return title == m.installed[0] || title == m.installed[1];
}

static void bad(const char *why) {
	printf("    model: %s\n", why);
	m.bad = 1;
}

static void ioctlv(ipc_req *r, u32 addr) {
	vec *v = virt(r->arg[3]);
	u32 n = r->arg[0], in = r->arg[1], out = r->arg[2];
	u64 tid = v[0].len == 8 ? *(u64 *) virt(v[0].data) : 0;

	if (n == 0x12) {
		if (in != 1 || out != 1 || v[0].len != 8 || v[1].len != 4)
			bad("ES_GETVIEWCNT's vectors");
		*(u32 *) virt(v[1].data) = has(tid) ? 1 : 0;
		reply_later(addr, 50, 0);
		return;
	} else if (n == 0x13) {
		if (in != 2 || out != 1 || v[0].len != 8 || v[1].len != 4 ||
				v[2].len != 0xd8 * *(u32 *) virt(v[1].data))
			bad("ES_GETVIEWS' vectors");
		reply_later(addr, 50, has(tid) ? 0 : -1017);
		return;
	} else if (n == 0x08) {
		if (in != 2 || out != 0 || v[0].len != 8 || v[1].len != 0xd8)
			bad("ES_LAUNCH's vectors");
		m.launched = tid;
		if (m.launch_slow) {
			// The refusal comes after the agent has stopped waiting for it, as
			// it sends its next request: a reused slot would take it as that
			// request's answer.
			m.held = addr;
		} else if (m.refuse_launch || !has(tid)) {
			reply_later(addr, 50, -1017);
		} else {
			m.launch_stage = 1;
		}
		return;
	} else {
		bad("an unknown ioctlv");
	}
	reply_later(addr, 50, -4);
}

static void receive(u32 addr) {
	ipc_req *r = virt(addr);
	u32 held = m.held;

	++m.requests;
	if (held) {   // the late reply first; this request's a millisecond after
		reply_later(held, 0, -1017);
		m.held = 0;
	}
	m.ctrl |= 2;   // ack
	m.irq = 1;
	switch (r->cmd) {
	case 1:   // open
		if (!strcmp(virt(r->arg[0]), "/dev/es"))
			reply_later(addr, 50, 7);
		else if (!strcmp(virt(r->arg[0]), "/dev/stm/immediate"))
			reply_later(addr, 50, m.stm_refused ? -6 : 8);
		else
			reply_later(addr, 50, -6);
		break;
	case 2:   // close
		++m.closes;
		if (!(m.stuck_mask & (1u << r->fd)))
			reply_later(addr, m.late_mask & (1u << r->fd) ? 1500000 : 50,
						r->fd == 0 || r->fd == 3 ? 0 : -4);
		if (r->fd == 31 && m.trailing_reply) {   // a transfer ending as its fd closed
			static ipc_req transfer;
			reply_later(phys(&transfer), 1000, -22);
		}
		break;
	case 6:   // ioctl
		if (r->arg[0] == 0x2001) {
			if (r->fd != 8)
				bad("STM's hot reset sent to a descriptor STM did not give");
			longjmp(m.end, END_STM);
		}
		reply_later(addr, 50, -4);
		break;
	case 7:
		ioctlv(r, addr);
		break;
	default:
		bad("an unknown command");
	}
	if (held && m.queued > 1)
		m.ready[m.queued - 1] += 1000;
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
	((ipc_req *) virt(m.queue[i]))->result = m.result[i];
	m.ctrl |= 4;
	m.irq = 1;
	for (j = i + 1; j < m.queued; ++j) {
		m.queue[j - 1] = m.queue[j];
		m.ready[j - 1] = m.ready[j];
		m.result[j - 1] = m.result[j];
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
	case HW_CHIPREVID:
		return m.vwii ? 0xcafe0000 : 0;
	}
	bad("a read of an unknown register");
	return 0;
}

static void hw_write(u32 a, u32 v) {
	switch (a) {
	case IPC_PPCMSG:
		m.msg = v;
		break;
	case IPC_PPCCTRL:
		// IY1 (0x10) and IY2 (0x20) are kept, as the code writes them back.
		if ((v & 6) && (v & 0x30) == 0x30)
			m.irq = 1;   // Dolphin: a Y write with its interrupt enabled
		if (v & 2) {
			m.ctrl &= ~2u;
			if (m.launch_stage == 1)
				m.launch_stage = 2;
		}
		if (v & 4)
			m.ctrl &= ~4u;
		if ((v & 8) && m.launch_stage == 3) {
			if (!m.launch_hangs)
				longjmp(m.end, END_LAUNCHED);
			m.launch_stage = 4;   // went ahead, and nothing came of it
		}
		if (v & 1)
			m.sent = 1;
		break;
	case IPC_IRQFLAG:
		m.irq = 0;
		break;
	case HW_RESETS:
		if (!(v & 1) && !m.hw_reset_dud)
			longjmp(m.end, END_HW_RESET);
		m.resets = v;
		break;
	default:
		bad("a write to an unknown address");
	}
}

// ---- Scenarios --------------------------------------------------------------

static u32 notes[12], nnotes;

static void note(u32 way, const char *text) {
	if (nnotes < 12)
		notes[nnotes++] = way;
	printf("    %s", text);
}

static int failures;

typedef struct {
	u64 title;            // the stub's (0: none)
	bool stub, may_reset;
	int end;
	u32 ret;              // when end is END_NONE
	u32 closes;           // 0: not checked
	u64 launched;         // the title the launch went ahead with (0: not checked)
} want_t;

static void scenario(const char *name, want_t w, const u32 *want_notes) {
	volatile int end;
	volatile u32 ret = 0;
	u32 i;

	printf("%s\n", name);
	nnotes = 0;
	m.ctrl = m.armmsg = m.msg = m.requests = m.closes = m.bad = m.irq = m.sent = 0;
	m.resets = 0xffffffff;
	m.queued = m.launch_stage = 0;
	m.launched = 0;
	if (!m.installed[0])
		m.installed[0] = OHBC;
	nptrs = 0;
	if (m.stale_reply) {
		static ipc_req stale;

		m.armmsg = phys(&stale);
		m.ctrl = 4;
	}
	deadline = now + 200000000ull;   // the longest way out takes about 40 s
	end = setjmp(m.end);
	if (!end)
		ret = way_out_run(w.title, w.stub, w.may_reset, note);
	bool ok = !m.bad && end == w.end && (end != END_NONE || ret == w.ret) &&
			  (!w.closes || m.closes == w.closes) && (!w.launched || m.launched == w.launched);
	for (i = 0; want_notes[i] != ~0u; ++i)
		if (i >= nnotes || want_notes[i] != notes[i])
			ok = false;
	if (i != nnotes)
		ok = false;
	// Handing over to the stub: nothing of IOS's left waiting in the stub's
	// first 5 ms (a reply due then would hold its first ack back for good).
	if (end == END_NONE) {
		u64 until = now + 5000;
		while (now < until && !((m.ctrl & 6) || m.irq)) {
			now += 100;
			step();
		}
		if ((m.ctrl & 6) || m.irq)
			ok = false;
	}
	printf("  %s: end %d, returned %u, %u notes, %u closes\n", ok ? "PASS" : "FAIL", end,
		   (unsigned) ret, (unsigned) nnotes, (unsigned) m.closes);
	if (!ok)
		++failures;
	memset(&m, 0, sizeof(m) - sizeof(m.end));
	m.dead_after = -1;
	hbc_agent_way_out_test = 0;
}

#define NOTES(...) ((const u32[]) { __VA_ARGS__, ~0u })
#define WANT(...) ((want_t) { .title = OHBC, .stub = true, .may_reset = true, __VA_ARGS__ })
#define ES HBC_WAY_ES_LAUNCH
#define STM HBC_WAY_STM_RESET
#define HW HBC_WAY_RESET
#define ANYWAY HBC_WAY_STUB_ANYWAY
#define SILENT HBC_WAY_NOT_CHECKED   // "IOS stopped answering", before the reset

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

	scenario("healthy IOS: the reload stub", WANT(.end = END_NONE, .ret = HBC_WAY_STUB, .closes = 32),
			 NOTES(HBC_WAY_STUB));

	m.stale_reply = true;
	scenario("a reply libogc left pending: drained, then the stub",
			 WANT(.end = END_NONE, .ret = HBC_WAY_STUB, .closes = 32), NOTES(HBC_WAY_STUB));

	m.trailing_reply = true;
	scenario("a reply after the last close's: drained before the stub",
			 WANT(.end = END_NONE, .ret = HBC_WAY_STUB, .closes = 32), NOTES(HBC_WAY_STUB));

	scenario("no reload stub: HBC through ES", WANT(.stub = false, .end = END_LAUNCHED, .closes = 32,
			 .launched = OHBC), NOTES(ES));

	m.stuck_mask = 1u << 3;
	scenario("fd 3's close never replies: HBC through ES", WANT(.end = END_LAUNCHED, .closes = 32),
			 NOTES(ES));

	m.stuck_mask = 1u << 3 | 1u << 9 | 1u << 12 | 1u << 20;
	scenario("four stuck closes: stop at the third, HBC through ES", WANT(.end = END_LAUNCHED,
			 .closes = 13), NOTES(ES));

	m.late_mask = 1u << 3;
	scenario("fd 3 replies 1.5 s late, among later replies: HBC through ES",
			 WANT(.end = END_LAUNCHED, .closes = 32), NOTES(ES));

	m.stuck_mask = 1u << 3;
	m.installed[0] = LULZ;
	scenario("the stub's title is not there, OHBC is not, LULZ is: LULZ",
			 WANT(.title = OTHER, .end = END_LAUNCHED, .launched = LULZ), NOTES(ES, ES, ES));

	m.stuck_mask = 1u << 3;
	scenario("no title from the stub: OHBC", WANT(.title = 0, .end = END_LAUNCHED, .launched = OHBC),
			 NOTES(ES));

	m.stuck_mask = 1u << 3;
	m.refuse_launch = true;
	m.ahbprot = true;
	scenario("stuck, ES refuses all three, IOS acks: STM first, though AHBPROT is open",
			 WANT(.end = END_STM), NOTES(ES, ES, ES, STM));

	m.stuck_mask = 1u << 3;
	m.refuse_launch = true;
	m.stm_refused = true;
	m.ahbprot = true;
	scenario("stuck, ES refuses, STM does not open, AHBPROT open: HW_RESETS",
			 WANT(.end = END_HW_RESET), NOTES(ES, ES, ES, STM, HW));

	m.stuck_mask = 1u << 3;
	m.refuse_launch = true;
	m.stm_refused = true;
	m.ahbprot = true;
	m.vwii = true;
	scenario("the same on a vWii: no HW_RESETS, the stub anyway",
			 WANT(.end = END_NONE, .ret = ANYWAY), NOTES(ES, ES, ES, STM, ANYWAY));

	m.stuck_mask = 1u << 3;
	m.launch_hangs = true;
	scenario("the launch acks twice and no reset comes: STM (needs the IPC flag cleared after Y2)",
			 WANT(.end = END_STM), NOTES(ES, ES, STM));

	m.stuck_mask = 1u << 3;
	m.launch_slow = true;
	scenario("ES answers the launch after 10 s: STM's open in another slot, not that reply",
			 WANT(.end = END_STM), NOTES(ES, ES, STM));

	m.dead_after = 0;
	m.ahbprot = true;
	scenario("IOS acks nothing, AHBPROT open: HW_RESETS", WANT(.end = END_HW_RESET, .closes = 0),
			 NOTES(SILENT, HW));

	m.dead_after = 5;
	m.ahbprot = true;
	scenario("IOS stops after 5 closes, AHBPROT open: HW_RESETS", WANT(.end = END_HW_RESET,
			 .closes = 5), NOTES(SILENT, HW));

	m.dead_after = 0;
	m.ahbprot = true;
	m.hw_reset_dud = true;
	scenario("IOS acks nothing, HW_RESETS does nothing: the stub anyway",
			 WANT(.end = END_NONE, .ret = ANYWAY), NOTES(SILENT, HW, ANYWAY));

	m.dead_after = 0;
	scenario("IOS acks nothing, AHBPROT closed: the stub anyway", WANT(.end = END_NONE, .ret = ANYWAY,
			 .closes = 0), NOTES(SILENT, ANYWAY));

	m.stuck_mask = 1u << 3;
	m.refuse_launch = true;
	m.ahbprot = true;
	scenario("a reset came in the last 10 minutes: no second one, the stub anyway",
			 WANT(.may_reset = false, .end = END_NONE, .ret = ANYWAY), NOTES(ES, ES, ES, ANYWAY));

	hbc_agent_way_out_test = WAY_TEST_STUCK_FD;
	scenario("test bit: fd 3 stuck", WANT(.end = END_LAUNCHED, .closes = 32), NOTES(ES));

	hbc_agent_way_out_test = WAY_TEST_NO_ACK;
	m.ahbprot = true;
	scenario("test bit: no ack", WANT(.end = END_HW_RESET, .closes = 0), NOTES(SILENT, HW));

	hbc_agent_way_out_test = WAY_TEST_STUCK_FD | WAY_TEST_NO_LAUNCH | WAY_TEST_AHBPROT;
	m.ahbprot = true;
	scenario("test bits: stuck, no launch, AHBPROT closed: STM", WANT(.end = END_STM),
			 NOTES(ES, ES, ES, STM));

	hbc_agent_way_out_test = WAY_TEST_STUCK_FD | WAY_TEST_NO_LAUNCH | WAY_TEST_NO_RESET;
	m.ahbprot = true;
	scenario("test bits: stuck, no launch, no reset: the stub anyway",
			 WANT(.end = END_NONE, .ret = ANYWAY, .closes = 32), NOTES(ES, ES, ES, ANYWAY));

	printf("%s\n", failures ? "FAILED" : "all way-out scenarios passed");
	return failures != 0;
}
