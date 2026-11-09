// sdk/usbguard on a PC (tests/host/test_usbguard.py drives it): fake EHCI
// registers, fake calls that block, fail or return late, and scripted
// pulls, replugs and frees from on_wait.
//
// usage: usbguard_host [options] [fns=K,K,...] [act=A@MS,...]
//   options:  noaccess       HW_AHBPROT reads 0: no register ahbprot
//             nodrive        no device on any port at the start
//             other          a second USB 2.0 device on port 1 (a hub, a LAN adapter)
//             nowait         no on_wait (no act runs)
//             reload         the reload call (no retry) instead of usbguard_call
//             mount          usbguard_mount, the fns as iface->startup
//             nofat          fatMountSimple fails
//             repeat=N       the call N times (giving up each time on act)
//             warn=MS replug=MS   usbguard_opts
//             after=release  after the calls: the blocked fn returns -4
//             after=pull     after the calls: the drive is pulled
//             after=free2    after the calls: usbguard_free twice
//   fns (one per call of the fn, the last repeats):
//             ok (7)  fail (-5)  late (7 after 400 ms)  block (until a pull
//             or a free: -4)  blockok (the same, then 3)  d2x (-101 after
//             300 ms)  d2xfast (-101 at once)
//   act (times from the start of the program):
//             pull@MS  replug@MS  replugother@MS (into port 2)  free@MS  giveup@MS
//
// Prints "phase NAME MS" at each phase change, "fn N KIND", "write PORT
// VALUE", "result R", then "check S", "pulled B", "blocked B".

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
static unsigned now_ms(void)
{
	static LARGE_INTEGER freq, t0;
	LARGE_INTEGER t;

	if (!freq.QuadPart) {
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);
	}
	QueryPerformanceCounter(&t);
	return (unsigned) ((t.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart);
}
#else
#include <time.h>
#include <unistd.h>
static void sleep_ms(unsigned ms) { usleep(ms * 1000); }
static unsigned now_ms(void)
{
	static struct timespec t0;
	struct timespec t;

	if (!t0.tv_sec)
		clock_gettime(CLOCK_MONOTONIC, &t0);
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned) ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}
#endif

#include <fat.h>
#include "usbguard_test.h"

#define EHCI 0x0d040000u
#define PORTSC0 (EHCI + 0x10 + 0x44)
#define CCS 0x1u
#define CSC 0x2u
#define PE 0x4u
#define RWC 0x2au
#define PP 0x1000u

static volatile unsigned portsc[4];
static int ahbprot = 1;
static volatile int released;	// the blocked fn returns -4 when set

static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

uint32_t usbguard_host_read32(uint32_t addr)
{
	if (addr == 0x0d800064)
		return ahbprot ? 0xffffffff : 0;
	if (addr == EHCI)
		return 0x01000010;	// caplength 0x10
	if (addr == EHCI + 4)
		return 0x2204;		// Hollywood's HCSPARAMS: 4 ports, no power switching
	if (addr >= PORTSC0 && addr < PORTSC0 + 16)
		return portsc[(addr - PORTSC0) / 4];
	return 0;
}

// RWC bits clear where 1 is written; CCS and the rest follow the device.
void usbguard_host_write32(uint32_t addr, uint32_t value)
{
	unsigned i, old;

	if (addr < PORTSC0 || addr >= PORTSC0 + 16)
		return;
	i = (addr - PORTSC0) / 4;
	old = portsc[i];
	say("write %u %04x\n", i, (unsigned) value);
	portsc[i] = (old & CCS) | (value & ~(RWC | CCS)) | (old & RWC & ~value);
	// The port disabled: the transfer on it fails, as when it is pulled.
	if ((old & PE) && !(value & PE))
		released = 1;
}

// ---- The fake calls -----------------------------------------------------

static char fns[16][12];
static int fn_count, fn_calls;

static const char *next_kind(void)
{
	int i = fn_calls < fn_count ? fn_calls : fn_count - 1;

	fn_calls++;
	say("fn %d %s\n", fn_calls, fns[i]);
	return fns[i];
}

static int32_t fake(void *arg)
{
	const char *k = next_kind();

	(void) arg;
	if (!strcmp(k, "ok"))
		return 7;
	if (!strcmp(k, "fail"))
		return -5;
	if (!strcmp(k, "late")) {
		sleep_ms(400);
		return 7;
	}
	if (!strcmp(k, "d2x")) {
		sleep_ms(300);
		return USBGUARD_D2X_GAVE_UP;
	}
	if (!strcmp(k, "d2xfast"))
		return USBGUARD_D2X_GAVE_UP;
	if (!strcmp(k, "blockok")) {	// answers when the drive goes, as IOS_Open did
		released = 0;
		while (!released)
			sleep_ms(2);
		return 3;
	}
	// block
	released = 0;
	while (!released)
		sleep_ms(2);
	return -4;
}

static bool fake_startup(void) { return fake(NULL) >= 0; }
static bool fake_inserted(void) { return true; }
static int fat_ok = 1;

bool fatMountSimple(const char *name, const DISC_INTERFACE *iface)
{
	(void) iface;
	say("fatMount %s\n", name);
	return fat_ok;
}

// ---- The scripted on_wait -----------------------------------------------

static struct {
	char what[16];
	unsigned at;
	int done;
} acts[16];
static int act_count;

static void pull(void)
{
	portsc[0] = (portsc[0] & ~(CCS | PE)) | CSC;
	released = 1;
}

static int last_phase = -1;
static const char *phase_names[] = { "WAITING", "STUCK", "REPLUG" };

