/* Host test of the crash screen's logic: channel/channelapp/source/panic_core.h
 * and the stub's channel/channelapp/stub/ipc_wait.h. Run by tests/test_panic_host.py. */
#include <stdio.h>
#include <string.h>
#include <gctypes.h>
#include "../../sdk/hbc_agent/hbc_agent.h"
#include "../../channel/channelapp/source/panic_core.h"
#include "../../channel/channelapp/stub/ipc_wait.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* a fake RAM: the walk reads addresses in MEM1 from 0x80100000 */
static u32 ram[0x1000];
static u32 reads_outside;
static u32 rd(u32 a) {
	if (a >= 0x80100000 && a < 0x80100000 + sizeof ram && !(a & 3))
		return ram[(a - 0x80100000) / 4];
	reads_outside++;
	return 0;
}
#define A(off) (0x80100000u + (off))
#define W(off, v) (ram[(off) / 4] = (v))

static void test_ram(void) {
	CHECK(panic_ram(0x80000000, 4));
	CHECK(panic_ram(0x817FFFFC, 4));
	CHECK(!panic_ram(0x817FFFFD, 4));
	CHECK(!panic_ram(0x81800000, 4));
	CHECK(!panic_ram(0x7FFFFFFC, 4));
	CHECK(!panic_ram(0x0, 4));
	CHECK(!panic_ram(0xFFFFFFFC, 8));   /* wraps */
	CHECK(panic_ram(0x90000000, 8));
	CHECK(panic_ram(0x93FFFFF8, 8));
	CHECK(!panic_ram(0x93FFFFFC, 8));
	CHECK(!panic_ram(0x94000000, 4));
	CHECK(!panic_ram(0x81800000, 0x10000000));
}

static void test_walk(void) {
	u32 out[8];
	u32 n;

	memset(ram, 0, sizeof ram);
	/* sp 0x100 -> 0x200 -> 0x300 -> 0 (end) */
	W(0x100, A(0x200)); W(0x200 + 4, 0x1111);
	W(0x200, A(0x300)); W(0x300 + 4, 0x2222);
	W(0x300, 0);
	reads_outside = 0;
	n = panic_backtrace(A(0x100), out, 8, rd);
	CHECK(n == 2 && out[0] == 0x1111 && out[1] == 0x2222);
	CHECK(reads_outside == 0);
	n = panic_backtrace(A(0x100), out, 1, rd);
	CHECK(n == 1 && out[0] == 0x1111);
	n = panic_backtrace(A(0x100), out, 0, rd);
	CHECK(n == 0);
	/* misaligned sp */
	n = panic_backtrace(A(0x102), out, 8, rd);
	CHECK(n == 0);
	/* misaligned chain */
	W(0x300, A(0x302));
	n = panic_backtrace(A(0x100), out, 8, rd);
	CHECK(n == 2);
	/* not monotonic: a loop back to a lower address */
	W(0x300, A(0x100));
	n = panic_backtrace(A(0x100), out, 8, rd);
	CHECK(n == 2);
	/* a chain pointing outside RAM */
	W(0x200, 0x12345678);
	n = panic_backtrace(A(0x100), out, 8, rd);
	CHECK(n == 1);
	/* sp outside RAM reads nothing */
	reads_outside = 0;
	n = panic_backtrace(0x1000, out, 8, rd);
	CHECK(n == 0 && reads_outside == 0);
	n = panic_backtrace(0x81800000, out, 8, rd);
	CHECK(n == 0 && reads_outside == 0);
	/* the 8 bytes at a chain must lie in RAM: 0x817FFFFC leaves only 4 */
	W(0x100, 0x817FFFFC);
	reads_outside = 0;
	n = panic_backtrace(A(0x100), out, 8, rd);
	CHECK(n == 0 && reads_outside == 0);
}

static void test_text(void) {
	char b[32], *d;

	d = panic_hex(b, 0xDEADBEEF); *d = 0;
	CHECK(!strcmp(b, "DEADBEEF"));
	d = panic_hex(b, 0); *d = 0;
	CHECK(!strcmp(b, "00000000"));
	d = panic_dec(b, 0); *d = 0;
	CHECK(!strcmp(b, "0"));
	d = panic_dec(b, 4294967295u); *d = 0;
	CHECK(!strcmp(b, "4294967295"));
	d = panic_str(panic_str(b, "ab"), "cd"); *d = 0;
	CHECK(!strcmp(b, "abcd"));
}

