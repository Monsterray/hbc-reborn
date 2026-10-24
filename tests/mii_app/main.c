// Test app for sdk/mii on a Wii (tests/wii_mii.py drives it).
//
// usage (argv from Wiiload):
//   mii_app.dol check              read-only: the database (slots, the hidden
//                                  database, isolation, which Miis this Wii
//                                  made) and the Miis on Wii Remote 1
//   mii_app.dol add NAME SEED      a random Mii named NAME, made by
//                                  "hbc-reborn" with a new ID, into the first
//                                  free slot; saved, then loaded again and
//                                  compared
//   mii_app.dol remove ID          take out the Mii with that ID (hex), only
//                                  if its creator is "hbc-reborn"
//   mii_app.dol hidden SEED        a random Mii into the hidden database,
//                                  saved and checked, then removed again,
//                                  saved and checked
//   mii_app.dol remote-add ID      copy the database's Mii with that ID to
//                                  the first free slot of Wii Remote 1;
//                                  written and read back
//   mii_app.dol remote-remove ID   take it off the Remote again (creator
//                                  "hbc-reborn" only)
//   mii_app.dol remote-both ID     remote-add, then remote-remove
// Each writes what it did to sd:/mii_test/result.txt (and the screen),
// ending with "done". Names other than this test's are never printed.
// With no arguments at all, they are read from sd:/mii_test/args.txt, one
// per line (for Dolphin, which boots a DOL without arguments:
// tests/dolphin_mii.py), and the app powers off at the end instead of
// returning.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "mii.h"

extern void __exception_setreload(int t);

#define CREATOR "hbc-reborn"

static FILE *report;
static int failures;

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

static void result(const char *what, int r) {
	say("%s: %d (%s)\n", what, r, mii_strerror(r));
	if (r < 0)
		++failures;
}

static void id_text(const uint8_t *id, char *out) {
	int i;

	for (i = 0; i < MII_ID_SIZE; ++i)
		sprintf(out + 2 * i, "%02x", id[i]);
}

static int parse_id(const char *s, uint8_t id[MII_ID_SIZE]) {
	unsigned v;
	int i;

	for (i = 0; i < MII_ID_SIZE; ++i) {
		if (!s[0] || !s[1] || sscanf(s, "%2x", &v) != 1)
			return 0;
		id[i] = (uint8_t) v;
		s += 2;
	}
	return 1;
}

static int ours(const mii *m) {
	char who[32];

	mii_get_name(m->creator, who, sizeof(who));
	return !strcmp(who, CREATOR);
}

static void describe(const char *where, int slot, const mii *m) {
	const char *why = NULL;
	char id[17], name[32] = "-";
	int r = mii_check(m, &why);

	id_text(m->id, id);
	if (ours(m))
		mii_get_name(m->name, name, sizeof(name));
	say("%s %d: id %s flags %08x %s, %s, check %d%s%s, home %d, local %d, fav %d, test name %s\n", where,
		slot, id, (unsigned) mii_id_flags(m->id), m->sex ? "female" : "male", ours(m) ? "ours" : "not ours",
		r, why ? " " : "", why ? why : "", mii_is_home(m), m->local_only, m->favorite, name);
}

static int wait_remote(int chan) {
	u64 end = gettime() + secs_to_ticks(15);
	u32 type;

	while (gettime() < end) {
		if (WPAD_Probe(chan, &type) == WPAD_ERR_NONE)
			return 1;
		usleep(100 * 1000);
	}
	return 0;
}

/* Whether this Wii's home word (from its Wi-Fi MAC) is the one in the first
 * Mii's ID: a Mii made on this Wii proves the ID recipe. The MAC is not
 * printed. */
static void mac_check(const mii *first) {
	u8 mac[6];
	u32 word = first ? (u32) first->id[4] << 24 | first->id[5] << 16 | first->id[6] << 8 | first->id[7] : 0;
	int r = mii_wifi_mac(mac);

	say("mii_wifi_mac: %d; home word %s the first Mii's\n", r,
		r < 0 ? "-" : mii_id_home(mac) == word ? "matches" : "differs from");
}

static void check(void) {
	mii_db db;
	mii_remote rm;
	mii m;
	int r, i, n = 0;
	u64 t0 = gettime();

	r = mii_db_load(&db);
	result("mii_db_load", r);
	if (r >= 0) {
		say("loaded in %llu ms: %d Miis, %d hidden, isolation %d, home word %s\n",
			ticks_to_millisecs(gettime() - t0), mii_db_count(&db), mii_hidden_count(&db), mii_db_isolated(&db),
			db.home ? "known" : "unknown");
		for (i = 0; i < MII_DB_SLOTS; ++i)
			if (mii_db_get(&db, i, &m) == MII_OK)
				describe("slot", i, &m);
		for (i = mii_hidden_first(&db); i >= 0 && n < 20; i = mii_hidden_next(&db, i), ++n)
			if (mii_hidden_get(&db, i, &m) == MII_OK)
				describe("hidden", i, &m);
		mii_db_free(&db);
	}
	for (i = 0; i < MII_DB_SLOTS && mii_get(i, &m) != MII_OK; ++i)
		;
	say("mii_get: first Mii in slot %d\n", i);
	mac_check(i < MII_DB_SLOTS ? &m : NULL);

	if (!wait_remote(0)) {
		say("no Wii Remote 1 within 15 s\n");
		return;
	}
	t0 = gettime();
	r = mii_remote_read(0, &rm);
	result("mii_remote_read", r);
	if (r >= 0) {
		say("read in %llu ms: copy %d, secret %03x\n", ticks_to_millisecs(gettime() - t0), rm.copy, rm.secret);
		for (i = 0; i < MII_REMOTE_SLOTS; ++i)
			if (mii_remote_get(&rm, i, &m) == MII_OK)
				describe("remote", i, &m);
	}
}

