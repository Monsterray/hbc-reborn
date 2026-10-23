// Copy NAND files to the SD card, read-only: for backing up and studying the
// Wii Menu's data (the Message Board's cdb.vff, play_rec.dat).
//
// usage (argv from Wiiload): nandget.dol [--unlock] NAND-DIR [SD-DIR]
//   copies every file in NAND-DIR (not subfolders) to SD-DIR, default
//   sd:/nandget, and writes SD-DIR/result.txt: one line per file with its
//   size or the ISFS error. Nothing on NAND is opened for writing.
//   --unlock lifts IOS's NAND permission check for the copy, as HBC's play
//   log does (channel/channelapp/source/playtime.c), and puts it back after:
//   that needs AHBPROT, and checks the patch on a real IOS.

#include <errno.h>
#include <stdarg.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

extern void __exception_setreload(int t);

#define CHUNK (64 * 1024)

static FILE *report;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	char t[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(t, sizeof(t), fmt, ap);
	va_end(ap);
	printf("%s", t);
	if (report)
		fputs(t, report);
}

static void copy(const char *nand, const char *sd) {
	static u8 buf[CHUNK] ATTRIBUTE_ALIGN(32);
	static fstats st ATTRIBUTE_ALIGN(32);
	s32 fd, r;
	u32 done = 0;
	FILE *out;

	fd = ISFS_Open(nand, ISFS_OPEN_READ);
	if (fd < 0) {
		say("%s: open %d\n", nand, (int) fd);
		return;
	}
	if ((r = ISFS_GetFileStats(fd, &st)) < 0) {
		say("%s: stats %d\n", nand, (int) r);
		ISFS_Close(fd);
		return;
	}
	out = fopen(sd, "wb");
	if (!out) {
		say("%s: cannot write %s (%d)\n", nand, sd, errno);
		ISFS_Close(fd);
		return;
	}
	while (done < st.file_length) {
		u32 n = st.file_length - done < CHUNK ? st.file_length - done : CHUNK;
		r = ISFS_Read(fd, buf, n);
		if (r <= 0) {
			say("%s: read %d at %u\n", nand, (int) r, (unsigned) done);
			break;
		}
		fwrite(buf, 1, r, out);
		done += r;
	}
	fclose(out);
	ISFS_Close(fd);
	say("%s: %u of %u bytes\n", nand, (unsigned) done, (unsigned) st.file_length);
}

// The same patch as playtime.c's fs_permissions().
static const u8 perm_check[] = { 0x42, 0x8B, 0xD0, 0x01, 0x25, 0x66 };

static int fs_permissions(int lifted) {
	u8 *p = (u8 *) *(u32 *) 0x80003134, *end = (u8 *) 0x94000000;
	u8 from = lifted ? 0xD0 : 0xE0, to = lifted ? 0xE0 : 0xD0;
	int n = 0;

	if (read32(0x0d800064) != 0xffffffff)
		return -1;
	if ((u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return -2;
	write16(0x0D8B420A, 2);
	for (; p < end - sizeof(perm_check); ++p) {
		if (p[0] == perm_check[0] && p[1] == perm_check[1] && p[2] == from &&
				!memcmp(p + 3, perm_check + 3, sizeof(perm_check) - 3)) {
			p[2] = to;
			DCFlushRange((void *) ((u32) p & ~31), 64);
			ICInvalidateRange((void *) ((u32) p & ~31), 64);
			++n;
		}
	}
	return n;
}

/* KD (IOS's WiiConnect24 service) owns /shared2/wc24: a read or write there
 * while it checks or sends mail can wait on it (a bench job's copy of the
 * mailboxes once waited an hour). Ask it to pause first, as sdk/mboard does
 * (NWC24OpenLib: /dev/net/kd/request ioctl 2), and let it go after (3). */
static s32 kd_fd = -1;

static s32 kd_ioctl(s32 cmd) {
	static u8 in[32] ATTRIBUTE_ALIGN(32), out[32] ATTRIBUTE_ALIGN(32);
	s32 r;

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));
	r = cmd == 2 ? IOS_Ioctl(kd_fd, cmd, in, sizeof(in), out, sizeof(out))
				 : IOS_Ioctl(kd_fd, cmd, NULL, 0, out, sizeof(out));
	return r < 0 ? r : (s32) (out[0] << 24 | out[1] << 16 | out[2] << 8 | out[3]);
}

static void kd_pause(const char *nand_dir) {
	static char path[] ATTRIBUTE_ALIGN(32) = "/dev/net/kd/request";

	if (strncmp(nand_dir, "/shared2/wc24", 13))
		return;
	kd_fd = IOS_Open(path, 0);
	if (kd_fd >= 0)
		say("WiiConnect24 paused: %d\n", (int) kd_ioctl(2));
	else
		say("WiiConnect24: no KD (%d)\n", (int) kd_fd);
}

static void kd_resume(void) {
	if (kd_fd < 0)
		return;
	say("WiiConnect24 resumed: %d\n", (int) kd_ioctl(3));
	IOS_Close(kd_fd);
	kd_fd = -1;
}

int main(int argc, char **argv) {
	static char names[64 * 13] ATTRIBUTE_ALIGN(32);
	const char *dir, *sd_dir;
	char nand[ISFS_MAXPATH + 16], sd[256];
	u32 count = 0, i;
	char *p;
	void *xfb;
	GXRModeObj *rmode;
	s32 r;

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

	int unlock = argc > 1 && !strcmp(argv[1], "--unlock"), lifted = 0;

	if (unlock) {
		--argc;
		++argv;
	}
	dir = argc > 1 ? argv[1] : "/title/00000001/00000002/data";
	sd_dir = argc > 2 ? argv[2] : "sd:/nandget";
	if (!fatInitDefault()) {
		printf("nandget: no SD card\n");
		sleep(5);
		return 1;
	}
	mkdir(sd_dir, 0777);
	snprintf(sd, sizeof(sd), "%s/result.txt", sd_dir);
	report = fopen(sd, "w");
	ISFS_Initialize();

	say("nandget %s -> %s\n", dir, sd_dir);
	kd_pause(dir);
	if (unlock) {
		lifted = fs_permissions(1);
		say("permission check lifted at %d site(s) (heap start %08x)\n", lifted,
			(unsigned) *(u32 *) 0x80003134);
	}
	r = ISFS_ReadDir(dir, NULL, &count);
	if (r < 0 || count > 64) {
		say("%s: readdir %d (count %u)\n", dir, (int) r, (unsigned) count);
	} else if ((r = ISFS_ReadDir(dir, names, &count)) < 0) {
		say("%s: readdir %d\n", dir, (int) r);
	} else {
		for (i = 0, p = names; i < count; ++i, p += strlen(p) + 1) {
			if (snprintf(nand, sizeof(nand), "%s/%s", dir, p) >= (int) sizeof(nand) ||
					snprintf(sd, sizeof(sd), "%s/%s", sd_dir, p) >= (int) sizeof(sd)) {
				say("%s: name too long\n", p);
				continue;
			}
			{
				u64 t0 = gettime();

				copy(nand, sd);
				say("  (%llu ms)\n", ticks_to_millisecs(gettime() - t0));
			}
		}
	}
	if (unlock && lifted > 0) {
		int back = fs_permissions(0);
		say("permission check restored at %d site(s)%s\n", back, back == lifted ? "" : " (MISMATCH)");
	}
	kd_resume();
	say("done\n");
	if (report)
		fclose(report);
	ISFS_Deinitialize();
	fatUnmount("sd:");
	return 0;
}
