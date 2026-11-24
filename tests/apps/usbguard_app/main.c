// Test app for sdk/usbguard on a Wii or in Dolphin (tests/dolphin/dolphin_usbguard.py
// drives the check; the stuck run is attended).
//
// usage (argv from Wiiload):
//   usbguard_app.dol check   a healthy USB: each wrapper once, which must
//                            answer as without the guard (no STUCK phase):
//                            usbguard_check, a guarded IOS_Open of
//                            /dev/usb/ven, usbguard_mount of the USB drive
//                            (if any), usbguard_reload_ios of the running
//                            IOS, and a guarded call after it. Never frees.
//   usbguard_app.dol stuck   for a person at the Wii, with a stuck drive:
//                            the same calls with usbguard_default_ui. Expect
//                            the warning within 5 s; then pull the drive, or
//                            press A on a GameCube controller to free it.
//
// Every line goes to the TV. The report is kept in memory and written to
// sd:/usbguard_test/result.txt at the end, only once no request is stuck
// (the SD card waits behind a stuck USB request too).

#include <gccore.h>
#include <fat.h>
#include <dirent.h>
#include <ogc/lwp_watchdog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "usbguard.h"

extern DISC_INTERFACE __io_usbstorage, __io_wiisd;

static char report[8192];
static int report_len;
static u64 t0;

static u32 ms_now(void)
{
	return (u32) ticks_to_millisecs(gettime() - t0);
}

static void say(const char *fmt, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s", line);
	if (report_len < (int) sizeof(report) - (int) strlen(line) - 1) {
		strcpy(report + report_len, line);
		report_len += strlen(line);
	}
}

static const char *const phase_names[] = { "WAITING", "STUCK", "REPLUG" };
static int last_phase = -1;
static bool stuck_seen, attended;

// Logs each phase change (into memory and on the TV: no IOS call), then the
// default UI in the attended run.
static bool on_wait(uint32_t ms, usbguard_phase phase, void *ctx)
{
	if ((int) phase != last_phase) {
		last_phase = phase;
		say("  phase %s at %u ms\n", phase_names[phase], (unsigned) ms);
	}
	if (phase != USBGUARD_PHASE_WAITING)
		stuck_seen = true;
	return attended ? usbguard_default_ui(ms, phase, ctx) : true;
}

static const char *rc_name(s32 r)
{
	static char buf[16];

	switch (r) {
	case USBGUARD_STUCK: return "STUCK";
	case USBGUARD_FREED: return "FREED";
	case USBGUARD_NO_WORKER: return "NO_WORKER";
	case USBGUARD_NO_DRIVE: return "NO_DRIVE";
	case USBGUARD_NOT_MOUNTED: return "NOT_MOUNTED";
	}
	snprintf(buf, sizeof(buf), "%d", (int) r);
	return buf;
}

static void state(const char *when)
{
	static const char *const names[] = { "OK", "CUT_OFF", "NO_ACCESS" };

	say("%s: check %s, drive %d, blocked %d, pulled %d\n", when, names[usbguard_check()],
	    usbguard_drive_present(), usbguard_blocked(), usbguard_pulled());
}

static s32 open_ven(void *arg)
{
	static const char ven[] ATTRIBUTE_ALIGN(32) = "/dev/usb/ven";
	s32 fd = IOS_Open(ven, 0);

	(void) arg;
	if (fd >= 0)
		IOS_Close(fd);
	return fd;
}

static s32 timed(const char *what, s32 r, u32 start)
{
	say("%s: %s in %u ms\n", what, rc_name(r), (unsigned) (ms_now() - start));
	last_phase = -1;
	return r;
}

static void write_report(void)
{
	FILE *f;

	if (usbguard_blocked()) {
		say("report: not written, a request is still stuck\n");
		return;
	}
	if (!fatMountSimple("sd", &__io_wiisd)) {
		say("report: no SD card\n");
		return;
	}
	mkdir("sd:/usbguard_test", 0777);
	f = fopen("sd:/usbguard_test/result.txt", "w");
	if (f) {
		fwrite(report, 1, report_len, f);
		fclose(f);
		printf("report: sd:/usbguard_test/result.txt\n");
	}
	fatUnmount("sd");
}

int main(int argc, char **argv)
{
	static void *xfb;
	GXRModeObj *rmode;
	usbguard_opts o = USBGUARD_OPTS_INIT;
	u32 start;
	s32 r;
	int ios;

	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	PAD_Init();
	t0 = gettime();

	attended = argc > 1 && !strcmp(argv[1], "stuck");
	o.on_wait = on_wait;
	ios = IOS_GetVersion();
	say("usbguard %s test (%s): IOS %d, AHBPROT %08x, can_free %d\n", USBGUARD_VERSION,
	    attended ? "stuck" : "check", ios, (unsigned) *(volatile u32 *) 0xCD800064,
	    usbguard_can_free());
	state("start");
	if (attended)
		printf("Stuck run: on the warning, pull the drive (then plug it back in),\n"
		       "or press A on a GameCube controller to go on without USB.\n");

	start = ms_now();
	timed("call IOS_Open /dev/usb/ven", usbguard_call(open_ven, NULL, &o), start);
	state("after the call");

	start = ms_now();
	r = timed("mount usb", usbguard_mount("usb", &__io_usbstorage, &o), start);
	if (r == 0) {
		DIR *d = opendir("usb:/");
		int n = 0;

		while (d && readdir(d))
			n++;
		if (d)
			closedir(d);
		say("usb:/ has %d entries\n", n);
		fatUnmount("usb");
	}
	state("after the mount");

	if (!usbguard_blocked()) {
		start = ms_now();
		timed("reload IOS", usbguard_reload_ios(ios, &o), start);
		say("IOS %d after the reload\n", IOS_GetVersion());
		// The same reload unguarded, for comparison: they should agree.
		start = ms_now();
		r = IOS_ReloadIOS(ios);
		say("plain reload IOS: %d in %u ms\n", (int) r, (unsigned) (ms_now() - start));
		start = ms_now();
		timed("call after the reload", usbguard_call(open_ven, NULL, &o), start);
	} else {
		say("reload IOS: skipped, a request is still stuck\n");
	}
	state("end");
	say("stuck phase seen: %d\n", stuck_seen);
	say("done\n");
	write_report();

	if (attended) {
		// START on a GameCube controller, or 60 s (no controller).
		u32 end = ms_now() + 60000;

		printf("\nPress START on a GameCube controller to exit (or wait 60 s).\n");
		while (ms_now() < end) {
			PAD_ScanPads();
			if (PAD_ButtonsDown(0) & PAD_BUTTON_START)
				break;
			VIDEO_WaitVSync();
		}
	} else {
		sleep(3);
	}
	return 0;
}
