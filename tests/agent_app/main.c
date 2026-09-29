// Test app for sdk/hbc_agent, driven by tests/wii_agent.py.
// usage (argv from Wiiload): agent_app.dol [crash | trap | exit | stay SECONDS]
//
//   (none)       run until the agent asks it to exit (up to 300 s)
//   stay N       the same, for up to N seconds
//   crash        a store to address 0x10 after 1 s, in agent_app_crash() (DSI)
//   trap         a trap instruction after 1 s, in agent_app_trap() (program)
//   exit         return at once
//
// Output goes to the network log (hbc_netlog.h) and the TV.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ogcsys.h>
#include <fat.h>

#include "hbc_netlog.h"
#include "hbc_agent.h"

static void *xfb;

static void video_init(void) {
	GXRModeObj *rmode;

	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
				 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

// Kept out of line so the crash report's pc and backtrace name it.
void __attribute__((noinline)) agent_app_crash(volatile u32 *where) {
	*where = 0xdeadbeef;
}

void __attribute__((noinline)) agent_app_trap(void) {
	__builtin_trap();
}

int main(int argc, char **argv) {
	hbc_agent_config cfg = { 0 };
	const char *mode = argc > 1 ? argv[1] : "";
	u32 seconds = 300, frames = 0;
	bool fat;
	s32 res;

	video_init();
	fat = fatInitDefault();
	hbc_netlog_init();

	cfg.name = "agent_app";
	cfg.version = "1";
	cfg.app_polls_exit = true;
	res = hbc_agent_init(&cfg);
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

	// One "frame" per vblank, like a game's main loop.
	while (!hbc_agent_exit_requested() && frames < seconds * 60) {
		VIDEO_WaitVSync();
		if (++frames % (60 * 5) == 0)
			printf("agent_app: running, %u s\n", frames / 60);
	}

	printf("agent_app: %s\n", hbc_agent_exit_requested() ? "exit requested" : "time up");
	return 0;
}
