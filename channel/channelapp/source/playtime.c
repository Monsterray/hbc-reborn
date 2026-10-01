// The Wii Menu's play record, and HBC's own play log on the Message Board.
//
// The Wii Menu writes play_rec.dat when it launches a title and logs it to
// the Message Board at its next start. HBC spoils that record, as it always
// has: one record cannot tell HBC from the apps it runs. Instead HBC logs
// each session itself, straight into the Message Board's play log in
// cdb.vff (cdblog.c, docs/messageboard.md): its own time in the menu, and
// each app it launched, timed from the launch to HBC's return. A pending
// launch is kept in HBC's data folder so it survives the app.
//
// cdb.vff belongs to the Wii Menu; IOS refuses HBC its folder. With AHBPROT,
// HBC lifts IOS's NAND permission check for the moment it writes, and puts
// it back. Without AHBPROT, or from a DOL with no installed title, there is
// no play log.

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../config.h"
#include "cdblog.h"
#include "devnet.h"
#include "isfs.h"
#include "playtime.h"
#include "title.h"

static char _playtime_path[] __attribute__((aligned(32))) =
	"/title/00000001/00000002/data/play_rec.dat";
static char cdb_path[] __attribute__((aligned(32))) =
	"/title/00000001/00000002/data/cdb.vff";
static char wiiid_path[] __attribute__((aligned(32))) =
	"/title/00000001/00000002/data/nocopy/cdbwiiid.dat";

#define PLAYTIME_THREAD_PRIO 40
#define PLAYTIME_STACK (16 * 1024)

// A play record the Wii Menu wrote this long before HBC started means the
// Wii Menu launched HBC (and not HBC's reload stub, after an app).
#define FROM_MENU_TICKS (120 * CDBLOG_TICKS_PER_SEC)
// No app session is logged as longer than this: the clock was likely changed.
#define MAX_SESSION_TICKS (24 * 3600 * CDBLOG_TICKS_PER_SEC)

#define MEM2_PROT 0x0D8B420A
#define STATE_MAGIC 0x4842504c  // 'HBPL'

typedef struct {
	u32 magic;
	u32 pending;  // an app was launched and has not come back yet
	cdblog_session app;
} state;

static lwp_t pt_thread = LWP_THREAD_NULL;
static u8 pt_stack[PLAYTIME_STACK] ATTRIBUTE_ALIGN(32);
static u64 hbc_boot, session_start;

// ---- IOS's NAND permission check ------------------------------------------

// In IOS's FS module: `cmp r3, r1; beq allowed`. The patch makes the branch
// unconditional (beq -> b). The same signature is patched by other homebrew
// (libruntimeiospatch); HBC writes it only around its own cdb.vff access.
static const u8 perm_check[] = { 0x42, 0x8B, 0xD0, 0x01, 0x25, 0x66 };

static bool ahbprot(void) {
	return read32(0x0d800064) == 0xffffffff;
}

