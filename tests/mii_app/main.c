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
//   mii_app.dol edit ID FIELD=N... change this test's Mii (creator
//                                  "hbc-reborn"): any mii.h field, name=TEXT,
//                                  birthday=M/D, mingle=0|1
//   mii_app.dol draw [SLOT]        sdk/mii's drawing: the Mii in SLOT (or the
//                                  next one there) built from sd:/mii/
//                                  RFL_Res.dat, its icon (normal, smiling;
//                                  and 256 px)
//                                  and a full-body capture, saved as
//                                  sd:/mii_test/*.rgba and checked
//   mii_app.dol walk COUNTS|max [SECONDS]
//                                  a crowd walking in from the horizon,
//                                  then off into it, with levels of
//                                  detail (walk_test)
//   mii_app.dol remote-rewrite     write Wii Remote 1's own Miis back
//                                  unchanged, after saving its memory to
//                                  sd:/mii_test/remote_backup.bin
// Each writes what it did to sd:/mii_test/result.txt (and the screen),
// ending with "done". Names other than this test's are never printed.
// With no arguments at all, they are read from sd:/mii_test/args.txt, one
// per line (for Dolphin, which boots a DOL without arguments:
// tests/dolphin_mii.py), and the app powers off at the end instead of
// returning.

#include <math.h>
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

#include <malloc.h>

#include "mii.h"
#include "miidraw.h"

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
	{
		mii a, b;

		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		r = mii_make_id(&a);
		if (r >= 0)
			r = mii_make_id(&b);
		result("mii_make_id twice in a row", r < 0 ? r : mii_same_id(a.id, b.id) ? MII_E_EXISTS : MII_OK);
	}

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

/* The numeric fields edit can set, by name (mii.h). */
static const struct {
	const char *name;
	size_t at;
} fields[] = {
#define FIELD(f) { #f, offsetof(mii, f) }
	FIELD(sex), FIELD(birth_month), FIELD(birth_day), FIELD(fav_color), FIELD(favorite), FIELD(height),
	FIELD(build), FIELD(face_type), FIELD(face_color), FIELD(face_tex), FIELD(local_only), FIELD(hair_type),
	FIELD(hair_color), FIELD(hair_flip), FIELD(brow_type), FIELD(brow_rotate), FIELD(brow_color),
	FIELD(brow_scale), FIELD(brow_y), FIELD(brow_x), FIELD(eye_type), FIELD(eye_rotate), FIELD(eye_y),
	FIELD(eye_color), FIELD(eye_scale), FIELD(eye_x), FIELD(nose_type), FIELD(nose_scale), FIELD(nose_y),
	FIELD(mouth_type), FIELD(mouth_color), FIELD(mouth_scale), FIELD(mouth_y), FIELD(glass_type),
	FIELD(glass_color), FIELD(glass_scale), FIELD(glass_y), FIELD(mustache), FIELD(beard_type),
	FIELD(beard_color), FIELD(beard_scale), FIELD(beard_y), FIELD(mole), FIELD(mole_scale), FIELD(mole_y),
	FIELD(mole_x),
#undef FIELD
};

/* Change this test's Mii in the database: FIELD=N for any field above,
 * name=TEXT, birthday=M/D (0/0 for none), mingle=0|1 (local_only's
 * opposite, as the Mii Channel shows it). Checked, saved, reloaded. */
static void edit(const char *text, int n, char **sets) {
	uint8_t id[MII_ID_SIZE], a[MII_SIZE], b[MII_SIZE];
	mii_db db;
	mii m, back;
	const char *why = NULL;
	int r, slot, i, k;

	if (!parse_id(text, id)) {
		result("bad id", MII_E_ARG);
		return;
	}
	if ((r = mii_db_load(&db)) < 0) {
		result("mii_db_load", r);
		return;
	}
	slot = mii_db_find(&db, id);
	if (slot < 0 || mii_db_get(&db, slot, &m) != MII_OK || !ours(&m)) {
		result("not one of this test's Miis", slot < 0 ? slot : MII_E_ARG);
		mii_db_free(&db);
		return;
	}
	describe("before: slot", slot, &m);
	for (i = 0; i < n; ++i) {
		char *eq = strchr(sets[i], '=');
		const char *key = sets[i], *val = eq ? eq + 1 : "";
		unsigned mo, d;

		if (!eq) {
			result("not FIELD=VALUE", MII_E_ARG);
			mii_db_free(&db);
			return;
		}
		*eq = 0;
		if (!strcmp(key, "name")) {
			mii_set_name(m.name, val);
		} else if (!strcmp(key, "birthday") && sscanf(val, "%u/%u", &mo, &d) == 2) {
			m.birth_month = (uint8_t) mo;
			m.birth_day = (uint8_t) d;
		} else if (!strcmp(key, "mingle")) {
			m.local_only = !atoi(val);
		} else {
			for (k = 0; k < (int) (sizeof(fields) / sizeof(fields[0])) && strcmp(fields[k].name, key); ++k)
				;
			if (k == (int) (sizeof(fields) / sizeof(fields[0]))) {
				say("unknown field %s\n", key);
				result("edit", MII_E_ARG);
				mii_db_free(&db);
				return;
			}
			((uint8_t *) &m)[fields[k].at] = (uint8_t) atoi(val);
		}
		say("set %s=%s\n", key, val);
	}
	r = mii_check(&m, &why);
	if (r < 0)
		say("mii_check: %s\n", why);
	r = r < 0 ? r : mii_db_put(&db, slot, &m);
	result("mii_db_put", r);
	if (r >= 0)
		result("mii_db_save", mii_db_save(&db));
	mii_db_free(&db);
	if (r < 0)
		return;
	if ((r = mii_db_load(&db)) < 0) {
		result("reload", r);
		return;
	}
	r = mii_db_get(&db, slot, &back);
	mii_pack(&m, a);
	mii_pack(&back, b);
	result("reloaded, the same Mii", r < 0 ? r : memcmp(a, b, MII_SIZE) ? MII_E_VERIFY : MII_OK);
	describe("after: slot", slot, &back);
	say("height %u, build %u, birthday %u/%u, favorite %u, colour %u, mingle %u\n", back.height, back.build,
		back.birth_month, back.birth_day, back.favorite, back.fav_color, !back.local_only);
	mii_db_free(&db);
}

