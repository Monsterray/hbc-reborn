// Put NAND files back from the SD card: the restore half of tests/nandget,
// for undoing a test's writes to the Wii Menu's data (cdb.vff) or the
// WiiConnect24 mailboxes from a nandget backup.
//
// usage (argv from Wiiload): nandput.dol [--unlock] SD-DIR NAND-DIR FILE...
//   copies SD-DIR/FILE over NAND-DIR/FILE for each FILE. The NAND file must
//   already exist with the same size (these are fixed-size files: nothing is
//   created, grown or shrunk), and the SD file is read whole before anything
//   is written. Each file is read back and compared afterwards. Writes
//   SD-DIR/put_result.txt, ending with "done".
//   --unlock lifts IOS's NAND permission check for the copy, as nandget does
//   (needed for the Wii Menu's files; needs AHBPROT), and puts it back after.
//   With no arguments at all, they are read from sd:/nandput_args.txt, one
//   per line (for Dolphin, which boots a DOL without arguments).

#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
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
	if (report) {
		fputs(t, report);
		fflush(report);
	}
}

static void put(const char *sd, const char *nand) {
	static u8 chunk[CHUNK] ATTRIBUTE_ALIGN(32);
	static fstats st ATTRIBUTE_ALIGN(32);
	u8 *data = NULL;
	u32 size, done, bad = 0;
	s32 fd, r;
	FILE *in;
	long len;

	in = fopen(sd, "rb");
	if (!in || fseek(in, 0, SEEK_END) || (len = ftell(in)) < 0 || fseek(in, 0, SEEK_SET)) {
		say("%s: cannot read (%d)\n", sd, errno);
		goto out;
	}
	size = (u32) len;
	data = malloc(size ? size : 1);
	if (!data || fread(data, 1, size, in) != size) {
		say("%s: cannot read %u bytes\n", sd, (unsigned) size);
		goto out;
	}
	fd = ISFS_Open(nand, ISFS_OPEN_RW);
	if (fd < 0) {
		say("%s: open %d\n", nand, (int) fd);
		goto out;
	}
	if ((r = ISFS_GetFileStats(fd, &st)) < 0 || st.file_length != size) {
		say("%s: size %u, backup %u (%d): not written\n", nand, (unsigned) st.file_length,
			(unsigned) size, (int) r);
		ISFS_Close(fd);
		goto out;
	}
	for (done = 0; done < size; done += r) {
		u32 n = size - done < CHUNK ? size - done : CHUNK;
		memcpy(chunk, data + done, n);
		r = ISFS_Write(fd, chunk, n);
		if (r != (s32) n) {
			say("%s: write %d at %u\n", nand, (int) r, (unsigned) done);
			break;
		}
	}
	ISFS_Seek(fd, 0, SEEK_SET);
	for (done = 0; done < size; done += r) {
		u32 n = size - done < CHUNK ? size - done : CHUNK;
		r = ISFS_Read(fd, chunk, n);
		if (r != (s32) n) {
			say("%s: read back %d at %u\n", nand, (int) r, (unsigned) done);
			bad = 1;
			break;
		}
		bad |= memcmp(chunk, data + done, n) != 0;
	}
	ISFS_Close(fd);
	say("%s: %u bytes %s\n", nand, (unsigned) size, bad ? "DIFFER after writing" : "restored, verified");
out:
	free(data);
	if (in)
		fclose(in);
}

// The same patch as nandget's and playtime.c's fs_permissions().
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
	static char nand[ISFS_MAXPATH + 16] ATTRIBUTE_ALIGN(32);
	char sd[256];
	int unlock = argc > 1 && !strcmp(argv[1], "--unlock"), lifted = 0, i;
	void *xfb;
	GXRModeObj *rmode;

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

	if (!fatInitDefault()) {
		printf("nandput: no SD card\n");
		sleep(5);
		return 1;
	}
	// Without arguments (Dolphin boots a DOL without any), one per line from
	// sd:/nandput_args.txt.
	if (argc < 2) {
		static char line[16][256], *list[17];
		FILE *f = fopen("sd:/nandput_args.txt", "r");

		list[0] = argv[0];
		for (argc = 1; f && argc < 17 && fgets(line[argc - 1], sizeof(line[0]), f); ++argc) {
			line[argc - 1][strcspn(line[argc - 1], "\r\n")] = 0;
			list[argc] = line[argc - 1];
		}
		if (f)
			fclose(f);
		argv = list;
		unlock = argc > 1 && !strcmp(argv[1], "--unlock");
	}
	if (unlock) {
		--argc;
		++argv;
	}
	if (argc < 4) {
		printf("usage: nandput.dol [--unlock] SD-DIR NAND-DIR FILE...\n");
		sleep(5);
		return 1;
	}
	snprintf(sd, sizeof(sd), "%s/put_result.txt", argv[1]);
	report = fopen(sd, "w");
	ISFS_Initialize();
	say("nandput %s -> %s\n", argv[1], argv[2]);
	kd_pause(argv[2]);
	if (unlock) {
		lifted = fs_permissions(1);
		say("permission check lifted at %d site(s)\n", lifted);
	}
	for (i = 3; i < argc; ++i) {
		if (snprintf(sd, sizeof(sd), "%s/%s", argv[1], argv[i]) >= (int) sizeof(sd) ||
				snprintf(nand, sizeof(nand), "%s/%s", argv[2], argv[i]) >= (int) sizeof(nand)) {
			say("%s: name too long\n", argv[i]);
			continue;
		}
		put(sd, nand);
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
