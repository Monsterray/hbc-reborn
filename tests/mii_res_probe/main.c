// Where is the Wii's Mii face resource (RFL_Res.dat), and can an app read it?
// A read-only probe for docs/research/mii-rendering.md.
//
// usage (argv from Wiiload): mii_res_probe.dol [--unlock] [SD-DIR]
//   writes SD-DIR/result.txt (default sd:/mii_res) and, when it finds one,
//   SD-DIR/RFL_Res.dat. It looks, as an app without NAND rights would:
//   1. /shared2/menu/FaceLib/RFL_Res.dat through ISFS, where RFL's NAND
//      loader looks first;
//   2. each content of the System Menu (title 1-2) through ES, by the
//      title's ticket views (ES_OpenTitleContent): a U8 archive holding a
//      file named RFL_Res.dat;
//   3. the shared contents (TMD type 0x8001) through ISFS, in /shared1 under
//      the names /shared1/content.map gives their SHA-1;
//   4. the private contents (/title/00000001/00000002/content/*.app) through
//      ISFS: IOS refuses them to an app (-102), so only with --unlock, which
//      lifts IOS's NAND permission check as tests/nandget --unlock does
//      (needs AHBPROT) and puts it back after.
// The first RFL_Res.dat found is saved. Nothing is opened for writing on NAND. The file found is Nintendo's: it
// stays on the developer's SD card and PC, never in the repository.

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

extern void __exception_setreload(int t);

#define MENU 0x0000000100000002ull
#define U8_MAGIC 0x55AA382D
#define CHUNK (64 * 1024)

static FILE *report;
static const char *sd_dir;

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
	return (u32) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

// A content, read whole into memory (the menu's are a few MiB at most).
typedef struct {
	u8 *data;
	u32 size;
} blob;

static bool es_read(const tikview *view, u16 index, u32 size, blob *out) {
	s32 cfd = ES_OpenTitleContent(MENU, view, index), r;
	u32 done = 0;

	if (cfd < 0) {
		say("  ES open: %d\n", (int) cfd);
		return false;
	}
	out->data = memalign(32, (size + 31) & ~31);
	out->size = size;
	if (!out->data) {
		say("  no memory for %u bytes\n", (unsigned) size);
		ES_CloseContent(cfd);
		return false;
	}
	while (done < size) {
		u32 n = size - done < CHUNK ? size - done : CHUNK;

		r = ES_ReadContent(cfd, out->data + done, (n + 31) & ~31);
		if (r <= 0) {
			say("  ES read %d at %u\n", (int) r, (unsigned) done);
			break;
		}
		done += r;
	}
	ES_CloseContent(cfd);
	if (done < size) {
		free(out->data);
		out->data = NULL;
		return false;
	}
	return true;
}

static bool isfs_read(const char *path, blob *out) {
	static fstats st ATTRIBUTE_ALIGN(32);
	s32 fd = ISFS_Open(path, ISFS_OPEN_READ), r;
	u32 done = 0;

	if (fd < 0) {
		say("  ISFS %s: open %d\n", path, (int) fd);
		return false;
	}
	if (ISFS_GetFileStats(fd, &st) < 0 || st.file_length > 16 * 1024 * 1024 ||
			!(out->data = memalign(32, (st.file_length + 31) & ~31))) {
		say("  ISFS %s: no stats, or too big\n", path);
		ISFS_Close(fd);
		return false;
	}
	out->size = st.file_length;
	while (done < out->size) {
		u32 n = out->size - done < CHUNK ? out->size - done : CHUNK;

		r = ISFS_Read(fd, out->data + done, n);
		if (r <= 0)
			break;
		done += r;
	}
	ISFS_Close(fd);
	if (done < out->size) {
		say("  ISFS %s: read stopped at %u\n", path, (unsigned) done);
		free(out->data);
		out->data = NULL;
		return false;
	}
	say("  ISFS %s: %u bytes\n", path, (unsigned) out->size);
	return true;
}

// The same patch as tests/nandget and playtime.c's fs_permissions().
static const u8 perm_check[] = { 0x42, 0x8B, 0xD0, 0x01, 0x25, 0x66 };

