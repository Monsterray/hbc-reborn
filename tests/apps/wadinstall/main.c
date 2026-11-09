// Install a WAD from the SD card through ES, for putting a fakesigned HBC
// build on the bench Wii without a WAD manager and a Wii Remote.
//
// usage (argv from Wiiload): wadinstall.dol SD-WAD [IOS]
//   reloads to IOS (249: a d2x cIOS, whose ES takes fakesigned titles),
//   then adds the ticket, the TMD and each content. Any failure before the
//   last step cancels the install, so nothing half-installed is left. Then
//   reads the installed TMD back and says its version. Writes
//   sd:/wadinstall/result.txt, ending with "done" (installed) or "failed".
//   Exits to the reload stub: HBC, as installed.

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>

extern void __exception_setreload(int t);

#define CHUNK (32 * 1024)

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

static u32 align64(u32 n) {
	return (n + 63) & ~63u;
}

// The WAD, read whole: its sections, each 64-byte aligned in the file.
static u8 *wad;
static u32 wad_size;

static int read_wad(const char *path) {
	FILE *f = fopen(path, "rb");
	long len;

	if (!f || fseek(f, 0, SEEK_END) || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) {
		say("%s: cannot read\n", path);
		if (f)
			fclose(f);
		return 0;
	}
	wad_size = (u32) len;
	wad = memalign(32, align64(wad_size));
	if (!wad || fread(wad, 1, wad_size, f) != wad_size) {
		say("%s: cannot read %u bytes\n", path, wad_size);
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

// A section copied to its own 32-byte aligned buffer, as ES's ioctlv wants.
static void *section(u32 at, u32 size) {
	void *p = memalign(32, align64(size ? size : 1));

	if (p)
		memcpy(p, wad + at, size);
	return p;
}

static int install(void) {
	u32 hdr, certs_n, crl_n, tik_n, tmd_n, data_n, at_certs, at_tik, at_tmd, at_data, at, i;
	signed_blob *certs = NULL, *tik = NULL, *stmd = NULL;
	const tmd *t;
	u8 *chunk = memalign(32, CHUNK);
	u64 tid;
	s32 r;
	int started = 0, ok = 0;

	if (wad_size < 0x20 || !chunk)
		goto out;
	hdr = be32(wad);
	certs_n = be32(wad + 8);
	crl_n = be32(wad + 12);
	tik_n = be32(wad + 16);
	tmd_n = be32(wad + 20);
	data_n = be32(wad + 24);
	at_certs = align64(hdr);
	at_tik = at_certs + align64(certs_n) + align64(crl_n);
	at_tmd = at_tik + align64(tik_n);
	at_data = at_tmd + align64(tmd_n);
	if (hdr != 0x20 || memcmp(wad + 4, "Is", 2) || crl_n || at_data + data_n > wad_size ||
			tmd_n < sizeof(sig_rsa2048) + sizeof(tmd)) {
		say("not a WAD this installs (header %08x, type %.2s, data to %u of %u)\n", hdr, wad + 4,
			at_data + data_n, wad_size);
		goto out;
	}
	certs = section(at_certs, certs_n);
	tik = section(at_tik, tik_n);
	stmd = section(at_tmd, tmd_n);
	if (!certs || !tik || !stmd) {
		say("no memory\n");
		goto out;
	}
	t = SIGNATURE_PAYLOAD(stmd);
	tid = t->title_id;
	say("title %08x-%08x, version %u (%u.%u.%u), %u contents\n", (u32) (tid >> 32), (u32) tid, t->title_version,
		t->title_version >> 11, t->title_version >> 5 & 63, t->title_version & 31, t->num_contents);
	// Every content within the data section before anything is written.
	for (i = 0, at = at_data; i < t->num_contents; ++i) {
		at += align64((u32) ((t->contents[i].size + 15) & ~15ull));
		if (at > at_data + data_n || t->contents[i].size > 0x1000000) {
			say("content %u runs past the data\n", i);
			goto out;
		}
	}

	r = ES_AddTicket(tik, tik_n, certs, certs_n, NULL, 0);
	say("ticket: %d\n", r);
	if (r < 0)
		goto out;
	r = ES_AddTitleStart(stmd, tmd_n, certs, certs_n, NULL, 0);
	say("TMD: %d\n", r);
	if (r < 0)
		goto out;
	started = 1;
	for (i = 0, at = at_data; i < t->num_contents; ++i) {
		const tmd_content *c = &t->contents[i];
		u32 len = (u32) ((c->size + 15) & ~15ull), done;
		s32 cfd = ES_AddContentStart(tid, c->cid);

		if (cfd < 0) {
			say("content %08x: start %d\n", c->cid, cfd);
			goto out;
		}
		for (done = 0; done < len; done += CHUNK) {
			u32 n = len - done < CHUNK ? len - done : CHUNK;

			memcpy(chunk, wad + at + done, n);
			if ((r = ES_AddContentData(cfd, chunk, n)) < 0) {
				say("content %08x: data at %u: %d\n", c->cid, done, r);
				goto out;
			}
		}
		r = ES_AddContentFinish(cfd);
		say("content %08x: %u bytes: %d\n", c->cid, (u32) c->size, r);
		if (r < 0)
			goto out;
		at += align64(len);
	}
	r = ES_AddTitleFinish();
	say("finish: %d\n", r);
	if (r < 0)
		goto out;
	started = 0;
	ok = 1;
	{   // what ES now holds
		static u8 back[0x4000] ATTRIBUTE_ALIGN(32);
		u32 size = 0;

		r = ES_GetStoredTMDSize(tid, &size);
		if (r >= 0 && size <= sizeof(back) && ES_GetStoredTMD(tid, (signed_blob *) back, size) >= 0) {
			const tmd *b = SIGNATURE_PAYLOAD((signed_blob *) back);

			say("installed: version %u (%u.%u.%u), %u contents\n", b->title_version, b->title_version >> 11,
				b->title_version >> 5 & 63, b->title_version & 31, b->num_contents);
			ok = b->title_version == t->title_version;
		} else {
			say("installed, but its TMD does not read back (%d)\n", r);
			ok = 0;
		}
	}
out:
	if (started)
		say("cancelled: %d\n", ES_AddTitleCancel());
	free(certs);
	free(tik);
	free(stmd);
	free(chunk);
	return ok;
}

int main(int argc, char **argv) {
	static char line[2][256];
	const char *path, *ios_arg;
	int ios, ok = 0;
	s32 r;
	GXRModeObj *rmode;
	void *xfb;

	__exception_setreload(8);
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	// Without arguments (Dolphin boots a DOL without any), from
	// sd:/wadinstall/args.txt, one per line. Read before the IOS reload.
	path = argc > 1 ? argv[1] : NULL;
	ios_arg = argc > 2 ? argv[2] : NULL;
	if (!path) {
		FILE *f;

		if (!fatInitDefault() || !(f = fopen("sd:/wadinstall/args.txt", "r"))) {
			printf("usage: wadinstall.dol SD-WAD [IOS]\n");
			sleep(5);
			return 1;
		}
		if (fgets(line[0], sizeof(line[0]), f)) {
			line[0][strcspn(line[0], "\r\n")] = 0;
			path = line[0];
		}
		if (fgets(line[1], sizeof(line[1]), f)) {
			line[1][strcspn(line[1], "\r\n")] = 0;
			ios_arg = line[1];
		}
		fclose(f);
		fatUnmount("sd");
	}
	ios = ios_arg && *ios_arg ? atoi(ios_arg) : 249;
	r = IOS_ReloadIOS(ios);
	if (!fatInitDefault()) {
		printf("wadinstall: no SD card after IOS %d (%d)\n", ios, r);
		sleep(5);
		return 1;
	}
	mkdir("sd:/wadinstall", 0777);
	report = fopen("sd:/wadinstall/result.txt", "w");
	say("wadinstall %s on IOS %d (reload %d, running %d v%d)\n", path ? path : "(none)", ios, r, IOS_GetVersion(),
		IOS_GetRevision());
	if (r < 0 || IOS_GetVersion() != ios)
		say("not on IOS %d: nothing installed\n", ios);
	else if (path && read_wad(path))
		ok = install();
	say("%s\n", ok ? "done" : "failed");
	if (report)
		fclose(report);
	fatUnmount("sd");
	return ok ? 0 : 1;
}