static void test_decode(void) {
	char c[PANIC_CAUSE_MAX];

	panic_decode(PANIC_EXC_DSI, 0, 0x40000000, c);
	CHECK(!strcmp(c, "load, no translation (bad address)"));
	panic_decode(PANIC_EXC_DSI, 0, 0x42000000, c);
	CHECK(!strcmp(c, "store, no translation (bad address)"));
	/* a store to address 0xA on a real Wii */
	panic_decode(PANIC_EXC_DSI, 0, 0x06000000, c);
	CHECK(!strcmp(c, "store, direct-store segment (bad address)"));
	panic_decode(PANIC_EXC_DSI, 0, 0x0A000000, c);
	CHECK(!strcmp(c, "store, protection"));
	panic_decode(PANIC_EXC_ISI, 0x40000000, 0, c);
	CHECK(!strcmp(c, "instruction fetch, no translation (bad address)"));
	panic_decode(PANIC_EXC_PROGRAM, 0x00020000, 0, c);
	CHECK(!strcmp(c, "trap"));
	panic_decode(PANIC_EXC_PROGRAM, 0x00080000, 0, c);
	CHECK(!strcmp(c, "illegal instruction"));
	panic_decode(PANIC_EXC_PROGRAM, 0x00100000, 0, c);
	CHECK(!strcmp(c, "floating point exception"));
	panic_decode(PANIC_EXC_ALIGN, 0, 0, c);
	CHECK(strstr(c, "misaligned"));
	panic_decode(1, 0xffffffff, 0xffffffff, c);
	CHECK(c[0] == 0);
	/* the longest causes stay inside PANIC_CAUSE_MAX */
	panic_decode(PANIC_EXC_DSI, 0, 0xffffffff, c);
	CHECK(strlen(c) < PANIC_CAUSE_MAX);
	panic_decode(PANIC_EXC_ISI, 0xffffffff, 0, c);
	CHECK(strlen(c) < PANIC_CAUSE_MAX);
	panic_decode(PANIC_EXC_PROGRAM, 0xffffffff, 0, c);
	CHECK(strlen(c) < PANIC_CAUSE_MAX);
}

static void test_loop(void) {
	hbc_crash_block b;
	u32 frames[1] = { 0x80001234 };
	panic_info in = { 3, 0x80100000, 0x9032, 0x80200000, 0, 0, 0x80300000, 0xA, 0x06000000, 40,
	                  0x81757518, "HBC 1.15.0", frames, 1 };

	/* nothing recorded before: reload HBC */
	CHECK(!panic_crash_loop(NULL, 40));
	/* HBC crashed at its start twice running: the System Menu */
	panic_fill_block(&b, &in, "store");
	CHECK(panic_block_valid(&b));
	CHECK(panic_crash_loop(&b, 40));
	CHECK(panic_crash_loop(&b, PANIC_LOOP_MS - 1));
	/* this crash came after HBC ran a while: reload */
	CHECK(!panic_crash_loop(&b, PANIC_LOOP_MS));
	/* the last one did: reload */
	in.uptime_ms = 8 * 60 * 1000;
	panic_fill_block(&b, &in, "store");
	CHECK(!panic_crash_loop(&b, 40));
	/* the last crash was an app's, not HBC's: reload */
	in.uptime_ms = 40;
	in.app = "agent_app";
	panic_fill_block(&b, &in, "store");
	CHECK(!panic_crash_loop(&b, 40));
	/* an app's fatal or hang, not an exception: reload */
	in.app = "HBC 1.15.0";
	panic_fill_block(&b, &in, "store");
	b.kind = HBC_CRASH_EXCEPTION + 1;
	CHECK(!panic_crash_loop(&b, 40));
	/* a stale or damaged block is not valid */
	panic_fill_block(&b, &in, "store");
	b.pc ^= 4;
	CHECK(!panic_block_valid(&b));
}