/* Write the Remote's own set back unchanged: the write path on a full Remote
 * whose Miis are not this test's. Its memory is saved to
 * sd:/mii_test/remote_backup.bin first. */
static void remote_rewrite(void) {
	static u8 before[2 * MII_REMOTE_BLOCK], after[2 * MII_REMOTE_BLOCK];
	mii_remote rm, back;
	FILE *f;
	int r, n = 0, i;
	u64 t0;

	if (!wait_remote(0)) {
		result("no Wii Remote 1 within 15 s", MII_E_NO_REMOTE);
		return;
	}
	if ((r = mii_remote_read_raw(0, before)) < 0) {
		result("mii_remote_read_raw", r);
		return;
	}
	f = fopen("sd:/mii_test/remote_backup.bin", "wb");
	if (!f || fwrite(before, 1, sizeof(before), f) != sizeof(before)) {
		if (f)
			fclose(f);
		result("saving remote_backup.bin", MII_E_IO);
		return;
	}
	fclose(f);
	say("remote_backup.bin: %u bytes saved\n", (unsigned) sizeof(before));
	mii_remote_parse(before, sizeof(before), &rm);
	for (i = 0; i < MII_REMOTE_SLOTS; ++i)
		n += !mii_id_empty(rm.raw[i] + 0x18);
	say("read: copy %d, %d Miis, secret %03x\n", rm.copy, n, rm.secret);
	if (rm.copy < 0) {
		result("no whole copy to write back", MII_E_FORMAT);
		return;
	}
	t0 = gettime();
	r = mii_remote_write(0, &rm);
	result("mii_remote_write", r);
	say("written in %llu ms\n", ticks_to_millisecs(gettime() - t0));
	if ((r = mii_remote_read_raw(0, after)) < 0) {
		result("mii_remote_read_raw again", r);
		return;
	}
	mii_remote_parse(after, sizeof(after), &back);
	result("the same Miis read back",
		   memcmp(back.raw, rm.raw, sizeof(rm.raw)) || back.secret != rm.secret ? MII_E_VERIFY : MII_OK);
	result("both copies the same", memcmp(after, after + MII_REMOTE_BLOCK, MII_REMOTE_BLOCK) ? MII_E_VERIFY : MII_OK);
	say("copy 0 bytes unchanged from before: %s; copy 1: %s\n",
		memcmp(after, before, MII_REMOTE_BLOCK) ? "no" : "yes",
		memcmp(after + MII_REMOTE_BLOCK, before + MII_REMOTE_BLOCK, MII_REMOTE_BLOCK) ? "no" : "yes");
}

/* ---- draw: sdk/mii's drawing (miidraw.h) ---------------------------------- */

/* A GX_TF_RGBA8 texture (4x4 tiles: 32 bytes AR, then 32 GB) to
 * sd:/mii_test/NAME.rgba, linear RGBA after a "W H 0 0" line; returns the
 * pixels with alpha, and those of the four corners with alpha. */
static int save_rgba8(const char *name, const u8 *tex, int w, int h, int *corner_alpha) {
	char path[64];
	FILE *f;
	int x, y, n = 0;

	snprintf(path, sizeof(path), "sd:/mii_test/%s.rgba", name);
	f = fopen(path, "wb");
	if (f)
		fprintf(f, "%d %d 0 0\n", w, h);
	*corner_alpha = 0;
	for (y = 0; y < h; ++y)
		for (x = 0; x < w; ++x) {
			const u8 *t = tex + ((y / 4) * (w / 4) + x / 4) * 64;
			int k = (y % 4) * 4 + x % 4;
			u8 p[4] = { t[2 * k + 1], t[32 + 2 * k], t[33 + 2 * k], t[2 * k] };

			if (f)
				fwrite(p, 1, 4, f);
			n += p[3] != 0;
			if ((x < 4 || x >= w - 4) && (y < 4 || y >= h - 4))
				*corner_alpha += p[3] != 0;
		}
	if (f)
		fclose(f);
	return n;
}

