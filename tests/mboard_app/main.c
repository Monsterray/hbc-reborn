// Test app for sdk/mboard: post to the Message Board, or only look.
//
// usage (argv from Wiiload):
//   mboard_app.dol check                  read-only: the Wii number, the
//                                         receive box, the clock, AHBPROT
//   mboard_app.dol post TITLE BODY        mboard_post(); "\n" in BODY is a new line
//   mboard_app.dol play NAME ID SECONDS   mboard_log_play()
//   mboard_app.dol id                     read-only: this Wii's whole Wii
//                                         number (its mail address), for
//                                         sending between Wiis; check shows
//                                         only its last four digits
//   mboard_app.dol full TITLE BODY        mboard_post_msg() with a Mii face
//                                         (sd:/mboard_test/face.bin, or the
//                                         Wii's first Mii), photo.jpg and
//                                         board.arc from sd:/mboard_test if
//                                         there, and a Reply button
// Each writes what it did to sd:/mboard_test/result.txt (and the screen),
// ending with "done". Built with -DMBOARD_APP_DOLPHIN (make dolphin), a run
// without arguments also posts and logs a fixed test text: Dolphin boots a
// DOL without any (tests/dolphin_mboard.py). -DMBOARD_APP_DOLPHIN_FULL
// (make dolphin-full) runs `full` with a fixed text instead.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <ogc/conf.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "mboard.h"

extern void __exception_setreload(int t);

static FILE *report;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	char t[512];
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

static u32 be32(const u8 *p) {
	return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

static s32 read_nand(const char *path, u8 *buf, u32 len) {
	s32 fd = IOS_Open(path, IPC_OPEN_READ), n;

	if (fd < 0)
		return fd;
	n = IOS_Read(fd, buf, len);
	IOS_Close(fd);
	return n;
}

static void check(void) {
	static u8 cfg[0x400] ATTRIBUTE_ALIGN(32);
	static u8 ctl[0x80] ATTRIBUTE_ALIGN(32);
	static u8 out[32] ATTRIBUTE_ALIGN(32);
	static char path[64] ATTRIBUTE_ALIGN(32);
	u64 tid = 0, id;
	u32 owner = 0, bias = 0;
	u16 group = 0;
	u8 attr, own, grp, oth;
	s32 n, fd;

	say("AHBPROT: %s, IOS%d v%d\n", read32(0x0d800064) == 0xffffffff ? "yes" : "no",
		IOS_GetVersion(), IOS_GetRevision());
	n = read_nand("/shared2/wc24/nwc24msg.cfg", cfg, sizeof(cfg));
	if (n == (s32) sizeof(cfg)) {
		id = (u64) be32(cfg + 8) << 32 | be32(cfg + 12);
		// Only the last four digits: the Wii number is the console's mail address.
		say("nwc24msg.cfg: version %u, Wii number ...%04u, stage %u, domain '%.40s'\n",
			be32(cfg + 4), (unsigned) (id % 10000), be32(cfg + 0x14), (const char *) cfg + 0x18);
	} else {
		say("nwc24msg.cfg: %d\n", (int) n);
	}
	n = read_nand("/shared2/wc24/mbox/wc24recv.ctl", ctl, sizeof(ctl));
	if (n == (s32) sizeof(ctl))
		say("wc24recv.ctl: %u messages, %u bytes, next id %u, free entry 0x%x, oldest %u, "
			"free space %u\n", be32(ctl + 8), be32(ctl + 0x10), be32(ctl + 0x18), be32(ctl + 0x1c),
			be32(ctl + 0x20), be32(ctl + 0x24));
	else
		say("wc24recv.ctl: %d\n", (int) n);
	ES_GetTitleID(&tid);
	snprintf(path, sizeof(path), "/title/%08x/%08x/data", (u32) (tid >> 32), (u32) tid);
	n = ISFS_GetAttr(path, &owner, &group, &attr, &own, &grp, &oth);
	say("title %016llx, data folder group %04x (%d)\n", tid, group, (int) n);
	CONF_GetCounterBias(&bias);
	fd = IOS_Open("/dev/net/kd/time", 0);
	if (fd >= 0) {
		memset(out, 0, sizeof(out));
		n = IOS_Ioctl(fd, 24, NULL, 0, out, sizeof(out));
		say("KD time: ioctl %d, result %d, difference %lld; local %llu s since 2000, bias %u\n",
			(int) n, (int) be32(out), (long long) ((u64) be32(out + 4) << 32 | be32(out + 8)),
			gettime() / MBOARD_TICKS_PER_SEC, bias);
		IOS_Close(fd);
	} else {
		say("KD time: open %d\n", (int) fd);
	}
}

// A whole SD file, or NULL.
static void *slurp(const char *path, u32 *size) {
	FILE *f = fopen(path, "rb");
	void *p = NULL;
	long n;

	*size = 0;
	if (!f)
		return NULL;
	if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) &&
			(p = malloc((size_t) n)) && fread(p, 1, (size_t) n, f) == (size_t) n) {
		*size = (u32) n;
	} else {
		free(p);
		p = NULL;
	}
	fclose(f);
	return p;
}

