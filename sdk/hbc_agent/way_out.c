// The way out when an app stops (../hbc_agent.h, "The way out"): back to
// HBC even when IOS has wedged.
//
// libogc's __reload() jumps to the reload stub at 0x80001800. The stub turns
// interrupts off, closes every IOS descriptor (0-31) and launches HBC through
// ES, waiting on each IPC with no time limit. When IOS, or one module in it,
// stops answering (USB Loader GX's DI read on d2x cIOS, 2026-10-24), the
// stub waits for good and only a power cycle helps. So the agent does the
// stub's steps itself, the same polled IPC with interrupts off, but with a
// time limit on each:
//
//   IOS acked every close     HBC through ES here (HBC_WAY_ES_LAUNCH), whether
//                             every close answered or one module is stuck
//   nothing acked             IOS itself is stuck: a reset, below
//
// A real wedge (USB Loader GX's 1 MiB DIP read on d2x, 2026-10-25) answered
// all 32 closes, and the stub, given the Wii then, froze in its own launch:
// so the stub runs only when every close answered and ES quickly refused
// every title (HBC_WAY_STUB), when IOS is well.
//
// The launch tries the stub's title, then this HBC's and the original's
// (OHBC, LULZ), so an app started by either comes back. When no launch goes
// ahead, or IOS is stuck, the console resets:
//   - while IOS still acks, /dev/stm/immediate's hot reset first
//     (HBC_WAY_STM_RESET): IOS takes it, so it can end a NAND write first;
//   - then, with AHBPROT open and not on a vWii, a system reset through
//     HW_RESETS (bit 0, RSTBINB, "System reset. Set to zero to reboot
//     system", WiiBrew Hardware/Hollywood Registers; HBC's own panic.c does
//     it), which needs no IOS at all (HBC_WAY_RESET).
// A reset reboots the console as a power-on does: boot2, then the System
// Menu, or Priiloader and whatever it is set to boot. The agent asks
// Priiloader for nothing (its "Abra" word would force its autoboot, and an
// autoboot of the app that wedged would loop). A reset is not tried again
// within 10 minutes of the last one (the crash block's reset_s, when it
// outlives the reset), so a Wii that wedges again at once stops instead of
// looping. Last, the stub anyway (HBC_WAY_STUB_ANYWAY).
//
// Each choice is written to the crash block's way_out and the kept log before
// it is taken, and drawn on the TV (screen_note()), which a power cycle does
// not lose: the capture card shows how far it got. tests/way_out_host runs
// this file on the PC against a model of the IPC registers (WAY_OUT_HOST).

#ifndef WAY_OUT_HOST
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/cache.h>
#include <ogc/ipc.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "../../channel/channelapp/config.h"
#include "agent_int.h"
#include "ov_draw.h"

static inline u32 hw_read(u32 a) {
	return read32(a);
}

static inline void hw_write(u32 a, u32 v) {
	write32(a, v);
}

static inline u32 now_us(void) {
	return ticks_to_microsecs(gettime());
}

#define flush(p, n) DCFlushRange(p, n)
#define invalidate(p, n) DCInvalidateRange(p, n)
#define phys(p) ((u32) (p) & 0x7fffffff)

static u32 ints_off(void) {
	u32 level;

	_CPU_ISR_Disable(level);
	return level;
}
#endif

// Set by a test app to make the agent see a failure it cannot cause itself:
// HBC_WAY_TEST_* bits (tests/agent_app's wayout mode). Never set otherwise.
u32 hbc_agent_way_out_test;
enum {
	WAY_TEST_STUCK_FD = 1,     // fd 3's close gets no reply
	WAY_TEST_NO_ACK = 2,       // IOS acks nothing
	WAY_TEST_NO_LAUNCH = 4,    // the ES launch fails
	WAY_TEST_AHBPROT = 8,      // AHBPROT reads as closed
	WAY_TEST_NO_RESET = 16,    // no reset: the stub instead (a bench run alone)
};

