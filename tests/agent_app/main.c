// Test app for sdk/hbc_agent, driven by tests/wii_agent.py.
// usage (argv from Wiiload): agent_app.dol [crash | trap | exit | stay SECONDS]
//
//   (none)       run until the agent asks it to exit (up to 300 s)
//   stay N       the same, for up to N seconds
//   crash        a store to address 0x10 after 1 s, in agent_app_crash() (DSI)
//   trap         a trap instruction after 1 s, in agent_app_trap() (program)
//   exit         return at once
//
// HOME (or `hbc.py key h`) opens the agent's overlay. The left bar slot is
// "Hello", and DEV > Save writes sd:/hbctest/agent_save.txt. Output goes to
// the network log (hbc_netlog.h) and the TV.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ogcsys.h>
#include <fat.h>
#include <wiiuse/wpad.h>

#include "hbc_netlog.h"
#include "hbc_agent.h"

static void *xfb;
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

void __attribute__((noinline)) agent_app_trap(void) {
	__builtin_trap();
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

int main(int argc, char **argv) {
	hbc_agent_config cfg = { 0 };
	const char *mode = argc > 1 ? argv[1] : "";
	u32 seconds = 300, frames = 0;
	bool fat;
	s32 res;

	video_init();
	WPAD_Init();
	fat = fatInitDefault();
	hbc_netlog_init();

	cfg.name = "agent_app";
	cfg.version = "1";
	cfg.app_polls_exit = true;
	cfg.on_save = save;
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
	if (!strcmp(mode, "stay") && argc > 2)
		seconds = atoi(argv[2]);

	res = hbc_agent_net_wait(10000);
	printf("agent_app: network %d\n", res);

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
		if (++frames % (60 * 5) == 0)
			printf("agent_app: running, %u s\n", frames / 60);
	}

	printf("agent_app: %s\n", hbc_agent_exit_requested() ? "exit requested" : "time up");
	return 0;
}
