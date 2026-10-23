// What a Wii's DNS answers, from a fresh network start (read-only).
//
// usage (argv from Wiiload): dnscheck.dol [NAME...]
//   With no names: cfh.wapp.wii.com (the WiiConnect24 user agreement),
//   nwcs.wapp.wii.com, rc24.xyz, and pi.hole (which resolves only when the
//   Wii asks a Pi-hole). Each start of the network asks DHCP for the DNS
//   server again and caches nothing from before, so this is also the check
//   after a DNS change: no flush is needed on the Wii.
// Writes sd:/dnscheck/result.txt (and the screen), ending with "done", and
// exits to HBC. Touches no NAND file. tests/wii_dns.py queues it.

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <network.h>

extern void __exception_setreload(int t);

static FILE *report;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	char t[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(t, sizeof(t), fmt, ap);
	va_end(ap);
	printf("%s", t);
	if (report) {
		fputs(t, report);
		fflush(report);
	}
}

int main(int argc, char **argv) {
	static const char *const names[] = { "cfh.wapp.wii.com", "nwcs.wapp.wii.com", "rc24.xyz",
										 "pi.hole" };
	const char *const *list = names;
	int count = sizeof(names) / sizeof(names[0]), i;
	struct hostent *h;
	void *xfb;
	GXRModeObj *rmode;
	s32 r = -1;

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
		mkdir("sd:/dnscheck", 0777);
		report = fopen("sd:/dnscheck/result.txt", "w");
	}
	if (argc > 1) {
		list = (const char *const *) argv + 1;
		count = argc - 1;
	}
	for (i = 0; i < 10 && (r = net_init()) == -EAGAIN; ++i)
		usleep(200 * 1000);
	say("net_init: %d\n", (int) r);
	if (r >= 0) {
		u32 ip = net_gethostip();

		say("this Wii: %u.%u.%u.%u\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
		for (i = 0; i < count; ++i) {
			h = net_gethostbyname(list[i]);
			if (h && h->h_addr_list && h->h_addr_list[0]) {
				const u8 *a = (const u8 *) h->h_addr_list[0];
				say("%s -> %u.%u.%u.%u\n", list[i], a[0], a[1], a[2], a[3]);
			} else {
				say("%s -> no answer\n", list[i]);
			}
		}
		net_deinit();
	}
	say("done\n");
	if (report) {
		fclose(report);
		fatUnmount("sd:");
	}
	return 0;
}
