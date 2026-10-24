// The way out when an app stops (../hbc_agent.h, "The way out"): back to
// HBC even when IOS has wedged.
//
// libogc's __reload() jumps to the reload stub at 0x80001800. The stub turns
// interrupts off, closes every IOS descriptor (0-31) and launches HBC through
// ES, waiting on each IPC with no time limit. When IOS, or one module in it,
// stops answering (USB Loader GX's DI read on d2x cIOS, 2026-10-24), the
// stub waits for good and only a power cycle helps. So the agent first does
// the stub's steps itself, the same polled IPC with interrupts off, but with
// a time limit on each, and picks the way out from what answered:
//
//   every close answered      the reload stub, as before (HBC_WAY_STUB)
//   IOS acked, but a close    one module is stuck: HBC through ES here,
//   never replied             skipping the closes (HBC_WAY_ES_LAUNCH)
//   nothing acked             IOS itself is stuck
//
// When the launch fails or IOS is stuck: a system reset through HW_RESETS
// (bit 0, RSTBINB, "System reset. Set to zero to reboot system", WiiBrew
// Hardware/Hollywood Registers; HBC's own panic.c does it), which the PPC can
// write only with AHBPROT open (HBC_WAY_RESET); else /dev/stm/immediate's hot
// reset, while IOS still acks (HBC_WAY_STM_RESET). Both reboot the console:
// boot2, then the System Menu, or Priiloader where it is installed; the agent
// first writes Priiloader's "Abra" at 0x817FEFF0, which asks for its autoboot.
// Last, the stub anyway (HBC_WAY_STUB_ANYWAY).
//
// Each choice is written to the crash block's way_out and the kept log before
// it is taken. tests/way_out_host runs this file on the PC against a model
// of the IPC registers (WAY_OUT_HOST).

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
#define PRIILOADER_MAGIC ((vu32 *) 0x817feff0)

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

// A request whose reply never came stays IOS's: the next one takes another.
#define SLOTS 6
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
	if ((hbc_agent_way_out_test & WAY_TEST_NO_ACK) || !wait_ack())
		return POLL_NO_ACK;
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

// HBC through ES, as the stub does it after its closes. Returns only when
// ES refused or did not answer; when it goes ahead, IOS reloads and resets
// the PPC into HBC.
static const char *launch_hbc(u64 title) {
	static u64 tid __attribute__((aligned(32)));
	static u32 count __attribute__((aligned(32)));
	static u8 views[0xd8 * 4] __attribute__((aligned(32)));
	ipc_req *r;
	s32 fd;
	u32 t, status;

	if (hbc_agent_way_out_test & WAY_TEST_NO_LAUNCH)
		return "launch refused (test)";
	if (ios_open("/dev/es", &fd) != POLL_OK || fd < 0)
		return "ES did not open";
	tid = title;
	put_vec(0, &tid, 8);
	put_vec(1, &count, 4);
	r = ioctlv_req(fd, ES_GETVIEWCNT, 1, 1);
	if (call(r, REPLY_US) != POLL_OK || r->result < 0)
		return "no ticket view count";
	invalidate(&count, 4);
	if (count < 1 || count > 4)
		return "no ticket views";
	put_vec(0, &tid, 8);
	put_vec(1, &count, 4);
	put_vec(2, views, 0xd8 * count);
	r = ioctlv_req(fd, ES_GETVIEWS, 2, 1);
	if (call(r, REPLY_US) != POLL_OK || r->result < 0)
		return "no ticket views";
	invalidate(views, sizeof(views));
	put_vec(0, &tid, 8);
	put_vec(1, views, 0xd8);
	r = ioctlv_req(fd, ES_LAUNCH, 2, 0);
	if (!r || send_req(r) != POLL_OK)
		return "launch not acked";
	// IOS acks a launch twice when it goes ahead, and replies when it refuses.
	t = now_us();
	while (now_us() - t < LAUNCH_US) {
		status = hw_read(IPC_PPCCTRL);
		if ((status & 0x22) == 0x22) {
			udelay(100);
			irq_ack();
			bell(2);
			bell(8);
			// Going ahead: the reset into HBC comes within a second or two.
			t = now_us();
			while (now_us() - t < LAUNCH_US)
				;
			return "launch acked, but no reset came";
		}
		// A reply: the launch's is a refusal; an earlier request's is late.
		if ((status & 0x14) == 0x14 && take_reply() == phys(r))
			return "launch refused";
	}
	return "launch not answered";
}

static bool ahbprot_open(void) {
	return !(hbc_agent_way_out_test & WAY_TEST_AHBPROT) && hw_read(HW_AHBPROT) == 0xffffffff;
}

// Priiloader's magic word: "Abra" asks it for its autoboot after the reset.
static void ask_priiloader(void) {
	*PRIILOADER_MAGIC = 0x41627261;
	flush((void *) PRIILOADER_MAGIC, 4);
}