static int full(const char *title, const char *body) {
	static u8 face[MBOARD_FACE_SIZE];
	mboard_msg m;
	u32 size;
	void *f;
	int res, slot;
	u64 t0;

	memset(&m, 0, sizeof(m));
	m.title = title;
	m.body = body;
	m.reply = 1;
	f = slurp("sd:/mboard_test/face.bin", &size);
	if (f && size == MBOARD_FACE_SIZE) {
		memcpy(face, f, MBOARD_FACE_SIZE);
		m.face = face;
		say("face: sd:/mboard_test/face.bin\n");
	} else {
		for (slot = 0; slot < 100; ++slot) {
			res = mboard_mii(slot, face);
			if (res == MBOARD_OK) {
				m.face = face;
				break;
			}
			if (res != MBOARD_E_NO_MII)
				break;
		}
		say("face: %s (%d %s)\n", m.face ? "a Mii from the Wii's database" : "none", res,
			mboard_strerror(res));
		if (m.face)
			say("  Mii slot %d\n", slot);
	}
	free(f);
	m.jpeg = slurp("sd:/mboard_test/photo.jpg", &m.jpeg_size);
	m.board = slurp("sd:/mboard_test/board.arc", &m.board_size);
	say("photo: %u bytes, board data: %u bytes\n", (unsigned) m.jpeg_size, (unsigned) m.board_size);
	t0 = gettime();
	res = mboard_post_msg(&m);
	say("mboard_post_msg: %d (%s) in %llu ms\n", res, mboard_strerror(res),
		ticks_to_millisecs(gettime() - t0));
	free((void *) m.jpeg);
	free((void *) m.board);
	return res;
}

// The whole Wii number, as nwc24msg.cfg holds it, and the account's domain:
// w<16 digits><domain> is this Wii's address.
static void wii_id(void) {
	static u8 cfg[0x400] ATTRIBUTE_ALIGN(32);
	u64 id;

	if (read_nand("/shared2/wc24/nwc24msg.cfg", cfg, sizeof(cfg)) != (s32) sizeof(cfg)) {
		say("nwc24msg.cfg: not readable\n");
		return;
	}
	id = (u64) be32(cfg + 8) << 32 | be32(cfg + 12);
	say("wii number: %016llu\naddress: w%016llu%.40s\nstage: %u\n", id, id,
		(const char *) cfg + 0x18, be32(cfg + 0x14));
}

static void unescape(char *s) {
	char *o = s;

	for (; *s; ++s) {
		if (s[0] == '\\' && s[1] == 'n') {
			*o++ = '\n';
			++s;
		} else {
			*o++ = *s;
		}
	}
	*o = 0;
}

int main(int argc, char **argv) {
	void *xfb;
	GXRModeObj *rmode;
	int res;
	u64 t0;

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
		mkdir("sd:/mboard_test", 0777);
		report = fopen("sd:/mboard_test/result.txt", "w");
	}
	ISFS_Initialize();
#ifdef MBOARD_APP_DOLPHIN
	if (argc < 2) {
		res = mboard_post("WiiStation", "Played Dolphin Test for 3 minutes.\nA second line.");
		say("mboard_post: %d (%s)\n", res, mboard_strerror(res));
		res = mboard_log_play("Dolphin Test", "WSDT01", 180);
		say("mboard_log_play: %d (%s)\n", res, mboard_strerror(res));
	}
#endif
#ifdef MBOARD_APP_DOLPHIN_FULL
	if (argc < 2)
		full("WiiStation", "Played Dolphin Full Test for 4 minutes.\nWith a Mii, a photo and board data.");
#endif

	say("mboard_app:");
	for (res = 1; res < argc; ++res)
		say(" [%s]", argv[res]);
	say("\n");
	check();
	t0 = gettime();
	if (argc == 4 && !strcmp(argv[1], "post")) {
		unescape(argv[3]);
		res = mboard_post(argv[2], argv[3]);
		say("mboard_post: %d (%s) in %llu ms\n", res, mboard_strerror(res),
			ticks_to_millisecs(gettime() - t0));
		check();
	} else if (argc == 5 && !strcmp(argv[1], "play")) {
		res = mboard_log_play(argv[2], argv[3], (u32) strtoul(argv[4], NULL, 10));
		say("mboard_log_play: %d (%s) in %llu ms\n", res, mboard_strerror(res),
			ticks_to_millisecs(gettime() - t0));
	} else if (argc == 2 && !strcmp(argv[1], "id")) {
		wii_id();
	} else if (argc == 4 && !strcmp(argv[1], "full")) {
		unescape(argv[3]);
		full(argv[2], argv[3]);
		check();
	} else if (argc > 1 && (argc != 2 || strcmp(argv[1], "check"))) {
		say("usage: check | id | post TITLE BODY | play NAME ID SECONDS | full TITLE BODY\n");
	}
	say("done\n");
	if (report) {
		fclose(report);
		fatUnmount("sd:");
	}
	ISFS_Deinitialize();
	return 0;
}