static bool on_wait(uint32_t ms, usbguard_phase phase, void *ctx)
{
	unsigned now = now_ms();
	int i;

	(void) ctx;
	if (now > 10000) {	// a test that never ends
		say("timeout\n");
		exit(3);
	}
	if ((int) phase != last_phase) {
		last_phase = phase;
		say("phase %s %u\n", phase_names[phase], (unsigned) ms);
	}
	for (i = 0; i < act_count; i++) {
		if (acts[i].done || now < acts[i].at)
			continue;
		acts[i].done = 1;
		say("act %s %u\n", acts[i].what, now);
		if (!strcmp(acts[i].what, "pull"))
			pull();
		else if (!strcmp(acts[i].what, "replug"))
			portsc[0] = CCS | CSC | PE | PP;	// IOS enabled the new drive
		else if (!strcmp(acts[i].what, "replugother"))
			portsc[2] = CCS | CSC | PE | PP;	// the drive, in another port
		else if (!strcmp(acts[i].what, "free"))
			say("free %d\n", usbguard_free());
		else if (!strcmp(acts[i].what, "giveup"))
			return false;
	}
	return true;
}

static const char *result_name(int32_t r)
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

int main(int argc, char **argv)
{
	static const char *states[] = { "OK", "CUT_OFF", "NO_ACCESS" };
	usbguard_opts o = USBGUARD_OPTS_INIT;
	DISC_INTERFACE iface = { fake_startup, fake_inserted };
	int i, mount = 0, reload = 0, repeat = 1, release_after = 0, pull_after = 0, free_twice = 0;

	now_ms();
	portsc[0] = CCS | PE | PP;	// a USB 2.0 drive, enabled
	portsc[1] = PP;
	portsc[2] = PP;
	portsc[3] = CCS | PE | PP | 0x2000;	// a USB 1.1 device, at the companion
	o.warn_ms = 200;
	o.poll_ms = 10;
	o.replug_ms = 300;
	o.on_wait = on_wait;
	strcpy(fns[0], "ok");
	fn_count = 1;
	for (i = 1; i < argc; i++) {
		char *a = argv[i];

		if (!strcmp(a, "noaccess"))
			ahbprot = 0;
		else if (!strcmp(a, "nodrive"))
			portsc[0] = PP;
		else if (!strcmp(a, "other"))
			portsc[1] = CCS | PE | PP;
		else if (!strcmp(a, "nowait"))
			o.on_wait = NULL;
		else if (!strcmp(a, "reload"))
			reload = 1;
		else if (!strcmp(a, "mount"))
			mount = 1;
		else if (!strcmp(a, "nofat"))
			fat_ok = 0;
		else if (!strncmp(a, "repeat=", 7))
			repeat = atoi(a + 7);
		else if (!strncmp(a, "warn=", 5))
			o.warn_ms = (uint32_t) atoi(a + 5);
		else if (!strncmp(a, "replug=", 7))
			o.replug_ms = (uint32_t) atoi(a + 7);
		else if (!strcmp(a, "after=release"))
			release_after = 1;
		else if (!strcmp(a, "after=pull"))
			pull_after = 1;
		else if (!strcmp(a, "after=free2"))
			free_twice = 1;
		else if (!strncmp(a, "fns=", 4)) {
			char *p = strtok(a + 4, ",");

			for (fn_count = 0; p && fn_count < 16; p = strtok(NULL, ","))
				snprintf(fns[fn_count++], sizeof(fns[0]), "%s", p);
		} else if (!strncmp(a, "act=", 4)) {
			char *p = strtok(a + 4, ",");

			for (act_count = 0; p && act_count < 16; p = strtok(NULL, ",")) {
				char *at = strchr(p, '@');

				if (!at)
					return 2;
				*at = 0;
				snprintf(acts[act_count].what, sizeof(acts[0].what), "%s", p);
				acts[act_count++].at = (unsigned) atoi(at + 1);
			}
		} else {
			fprintf(stderr, "unknown argument %s\n", a);
			return 2;
		}
	}
	for (i = 0; i < repeat; i++) {
		int32_t r;

		last_phase = -1;
		for (int j = 0; j < act_count; j++)
			if (!strcmp(acts[j].what, "giveup"))
				acts[j].done = 0;
		r = mount ? usbguard_mount("usb", &iface, &o) :
		    reload ? usbguard_test_reload_call(fake, NULL, &o) : usbguard_call(fake, NULL, &o);
		say("result %s %u\n", result_name(r), now_ms());
	}
	say("check %s\n", states[usbguard_check()]);
	say("pulled %d\n", usbguard_pulled());
	say("blocked %d\n", usbguard_blocked());
	if (pull_after) {
		pull();
		say("pulled %d\n", usbguard_pulled());
		say("check %s\n", states[usbguard_check()]);
	}
	if (free_twice) {
		say("free %d\n", usbguard_free());
		say("free %d\n", usbguard_free());
	}
	if (release_after) {
		released = 1;
		sleep_ms(100);
		say("released\n");
		say("blocked %d\n", usbguard_blocked());
		last_phase = -1;
		say("result %s %u\n", result_name(usbguard_call(fake, NULL, &o)), now_ms());
	}
	usbguard_port_change ch[8];
	int n = usbguard_freed_ports(ch, 8);
	for (i = 0; i < n; i++)
		say("freed %u %04x %04x\n", ch[i].port, (unsigned) ch[i].before,
		    (unsigned) ch[i].after);
	say("calls %d\n", fn_calls);
	return 0;
}