// Returns only when nothing happened (Dolphin ignores the bit).
static void hw_reset(void) {
	ask_priiloader();
	hw_write(HW_RESETS, hw_read(HW_RESETS) & ~1u);
	udelay(2000000);
}

#define STM_HOTRESET 0x2001

static void stm_reset(void) {
	static u32 in[8] __attribute__((aligned(32))), out[8] __attribute__((aligned(32)));
	ipc_req *r;
	s32 fd;

	ask_priiloader();
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
	if (send_req(r) == POLL_OK)
		udelay(3000000);
}

// The whole decision, given the stub's title, and whether a stub is there.
// Calls note() before each attempt. Returns only to fall back on the stub.
static u32 way_out_run(u64 title, bool stub, void (*note)(u32 way, const char *text)) {
	char text[160];
	u32 t0 = now_us(), answered = 0, stuck = 0, first_stuck = 0, ms;
	bool acks = true;
	s32 fd, res;

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
	if (acks && !stuck && stub) {
		snprintf(text, sizeof(text), "agent: way out: IOS closed all 32 descriptors in %u ms: "
				 "the reload stub\n", (unsigned) ms);
		note(HBC_WAY_STUB, text);
		return HBC_WAY_STUB;
	}
	if (!acks)
		snprintf(text, sizeof(text), "agent: way out: IOS stopped answering after %u closes "
				 "(%u ms)\n", (unsigned) answered, (unsigned) ms);
	else if (stuck)
		snprintf(text, sizeof(text), "agent: way out: no reply to the close of fd %u "
				 "(%u stuck, %u ms): HBC through ES\n", (unsigned) first_stuck,
				 (unsigned) stuck, (unsigned) ms);
	else
		snprintf(text, sizeof(text), "agent: way out: no reload stub: HBC through ES\n");
	if (acks) {
		note(HBC_WAY_ES_LAUNCH, text);
		snprintf(text, sizeof(text), "agent: way out: %s\n", launch_hbc(title));
	}
	note(acks ? HBC_WAY_ES_LAUNCH : ahbprot_open() && !(hbc_agent_way_out_test & WAY_TEST_NO_RESET) ?
		 HBC_WAY_RESET : HBC_WAY_STUB_ANYWAY, text);
	if (ahbprot_open() && !(hbc_agent_way_out_test & WAY_TEST_NO_RESET)) {
		note(HBC_WAY_RESET, "agent: way out: system reset (HW_RESETS)\n");
		hw_reset();
	}
	if (acks && !(hbc_agent_way_out_test & WAY_TEST_NO_RESET)) {
		note(HBC_WAY_STM_RESET, "agent: way out: STM hot reset\n");
		stm_reset();
	}
	note(HBC_WAY_STUB_ANYWAY, "agent: way out: nothing reset the Wii: the reload stub anyway\n");
	return HBC_WAY_STUB_ANYWAY;
}

#ifndef WAY_OUT_HOST
static bool stub_found(void) {
	return *(u32 *) 0x80001804 == 0x53545542 && *(u32 *) 0x80001808 == 0x48415858;
}

void agent_leave(void) {
	u64 title = *STUB_ADDR_MAGIC == STUB_MAGIC && *STUB_ADDR_TITLE ? *STUB_ADDR_TITLE : MY_TITLEID;

	if (!(agent_cfg()->no_safety & HBC_AGENT_NO_IOS_CHECK)) {
		ints_off();
		way_out_run(title, stub_found(), agent_way_out_note);
	}
	__reload();
}

// For hbc_agent_fatal(), which still exits through exit(): whether IOS opens
// /dev/es within ms, through libogc's own IPC (nothing is closed). Thread
// context with interrupts on; returns true when it cannot tell.
static volatile s32 probe_fd;
static volatile bool probe_done;

static s32 probe_cb(s32 result, void *user) {
	(void) user;
	probe_fd = result;
	probe_done = true;
	return 0;
}

bool agent_ios_answers(u32 ms) {
	static const char path[] __attribute__((aligned(32))) = "/dev/es";
	u32 t = now_us();

	if (agent_cfg()->no_safety & HBC_AGENT_NO_IOS_CHECK)
		return true;
	if (!(mfmsr() & 0x8000))
		return true;   // interrupts off (MSR[EE]): no IPC callback could come
	probe_done = false;
	if (IOS_OpenAsync(path, 0, probe_cb, NULL) < 0)
		return true;
	while (!probe_done && now_us() - t < ms * 1000)
		usleep(10 * 1000);
	if (!probe_done)
		return false;
	if (probe_fd >= 0)
		IOS_CloseAsync(probe_fd, NULL, NULL);
	return true;
}
#endif
