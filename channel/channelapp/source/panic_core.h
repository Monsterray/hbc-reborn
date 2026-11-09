// The parts of the crash screen (panic.c) that are plain logic: where a stack
// walk may read, the backtrace, what a fault's registers say, the crash block
// HBC leaves for `hbc.py crash`. No libogc, no heap, no newlib: the crash screen
// runs inside an exception, where a held lock or a corrupt heap must not matter.
// tests/host/panic_host/test.c runs this file on the PC.
//
// Include after <gctypes.h> and sdk/hbc_agent/hbc_agent.h.

#ifndef PANIC_CORE_H
#define PANIC_CORE_H

#define PANIC_CAUSE_MAX 128
#define PANIC_MEM1_END 0x81800000u
#define PANIC_MEM2_START 0x90000000u
#define PANIC_MEM2_END 0x94000000u

// May the n bytes at p be read as stack or code, without faulting? MEM1 and
// MEM2 (the heap, where a thread's stack may be) are mapped; the whole read
// must end inside the same one.
static inline int panic_ram(u32 p, u32 n) {
	u32 end = p + n;

	if (end < p)
		return 0;
	return (p >= 0x80000000u && end <= PANIC_MEM1_END) ||
		   (p >= PANIC_MEM2_START && end <= PANIC_MEM2_END);
}

typedef u32 (*panic_read_fn)(u32 addr);

// The return addresses up the back chain from sp, as the agent records them
// (sdk/hbc_agent/agent.c): each frame's back chain at 0(sp), the saved LR of
// the frame it called at 4(back chain). LR itself is not in the chain: the
// caller prints it first (4(r1) of the faulting function is its callee's
// stale slot). A back chain must be aligned, above the frame it came from,
// and the 8 bytes read at it must lie in RAM. Returns the count.
static inline u32 panic_backtrace(u32 sp, u32 *out, u32 max, panic_read_fn rd) {
	u32 n = 0;

	while (n < max && !(sp & 3) && panic_ram(sp, 4)) {
		u32 next = rd(sp);

		if ((next & 3) || next <= sp || !panic_ram(next, 8))
			break;
		out[n++] = rd(next + 4);
		sp = next;
	}
	return n;
}

// ---- Text without printf -----------------------------------------------------

static inline char *panic_str(char *d, const char *s) {
	while (*s)
		*d++ = *s++;
	return d;
}

static inline char *panic_hex(char *d, u32 v) {
	int i;

	for (i = 0; i < 8; i++)
		*d++ = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
	return d;
}

