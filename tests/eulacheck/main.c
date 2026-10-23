// Why "User Agreements" might not open (read-only): the hidden EULA channel.
//
// Wii Settings > Internet > User Agreements starts the hidden channel HAKx
// (00010008-4841424x: E USA, P Europe, J Japan, K Korea). This lists, from
// ES's own title list and the console's settings, never writing anything:
//   - the Wii Menu's version, the console's region, country and language;
//   - each HAKx installed: its version, the IOS it runs on, and how many of
//     its TMD's contents are on the NAND;
//   - that IOS: installed or not, its version and contents (a stub has
//     few or none).
// Writes sd:/eulacheck/result.txt (and the screen), ending with "done", and
// exits to HBC. tests/wii_eulacheck.py queues it.

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <ogc/conf.h>

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

static u64 *titles;
static u32 n_titles;

static int installed(u64 tid) {
	u32 i;

	for (i = 0; i < n_titles; ++i)
		if (titles[i] == tid)
			return 1;
	return 0;
}

// The title's TMD: version, the IOS it runs on, contents listed and present.
static int describe(u64 tid, const char *what, u64 *ios) {
	u32 size = 0, present = 0;
	signed_blob *stmd;
	s32 r;

	if (!installed(tid)) {
		say("%s %08x-%08x: not installed\n", what, (u32) (tid >> 32), (u32) tid);
		return 0;
	}
	r = ES_GetStoredTMDSize(tid, &size);
	if (r < 0 || !size || size > 0x10000) {
		say("%s %08x-%08x: installed, TMD unreadable (%d)\n", what, (u32) (tid >> 32), (u32) tid, (int) r);
		return 1;
	}
	stmd = memalign(32, (size + 31) & ~31u);
	if (!stmd || (r = ES_GetStoredTMD(tid, stmd, size)) < 0) {
		say("%s %08x-%08x: installed, TMD read failed (%d)\n", what, (u32) (tid >> 32), (u32) tid, (int) r);
		free(stmd);
		return 1;
	}
	{
		const tmd *t = SIGNATURE_PAYLOAD(stmd);

		ES_GetTitleContentsCount(tid, &present);
		say("%s %08x-%08x: version %u, IOS%u, %u contents in its TMD, %u on the NAND\n", what,
			(u32) (tid >> 32), (u32) tid, t->title_version, (u32) t->sys_version, t->num_contents,
			(unsigned) present);
		if (ios)
			*ios = t->sys_version;
	}
	free(stmd);
	return 1;
}

int main(void) {
	static const char codes[] = "EPJK";
	static const char *const regions[] = { "JPN", "USA", "EUR", "KOR" };
	static u8 sadr[0x1007] ATTRIBUTE_ALIGN(32);
	void *xfb;
	GXRModeObj *rmode;
	s32 region;
	int i, found = 0;

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
		mkdir("sd:/eulacheck", 0777);
		report = fopen("sd:/eulacheck/result.txt", "w");
	}
	CONF_Init();
	region = CONF_GetRegion();
	say("region %d (%s), area %d, language %d", (int) region,
		region >= 0 && region < 4 ? regions[region] : "?", (int) CONF_GetArea(), (int) CONF_GetLanguage());
	if (CONF_Get("IPL.SADR", sadr, sizeof(sadr)) > 0)
		say(", country code %u", sadr[0]);
	say("\n");

	if (ES_GetNumTitles(&n_titles) < 0 || !n_titles ||
			!(titles = memalign(32, (n_titles * sizeof(u64) + 31) & ~31u)) ||
			ES_GetTitles(titles, n_titles) < 0) {
		say("ES title list: unreadable\n");
	} else {
		say("%u titles installed\n", (unsigned) n_titles);
		describe(0x0000000100000002ull, "Wii Menu", NULL);
		for (i = 0; i < 4; ++i) {
			u64 ios = 0, tid = 0x0001000848414b00ull | (u8) codes[i];
			char what[16];

			snprintf(what, sizeof(what), "EULA HAK%c", codes[i]);
			if (describe(tid, what, &ios)) {
				++found;
				if (ios)
					describe(ios, "  its IOS", NULL);
			}
		}
		say("%d EULA channel%s installed\n", found, found == 1 ? "" : "s");
		// The current HAKE (version 3) runs on IOS56.
		describe(0x0000000100000038ull, "IOS56 (the current HAKE's)", NULL);
	}
	say("done\n");
	if (report) {
		fclose(report);
		fatUnmount("sd:");
	}
	return 0;
}