// Returns how many sites changed.
static int fs_permissions(bool lifted) {
	u8 *p = (u8 *) *(u32 *) 0x80003134, *end = (u8 *) 0x94000000;
	u8 from = lifted ? 0xD0 : 0xE0, to = lifted ? 0xE0 : 0xD0;
	int n = 0;

	if (!ahbprot() || (u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return 0;
	write16(MEM2_PROT, 2);
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

// ---- cdb.vff through IOS --------------------------------------------------

#define BOUNCE (8 * 1024)
static u8 bounce[BOUNCE] ATTRIBUTE_ALIGN(32);

static int ios_read(void *ctx, u32 off, void *buf, u32 len) {
	s32 fd = (s32) ctx;
	u8 *out = buf;

	while (len) {
		u32 n = len < BOUNCE ? len : BOUNCE;
		if (IOS_Seek(fd, off, SEEK_SET) != (s32) off || IOS_Read(fd, bounce, n) != (s32) n)
			return -1;
		memcpy(out, bounce, n);
		out += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int ios_write(void *ctx, u32 off, const void *buf, u32 len) {
	s32 fd = (s32) ctx;
	const u8 *in = buf;

	while (len) {
		u32 n = len < BOUNCE ? len : BOUNCE;
		memcpy(bounce, in, n);
		if (IOS_Seek(fd, off, SEEK_SET) != (s32) off || IOS_Write(fd, bounce, n) != (s32) n)
			return -1;
		in += n;
		off += n;
		len -= n;
	}
	return 0;
}

static const char *result_name(int res) {
	switch (res) {
	case CDBLOG_ADDED: return "added";
	case CDBLOG_COMBINED: return "added to its line";
	case CDBLOG_CREATED: return "a new day";
	case CDBLOG_E_IO: return "NAND error";
	case CDBLOG_E_FORMAT: return "unknown format, nothing written";
	case CDBLOG_E_FULL: return "the day is full";
	default: return "out of memory";
	}
}

static void log_session(const cdblog_session *s) {
	static u8 wiiid[32] ATTRIBUTE_ALIGN(32);
	cdblog_io io;
	bool have_id = false;
	int patched, res;
	s32 fd;

	if (!ahbprot() || !title_get_path()[0] || s->end <= s->start)
		return;
	patched = fs_permissions(true);
	fd = IOS_Open(wiiid_path, IPC_OPEN_READ);
	if (fd >= 0) {
		have_id = IOS_Read(fd, wiiid, sizeof(wiiid)) >= 8;
		IOS_Close(fd);
	}
	fd = IOS_Open(cdb_path, IPC_OPEN_RW);
	if (fd < 0) {
		res = fd;
		hlog("Play log: cannot open the Message Board (%d)\n", (int) fd);
	} else {
		io.ctx = (void *) fd;
		io.read = ios_read;
		io.write = ios_write;
		res = cdblog_add(&io, s, gettime(), have_id ? wiiid : NULL);
		IOS_Close(fd);
		hlog("Play log: %u min, %s\n", (unsigned) ((s->end - s->start) / CDBLOG_TICKS_PER_SEC / 60),
			 result_name(res));
	}
	if (patched)
		fs_permissions(false);
	devnet_boot_mark(res >= 0 ? "playlog_logged" : "playlog_failed");
}

// ---- HBC's state: a launch that has not come back yet ---------------------

static void state_path(char *buf, size_t size) {
	snprintf(buf, size, "%s/playlog.bin", title_get_path());
}

static bool state_get(state *st) {
	STACK_ALIGN(char, path, 64, 32);
	u8 *buf = NULL;
	s32 n;

	if (!title_get_path()[0])
		return false;
	state_path(path, 64);
	n = isfs_get(path, &buf, sizeof(*st), sizeof(*st), false);
	if (n == sizeof(*st))
		memcpy(st, buf, sizeof(*st));
	free(buf);
	return n == sizeof(*st) && st->magic == STATE_MAGIC;
}

static void state_put(const state *st) {
	STACK_ALIGN(char, path, 64, 32);
	static state copy ATTRIBUTE_ALIGN(32);

	if (!title_get_path()[0])
		return;
	state_path(path, 64);
	copy = *st;
	isfs_put(path, &copy, sizeof(copy));
}

// ---- The Wii Menu's play record -------------------------------------------

static bool record_valid(const u8 *r) {
	u32 sum = 0, i;

	for (i = 1; i < 32; ++i)
		sum += *(const u32 *) (r + 4 * i);
	return sum && sum == *(const u32 *) r;
}

// Spoil the Wii Menu's record (so it cannot log HBC too), and tell whether
// it was a fresh one for this start: then the Wii Menu launched HBC, at the
// time it gives.
static bool playtime_clear(bool *from_menu, u64 *menu_start) {
	static u8 rec[128] __attribute__((aligned(32)));
	s32 res;
	s32 pt_fd = -1;

	*from_menu = false;
	pt_fd = IOS_Open(_playtime_path, IPC_OPEN_RW);
	if (pt_fd < 0) {
		gprintf("playtime open failed: %d\n", pt_fd);
		return false;
	}
	if (IOS_Read(pt_fd, rec, sizeof(rec)) == sizeof(rec) && record_valid(rec)) {
		u64 start = *(u64 *) (rec + 0x58);
		if (start <= hbc_boot && hbc_boot - start < FROM_MENU_TICKS) {
			*from_menu = true;
			*menu_start = start;
		}
	}
	IOS_Seek(pt_fd, 0, SEEK_SET);
	memset(rec, 0, 4);
	res = IOS_Write(pt_fd, rec, 4);
	IOS_Close(pt_fd);
	if (res != 4) {
		gprintf("error destroying playtime (%d)\n", res);
		return false;
	}
	return true;
}

static void *playtime_func(void *arg) {
	bool from_menu;
	u64 menu_start = 0;
	state st;

	(void) arg;
	devnet_boot_mark(playtime_clear(&from_menu, &menu_start) ? "playtime_cleared" : "playtime_failed");
	session_start = from_menu ? menu_start : hbc_boot;

	// An app HBC launched: back through HBC's reload stub, it ran until this
	// start. After the Wii Menu (the app went there, or the Wii was off), its
	// end is unknown, so it is not logged.
	if (state_get(&st) && st.pending) {
		st.pending = 0;
		state_put(&st);
		st.app.end = hbc_boot;
		if (from_menu)
			hlog("Play log: the last app's end is unknown (the Wii Menu ran since)\n");
		else if (st.app.end > st.app.start && st.app.end - st.app.start < MAX_SESSION_TICKS)
			log_session(&st.app);
	}
	return NULL;
}

void playtime_destroy(u64 started) {
	if (pt_thread != LWP_THREAD_NULL)
		return;
	hbc_boot = session_start = started;
	if (LWP_CreateThread(&pt_thread, playtime_func, NULL, pt_stack, PLAYTIME_STACK,
			PLAYTIME_THREAD_PRIO) < 0) {
		pt_thread = LWP_THREAD_NULL;
		playtime_func(NULL);
	}
}

void playtime_wait(void) {
	if (pt_thread == LWP_THREAD_NULL)
		return;
	LWP_JoinThread(pt_thread, NULL);
	pt_thread = LWP_THREAD_NULL;
}

// ---- Leaving HBC ----------------------------------------------------------

static void utf8_to_name(u16 *out, const char *s) {
	const u8 *p = (const u8 *) s;
	int n = 0;

	memset(out, 0, 40 * sizeof(u16));
	while (*p && n < 40) {
		u32 c = *p++;
		if (c >= 0xc0 && c < 0xe0 && (*p & 0xc0) == 0x80)
			c = (c & 0x1f) << 6 | (*p++ & 0x3f);
		else if (c >= 0xe0 && c < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
			c = (c & 0x0f) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f);
			p += 2;
		} else if (c >= 0x80) {
			c = '?';
		}
		out[n++] = (u16) c;
	}
}

// A six-character ASCII ID from the app's folder: the Message Board shows
// only the name, and HBC combines a day's sessions of the same name and ID.
static void folder_id(char *id, const char *dir) {
	int n = 0;

	memset(id, 0, 6);
	for (; dir && *dir && n < 6; ++dir) {
		char c = *dir;
		if (c >= 'a' && c <= 'z')
			c -= 32;
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			id[n++] = c;
	}
	if (!n)
		memcpy(id, "HBAPP", 5);
}

void playtime_leave(const char *app_name, const char *app_dir) {
	cdblog_session hbc;
	u64 now = gettime();
	u32 tid = (u32) (MY_TITLEID & 0xffffffff);
	state st;

	playtime_wait();
	if (!ahbprot() || !title_get_path()[0])
		return;
	memset(&hbc, 0, sizeof(hbc));
	utf8_to_name(hbc.name, "Homebrew Channel");
	memcpy(hbc.id, &tid, 4);
	hbc.start = session_start;
	hbc.end = now;
	log_session(&hbc);

	if (app_name) {
		memset(&st, 0, sizeof(st));
		st.magic = STATE_MAGIC;
		st.pending = 1;
		utf8_to_name(st.app.name, app_name);
		folder_id(st.app.id, app_dir);
		st.app.start = gettime();
		state_put(&st);
	}
}