static int reload_has(const uint8_t id[MII_ID_SIZE], const mii *want, int hidden) {
	mii_db db;
	mii m;
	uint8_t a[MII_SIZE], b[MII_SIZE];
	int r = mii_db_load(&db), i;

	if (r < 0) {
		result("reload", r);
		return 0;
	}
	i = hidden ? mii_hidden_find(&db, id) : mii_db_find(&db, id);
	if (i >= 0 && want) {
		r = hidden ? mii_hidden_get(&db, i, &m) : mii_db_get(&db, i, &m);
		mii_pack(&m, a);
		mii_pack(want, b);
		if (r < 0 || memcmp(a, b, hidden ? 0x36 : MII_SIZE))
			i = -100;
	}
	mii_db_free(&db);
	return i;
}

static void add(const char *name, uint32_t seed) {
	mii_db db;
	mii m;
	char id[17];
	int r, slot;
	u64 t0;

	mii_random(&m, MII_ANY, MII_ADULT, &seed);
	mii_set_name(m.name, name);
	mii_set_name(m.creator, CREATOR);
	result("mii_make_id", mii_make_id(&m));
	id_text(m.id, id);
	say("new Mii %s, home %d\n", id, mii_is_home(&m));
	if ((r = mii_db_load(&db)) < 0) {
		result("mii_db_load", r);
		return;
	}
	slot = mii_db_add(&db, &m);
	result("mii_db_add", slot);
	if (slot >= 0) {
		t0 = gettime();
		r = mii_db_save(&db);
		result("mii_db_save", r);
		say("saved in %llu ms\n", ticks_to_millisecs(gettime() - t0));
		r = reload_has(m.id, &m, 0);
		say("reloaded: slot %d (%s)\n", r, r == slot ? "the same Mii" : "MISMATCH");
		if (r != slot)
			++failures;
		mii_db_get(&db, slot, &m);
		describe("slot", slot, &m);
	}
	mii_db_free(&db);
}

static void remove_id(const char *text) {
	uint8_t id[MII_ID_SIZE];
	mii_db db;
	mii m;
	int r, slot;

	if (!parse_id(text, id)) {
		result("bad id", MII_E_ARG);
		return;
	}
	if ((r = mii_db_load(&db)) < 0) {
		result("mii_db_load", r);
		return;
	}
	slot = mii_db_find(&db, id);
	if (slot >= 0 && mii_db_get(&db, slot, &m) == MII_OK && ours(&m)) {
		result("mii_db_remove", mii_db_remove(&db, slot));
		result("mii_db_save", mii_db_save(&db));
		r = reload_has(id, NULL, 0);
		say("reloaded: %s\n", r == MII_E_EMPTY ? "gone" : "STILL THERE");
		if (r != MII_E_EMPTY)
			++failures;
	} else {
		result("not one of this test's Miis", slot < 0 ? slot : MII_E_ARG);
	}
	mii_db_free(&db);
}

static void hidden(uint32_t seed) {
	mii_db db;
	mii m;
	char id[17];
	int r, i, before;

	mii_random(&m, MII_ANY, MII_ANY, &seed);
	mii_set_name(m.name, "HBC hidden");
	mii_set_name(m.creator, CREATOR);
	result("mii_make_id", mii_make_id(&m));
	id_text(m.id, id);
	say("new Mii %s\n", id);
	if ((r = mii_db_load(&db)) < 0) {
		result("mii_db_load", r);
		return;
	}
	before = mii_hidden_count(&db);
	i = mii_hidden_put(&db, &m);
	result("mii_hidden_put", i);
	if (i >= 0) {
		result("mii_db_save", mii_db_save(&db));
		r = reload_has(m.id, &m, 1);
		say("reloaded: hidden %d (%s)\n", r, r == i ? "the same Mii" : "MISMATCH");
		if (r != i)
			++failures;
		result("mii_hidden_remove", mii_hidden_remove(&db, i));
		result("mii_db_save", mii_db_save(&db));
		r = reload_has(m.id, NULL, 1);
		say("reloaded: %s\n", r == MII_E_EMPTY ? "gone" : "STILL THERE");
		if (r != MII_E_EMPTY)
			++failures;
		say("hidden count %d before, %d after\n", before, mii_hidden_count(&db));
	}
	mii_db_free(&db);
}