static void test_block(void) {
	hbc_crash_block b;
	u32 frames[3] = { 0x80001234, 0x80002345, 0x80003456 };
	panic_info in = { 3, 0x80100000, 0x9032, 0x80200000, 0x24000000, 0x5, 0x80300000, 0x10, 0x40000000, 12345,
	                  0xDEAD1234, "HBC 1.14.32", frames, 3 };

	memset(&b, 0xAA, sizeof b);
	panic_fill_block(&b, &in, "store, no translation (bad address)");
	/* devnet_early_init's validity test */
	CHECK(b.magic == HBC_CRASH_MAGIC && b.version == HBC_CRASH_VERSION && b.check == hbc_crash_check(&b));
	CHECK(b.kind == HBC_CRASH_EXCEPTION && b.exception == 3);
	CHECK(b.frames[0] == 0x80001234 && b.frames[2] == 0x80003456 && b.frames[3] == 0);
	CHECK(!strcmp(b.app, "HBC 1.14.32"));
	CHECK(!strcmp(b.reason, "thread DEAD1234: store, no translation (bad address)"));
	CHECK(b.way_magic == 0 && b.way_out == 0);
	CHECK(b.uptime_ms == 12345 && b.dar == 0x10 && b.dsisr == 0x40000000);
	/* a long app name and cause are cut, and stay terminated */
	in.app = "an application name much too long";
	{
		char big[200];
		memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
		panic_fill_block(&b, &in, big);
		CHECK(strlen(b.app) == 19 && strlen(b.reason) < HBC_CRASH_REASON);
		CHECK(b.check == hbc_crash_check(&b));
	}
	/* more frames than the block holds */
	{
		u32 many[30];
		int i;
		for (i = 0; i < 30; i++) many[i] = 0x80000000 + i;
		in.frames = many; in.nframes = 30; in.app = "HBC";
		panic_fill_block(&b, &in, "");
		CHECK(b.frames[HBC_CRASH_FRAMES - 1] == 0x80000000 + HBC_CRASH_FRAMES - 1);
		CHECK(!strcmp(b.reason, "thread DEAD1234"));
	}
}

static void test_ipc(void) {
	u32 t;
	int r;

	CHECK(ipc_wait_step(0x22, IPC_ACK, 0, 100) == IPC_WAIT_DONE);
	CHECK(ipc_wait_step(0x14, IPC_REPLY, 0, 100) == IPC_WAIT_DONE);
	CHECK(ipc_wait_step(0x00, IPC_ACK, 0, 100) == IPC_WAIT_AGAIN);
	CHECK(ipc_wait_step(0x00, IPC_ACK, 100, 100) == IPC_WAIT_AGAIN);
	CHECK(ipc_wait_step(0x00, IPC_ACK, 101, 100) == IPC_WAIT_TIMEOUT);
	CHECK(ipc_wait_step(0x14, IPC_ACK, 0, 100) == IPC_WAIT_TAKE_REPLY);
	CHECK(ipc_wait_step(0x34, IPC_ACK, 0, 100) == IPC_WAIT_TAKE_REPLY);
	CHECK(ipc_wait_step(0x22, IPC_REPLY, 0, 100) == IPC_WAIT_AGAIN);   /* an ack is not a reply */
	CHECK(ipc_wait_step(0x22, IPC_REPLY, 101, 100) == IPC_WAIT_TIMEOUT);
	CHECK(ipc_wait_step(0x00, IPC_REPLY, 0, 0) == IPC_WAIT_AGAIN);
	CHECK(ipc_wait_step(0x36, IPC_ACK, 0, 100) == IPC_WAIT_DONE);

	/* IOS never answers: the loop ends at the limit, across a time base wrap */
	r = 0;
	for (t = 0xFFFFFF00u; ; t++) {
		u32 elapsed = t - 0xFFFFFF00u;
		int s = ipc_wait_step(0, IPC_ACK, elapsed, 1000);
		if (s == IPC_WAIT_TIMEOUT) break;
		CHECK(s == IPC_WAIT_AGAIN);
		if (++r > 100000) { CHECK(0 && "wait never ended"); break; }
	}
	CHECK(r == 1001);
	/* a late reply is taken, then the ack comes */
	CHECK(ipc_wait_step(0x14, IPC_ACK, 5, 1000) == IPC_WAIT_TAKE_REPLY);
	CHECK(ipc_wait_step(0x22, IPC_ACK, 6, 1000) == IPC_WAIT_DONE);
}

int main(void) {
	test_ram();
	test_walk();
	test_text();
	test_decode();
	test_block();
	test_loop();
	test_ipc();
	if (fails) {
		printf("%d failures\n", fails);
		return 1;
	}
	printf("all panic scenarios passed\n");
	return 0;
}