// ---- Polled IPC with time limits, after the stub's ios.c -------------------

#define IPC_PPCMSG 0x0d000000
#define IPC_PPCCTRL 0x0d000004
#define IPC_ARMMSG 0x0d000008
#define IPC_IRQFLAG 0x0d000030
#define HW_AHBPROT 0x0d800064
#define HW_RESETS 0x0d800194
#define HW_CHIPREVID 0x0d8005a0   // 0xcafe in the top half on a Wii U (vWii)

#define ACK_US 500000      // IOS acks in microseconds
#define REPLY_US 1000000   // a close or an open
#define LAUNCH_US 10000000 // an ES launch reads the title from NAND

typedef struct {
	u32 cmd;
	s32 result;
	s32 fd;
	u32 arg[5];
	u32 user[8];
} ipc_req;

// A request whose reply never came stays IOS's: the next one takes another
// (three stuck closes, an ES launch per title, STM's open and ioctl).
#define SLOTS 10
static ipc_req slot[SLOTS] __attribute__((aligned(64)));
static u32 used;

typedef enum { POLL_OK, POLL_NO_ACK, POLL_NO_REPLY } ipc_status;

static void bell(u32 w) {
	hw_write(IPC_PPCCTRL, (hw_read(IPC_PPCCTRL) & 0x30) | w);
}

static void irq_ack(void) {
	hw_write(IPC_IRQFLAG, 0x40000000);
}

static void udelay(u32 us) {
	u32 t = now_us();

	while (now_us() - t < us)
		;
}

static bool wait_bits(u32 bits, u32 us) {
	u32 t = now_us();

	while ((hw_read(IPC_PPCCTRL) & bits) != bits)
		if (now_us() - t > us)
			return false;
	return true;
}

// A reply waiting in ARMMSG: taken, and its address returned. IOS acks and
// replies only while Y1, Y2 and the PPC's IPC interrupt are all clear
// (Dolphin's WiiIPC::IsReady()), so a reply left there holds up every
// request after it.
static u32 take_reply(void) {
	u32 reply;

	udelay(100);
	reply = hw_read(IPC_ARMMSG);
	bell(4);
	irq_ack();
	bell(8);
	return reply;
}

// What libogc or the app left in flight: acks and replies taken and dropped.
static void ipc_drain(void) {
	int i;

	for (i = 0; i < 10; ++i) {
		if ((hw_read(IPC_PPCCTRL) & 0x22) == 0x22)
			bell(2);
		if ((hw_read(IPC_PPCCTRL) & 0x14) == 0x14)
			take_reply();
		irq_ack();
		udelay(1000);
	}
}

static ipc_req *req_new(u32 cmd, s32 fd) {
	ipc_req *r;

	if (used >= SLOTS)
		return NULL;
	r = &slot[used];
	memset(r, 0, sizeof(*r));
	r->cmd = cmd;
	r->fd = fd;
	return r;
}

// IOS's ack; a late reply to an earlier request that comes first is taken,
// or IOS would ack nothing more.
static bool wait_ack(void) {
	u32 t = now_us(), c;

	for (;;) {
		c = hw_read(IPC_PPCCTRL);
		if ((c & 0x22) == 0x22)
			return true;
		if ((c & 0x14) == 0x14)
			take_reply();
		if (now_us() - t > ACK_US)
			return false;
	}
}

static ipc_status send_req(ipc_req *r) {
	flush(r, sizeof(*r));
	hw_write(IPC_PPCMSG, phys(r));
	bell(1);
	if ((hbc_agent_way_out_test & WAY_TEST_NO_ACK) || !wait_ack()) {
		++used;   // IOS may still take it, and answer into it
		return POLL_NO_ACK;
	}
	udelay(100);
	bell(2);
	irq_ack();
	return POLL_OK;
}

