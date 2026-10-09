// Test app for sdk/hbc_agent, driven by tests/wii_agent.py.
// usage (argv from Wiiload): agent_app.dol [MODE [N]] [optin]
//
//   (none)       run until the agent asks it to exit (up to 300 s)
//   stay N       the same, for up to N seconds
//   crash        a store to address 0x10 after 1 s, in agent_app_crash() (DSI)
//   trap         a trap instruction after 1 s, in agent_app_trap() (program)
//   fatal        hbc_agent_fatal(0x81, ...) after 1 s, in agent_app_fatal()
//   hang         arms the hang watchdog (5 s), then spins in agent_app_hang()
//   exit         return at once
//   assert       a failed assert() after 1 s, in agent_app_assert()
//   abort        abort() after 1 s
//   overflow     recursion down the main stack, 256 bytes a level: the stack
//                guard's breakpoint stops it (a real Wii); without one
//                (Dolphin) it waits just past the markers for their check
//   deadlock     two threads take two mutexes in opposite order, main waits
//                on one of them with the hang watchdog armed
//   mallocfail   malloc(1.5 GB) (fails), then runs as with no mode
//   stubsmash    changes 32 bytes of the reload stub's area past HBC's stub
//                (0x80002f00) after 1 s, then runs as with no mode; with
//                optin the agent puts them back at exit
//   reset        after 2 s, calls the Reset callback the agent installed,
//                as the button would
//   flip N       runs as with no mode, two framebuffers, a flip every N
//                vertical blanks (60/N frames a second); N 0 never flips
//   stop         hbc_agent_stop(), then what a loader does: unmount, WPAD off,
//                IOS_ReloadIOS(58), 12 s past the hang watchdog's 5 s; writes
//                sd:/hbctest/agent_stop.txt and exits (tests/wii_agent_stop.py)
//   restart      the same, then hbc_agent_init() again and runs as with no mode
//   stopcrash    hbc_agent_stop(), then the store to 0x10: libogc's own crash
//   listen       hbc_agent_listen(false), its own server on TCP 4299 answering
//                one 16-byte request with "MINE", hbc_agent_listen(true), then
//                runs as with no mode
//   optin        (anywhere after the mode) guard_reload_stub and track_memory
//   nosafety=N   (anywhere after the mode) cfg.no_safety = N, HBC_AGENT_NO_* bits
//
// A build with -DAGENT_APP_MODE='"crash"' takes that mode when it has no
// argv, for a direct boot in Dolphin (tests/dolphin_ogc_crash.py).
// HOME (or `hbc.py key h`) opens the agent's overlay. The left bar slot is
// "Hello", and DEV > Save writes sd:/hbctest/agent_save.txt. Output goes to
// the network log (hbc_netlog.h) and the TV.

#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>
#include <fat.h>
#include <wiiuse/wpad.h>

#include <network.h>
#include <sdcard/wiisd_io.h>

#include "hbc_netlog.h"
#include "hbc_agent.h"

#if __has_include(<tuxedo/thread.h>)
extern void *__ppc_main_sp;
#define MAIN_STACK_LO ((u32) __ppc_main_sp - 0x20000)
#else
#include <ogc/lwp_threads.h>
#define MAIN_STACK_LO ((u32) _thr_main->stack)
#endif

static void *xfb, *xfb2;
static GXRModeObj *rmode;

// Colour bars under the console, so the overlay has something to dim.
static void bars(void) {
	static const u32 colors[] = { 0xb48080b4, 0xa22ca28e, 0x839c8372, 0x70487052,
								  0x54b854ae, 0x41644192, 0x23d4238a, 0x10801080 };
	u32 *fb = xfb;
	int x, y;

	for (y = rmode->xfbHeight / 2; y < rmode->xfbHeight; ++y)
		for (x = 0; x < rmode->fbWidth / 2; ++x)
			fb[y * rmode->fbWidth / 2 + x] = colors[x * 8 / (rmode->fbWidth / 2)];
}

static void video_init(void) {
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight / 2,
				 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	bars();
}