static void remote_change(const char *text, int adding) {
	uint8_t id[MII_ID_SIZE];
	mii_remote rm, back;
	mii m;
	int r, i, slot = -1;
	u64 t0;

	if (!parse_id(text, id)) {
		result("bad id", MII_E_ARG);
		return;
	}
	if (adding) {
		mii_db db;
		if ((r = mii_db_load(&db)) < 0) {
			result("mii_db_load", r);
			return;
		}
		r = mii_db_find(&db, id);
		if (r >= 0)
			r = mii_db_get(&db, r, &m);
		mii_db_free(&db);
		if (r < 0 || !ours(&m)) {
			result("not one of this test's Miis in the database", r < 0 ? r : MII_E_ARG);
			return;
		}
	}
	if (!wait_remote(0)) {
		result("no Wii Remote 1 within 15 s", MII_E_NO_REMOTE);
		return;
	}
	if ((r = mii_remote_read(0, &rm)) < 0) {
		result("mii_remote_read", r);
		return;
	}
	say("read: copy %d\n", rm.copy);
	for (i = 0; i < MII_REMOTE_SLOTS; ++i) {
		mii x;
		if (mii_remote_get(&rm, i, &x) == MII_OK) {
			if (mii_same_id(x.id, id))
				slot = i;
		} else if (adding && slot < 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		result(adding ? "no free slot on the Remote" : "that Mii is not on the Remote",
			   adding ? MII_E_FULL : MII_E_EMPTY);
		return;
	}
	if (adding) {
		r = mii_remote_put(&rm, slot, &m);
	} else {
		r = mii_remote_get(&rm, slot, &m);
		r = r < 0 ? r : !ours(&m) ? MII_E_ARG : mii_remote_remove(&rm, slot);
	}
	result(adding ? "mii_remote_put" : "mii_remote_remove", r);
	if (r < 0)
		return;
	t0 = gettime();
	r = mii_remote_write(0, &rm);
	result("mii_remote_write", r);
	say("written in %llu ms, slot %d\n", ticks_to_millisecs(gettime() - t0), slot);
	r = mii_remote_read(0, &back);
	result("mii_remote_read again", r);
	if (r >= 0) {
		r = memcmp(back.raw, rm.raw, sizeof(rm.raw)) ? MII_E_VERIFY : MII_OK;
		result("the Remote holds what was written", r);
		say("read back: copy %d\n", back.copy);
	}
}

/* Arguments from sd:/mii_test/args.txt, one per line. */
static int sd_args(char **argv, int max) {
	static char text[512];
	FILE *f = fopen("sd:/mii_test/args.txt", "r");
	size_t n;
	int argc = 1;
	char *p;

	if (!f)
		return 1;
	n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = 0;
	for (p = strtok(text, "\r\n"); p && argc < max; p = strtok(NULL, "\r\n"))
		argv[argc++] = p;
	return argc;
}

int main(int argc, char **argv) {
	static char *sd_argv[8];
	void *xfb;
	GXRModeObj *rmode;
	int i, from_sd = 0;

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
	WPAD_Init();

	if (fatInitDefault()) {
		mkdir("sd:/mii_test", 0777);
		if (argc < 2) {
			sd_argv[0] = "mii_app";
			argc = sd_args(sd_argv, 8);
			argv = sd_argv;
			from_sd = 1;
		}
		report = fopen("sd:/mii_test/result.txt", "w");
	}
	say("mii_app:");
	for (i = 1; i < argc; ++i)
		say(" [%s]", argv[i]);
	say("\n");
	if (argc == 2 && !strcmp(argv[1], "check"))
		check();
	else if (argc == 4 && !strcmp(argv[1], "add"))
		add(argv[2], (uint32_t) strtoul(argv[3], NULL, 0));
	else if (argc == 3 && !strcmp(argv[1], "remove"))
		remove_id(argv[2]);
	else if (argc == 3 && !strcmp(argv[1], "hidden"))
		hidden((uint32_t) strtoul(argv[2], NULL, 0));
	else if (argc == 3 && !strcmp(argv[1], "remote-add"))
		remote_change(argv[2], 1);
	else if (argc == 3 && !strcmp(argv[1], "remote-remove"))
		remote_change(argv[2], 0);
	else if (argc == 3 && !strcmp(argv[1], "remote-both")) {
		remote_change(argv[2], 1);
		remote_change(argv[2], 0);
	}
	else
		say("usage: check | add NAME SEED | remove ID | hidden SEED | remote-add ID | remote-remove ID\n");
	say("failures %d\ndone\n", failures);
	if (report) {
		fclose(report);
		fatUnmount("sd:");
	}
	WPAD_Shutdown();
	ISFS_Deinitialize();
	if (from_sd)
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);   /* Dolphin in batch mode closes */
	return 0;
}