static ipc_status recv_reply(ipc_req *r, u32 us) {
	u32 t = now_us();

	for (;;) {
		u32 spent = now_us() - t, reply;

		if (spent >= us || !wait_bits(0x14, us - spent)) {
			++used;   // IOS may still answer into it
			return POLL_NO_REPLY;
		}
		reply = take_reply();
		if (reply == phys(r))
			break;
		// A late reply to an earlier request: not this one.
	}
	invalidate(r, sizeof(*r));
	return POLL_OK;
}

static ipc_status call(ipc_req *r, u32 us) {
	ipc_status s;

	if (!r)
		return POLL_NO_REPLY;
	s = send_req(r);
	return s == POLL_OK ? recv_reply(r, us) : s;
}

static ipc_status ios_close(s32 fd, s32 *res) {
	ipc_req *r = req_new(2, fd);
	ipc_status s;

	if ((hbc_agent_way_out_test & WAY_TEST_STUCK_FD) && fd == 3 && r) {
		s = send_req(r);
		if (s == POLL_OK)
			s = recv_reply(r, 0);
	} else {
		s = call(r, REPLY_US);
	}
	*res = s == POLL_OK ? r->result : 0;
	return s;
}

static ipc_status ios_open(const char *path, s32 *fd) {
	static char name[32] __attribute__((aligned(32)));
	ipc_req *r = req_new(1, 0);
	ipc_status s;

	strncpy(name, path, sizeof(name) - 1);
	flush(name, sizeof(name));
	if (r) {
		r->arg[0] = phys(name);
		r->arg[1] = 0;
	}
	s = call(r, REPLY_US);
	*fd = s == POLL_OK ? r->result : -1;
	return s;
}

typedef struct {
	u32 data, len;
} vec;

static vec vecs[3] __attribute__((aligned(32)));

static void put_vec(int i, void *p, u32 len) {
	flush(p, len);
	vecs[i].data = phys(p);
	vecs[i].len = len;
}

static ipc_req *ioctlv_req(s32 fd, u32 n, u32 in, u32 out) {
	ipc_req *r = req_new(7, fd);

	if (!r)
		return NULL;
	flush(vecs, sizeof(vecs));
	r->arg[0] = n;
	r->arg[1] = in;
	r->arg[2] = out;
	r->arg[3] = phys(vecs);
	return r;
}

// ---- The ways out -----------------------------------------------------------

#define ES_GETVIEWCNT 0x12
#define ES_GETVIEWS 0x13
#define ES_LAUNCH 0x08

typedef enum {
	LAUNCH_NOT_HERE,    // ES answered that this title cannot launch: try the next
	LAUNCH_UNSURE       // no answer, or one that left IOS busy: no next title
} launch_end;