// Kept out of line so the crash report's pc and backtrace name it.
void __attribute__((noinline)) agent_app_crash(volatile u32 *where) {
	*where = 0xdeadbeef;
}

void __attribute__((noinline)) agent_app_fatal(void) {
	hbc_agent_fatal(0x81, "test fatal, value %d", 42);
}

void __attribute__((noinline)) agent_app_hang(void) {
	while (true)
		__asm__ volatile ("");  // spin: the CPU stays busy, nothing blocks
}

void __attribute__((noinline)) agent_app_trap(void) {
	__builtin_trap();
}

void __attribute__((noinline)) agent_app_assert(int argc) {
	assert(argc == 99);
}

static u32 overflow_stop;
static volatile bool overflow_deeper = true;   // never false; gcc cannot tell

// 256 bytes a level, every byte written, so the breakpoint's doubleword and
// the markers below it are hit on the way down. Stops (and waits) just past
// the markers, before libogc's own data below the stack.
void __attribute__((noinline)) agent_app_overflow(int depth) {
	volatile u8 pad[240];
	u32 sp;
	int i;

	for (i = 0; i < (int) sizeof(pad); ++i)
		pad[i] = depth;
	__asm__ volatile ("mr %0,1" : "=r" (sp));
	if (sp < overflow_stop) {
		printf("agent_app: no stop at %u levels, sp %08x; waiting\n", depth, (unsigned) sp);
		while (true)
			sleep(1);
	}
	if (overflow_deeper)
		agent_app_overflow(depth + 1);
	pad[0] = 0;
}

static mutex_t lock_a, lock_b;

static void *deadlock_thread(void *arg) {
	(void) arg;
	LWP_MutexLock(lock_b);
	usleep(200 * 1000);
	LWP_MutexLock(lock_a);   // main holds a, waits for b
	return NULL;
}

static bool save(void *user) {
	FILE *f;

	(void) user;
	mkdir("sd:/hbctest", 0777);
	f = fopen("sd:/hbctest/agent_save.txt", "w");
	if (!f)
		return false;
	fprintf(f, "saved by agent_app\n");
	printf("agent_app: saved\n");
	return fclose(f) == 0;
}

static void hello(void *user) {
	(void) user;
	printf("agent_app: hello from the app slot\n");
}

#ifndef AGENT_APP_MODE
#define AGENT_APP_MODE ""
#endif

// hbc_agent_stop() as a loader uses it. Returns true when the app should
// exit (stop), false to start the agent again (restart).
static bool stop_like_a_loader(const char *mode) {
	char net[160];
	int n = 0, i;
	s32 first, second;
	FILE *f;

	hbc_agent_alive();   // armed: without the stop, a hang after 5 s
	first = hbc_agent_stop();
	second = hbc_agent_stop();
	printf("agent_app: stop %d, again %d\n", (int) first, (int) second);
	if (!strcmp(mode, "stopcrash")) {
		sleep(1);
		agent_app_crash((volatile u32 *) 0x10);
	}
	// What a loader shuts down itself before IOS goes: each keeps a handle
	// to the old IOS otherwise.
	fatUnmount("sd:");
	__io_wiisd.shutdown();
	net_deinit();
	WPAD_Shutdown();
	i = IOS_ReloadIOS(58);
	// Past the watchdog's limit, watching the network: the agent must not
	// start it again on the new IOS.
	n += snprintf(net + n, sizeof(net) - n, "reload %d, net", i);
	for (i = 0; i < 12; ++i) {
		sleep(1);
		n += snprintf(net + n, sizeof(net) - n, " %d", (int) net_get_status());
	}
	if (fatInitDefault()) {
		mkdir("sd:/hbctest", 0777);
		f = fopen("sd:/hbctest/agent_stop.txt", "w");
		if (f) {
			hbc_agent_alive();
			hbc_agent_hold(true);
			hbc_agent_hold(false);
			fprintf(f, "stop %d\nagain %d\n%s\n", (int) first, (int) second, net);
			fprintf(f, "exit_requested %d\nhome_pending %d\nhome %d\nlisten %d\n",
					hbc_agent_exit_requested(), hbc_agent_home_pending(),
					(int) hbc_agent_home(rmode), (int) hbc_agent_listen(false));
			fclose(f);
		}
	}
	if (!strcmp(mode, "stop")) {
		fatUnmount("sd:");
		return true;
	}
	WPAD_Init();
	return false;
}

