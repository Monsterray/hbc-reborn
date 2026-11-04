// Test app for sdk/slotled on a Wii (tests/wii_slotled.py drives it).
//
// usage (argv from Wiiload):
//   slotled_app.dol check        read-only: which GPIO bank reaches the pin,
//                                the registers, the saved Slot illumination
//                                (IPL.IDL's raw bytes), what the timer costs
//                                (a busy loop's count with the light off,
//                                then pulsing), and that the light is left
//                                as it was found
//   slotled_app.dol demo         for a person at the Wii: each direct effect
//                                for a few seconds, named on the TV
//   slotled_app.dol ios [FIRST [LAST]]
//                                for a person at the Wii: IOS's levels, then
//                                its flash patterns FIRST..LAST (0..29), 5 s
//                                each
//   slotled_app.dol save N       writes SYSCONF: Slot illumination N (0..2),
//                                read back. Only when asked to.
//
// Every line goes to the TV and to sd:/slotled_test/result.txt.

#include <gccore.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "slotled.h"

extern void __exception_setreload(int t);

static FILE *report;

static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (report) {
		va_start(ap, fmt);
		vfprintf(report, fmt, ap);
		va_end(ap);
		fflush(report);
	}
}

static void hold(u32 ms)
{
	usleep(ms * 1000);
}

// A busy loop's count over 500 ms: less with the timer's interrupts.
static u32 spin(void)
{
	u64 end = gettime() + millisecs_to_ticks(500);
	volatile u32 n = 0;

	while (gettime() < end)
		n++;
	return n;
}

static void check(void)
{
	static const u32 banks[] = { 0xCD0000C0, 0xCD8000C0 };
	u8 idl[3] = { 0, 0, 0 };
	u32 idle, busy, before;
	int i, r;

	for (i = 0; i < 2; i++) {
		volatile u32 *b = (volatile u32 *)banks[i];

		say("GPIOB at %08x: OUT %08x DIR %08x IN %08x\n", banks[i], b[0], b[1], b[2]);
	}
	before = *(volatile u32 *)0xCD0000C0 & 0x20;
	r = slotled_init();
	say("slotled_init: %d\n", r);
	CONF_Init();
	r = CONF_Get("IPL.IDL", idl, sizeof idl);
	say("IPL.IDL: %d bytes: %02x %02x %02x\n", r, idl[0], idl[1], idl[2]);
	say("slotled_saved_level: %d\n", slotled_saved_level());
	if (slotled_get() < 0)
		return;
	idle = spin();
	say("slotled_pulse: %d\n", slotled_pulse(0, 255, 1000, 0));
	busy = spin();
	say("busy loop: %u off, %u pulsing (%u.%u%% to the timer)\n", idle, busy,
	    idle > busy ? (idle - busy) * 100 / idle : 0,
	    idle > busy ? (idle - busy) * 1000 / idle % 10 : 0);
	say("slotled_set(128): %d, then 255, 0\n", slotled_set(128));
	hold(300);
	say("slotled_get: %d\n", slotled_get());
	slotled_set(255);
	hold(50);
	say("at 255: OUT %08x, busy %d\n", *(volatile u32 *)0xCD0000C0, slotled_busy());
	slotled_set(0);
	hold(50);
	say("at 0: OUT %08x, busy %d\n", *(volatile u32 *)0xCD0000C0, slotled_busy());
	say("slotled_blink 3x: %d\n", slotled_blink(255, 100, 100, 3));
	r = (int)ticks_to_millisecs(gettime());
	slotled_wait();
	say("slotled_wait: %d ms (600 expected)\n", (int)ticks_to_millisecs(gettime()) - r);
	slotled_shutdown();
	say("pin %s at the start, %s now\n", before ? "on" : "off",
	    (*(volatile u32 *)0xCD0000C0 & 0x20) ? "on" : "off");
}

// Slot illumination as the file on the NAND has it now (libogc's CONF_Get
// keeps the copy it read at the start).
static int sysconf_now(void)
{
	static u8 sc[0x4000] ATTRIBUTE_ALIGN(32);
	static const char path[] ATTRIBUTE_ALIGN(32) = "/shared2/sys/SYSCONF";
	s32 fd = IOS_Open(path, 1), r;

	if (fd < 0)
		return fd;
	r = IOS_Read(fd, sc, sizeof sc);
	IOS_Close(fd);
	return r == (s32)sizeof sc ? slotled_sysconf_illum(sc, sizeof sc, -1) : -1;
}