// HBC through ES, as the stub does it after its closes. Returns only when
// ES refused or did not answer, with what happened in *why; when it goes
// ahead, IOS reloads and resets the PPC into HBC.
static launch_end launch_hbc(u64 title, const char **why) {
	static u64 tid __attribute__((aligned(32)));
	static struct { u32 n, pad[7]; } count __attribute__((aligned(32)));   // a cache line of its own
	static u8 views[0xd8 * 4] __attribute__((aligned(32)));
	ipc_req *r;
	s32 fd;
	u32 t, status;

	if (hbc_agent_way_out_test & WAY_TEST_NO_LAUNCH) {
		*why = "launch refused (test)";
		return LAUNCH_NOT_HERE;
	}
	if (ios_open("/dev/es", &fd) != POLL_OK || fd < 0) {
		*why = "ES did not open";
		return LAUNCH_UNSURE;
	}
	tid = title;
	count.n = 0;
	put_vec(0, &tid, 8);
	put_vec(1, &count, 4);
	r = ioctlv_req(fd, ES_GETVIEWCNT, 1, 1);
	if (call(r, REPLY_US) != POLL_OK) {
		*why = "no answer to the ticket view count";
		return LAUNCH_UNSURE;
	}
	invalidate(&count, sizeof(count));
	if (r->result < 0 || count.n < 1 || count.n > 4) {
		*why = "no ticket for that title";
		return LAUNCH_NOT_HERE;
	}
	put_vec(0, &tid, 8);
	put_vec(1, &count, 4);
	put_vec(2, views, 0xd8 * count.n);
	r = ioctlv_req(fd, ES_GETVIEWS, 2, 1);
	if (call(r, REPLY_US) != POLL_OK) {
		*why = "no answer to the ticket views";
		return LAUNCH_UNSURE;
	}
	if (r->result < 0) {
		*why = "no ticket views";
		return LAUNCH_NOT_HERE;
	}
	invalidate(views, sizeof(views));
	put_vec(0, &tid, 8);
	put_vec(1, views, 0xd8);
	r = ioctlv_req(fd, ES_LAUNCH, 2, 0);
	if (!r || send_req(r) != POLL_OK) {
		*why = "launch not acked";
		return LAUNCH_UNSURE;
	}
	// IOS acks a launch twice when it goes ahead, and replies when it refuses.
	t = now_us();
	while (now_us() - t < LAUNCH_US) {
		status = hw_read(IPC_PPCCTRL);
		if ((status & 0x22) == 0x22) {
			// The second ack: taken as the stub's ipc_send_twoack() does,
			// Y2 before the interrupt flag (clearing Y2 with IY2 set raises
			// the flag again: Dolphin's WiiIPC), then X2.
			udelay(100);
			bell(2);
			irq_ack();
			bell(8);
			// Going ahead: the reset into HBC comes within a second or two.
			t = now_us();
			while (now_us() - t < LAUNCH_US)
				;
			++used;   // its reply, if one ever comes, is not ours to reuse
			*why = "launch acked, but no reset came";
			return LAUNCH_UNSURE;
		}
		// A reply: the launch's is a refusal; an earlier request's is late.
		if ((status & 0x14) == 0x14 && take_reply() == phys(r)) {
			*why = "launch refused";
			return LAUNCH_NOT_HERE;
		}
	}
	++used;
	*why = "launch not answered";
	return LAUNCH_UNSURE;
}

static bool ahbprot_open(void) {
	return !(hbc_agent_way_out_test & WAY_TEST_AHBPROT) && hw_read(HW_AHBPROT) == 0xffffffff;
}

// On a Wii U, HW_RESETS is not shown to reboot into the vWii: STM only.
static bool vwii(void) {
	return (hw_read(HW_CHIPREVID) >> 16) == 0xcafe;
}

// Returns only when nothing happened (Dolphin ignores the bit).
static void hw_reset(void) {
	hw_write(HW_RESETS, hw_read(HW_RESETS) & ~1u);
	udelay(2000000);
}

#define STM_HOTRESET 0x2001

static void stm_reset(void) {
	static u32 in[8] __attribute__((aligned(32))), out[8] __attribute__((aligned(32)));
	ipc_req *r;
	s32 fd;

	if (ios_open("/dev/stm/immediate", &fd) != POLL_OK || fd < 0)
		return;
	r = req_new(6, fd);
	if (!r)
		return;
	flush(in, sizeof(in));
	flush(out, sizeof(out));
	r->arg[0] = STM_HOTRESET;
	r->arg[1] = phys(in);
	r->arg[2] = sizeof(in);
	r->arg[3] = phys(out);
	r->arg[4] = sizeof(out);
	if (send_req(r) == POLL_OK) {
		++used;
		udelay(3000000);
	}
}

// The titles to launch HBC by: the stub's (title, 0 if none), then OHBC and
// LULZ, each once.
static int hbc_titles(u64 title, u64 out[3]) {
	static const u64 known[2] = { 0x000100014f484243ull, 0x000100014c554c5aull };
	int n = 0, i;

	if (title)
		out[n++] = title;
	for (i = 0; i < 2; ++i)
		if (known[i] != title)
			out[n++] = known[i];
	return n;
}