// hbc_agent_listen(false): the Wiiload port is the app's, then the agent's again.
static void own_server(void) {
	struct sockaddr_in sa;
	u32 len = sizeof(sa), got = 0;
	s32 ls, s, res;
	u8 req[16];

	sleep(5);   // the test sees the agent first
	res = hbc_agent_listen(false);
	ls = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_len = sizeof(sa);
	sa.sin_port = htons(4299);
	sa.sin_addr.s_addr = INADDR_ANY;
	printf("agent_app: listen(false) %d, bind %d\n", (int) res,
		   (int) net_bind(ls, (struct sockaddr *) &sa, sizeof(sa)));
	net_listen(ls, 1);
	s = net_accept(ls, (struct sockaddr *) &sa, &len);
	if (s >= 0) {
		while (got < sizeof(req) && (res = net_recv(s, req + got, sizeof(req) - got, 0)) > 0)
			got += res;
		net_send(s, "MINE", 4, 0);
		net_close(s);
	}
	net_close(ls);
	printf("agent_app: own server answered (%d), listen(true) %d\n", (int) s,
		   (int) hbc_agent_listen(true));
}

int main(int argc, char **argv) {
	hbc_agent_config cfg = { 0 };
	const char *mode = argc > 1 ? argv[1] : AGENT_APP_MODE;
	u32 seconds = 300, frames = 0, flip_every = 0;
	bool fat;
	s32 res;

	video_init();
	WPAD_Init();
	fat = fatInitDefault();
	hbc_netlog_init();

	cfg.name = "agent_app";
	cfg.hang_s = 5;  // the test's hang; apps leave it at the default 60
	cfg.version = "1";
	cfg.app_polls_exit = true;
	cfg.on_save = save;
	for (int i = 2; i < argc; ++i)
		if (!strcmp(argv[i], "optin"))
			cfg.guard_reload_stub = cfg.track_memory = true;
		else if (!strncmp(argv[i], "nosafety=", 9))
			cfg.no_safety = strtoul(argv[i] + 9, NULL, 0);
	{
		// What starting the agent costs: heap, and MEM1/MEM2 arena.
		struct mallinfo m0 = mallinfo();
		u32 a1 = SYS_GetArena1Size(), a2 = SYS_GetArena2Size();

		res = hbc_agent_init(&cfg);
		struct mallinfo m1 = mallinfo();
		printf("agent_app: init cost heap %d bytes (in use), arena1 %d, arena2 %d\n",
			   m1.uordblks - m0.uordblks, (int) (a1 - SYS_GetArena1Size()),
			   (int) (a2 - SYS_GetArena2Size()));
	}
	hbc_agent_set_slot(0, "Hello", hello, NULL);
	printf("agent_app: agent %d, fat %d, mode '%s'\n", res, fat, mode);

	if (!strcmp(mode, "exit"))
		return 0;
	// What HBC gave the app from its meta.xml (tests/dolphin_meta.py): the
	// arguments, and the IOS and AHBPROT that ahb_access and no_ios_reload set.
	if (!strcmp(mode, "meta")) {
		printf("agent_app: argc %d\n", argc);
		for (int i = 0; i < argc; ++i)
			printf("agent_app: argv[%d] '%s'\n", i, argv[i]);
		printf("agent_app: IOS %d v%d, AHBPROT %s, DVD %s\n", IOS_GetVersion(),
			   IOS_GetRevision(), read32(0x0d800064) == 0xffffffff ? "open" : "closed",
			   read32(0x0d800180) & (1 << 21) ? "off" : "on");
		return 0;
	}
	if (!strcmp(mode, "stay") && argc > 2)
		seconds = atoi(argv[2]);

	res = hbc_agent_net_wait(10000);
	printf("agent_app: network %d\n", res);

	if (!strcmp(mode, "stop") || !strcmp(mode, "restart") || !strcmp(mode, "stopcrash")) {
		if (stop_like_a_loader(mode))
			return 0;
		res = hbc_agent_init(&cfg);
		printf("agent_app: init again %d\n", (int) res);
	}
	if (!strcmp(mode, "listen"))
		own_server();

	if (!strcmp(mode, "fatal")) {
		sleep(1);
		printf("agent_app: stopping (fatal)\n");
		agent_app_fatal();
	}
	if (!strcmp(mode, "hang")) {
		hbc_agent_alive();
		sleep(1);
		printf("agent_app: hanging\n");
		agent_app_hang();
	}

	if (!strcmp(mode, "assert") || !strcmp(mode, "abort")) {
		sleep(1);
		printf("agent_app: stopping (%s)\n", mode);
		if (!strcmp(mode, "abort"))
			abort();
		agent_app_assert(argc);
	}
	if (!strcmp(mode, "overflow")) {
		sleep(1);
		overflow_stop = MAIN_STACK_LO + 600;
		printf("agent_app: overflowing the main stack (bottom %08x)\n", (unsigned) MAIN_STACK_LO);
		agent_app_overflow(0);
	}
	if (!strcmp(mode, "deadlock")) {
		lwp_t t;

		LWP_MutexInit(&lock_a, false);
		LWP_MutexInit(&lock_b, false);
		hbc_agent_alive();
		LWP_MutexLock(lock_a);
		LWP_CreateThread(&t, deadlock_thread, NULL, NULL, 16 * 1024, 50);
		usleep(100 * 1000);
		printf("agent_app: deadlocking\n");
		LWP_MutexLock(lock_b);
	}
	if (!strcmp(mode, "mallocfail")) {
		void *p = malloc(1536u << 20);

		printf("agent_app: malloc(1.5 GB) gave %p\n", p);
	}
	if (!strcmp(mode, "stubsmash")) {
		sleep(1);
		memset((void *) 0x80002f00, 0x5a, 32);
		DCFlushRange((void *) 0x80002f00, 32);
		printf("agent_app: changed 32 bytes at 0x80002f00\n");
	}
	if (!strcmp(mode, "reset")) {
		resetcallback cb = SYS_SetResetCallback(NULL);

		SYS_SetResetCallback(cb);
		sleep(2);
		printf("agent_app: pressing Reset\n");
		((void (*)(u32, void *)) cb)(0, NULL);
	}
	if (!strcmp(mode, "flip") && argc > 2) {
		flip_every = atoi(argv[2]);
		xfb2 = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
		memcpy(xfb2, xfb, rmode->fbWidth * rmode->xfbHeight * 2);
	}

	if (!strcmp(mode, "crash") || !strcmp(mode, "trap")) {
		sleep(1);
		printf("agent_app: crashing (%s)\n", mode);
		if (!strcmp(mode, "crash"))
			agent_app_crash((volatile u32 *) 0x10);
		agent_app_trap();
	}

	// One frame per vblank, like a game's main loop.
	while (!hbc_agent_exit_requested() && frames < seconds * 60) {
		u32 down = 0;
		int chan;

		WPAD_ScanPads();
		for (chan = 0; chan < 4; ++chan)
			down |= WPAD_ButtonsDown(chan);
		if ((down & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) || hbc_agent_home_pending()) {
			printf("agent_app: overlay\n");
			res = hbc_agent_home(rmode);
			printf("agent_app: overlay closed (%d)\n", res);
		}
		VIDEO_WaitVSync();
		if (flip_every && frames % flip_every == 0) {
			VIDEO_SetNextFramebuffer(frames / flip_every & 1 ? xfb2 : xfb);
			VIDEO_Flush();
		}
		if (++frames % (60 * 5) == 0)
			printf("agent_app: running, %u s\n", frames / 60);
	}

	printf("agent_app: %s\n", hbc_agent_exit_requested() ? "exit requested" : "time up");
	return 0;
}
