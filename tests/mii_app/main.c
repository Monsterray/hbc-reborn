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
//                                  RFL_Res.dat, its icon (normal, smiling)
//                                  and a full-body capture, saved as
//                                  sd:/mii_test/*.rgba and checked
//   mii_app.dol remote-rewrite     write Wii Remote 1's own Miis back
//                                  unchanged, after saving its memory to
//                                  sd:/mii_test/remote_backup.bin
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
	memset(&pose, 0, sizeof(pose));
	pose.arm_raise[0] = 2.3f;
	pose.expression = MII_EXPR_SMILE;
	t0 = gettime();
	r = mii_capture(&model, &pose, 128, 256, 0.04f, 1, (GXColor) { 0, 0, 0, 255 }, body);
	GX_DrawDone();
	result("mii_capture", r);
	say("captured in %llu us\n", ticks_to_microsecs(gettime() - t0));
	n = save_rgba8("body", body, 128, 256, &corners);
	say("body: %d of %d pixels drawn, %d in the corners\n", n, 128 * 256, corners);
	result("a Mii in the capture and clear corners", n > 128 * 256 / 8 && !corners ? MII_OK : MII_E_VERIFY);
	mii_model_free(&model);
	mii_res_free(&res);
	mii_db_free(&db);
	free(icon);
	free(body);
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