static void demo(void)
{
	static const slotled_step wave[] = {
		{ 255, 1, 300 }, { 40, 1, 300 }, { 180, 1, 200 }, { 0, 1, 400 }, { 0, 0, 300 }
	};
	static const slotled_effect custom = { wave, 5, 0 };
	static const int levels[] = { 8, 32, 96, 255 };
	int r = slotled_init();
	unsigned i;

	say("slotled_init: %d\n", r);
	if (r < 0)
		return;
	say("1. steady levels: 8, 32, 96, 255 (2 s each)\n");
	for (i = 0; i < sizeof levels / sizeof levels[0]; i++) {
		say("   %d\n", levels[i]);
		slotled_set(levels[i]);
		hold(2000);
	}
	say("2. blink, 200 ms on, 200 ms off (4 s)\n");
	slotled_blink(255, 200, 200, 0);
	hold(4000);
	say("3. pulse, 0 to 255, every 2 s (6 s)\n");
	slotled_pulse(0, 255, 2000, 0);
	hold(6000);
	say("4. heartbeat, every 1 s (5 s)\n");
	slotled_heartbeat(255, 1000, 0);
	hold(5000);
	say("5. Morse: SOS at 15 WPM, twice\n");
	slotled_morse("SOS", 255, 80, 2);
	slotled_wait();
	say("6. flashes over a dim glow: activity (5 s)\n");
	slotled_set(24);
	hold(500);
	for (r = 0; r < 12; r++) {
		slotled_flash(255, 40);
		hold(200 + (r % 3) * 150);
	}
	say("7. your own steps: a wave (6 s)\n");
	slotled_play(&custom);
	hold(6000);
	say("8. fade out\n");
	{
		static slotled_step out[1] = { { 0, 1, 1500 } };
		static const slotled_effect e = { out, 1, 1 };

		slotled_set(255);
		hold(300);
		slotled_play(&e);
		slotled_wait();
	}
	slotled_shutdown();
}

static void ios(int first, int last)
{
	int i, r;

	say("IOS levels bright, dim, off: 3 s each\n");
	for (i = SLOTLED_ILLUM_BRIGHT; i >= SLOTLED_ILLUM_OFF; i--) {
		r = slotled_ios_level(i);
		say("  level %d: %d\n", i, r);
		hold(3000);
	}
	slotled_ios_level(SLOTLED_ILLUM_BRIGHT);
	for (i = first; i <= last; i++) {
		u32 in[8] = { 0 }, out[8];

		// Priorities rise, so each pattern replaces the one before.
		in[0] = (u32)i | (u32)(i * 8 + 8) << 8;
		r = slotled_ios_flash_raw(in, out);
		say("pattern %d: %d, out %08x %08x\n", i, r, out[0], out[1]);
		hold(5000);
	}
	{
		int saved = slotled_saved_level();

		say("back to the saved level %d: %d\n", saved,
		    slotled_ios_level(saved < 0 ? SLOTLED_ILLUM_DIM : saved));
	}
}

int main(int argc, char **argv)
{
	void *xfb;
	GXRModeObj *rmode;
	int i;

	__exception_setreload(5);
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	if (fatInitDefault()) {
		mkdir("sd:/slotled_test", 0777);
		report = fopen("sd:/slotled_test/result.txt", "w");
	}
	say("slotled_app (slotled %s):", SLOTLED_VERSION);
	for (i = 1; i < argc; ++i)
		say(" [%s]", argv[i]);
	say("\n");
	if (argc < 2 || !strcmp(argv[1], "check")) {
		check();
	} else if (!strcmp(argv[1], "demo")) {
		demo();
	} else if (!strcmp(argv[1], "ios")) {
		ios(argc > 2 ? atoi(argv[2]) : 0, argc > 3 ? atoi(argv[3]) : SLOTLED_IOS_PATTERNS - 1);
	} else if (argc == 3 && !strcmp(argv[1], "save")) {
		say("before: %d\n", slotled_saved_level());
		say("slotled_save_level(%d): %d\n", atoi(argv[2]), slotled_save_level(atoi(argv[2])));
		say("SYSCONF now: %d\n", sysconf_now());
	} else {
		say("usage: check | demo | ios [FIRST [LAST]] | save N\n");
	}
	say("done\n");
	if (report) {
		fclose(report);
		fatUnmount("sd:");
	}
	return 0;
}