// The whole decision, given the stub's title (0 if none), whether a stub is
// there, and whether a reset may be tried. Calls note() before each attempt.
// Returns only to fall back on the stub. Not reentrant: interrupts are off.
static u32 way_out_run(u64 title, bool stub, bool may_reset,
					   void (*note)(u32 way, const char *text)) {
	static char text[160];
	u64 titles[3];
	u32 t0 = now_us(), answered = 0, stuck = 0, first_stuck = 0, ms;
	bool acks = true, resets = may_reset && !(hbc_agent_way_out_test & WAY_TEST_NO_RESET);
	s32 fd, res;
	int i, n;

	used = 0;
	ipc_drain();
	for (fd = 0; fd < 32 && acks && stuck < 3; ++fd) {
		switch (ios_close(fd, &res)) {
		case POLL_OK:
			++answered;
			break;
		case POLL_NO_REPLY:
			if (!stuck++)
				first_stuck = fd;
			break;
		case POLL_NO_ACK:
			acks = false;
			break;
		}
	}
	ms = (now_us() - t0) / 1000;
	if (acks) {
		bool unsure = false;

		if (stuck)
			snprintf(text, sizeof(text), "agent: way out: no reply to the close of fd %u "
					 "(%u stuck, %u ms): HBC through ES\n", (unsigned) first_stuck,
					 (unsigned) stuck, (unsigned) ms);
		else
			snprintf(text, sizeof(text), "agent: way out: IOS closed all 32 descriptors in %u "
					 "ms: HBC through ES\n", (unsigned) ms);
		note(HBC_WAY_ES_LAUNCH, text);
		n = hbc_titles(title, titles);
		for (i = 0; i < n; ++i) {
			const char *why = "";
			launch_end e = launch_hbc(titles[i], &why);

			snprintf(text, sizeof(text), "agent: way out: %08x%08x: %s\n",
					 (unsigned) (titles[i] >> 32), (unsigned) titles[i], why);
			note(HBC_WAY_ES_LAUNCH, text);
			if (e == LAUNCH_UNSURE) {
				unsure = true;
				break;
			}
		}
		// Every close answered and ES said at once that no title launches
		// (no HBC installed): IOS is well, so the stub, which another loader
		// may have put there, runs as it would without the agent.
		if (!stuck && !unsure && stub) {
			// A reply that came after the last one taken (an app's transfer
			// ending as its descriptor closed) would leave Y1 set, which the
			// stub does not expect: taken first.
			ipc_drain();
			note(HBC_WAY_STUB, "agent: way out: ES launched no HBC title: the reload stub\n");
			return HBC_WAY_STUB;
		}
	} else {
		snprintf(text, sizeof(text), "agent: way out: IOS stopped answering after %u closes "
				 "(%u ms)\n", (unsigned) answered, (unsigned) ms);
		note(HBC_WAY_NOT_CHECKED, text);
	}
	if (!resets) {
		note(HBC_WAY_STUB_ANYWAY, may_reset ? "agent: way out: no reset (test): the reload stub anyway\n" :
			 "agent: way out: a reset already came in the last 10 minutes: no second one, "
			 "the reload stub anyway\n");
		return HBC_WAY_STUB_ANYWAY;
	}
	if (acks) {
		note(HBC_WAY_STM_RESET, "agent: way out: STM hot reset\n");
		stm_reset();
	}
	if (ahbprot_open() && !vwii()) {
		note(HBC_WAY_RESET, "agent: way out: system reset (HW_RESETS)\n");
		hw_reset();
	}
	note(HBC_WAY_STUB_ANYWAY, "agent: way out: nothing reset the Wii: the reload stub anyway\n");
	return HBC_WAY_STUB_ANYWAY;
}