static void draw_test(int slot) {
	static u8 fifo[256 * 1024] ATTRIBUTE_ALIGN(32);
	static mii_db db;
	static mii_res res;
	static mii_model model;
	u8 *icon = memalign(32, 96 * 96 * 4), *body = memalign(32, 128 * 256 * 4);
	mii m;
	mii_pose pose;
	u64 t0;
	int r, n, corners, e;

	GX_Init(fifo, sizeof(fifo));
	if ((r = mii_res_load(&res, NULL)) < 0) {
		result("mii_res_load sd:/mii/RFL_Res.dat (HOME > HBC > Save Mii faces)", r);
		return;
	}
	say("RFL_Res.dat: version %#x, %u bytes\n", res.version, (unsigned) res.size);
	if ((r = mii_db_load(&db)) < 0) {
		result("mii_db_load", r);
		return;
	}
	for (; slot < MII_DB_SLOTS && mii_db_get(&db, slot, &m) < 0; ++slot)
		;
	if (slot >= MII_DB_SLOTS) {
		result("a Mii in the database", MII_E_EMPTY);
		return;
	}
	say("slot %d\n", slot);
	t0 = gettime();
	r = mii_model_make(&res, &m, 128, &model);
	result("mii_model_make", r);
	if (r < 0)
		return;
	say("built in %llu ms\n", ticks_to_millisecs(gettime() - t0));
	for (e = 0; e < 2; ++e) {
		char name[16];
		static const char *names[] = { "normal", "smile" };

		r = mii_icon(&model, e ? MII_EXPR_SMILE : MII_EXPR_NORMAL, 96, (GXColor) { 0, 0, 0, 0 }, icon);
		result("mii_icon", r);
		GX_DrawDone();
		snprintf(name, sizeof(name), "icon_%s", names[e]);
		n = save_rgba8(name, icon, 96, 96, &corners);
		say("%s: %d of %d pixels drawn, %d in the corners\n", name, n, 96 * 96, corners);
		result("an icon with a head in it and clear corners",
			   n > 96 * 96 / 4 && !corners ? MII_OK : MII_E_VERIFY);
	}
	/* A large one, as an app would show a Mii picked from a list. */
	{
		u8 *big = memalign(32, 256 * 256 * 4);

		r = big ? mii_icon(&model, MII_EXPR_SMILE, 256, (GXColor) { 0, 0, 0, 0 }, big) : MII_E_MEM;
		GX_DrawDone();
		result("mii_icon 256", r);
		if (big) {
			n = save_rgba8("icon_large", big, 256, 256, &corners);
			say("icon_large: %d of %d pixels drawn, %d in the corners\n", n, 256 * 256, corners);
			result("a large icon with a head in it and clear corners",
				   n > 256 * 256 / 4 && !corners ? MII_OK : MII_E_VERIFY);
			free(big);
		}
	}
	memset(&pose, 0, sizeof(pose));
	pose.arm_raise[0] = 2.3f;
	pose.expression = MII_EXPR_SMILE;
	/* The Mii Channel's body for the Mii's sex, if Save Mii faces saved it. */
	{
		static mii_body wb;

		if (mii_body_load(&wb, m.sex == MII_FEMALE ? MII_BODY_FEMALE_PATH : MII_BODY_MALE_PATH) == MII_OK &&
				mii_model_set_body(&model, &wb) == MII_OK) {
			pose.anim = 1 + mii_body_anim(&wb, "bodyGreeting");
			pose.frame = 40;
			say("the Wii's %s body, waving\n", m.sex == MII_FEMALE ? "female" : "male");
		}
	}
	t0 = gettime();
	r = mii_capture(&model, &pose, 128, 256, 0.04f, 1, (GXColor) { 0, 0, 0, 255 }, body);
	GX_DrawDone();
	result("mii_capture", r);
	say("captured in %llu us\n", ticks_to_microsecs(gettime() - t0));
	n = save_rgba8("body", body, 128, 256, &corners);
	say("body: %d of %d pixels drawn, %d in the corners\n", n, 128 * 256, corners);
	result("a Mii in the capture and clear corners", n > 128 * 256 / 8 && !corners ? MII_OK : MII_E_VERIFY);
	/* The paired-single pose (mii_body_world, what mii_draw draws with)
	 * against the portable one (mii_body_matrices), over a walk, and the
	 * time each takes a Mii. */
	if (model.body) {
		static float pm[MII_BODY_BONES][3][4], nm[MII_BODY_BONES][3][3];
		static Mtx w[MII_BODY_BONES];
		mii_pose wp;
		float worst = 0;
		int f, b, i2, j2, walk = 1 + mii_body_anim(model.body, "bodyWalk");
		u64 tp = 0, tw = 0, t1;

		memset(&wp, 0, sizeof(wp));
		wp.anim = walk;
		for (f = 0; f < 40; ++f) {
			wp.frame = f + 0.5f;
			t1 = gettime();
			mii_body_matrices(&model, &wp, pm, nm);
			tp += gettime() - t1;
			t1 = gettime();
			mii_body_world(&model, &wp, w);
			tw += gettime() - t1;
			for (b = 0; b < model.body->nbones; ++b)
				for (i2 = 0; i2 < 3; ++i2)
					for (j2 = 0; j2 < 4; ++j2) {
						float s = j2 < 3 ? model.bone_scale[b][j2] : 1, d = fabsf(w[b][i2][j2] * s - pm[b][i2][j2]);

						worst = d > worst ? d : worst;
					}
		}
		say("pose: paired singles against the portable code, largest difference %.5f; %llu ns against %llu ns "
			"a Mii (the memo answers from the second frame)\n", worst, ticks_to_nanosecs(tw / 40),
			ticks_to_nanosecs(tp / 40));
		result("the paired-single pose matches", worst < 0.01f ? MII_OK : MII_E_VERIFY);
	}
	/* Its levels of detail, made from the body (mii_body_make_lods), standing. */
	if (model.body && mii_body_make_lods((mii_body *) model.body, NULL, 0) == MII_OK) {
		int l;

		for (l = 0; l <= model.body->nlods; ++l) {
			char name[24];

			memset(&pose, 0, sizeof(pose));
			pose.lod = l;
			r = mii_capture(&model, &pose, 128, 256, 0.04f, 1, (GXColor) { 0, 0, 0, 255 }, body);
			GX_DrawDone();
			snprintf(name, sizeof(name), "body_lod%d", l);
			n = save_rgba8(name, body, 128, 256, &corners);
			say("%s: %d triangles, %d pixels drawn\n", name, l ? model.body->lod[l - 1].tris : model.body->full_tris, n);
		}
	}
	/* Each face drawn by the GPU (mii_model_make_faces) against the CPU's,
	 * texel by texel (both RGB5A3), and the time each takes. */
	{
		mii_model_opts oc = { 128, 0, NULL, 0, 0 }, og = { 128, 0, NULL, 0, 1 };
		mii_model cm, gm;
		u64 t1, t2, t3;
		int e, worst = 0, rc, rg;
		long sum = 0, texels = 0;

		t1 = gettime();
		rc = mii_model_build(&res, &m, &oc, &cm);
		t2 = gettime();
		rg = mii_model_build(&res, &m, &og, &gm);
		t3 = gettime();
		if (rg == MII_OK)
			rg = mii_model_make_faces(&gm, &res, (GXColor) { 0, 0, 0, 255 });
		say("faces: the CPU built the model with 7 in %llu us; the GPU drew them in %llu us (model %llu us)\n",
			ticks_to_microsecs(t2 - t1), ticks_to_microsecs(gettime() - t3), ticks_to_microsecs(t3 - t2));
		for (e = 0; rc == MII_OK && rg == MII_OK && e < MII_EXPR_COUNT; ++e) {
			const u8 *a = cm.mask[e].gx, *b = gm.mask[e].gx;
			int k, c;

			DCInvalidateRange((void *) b, mii_tex_rgb5a3_size(128, 128));
			for (k = 0; k < 128 * 128; ++k) {
				int va = a[2 * k] << 8 | a[2 * k + 1], vb = b[2 * k] << 8 | b[2 * k + 1], ca[4], cb[4];
				const int *pv[2] = { ca, cb };
				int v[2] = { va, vb }, j;

				for (j = 0; j < 2; ++j) {
					int *o = (int *) pv[j], x = v[j];

					if (x & 0x8000) {
						o[0] = (x >> 10 & 31) * 255 / 31; o[1] = (x >> 5 & 31) * 255 / 31; o[2] = (x & 31) * 255 / 31; o[3] = 255;
					} else {
						o[0] = (x >> 8 & 15) * 17; o[1] = (x >> 4 & 15) * 17; o[2] = (x & 15) * 17; o[3] = (x >> 12 & 7) * 255 / 7;
					}
				}
				for (c = 0; c < 4; ++c) {
					int d = abs(ca[c] - cb[c]);

					sum += d;
					worst = d > worst ? d : worst;
				}
				texels += 4;
			}
			if (e == MII_EXPR_NORMAL) {   /* both as pictures */
				u8 *pic = memalign(32, 128 * 128 * 4);
				int j;

				for (j = 0; pic && j < 2; ++j) {
					const u8 *src = j ? b : a;
					int x, y;

					for (y = 0; y < 128; ++y)
						for (x = 0; x < 128; ++x) {
							const u8 *q = src + 32 * ((y / 4) * 32 + x / 4) + 2 * (4 * (y % 4) + x % 4);
							int v = q[0] << 8 | q[1], o[4];
							u8 *t = pic + 64 * ((y / 4) * 32 + x / 4) + 2 * (4 * (y % 4) + x % 4);

							if (v & 0x8000) {
								o[0] = (v >> 10 & 31) * 255 / 31; o[1] = (v >> 5 & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; o[3] = 255;
							} else {
								o[0] = (v >> 8 & 15) * 17; o[1] = (v >> 4 & 15) * 17; o[2] = (v & 15) * 17; o[3] = (v >> 12 & 7) * 255 / 7;
							}
							t[0] = (u8) o[3]; t[1] = (u8) o[0]; t[32] = (u8) o[1]; t[33] = (u8) o[2];   /* RGBA8 tiles */
						}
					save_rgba8(j ? "face_gpu" : "face_cpu", pic, 128, 128, &corners);
				}
				free(pic);
			}
		}
		say("faces: GPU against CPU, mean difference %.2f, largest %d (of 255, each channel)\n",
			texels ? (double) sum / texels : -1.0, worst);
		result("the GPU's faces match the CPU's", rc == MII_OK && rg == MII_OK && texels &&
			   (double) sum / texels < 3 ? MII_OK : MII_E_VERIFY);
		if (rc == MII_OK)
			mii_model_free(&cm);
		if (rg == MII_OK)
			mii_model_free(&gm);
	}
	/* The head's coarser levels (mii_model_opts.lods), 128 x 128 each. */
	{
		mii_model_opts o = { 128, 0, NULL, MII_HEAD_LODS, 0 };
		mii_model lm;
		u64 t1 = gettime();
		int l;

		r = mii_model_build(&res, &m, &o, &lm);
		say("built with %d head levels in %llu us\n", MII_HEAD_LODS, ticks_to_microsecs(gettime() - t1));
		for (l = 0; r == MII_OK && l <= MII_HEAD_LODS; ++l) {
			char name[24];
			int i, tris = 0;

			memset(&pose, 0, sizeof(pose));
			pose.head_only = 1;
			pose.lod = l;
			mii_capture(&lm, &pose, 128, 128, 0.04f, 1, (GXColor) { 0, 0, 0, 255 }, body);
			GX_DrawDone();
			for (i = 0; i < MII_HEAD_PARTS; ++i)
				tris += (l ? lm.part[i].lod_count[l - 1] : lm.part[i].count) / 3;
			snprintf(name, sizeof(name), "head_lod%d", l);
			n = save_rgba8(name, body, 128, 128, &corners);
			say("%s: %d triangles, %d pixels drawn\n", name, tris, n);
		}
		if (r == MII_OK)
			mii_model_free(&lm);
	}
	mii_model_free(&model);
	mii_res_free(&res);
	mii_db_free(&db);
	free(icon);
	free(body);
}

/* Two framebuffers: each frame is copied to the one the TV is not showing,
 * and shown from the next vertical blank. With one, a frame slower than
 * the TV's copied over the picture while it was being shown: the crowd
 * tests tore. */
static void present(GXRModeObj *rmode) {
	static void *xfbs[2];
	static int back;

	if (!xfbs[0]) {
		xfbs[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
		xfbs[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	}
	GX_CopyDisp(xfbs[back], GX_TRUE);
	GX_DrawDone();   /* the copy is in memory */
	VIDEO_SetNextFramebuffer(xfbs[back]);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	back ^= 1;
}

/* A crowd: COUNTS (e.g. 20,40,60) random Miis, half female, walking across a
 * field with the Mii Channel's bodies, SECONDS each, faces MASK px. For each
 * count: memory a Mii takes, the CPU time to pose and send them, the GPU
 * time after (GX_DrawDone), frames a second. Stops when a count cannot be
 * built (memory) or runs under 30 frames a second. */
static void crowd_test(const char *counts, int seconds, int mask, void *xfb, GXRModeObj *rmode) {
	static u8 fifo[256 * 1024] ATTRIBUTE_ALIGN(32);
	static mii_res res;
	static mii_body bodies[2];
	static mii_model models[200];
	static float phase[200], lane[200];
	int built = 0, r, walk[2], count;
	uint32_t seed = 0x434c494f;
	const char *p = counts;
	struct mallinfo m0 = mallinfo();

	(void) xfb;   /* present() shows two framebuffers of its own */
	GX_Init(fifo, sizeof(fifo));
	GX_SetCopyClear((GXColor) { 40, 60, 90, 255 }, 0x00ffffff);
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
	GX_SetDispCopyYScale((f32) rmode->xfbHeight / (f32) rmode->efbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering, rmode->viHeight == 2 * rmode->xfbHeight ? GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_BACK);
	GX_SetColorUpdate(GX_TRUE);
	if ((r = mii_res_load(&res, NULL)) < 0 || (r = mii_body_load(&bodies[0], MII_BODY_MALE_PATH)) < 0 ||
			(r = mii_body_load(&bodies[1], MII_BODY_FEMALE_PATH)) < 0) {
		result("RFL_Res.dat and the Mii Channel bodies (Save Mii faces)", r);
		return;
	}
	walk[0] = 1 + mii_body_anim(&bodies[0], "bodyWalk");
	walk[1] = 1 + mii_body_anim(&bodies[1], "bodyWalk");
	say("crowd: faces %d px, %d s a count; MEM1+MEM2 arena %u KB free at the start\n", mask, seconds,
		(unsigned) ((SYS_GetArena1Hi() - SYS_GetArena1Lo()) + (SYS_GetArena2Hi() - SYS_GetArena2Lo())) / 1024);
	while (*p) {
		struct mallinfo mb = mallinfo(), ma;
		u64 start, t, cpu = 0, gpu = 0, cpu_max = 0, gpu_max = 0;
		int frames = 0, cols, i;
		float depth, fps;
		Mtx44 proj;
		Mtx view;
		guVector eye, up = { 0, 1, 0 }, at = { 0, 40, 0 };

		count = atoi(p);
		while (*p && *p != ',')
			++p;
		if (*p == ',')
			++p;
		if (count < 1 || count > 200)
			break;
		for (; built < count; ++built) {
			mii m;
			int sex = built & 1;

			mii_random(&m, sex ? MII_FEMALE : MII_MALE, MII_ANY, &seed);
			r = mii_model_make(&res, &m, mask, &models[built]);
			if (r == MII_OK)
				r = mii_model_set_body(&models[built], &bodies[sex]);
			if (r < 0)
				break;
			phase[built] = (float) (seed % 40);
			lane[built] = (float) (seed % 1000) / 1000;
		}
		ma = mallinfo();
		if (built < count) {
			say("crowd %d: built %d, then %d: stop (memory: %u KB in use)\n", count, built, r,
				(unsigned) (ma.uordblks / 1024));
			break;
		}
		/* Rows of ten, 70 apart, walking along X; the camera back and up. */
		cols = count < 10 ? count : 10;
		depth = (count + cols - 1) / cols * 70.0f;
		eye.x = 0;
		eye.y = 160 + depth * 0.5f;
		eye.z = 420 + depth * 1.2f;
		at.z = -depth / 2;
		guLookAt(view, &eye, &up, &at);
		guPerspective(proj, 45, (f32) rmode->fbWidth / rmode->efbHeight, 50, 6000);
		start = gettime();
		while (diff_sec(start, gettime()) < (u32) seconds) {
			float secs = diff_usec(start, gettime()) / 1e6f;
			u64 a, b;

			a = gettime();
			GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
			GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
			for (i = 0; i < count; ++i) {
				mii_pose pose;
				Mtx place, turn;
				float x = fmodf(secs * 40 + phase[i] * 20 + lane[i] * 300, 700) - 350;

				memset(&pose, 0, sizeof(pose));
				pose.anim = walk[i & 1];
				pose.frame = secs * 60 + phase[i];
				guMtxRotRad(turn, 'y', (float) M_PI / 2);   /* face the way they walk */
				guMtxTransApply(turn, place, x, 0, -(i / cols) * 70.0f - lane[i] * 20);
				mii_draw(&models[i], &pose, view, place);
			}
			b = gettime();
			GX_DrawDone();
			t = gettime();
			cpu += b - a;
			gpu += t - b;
			if (b - a > cpu_max)
				cpu_max = b - a;
			if (t - b > gpu_max)
				gpu_max = t - b;
			present(rmode);
			++frames;
		}
		fps = frames / (diff_usec(start, gettime()) / 1e6f);
		say("crowd %d: %.1f frames a second; CPU %llu us a frame (max %llu), GPU after it %llu us "
			"(max %llu); %u KB a Mii (%u KB in use)\n",
			count, fps, ticks_to_microsecs(cpu / frames), ticks_to_microsecs(cpu_max),
			ticks_to_microsecs(gpu / frames), ticks_to_microsecs(gpu_max),
			(unsigned) ((ma.uordblks - m0.uordblks) / 1024 / count), (unsigned) (ma.uordblks / 1024));
		(void) mb;
		if (fps < 30) {
			say("crowd %d: under 30 frames a second: stop\n", count);
			break;
		}
	}
	for (count = 0; count < built; ++count)
		mii_model_free(&models[count]);
	mii_body_free(&bodies[0]);
	mii_body_free(&bodies[1]);
	mii_res_free(&res);
}

/* The ground: stripes across the walk, so distance reads on the TV. */
static void ground(Mtx view) {
	int i;

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_LoadPosMtxImm(view, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_Begin(GX_QUADS, GX_VTXFMT2, 4 * 44);
	for (i = 0; i < 44; ++i) {
		float z0 = 600 - i * 200.0f, z1 = z0 - 200;
		u8 g = i & 1 ? 120 : 100;

		GX_Position3f32(-3000, 0, z0); GX_Color4u8(70, g, 60, 255);
		GX_Position3f32(3000, 0, z0); GX_Color4u8(70, g, 60, 255);
		GX_Position3f32(3000, 0, z1); GX_Color4u8(70, g, 60, 255);
		GX_Position3f32(-3000, 0, z1); GX_Color4u8(70, g, 60, 255);
	}
	GX_End();
}

/* How far the heap has grown: newlib takes MEM1's arena, then MEM2's
 * (mallinfo counts the gap between them once it reaches MEM2). */
static u32 heap_grown(u32 *m1, u32 *m2) {
	return ((u32) SYS_GetArena1Lo() - *m1) + ((u32) SYS_GetArena2Lo() - *m2);
}

/* A crowd walking in from the horizon to the bottom of the screen, then
 * turning and walking off into the distance (`walk COUNTS SECONDS`):
 * COUNTS random Miis, half female, in ranks of 8 (the front one stops at
 * the bottom of the screen), through one mii_cache,
 * built with the normal face only. Each frame every Mii takes a level of
 * detail by its height on screen (mii_body_lod_for), poses every frame near,
 * every second frame at level 3 and every fourth at 4 (mii_pose.reuse), and
 * eight groups walk in step (the bodies' pose memo). Faces follow distance:
 * 128 px over 150 pixels tall, 64 over 60, else 32, up to 3 rebuilt a
 * frame. For each count and direction: frames a second, CPU and GPU time,
 * the Miis at each level, pose memo hits, face rebuilds, memory. A count
 * that cannot be built is the memory limit; under 15 frames a second
 * stops. `walk max SECONDS` builds until memory runs out and walks those. */
static void walk_test(const char *counts, int seconds, void *xfb, GXRModeObj *rmode) {
	static u8 fifo[256 * 1024] ATTRIBUTE_ALIGN(32);
	static mii_res res;
	static mii_body bodies[2];
	enum { MAXN = 6000, COLS = 8 };
	mii_model **models = calloc(MAXN, sizeof(mii_model *));   /* each its own: memory counts them */
	float *lane = calloc(MAXN, sizeof(float));
	mii_cache *cache = mii_cache_new();
	int built = 0, r, walk[2], count, i, max_mode = !strcmp(counts, "max");
	u32 seed = 0x434c494f, a1 = (u32) SYS_GetArena1Lo(), a2 = (u32) SYS_GetArena2Lo(), base;
	const char *p = counts;
	Mtx44 proj;
	Mtx view;
	guVector eye = { 0, 260, 520 }, up = { 0, 1, 0 }, at = { 0, 0, -700 };
	float half_h = (float) rmode->efbHeight / 2, focal = 1 / tanf(22.5f * (float) M_PI / 180);
	GXColor sky = { 150, 190, 230, 255 };

	if (!models || !lane || !cache) {
		say("walk: no memory to start\n");
		return;
	}
	(void) xfb;   /* present() shows two framebuffers of its own */
	GX_Init(fifo, sizeof(fifo));
	GX_SetCopyClear(sky, 0x00ffffff);
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
	GX_SetDispCopyYScale((f32) rmode->xfbHeight / (f32) rmode->efbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering, rmode->viHeight == 2 * rmode->xfbHeight ? GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetColorUpdate(GX_TRUE);
	if ((r = mii_res_load(&res, NULL)) < 0 || (r = mii_body_load(&bodies[0], MII_BODY_MALE_PATH)) < 0 ||
			(r = mii_body_load(&bodies[1], MII_BODY_FEMALE_PATH)) < 0) {
		result("RFL_Res.dat and the Mii Channel bodies (Save Mii faces)", r);
		return;
	}
	for (i = 0; i < 2; ++i) {
		u64 t0 = gettime();

		r = mii_body_make_lods(&bodies[i], NULL, 0);
		say("%s body: %d triangles; levels %d %d %d %d, made in %llu us (%d)\n", i ? "female" : "male",
			bodies[i].full_tris, bodies[i].lod[0].tris, bodies[i].lod[1].tris, bodies[i].lod[2].tris,
			bodies[i].lod[3].tris, ticks_to_microsecs(gettime() - t0), r);
		walk[i] = 1 + mii_body_anim(&bodies[i], "bodyWalk");
	}
	base = heap_grown(&a1, &a2);
	guLookAt(view, &eye, &up, &at);
	guPerspective(proj, 45, (f32) rmode->fbWidth / rmode->efbHeight, 20, 12000);
	say("walk: %d s each way; heap %u KB in use for the bodies and faces file\n", seconds, base / 1024);
	while (*p) {
		u64 t0;
		int dir, step_built;

		count = max_mode ? MAXN : atoi(p);
		if (!max_mode) {
			while (*p && *p != ',')
				++p;
			if (*p == ',')
				++p;
		} else {
			p = "";
		}
		if (count < 1 || count > MAXN)
			break;
		t0 = gettime();
		step_built = built;
		for (; built < count; ++built) {
			mii m;
			int sex = built & 1;
			mii_model_opts o = { 32, MII_EXPR_BIT(MII_EXPR_NORMAL), cache, MII_HEAD_LODS, 1 };

			mii_random(&m, sex ? MII_FEMALE : MII_MALE, MII_ANY, &seed);
			models[built] = malloc(sizeof(mii_model));
			r = models[built] ? mii_model_build(&res, &m, &o, models[built]) : MII_E_MEM;
			if (r == MII_OK) {
				r = mii_model_set_body(models[built], &bodies[sex]);
				if (r < 0)
					mii_model_free(models[built]);
			}
			if (r < 0) {
				free(models[built]);
				models[built] = NULL;
			}
			if (r < 0)
				break;
			lane[built] = (float) (seed % 1000) / 1000;
		}
		{
			int e, users;
			u32 bytes;

			mii_cache_stats(cache, &e, &bytes, &users);
			say("walk %d: built %d in %llu ms; heap %u KB for the Miis, %u KB a Mii; cache %d parts for %d "
				"uses, %u KB\n", count, built, ticks_to_microsecs(gettime() - t0) / 1000,
				(heap_grown(&a1, &a2) - base) / 1024, built ? (heap_grown(&a1, &a2) - base) / 1024 / built : 0,
				e, users, bytes / 1024);
		}
		{   /* their faces, by the GPU (mii_model_opts.gpu_faces) */
			u64 f0 = gettime();
			int made = 0;

			for (i = step_built; i < built; ++i)
				made += mii_model_make_faces(models[i], &res, sky) == MII_OK;
			say("walk %d: %d faces drawn by the GPU in %llu ms\n", count, made,
				ticks_to_microsecs(gettime() - f0) / 1000);
		}
		if (built < count) {
			if (!max_mode || built == step_built) {
				say("walk %d: built %d, then %d: the memory limit\n", count, built, r);
				break;
			}
			count = built;   /* max: walk what fit */
		}
		for (dir = 0; dir < 2; ++dir) {   /* 0: in from the horizon; 1: away into it */
			u64 start = gettime(), cpu = 0, gpu = 0, cpu_max = 0, remask_t = 0;
			u32 hits0 = bodies[0].memo_hits + bodies[1].memo_hits, miss0 = bodies[0].memo_misses + bodies[1].memo_misses;
			long at_lod[MII_BODY_LODS + 1] = { 0 }, drawn = 0;
			int frames = 0, remasks = 0;
			float fps, far = -4200, near = 180;

			while (diff_sec(start, gettime()) < (u32) seconds) {
				float t = diff_usec(start, gettime()) / 1e6f / seconds;   /* 0 to 1 */
				/* The front rank's z: from the horizon to the bottom of the screen,
				 * where it stops (walking on past, the front ranks went through
				 * the camera and its near plane cut them open), or back. */
				float front = dir ? near - t * (near - far) : far + t * (near - far);
				float speed = (near - far) / seconds;   /* units a second */
				float gait = speed / 90 < 2 ? speed / 90 : 2;   /* bodyWalk covers about 90 a second */
				u64 budget = secs_to_ticks(2) / 1000;   /* 2 ms of face rebuilds a frame */
				u64 a, b, c, remask_frame = 0;

				/* Between frames: faces whose size changed, by the GPU, up to
				 * the budget. */
				for (i = 0; i < count && remask_frame < budget; ++i)
					if (models[i]->faces_pending) {
						c = gettime();
						if (mii_model_make_faces(models[i], &res, sky) == MII_OK)
							++remasks;
						remask_t += gettime() - c;
						remask_frame += gettime() - c;
					}
				GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
				GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
				GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
				a = gettime();
				GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
				GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
				ground(view);
				for (i = 0; i < count; ++i) {
					int rank = i / COLS, col = i % COLS, sex = i & 1, lod, want;
					float x = (col - (COLS - 1) / 2.0f) * 70 + (lane[i] - 0.5f) * 30;
					float z = dir ? front - rank * 90 : front - rank * 90, vz, px;
					mii_pose pose;
					Mtx place;

					vz = -(view[2][0] * x + view[2][2] * z + view[2][3]);
					if (vz < 30)
						continue;
					px = 120 * focal * half_h / vz;
					lod = mii_body_lod_for(&bodies[sex], px);
					want = px >= 150 ? 128 : px >= 60 ? 64 : 32;
					if (want != models[i]->mask_size)   /* drawn before a later frame */
						mii_model_set_mask_size(models[i], &res, want);
					memset(&pose, 0, sizeof(pose));
					pose.anim = walk[sex];
					pose.frame = floorf(diff_usec(start, gettime()) / 1e6f * 60 * gait) + (i % 8) * 5;
					pose.lod = lod;
					pose.reuse = (lod >= 4 && (frames + i) % 4) || (lod == 3 && (frames + i) % 2);
					pose.yaw = dir ? (float) M_PI : 0;
					guMtxIdentity(place);
					guMtxTransApply(place, place, x, 0, z);
					mii_draw(models[i], &pose, view, place);
					++at_lod[lod];
					++drawn;
				}
				b = gettime();
				GX_DrawDone();
				c = gettime();
				cpu += b - a;
				gpu += c - b;
				if (b - a > cpu_max)
					cpu_max = b - a;
				present(rmode);
				++frames;
			}
			fps = frames / (diff_usec(start, gettime()) / 1e6f);
			{
				u32 hits = bodies[0].memo_hits + bodies[1].memo_hits - hits0;
				u32 miss = bodies[0].memo_misses + bodies[1].memo_misses - miss0;

				say("walk %d %s: %.1f frames a second; CPU %llu us a frame (max %llu), GPU after it %llu us; "
					"Miis a frame at levels 0-4: %ld %ld %ld %ld %ld (of %ld drawn); poses memo %u%% of %u; "
					"faces rebuilt %d (%llu us each); heap %u KB\n",
					count, dir ? "away" : "toward", fps, ticks_to_microsecs(cpu / frames),
					ticks_to_microsecs(cpu_max), ticks_to_microsecs(gpu / frames), at_lod[0] / frames,
					at_lod[1] / frames, at_lod[2] / frames, at_lod[3] / frames, at_lod[4] / frames,
					drawn / frames, hits + miss ? 100 * hits / (hits + miss) : 0, hits + miss, remasks,
					remasks ? ticks_to_microsecs(remask_t / remasks) : 0, (heap_grown(&a1, &a2) - base) / 1024);
				if (fps < 15) {
					say("walk %d: under 15 frames a second: stop\n", count);
					p = "";
				}
			}
		}
	}
	for (i = 0; i < built; ++i) {
		mii_model_free(models[i]);
		free(models[i]);
	}
	mii_cache_free(cache);
	free(models);
	free(lane);
	mii_body_free(&bodies[0]);
	mii_body_free(&bodies[1]);
	mii_res_free(&res);
}

/* Where the GPU's time goes (`gpuprobe COUNT DISTANCE`): COUNT Miis standing
 * in ranks DISTANCE away, drawn 4 s each way: whole; heads only; whole but
 * scissored to one pixel (no pixels: what is left is commands and vertices);
 * and the frame with no Miis. For each: CPU, GPU after it, frames. */
static void gpu_probe(int count, float dist, void *xfb, GXRModeObj *rmode) {
	static u8 fifo[256 * 1024] ATTRIBUTE_ALIGN(32);
	static mii_res res;
	static mii_body bodies[2];
	static const char *what[] = { "whole", "heads only", "no pixels", "no Miis", "whole, level 3",
								  "level 3, walking (a pose a frame)" };
	mii_model **models = calloc(count, sizeof(mii_model *));
	mii_cache *cache = mii_cache_new();
	int i, mode, r, built = 0;
	u32 seed = 0x434c494f;
	Mtx44 proj;
	Mtx view;
	guVector eye = { 0, 260, 520 }, up = { 0, 1, 0 }, at = { 0, 0, -700 };

	(void) xfb;   /* present() shows two framebuffers of its own */
	GX_Init(fifo, sizeof(fifo));
	GX_SetCopyClear((GXColor) { 150, 190, 230, 255 }, 0x00ffffff);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
	GX_SetDispCopyYScale((f32) rmode->xfbHeight / (f32) rmode->efbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering, rmode->viHeight == 2 * rmode->xfbHeight ? GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetColorUpdate(GX_TRUE);
	if (!models || !cache || (r = mii_res_load(&res, NULL)) < 0 || (r = mii_body_load(&bodies[0], MII_BODY_MALE_PATH)) < 0 ||
			(r = mii_body_load(&bodies[1], MII_BODY_FEMALE_PATH)) < 0) {
		say("gpuprobe: no faces file, bodies or memory\n");
		return;
	}
	mii_body_make_lods(&bodies[0], NULL, 0);
	mii_body_make_lods(&bodies[1], NULL, 0);
	for (; built < count; ++built) {
		mii m;
		mii_model_opts o = { 32, MII_EXPR_BIT(MII_EXPR_NORMAL), cache, MII_HEAD_LODS, 1 };

		mii_random(&m, built & 1 ? MII_FEMALE : MII_MALE, MII_ANY, &seed);
		models[built] = malloc(sizeof(mii_model));
		if (!models[built] || mii_model_build(&res, &m, &o, models[built]) < 0 ||
				mii_model_set_body(models[built], &bodies[built & 1]) < 0)
			break;
	}
	for (i = 0; i < built; ++i)
		mii_model_make_faces(models[i], &res, (GXColor) { 150, 190, 230, 255 });
	guLookAt(view, &eye, &up, &at);
	guPerspective(proj, 45, (f32) rmode->fbWidth / rmode->efbHeight, 20, 12000);
	for (mode = 0; mode < 6; ++mode) {
		u64 start = gettime(), cpu = 0, gpu = 0;
		int frames = 0;

#ifdef MII_PROFILE
		memset(&mii_profile, 0, sizeof(mii_profile));
#endif
		while (diff_sec(start, gettime()) < 4) {
			u64 a, b, c;

			a = gettime();
			GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
			if (mode == 2)
				GX_SetScissor(0, 0, 1, 1);
			else
				GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
			GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
			for (i = 0; i < built && mode != 3; ++i) {
				mii_pose pose;
				Mtx place;
				float x = (i % 16 - 7.5f) * 60, z = -dist - (i / 16) * 80;

				memset(&pose, 0, sizeof(pose));
				pose.head_only = mode == 1;
				pose.lod = mode >= 4 ? bodies[i & 1].nlods : 0;
				if (mode == 5) {   /* walking, eight groups in step, as walk_test */
					pose.anim = 1 + mii_body_anim(&bodies[i & 1], "bodyWalk");
					pose.frame = (float) (frames + (i % 8) * 5);
				}
				guMtxIdentity(place);
				guMtxTransApply(place, place, x, 0, z);
				mii_draw(models[i], &pose, view, place);
			}
			b = gettime();
			GX_DrawDone();
			c = gettime();
			cpu += b - a;
			gpu += c - b;
			GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
			present(rmode);
			++frames;
		}
		say("gpuprobe %d at %.0f, %s: CPU %llu us, GPU after it %llu us a frame (%d frames)\n", built, dist,
			what[mode], ticks_to_microsecs(cpu / frames), ticks_to_microsecs(gpu / frames), frames);
#ifdef MII_PROFILE
		if (mii_profile.draws) {   /* nanoseconds a Mii */
			u64 d = mii_profile.draws;

			say("  a Mii: setup %llu, pose %llu, palette %llu, body GX %llu, head %llu ns (%u draws)\n",
				ticks_to_nanosecs(mii_profile.setup / d), ticks_to_nanosecs(mii_profile.pose / d),
				ticks_to_nanosecs(mii_profile.palette / d), ticks_to_nanosecs(mii_profile.body / d),
				ticks_to_nanosecs(mii_profile.head / d), (unsigned) d);
		}
		memset(&mii_profile, 0, sizeof(mii_profile));
#endif
	}
	for (i = 0; i < built; ++i) {
		mii_model_free(models[i]);
		free(models[i]);
	}
	free(models);
	mii_cache_free(cache);
	mii_body_free(&bodies[0]);
	mii_body_free(&bodies[1]);
	mii_res_free(&res);
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
	else if (argc == 2 && !strcmp(argv[1], "remote-rewrite"))
		remote_rewrite();
	else if (argc >= 2 && argc <= 3 && !strcmp(argv[1], "draw"))
		draw_test(argc == 3 ? atoi(argv[2]) : 0);
	else if (argc >= 4 && !strcmp(argv[1], "edit"))
		edit(argv[2], argc - 3, argv + 3);
	else if (argc >= 3 && !strcmp(argv[1], "gpuprobe"))
		gpu_probe(atoi(argv[2]), argc > 3 ? (float) atof(argv[3]) : 1500, xfb, rmode);
	else if (argc >= 3 && !strcmp(argv[1], "walk"))
		walk_test(argv[2], argc > 3 ? atoi(argv[3]) : 15, xfb, rmode);
	else if (argc >= 3 && !strcmp(argv[1], "crowd"))
		crowd_test(argv[2], argc > 3 ? atoi(argv[3]) : 10, argc > 4 ? atoi(argv[4]) : 128, xfb, rmode);
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