static inline char *panic_dec(char *d, u32 v) {
	char t[10];
	int n = 0;

	do {
		t[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		*d++ = t[--n];
	return d;
}

// ---- What the registers say ----------------------------------------------------

// tuxedo/ppc/exception.h's PPC_EXCPT_DSI, _ISI, _ALIGN and _UNDEF (the Program exception)
#define PANIC_EXC_DSI 3
#define PANIC_EXC_ISI 4
#define PANIC_EXC_ALIGN 6
#define PANIC_EXC_PROGRAM 7

// A short cause for the fault, from SRR1 (the context's msr) and DSISR, into
// out (PANIC_CAUSE_MAX bytes). Empty when there is nothing to add. PowerPC bit
// numbers count from the most significant bit (750CL manual): DSISR bit 1
// no translation, 4 protection, 5 direct-store segment, 6 store, 9 DABR
// match. A real Wii's segment registers mark 0-0x7FFFFFFF direct-store, so a
// stray pointer below 0x80000000 gives bit 5 (DSISR 06000000 for a store to
// address 0xA on the bench Wii), where Dolphin gives bit 1. SRR1 for ISI:
// 1 no translation, 3 no-execute or guarded, 4 protection; SRR1 for Program:
// 11 FP, 12 illegal, 13 privileged, 14 trap.
static inline void panic_decode(u32 exid, u32 srr1, u32 dsisr, char *out) {
	char *d = out;
	const char *sep = "";

#define ADD(s) do { d = panic_str(d, sep); d = panic_str(d, s); sep = ", "; } while (0)
	if (exid == PANIC_EXC_DSI) {
		ADD(dsisr & 0x02000000 ? "store" : "load");
		if (dsisr & 0x40000000)
			ADD("no translation (bad address)");
		if (dsisr & 0x08000000)
			ADD("protection");
		if (dsisr & 0x04000000)
			ADD("direct-store segment (bad address)");
		if (dsisr & 0x00400000)
			ADD("DABR match");
	} else if (exid == PANIC_EXC_ISI) {
		ADD("instruction fetch");
		if (srr1 & 0x40000000)
			ADD("no translation (bad address)");
		if (srr1 & 0x10000000)
			ADD("no-execute or guarded");
		if (srr1 & 0x08000000)
			ADD("protection");
	} else if (exid == PANIC_EXC_PROGRAM) {
		if (srr1 & 0x00100000)
			ADD("floating point exception");
		if (srr1 & 0x00080000)
			ADD("illegal instruction");
		if (srr1 & 0x00040000)
			ADD("privileged instruction");
		if (srr1 & 0x00020000)
			ADD("trap");
	} else if (exid == PANIC_EXC_ALIGN) {
		ADD("misaligned access (see DAR)");
	}
#undef ADD
	*d = 0;
}

// ---- The crash block ---------------------------------------------------------------

typedef struct {
	u32 exid, pc, msr, lr, cr, ctr, sp, dar, dsisr, uptime_ms;
	u32 thread;
	const char *app;   // "HBC 1.14.32"
	const u32 *frames; // the back-chain walk
	u32 nframes;
} panic_info;

// Fills the block `hbc.py crash` reads after the reload, as agent.c does for
// an app (agent_record_kind) and devnet_early_init() takes it: version 2, kind
// exception, the check over its first 47 words. The way-out words stay zero:
// "not_checked", the reload stub.
static inline void panic_fill_block(hbc_crash_block *b, const panic_info *in, const char *cause) {
	u32 i, *w = (u32 *) b;
	char *d;

	for (i = 0; i < sizeof(*b) / 4; i++)
		w[i] = 0;
	b->magic = HBC_CRASH_MAGIC;
	b->version = HBC_CRASH_VERSION;
	b->exception = in->exid;
	b->pc = in->pc;
	b->msr = in->msr;
	b->lr = in->lr;
	b->cr = in->cr;
	b->ctr = in->ctr;
	b->dar = in->dar;
	b->dsisr = in->dsisr;
	b->sp = in->sp;
	b->uptime_ms = in->uptime_ms;
	for (i = 0; i < in->nframes && i < HBC_CRASH_FRAMES; i++)
		b->frames[i] = in->frames[i];
	for (i = 0; in->app[i] && i < sizeof(b->app) - 1; i++)
		b->app[i] = in->app[i];
	b->kind = HBC_CRASH_EXCEPTION;
	// The thread, and the cause; the app's own name field is the HBC version.
	d = b->reason;
	d = panic_str(d, "thread ");
	d = panic_hex(d, in->thread);
	if (cause && cause[0]) {
		d = panic_str(d, ": ");
		for (i = 0; cause[i] && d < b->reason + HBC_CRASH_REASON - 1; i++)
			*d++ = cause[i];
	}
	*d = 0;
	b->check = hbc_crash_check(b);
}

// devnet_early_init's test for a block worth keeping.
static inline int panic_block_valid(const hbc_crash_block *b) {
	return b->magic == HBC_CRASH_MAGIC && b->version == HBC_CRASH_VERSION &&
		b->check == hbc_crash_check(b);
}

// ---- A crash on every start ----------------------------------------------------

// The stub starts the installed channel again, so an HBC that crashes on every
// start would reload for good, on a black screen when it crashes before its
// video is up. A crash this soon after HBC started, when the record before it
// (prev: the MEM2 block, or the copy HBC took of it at start) is HBC's own
// crash this soon after its start, leaves for the System Menu instead.
#define PANIC_LOOP_MS 30000

static inline int panic_crash_loop(const hbc_crash_block *prev, u32 uptime_ms) {
	return prev && uptime_ms < PANIC_LOOP_MS && prev->kind == HBC_CRASH_EXCEPTION &&
		prev->uptime_ms < PANIC_LOOP_MS && prev->app[0] == 'H' && prev->app[1] == 'B' &&
		prev->app[2] == 'C' && prev->app[3] == ' ';
}

#endif