#ifndef WAY_OUT_HOST
// ---- The steps on the TV ----------------------------------------------------
//
// Drawn into the framebuffer the VI shows (its TFBL register, as HBC's
// panic.c finds it), through the uncached address, so nothing waits on the
// cache or on libogc: the app may have stopped mid-GX or with a lock held.
// With interrupts off no retrace flips to another buffer; the panel is drawn
// whole for each step, so a frame the GPU finished copying since cannot keep
// a step hidden. Integer code only (ov_draw's text and fills).

#define VI_VTR 0x0c002000
#define VI_TFBL 0x0c00201c
#define SCREEN_W 640
#define SCREEN_LINES 8
#define SCREEN_TEXT 90

static char screen_text[SCREEN_LINES][SCREEN_TEXT];
static int screen_n;

static void screen_note(const char *text) {
	static const char prefix[] = "agent: way out: ";
	u32 fb = read32(VI_TFBL) & 0x1fffffff, vtr = read32(VI_VTR) >> 16, lines, len, i;
	ov_canvas c;
	char *line;

	if (!strncmp(text, prefix, sizeof(prefix) - 1))
		text += sizeof(prefix) - 1;
	if (screen_n == SCREEN_LINES) {   // the oldest goes
		memmove(screen_text[0], screen_text[1], sizeof(screen_text[0]) * (SCREEN_LINES - 1));
		--screen_n;
	}
	line = screen_text[screen_n++];
	for (len = 0; text[len] && text[len] != '\n' && len < SCREEN_TEXT - 1; ++len)
		line[len] = text[len];
	line[len] = 0;

	if (fb & 0x10000000)   // the address is in 32-byte units
		fb <<= 5;
	lines = (vtr & 0xf) > 10 ? vtr >> 4 : 2 * (vtr >> 4);
	if (lines < 240 || lines > 576)
		lines = 480;
	if (!agent_ram(fb | 0x80000000, SCREEN_W * 2 * lines))
		return;
	c.fb = (u8 *) (fb | 0xc0000000);
	c.w = SCREEN_W;
	c.h = lines;
	c.stride = SCREEN_W * 2;
	ov_noclip(&c);
	ov_fill(&c, 24, 24, SCREEN_W - 48, 36 + 20 * screen_n, ov_rgb(0, 0, 0), 255);
	ov_text(&c, ov_th->bold, 36, 30, "HBC agent: leaving the app", ov_rgb(255, 255, 255));
	for (i = 0; i < (u32) screen_n; ++i)
		ov_text(&c, ov_th->regular, 36, 52 + 20 * i, screen_text[i], ov_rgb(255, 255, 255));
}

static void note_and_show(u32 way, const char *text) {
	agent_way_out_note(way, text);
	screen_note(text);
}

// A reload stub to jump into: the magic "STUBHAXX" at 4, and code at 0, an
// unconditional branch over the header (HBC's is 0x48000010). Dolphin, booting
// a DOL directly, leaves the magic with 0 at 0x80001800 and catches the jump
// with its HBReload hook; with the MMU on it does not, and USB Loader GX's way
// out ran 0x00000000. Without a stub, the way out launches HBC through ES.
static bool stub_found(void) {
	return *(u32 *) 0x80001804 == 0x53545542 && *(u32 *) 0x80001808 == 0x48415858 &&
		   (*(u32 *) 0x80001800 >> 26) == 18;
}

void agent_leave(void) {
	u64 title = *STUB_ADDR_MAGIC == STUB_MAGIC ? *STUB_ADDR_TITLE : 0;
	bool checked = !(agent_cfg()->no_safety & HBC_AGENT_NO_IOS_CHECK);

	if (checked) {
		ints_off();
		way_out_run(title, stub_found(), !agent_reset_recently(), note_and_show);
	}
	// libogc's __reload() jumps to the stub, or without one asks STM to
	// reboot through the IPC the way out has just left in its own state: a
	// checked way out without a stub stops here instead, its kept log said.
	if (!checked || stub_found())
		__reload();
	note_and_show(HBC_WAY_STUB_ANYWAY, "agent: way out: no reload stub to go to: stopped\n");
	for (;;)
		;
}
#endif