static int fs_permissions(int lifted) {
	u8 *p = (u8 *) *(u32 *) 0x80003134, *end = (u8 *) 0x94000000;
	u8 from = lifted ? 0xD0 : 0xE0, to = lifted ? 0xE0 : 0xD0;
	int n = 0;

	if (*(vu32 *) 0xcd800064 != 0xffffffff)
		return -1;
	if ((u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return -2;
	*(vu16 *) 0xcd8b420a = 2;
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

static void save(const char *name, const u8 *data, u32 size) {
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", sd_dir, name);
	f = fopen(path, "wb");
	if (!f) {
		say("  cannot write %s (%d)\n", path, errno);
		return;
	}
	fwrite(data, 1, size, f);
	fclose(f);
	say("  saved %s (%u bytes)\n", path, (unsigned) size);
}

// A U8 archive: 12-byte nodes from the root (whose size is the node count),
// then the names. Returns how many RFL_Res.dat it found.
static int search_u8(const blob *b, u16 index) {
	const u8 *d = b->data, *nodes, *names;
	u32 root, count, i, found = 0;

	if (b->size < 0x20 || be32(d) != U8_MAGIC)
		return 0;
	root = be32(d + 4);
	if (root + 12 > b->size)
		return 0;
	nodes = d + root;
	count = be32(nodes + 8);
	if (!count || root + count * 12 > b->size)
		return 0;
	names = nodes + count * 12;
	for (i = 1; i < count; ++i) {
		const u8 *n = nodes + i * 12;
		u32 name = be32(n) & 0xffffff, off = be32(n + 4), size = be32(n + 8);
		const char *s = (const char *) names + name;

		if (n[0] != 0 || (u32) (s - (const char *) d) >= b->size)
			continue;
		if (!strcasecmp(s, "RFL_Res.dat")) {
			say("  content %u: U8 file RFL_Res.dat at %u, %u bytes, starts %08x %08x\n",
				(unsigned) index, (unsigned) off, (unsigned) size,
				off + 8 <= b->size ? (unsigned) be32(d + off) : 0,
				off + 8 <= b->size ? (unsigned) be32(d + off + 4) : 0);
			if (off + size <= b->size && !found)
				save("RFL_Res.dat", d + off, size);
			++found;
		}
	}
	say("  content %u: U8 archive, %u nodes\n", (unsigned) index, (unsigned) count);
	return found;
}

static void isfs_try(const char *path) {
	static fstats st ATTRIBUTE_ALIGN(32);
	s32 fd = ISFS_Open(path, ISFS_OPEN_READ);

	if (fd < 0) {
		say("  ISFS %s: open %d\n", path, (int) fd);
		return;
	}
	if (ISFS_GetFileStats(fd, &st) >= 0)
		say("  ISFS %s: opens, %u bytes\n", path, (unsigned) st.file_length);
	else
		say("  ISFS %s: opens, no stats\n", path);
	ISFS_Close(fd);
}

int main(int argc, char **argv) {
	static u8 tmd_buf[MAX_SIGNED_TMD_SIZE] ATTRIBUTE_ALIGN(32);
	char path[256];
	tikview *views = NULL;
	u32 tmd_size = 0, nviews = 0, i;
	int found = 0, unlock = 0, lifted = 0;
	tmd *t;
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

	if (argc > 1 && !strcmp(argv[1], "--unlock")) {
		unlock = 1;
		--argc;
		++argv;
	}
	sd_dir = argc > 1 ? argv[1] : "sd:/mii_res";
	if (!fatInitDefault()) {
		printf("mii_res_probe: no SD card\n");
		sleep(5);
		return 1;
	}
	mkdir(sd_dir, 0777);
	snprintf(path, sizeof(path), "%s/result.txt", sd_dir);
	report = fopen(path, "w");
	ISFS_Initialize();
	say("mii_res_probe: IOS %d v%d, AHBPROT %s\n", IOS_GetVersion(), IOS_GetRevision(),
		*(vu32 *) 0xcd800064 == 0xffffffff ? "open" : "closed");

	say("1. RFL's own NAND path\n");
	isfs_try("/shared2/menu/FaceLib/RFL_Res.dat");

	say("2. the System Menu's contents through ES\n");
	if ((r = ES_GetStoredTMDSize(MENU, &tmd_size)) < 0 || tmd_size > sizeof(tmd_buf) ||
			(r = ES_GetStoredTMD(MENU, (signed_blob *) tmd_buf, tmd_size)) < 0) {
		say("  TMD: %d\n", (int) r);
		goto done;
	}
	t = SIGNATURE_PAYLOAD((signed_blob *) tmd_buf);
	say("  System Menu v%u (region %u), %u contents, boot %u\n", (unsigned) t->title_version,
		(unsigned) t->region, (unsigned) t->num_contents, (unsigned) t->boot_index);
	if ((r = ES_GetNumTicketViews(MENU, &nviews)) < 0 || !nviews) {
		say("  ticket views: %d (%u)\n", (int) r, (unsigned) nviews);
	} else {
		views = memalign(32, sizeof(tikview) * nviews);
		if ((r = ES_GetTicketViews(MENU, views, nviews)) < 0) {
			say("  ticket views: %d\n", (int) r);
			free(views);
			views = NULL;
		}
	}
	for (i = 0; i < t->num_contents; ++i) {
		tmd_content *c = &t->contents[i];
		blob b = { NULL, 0 };

		say("  content %u: id %08x, type %04x, %u bytes\n", (unsigned) c->index,
			(unsigned) c->cid, (unsigned) c->type, (unsigned) c->size);
		if (!views || c->size > 16 * 1024 * 1024 || !es_read(&views[0], c->index, (u32) c->size, &b))
			continue;
		found += search_u8(&b, c->index);
		free(b.data);
	}
	say("  RFL_Res.dat found %d time(s) through ES\n", found);

	say("3. the shared contents through /shared1\n");
	{
		blob map = { NULL, 0 };

		if (isfs_read("/shared1/content.map", &map)) {
			for (i = 0; i < t->num_contents; ++i) {
				tmd_content *c = &t->contents[i];
				u32 e;

				if (!(c->type & 0x8000))
					continue;
				for (e = 0; e + 28 <= map.size; e += 28) {
					blob b = { NULL, 0 };

					if (memcmp(map.data + e + 8, c->hash, 20))
						continue;
					snprintf(path, sizeof(path), "/shared1/%.8s.app", (const char *) map.data + e);
					say("  content %u is %s\n", (unsigned) c->index, path);
					if (isfs_read(path, &b)) {
						say("  content %u: starts %08x\n", (unsigned) c->index, (unsigned) be32(b.data));
						found += search_u8(&b, c->index);
						free(b.data);
					}
					break;
				}
			}
			free(map.data);
		}
	}

	say("4. the private contents%s\n", unlock ? ", permission check lifted" : " (no --unlock: one try)");
	if (unlock) {
		lifted = fs_permissions(1);
		say("  permission check lifted at %d site(s)\n", lifted);
	}
	for (i = 0; i < t->num_contents; ++i) {
		tmd_content *c = &t->contents[i];
		blob b = { NULL, 0 };

		if (c->type & 0x8000)
			continue;
		snprintf(path, sizeof(path), "/title/00000001/00000002/content/%08x.app", (unsigned) c->cid);
		if (isfs_read(path, &b)) {
			say("  content %u: starts %08x\n", (unsigned) c->index, (unsigned) be32(b.data));
			found += search_u8(&b, c->index);
			free(b.data);
		}
		if (!unlock)
			break;
	}
	if (lifted > 0)
		say("  permission check restored at %d site(s)\n", fs_permissions(0));
	say("  RFL_Res.dat found %d time(s) in all\n", found);

done:
	say("mii_res_probe: done\n");
	if (report)
		fclose(report);
	fatUnmount("sd:");
	ISFS_Deinitialize();
	free(views);
	sleep(2);
	return 0;
}
