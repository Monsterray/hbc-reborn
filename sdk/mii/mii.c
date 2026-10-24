/*
 * mii.c - Miis from homebrew; see mii.h and docs/mii.md. Public domain.
 *
 * In order: bytes and the CRC, a Mii (its bit fields, the check, names,
 * new Miis, the creation ID), files, the database (RFL_DB.dat: the 100
 * slots and the hidden database), the Wii Remote's layout, and the libogc
 * half (NAND, the Wi-Fi MAC, the Remote's memory), which MII_NO_OGC leaves
 * out.
 *
 * The layouts and rules are RFL's, the Mii library the Wii Menu and games
 * link (RVLFaceLib in the Wii Menu decompilation, koopthekoopa/wii-ipl);
 * docs/mii.md names the source of each. The code is written from those
 * descriptions, not copied.
 */

#include <string.h>

#include "mii.h"

/* ---- Bytes ---------------------------------------------------------------- */

static uint16_t get16(const uint8_t *p) {
	return (uint16_t) (p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t v) {
	p[0] = v >> 8;
	p[1] = (uint8_t) v;
}

static uint32_t get32(const uint8_t *p) {
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = (uint8_t) v;
}

/* CRC-16/XMODEM, bitwise: RFLiCalculateCRC run over data whose last two
 * bytes are its CRC gives 0, which is the same thing. */
uint16_t mii_crc16(const void *data, size_t size) {
	const uint8_t *p = data;
	uint16_t c = 0;
	int k;

	while (size--) {
		c ^= (uint16_t) (*p++ << 8);
		for (k = 0; k < 8; ++k)
			c = c & 0x8000 ? (uint16_t) (c << 1 ^ 0x1021) : (uint16_t) (c << 1);
	}
	return c;
}

const char *mii_strerror(int result) {
	switch (result) {
	case MII_OK: return "done";
	case MII_E_IO: return "a NAND or Wii Remote transfer failed";
	case MII_E_FORMAT: return "the data is not as the Wii makes it; nothing was written";
	case MII_E_FULL: return "no free slot";
	case MII_E_MEM: return "out of memory";
	case MII_E_ARG: return "bad argument";
	case MII_E_INVALID: return "the Wii would not accept this Mii";
	case MII_E_EXISTS: return "that Mii is already in another slot";
	case MII_E_EMPTY: return "no Mii there";
	case MII_E_CRC: return "the Mii's CRC does not match";
	case MII_E_NO_REMOTE: return "no Wii Remote connected there";
	case MII_E_TIMEOUT: return "the Wii Remote did not answer";
	case MII_E_VERIFY: return "what was read back differs from what was written";
	case MII_E_CHANGED: return "the Mii database changed since it was loaded";
	case MII_E_DENIED: return "IOS refused the file";
	}
	return result >= 0 ? "done" : "unknown error";
}

/* ---- A Mii's bit fields ------------------------------------------------------
 * Big-endian u16 words, each field from the top bit down (PowerPC packs
 * bit fields from the most significant bit). shift is the field's lowest bit
 * in its word. RFLiCharData in RFLi_Types.h. */

typedef struct {
	uint8_t word, shift, bits, at;
} field;

#define F(word, shift, bits, member) { word, shift, bits, (uint8_t) offsetof(mii, member) }
#define PAD(word, shift, bits, k) { word, shift, bits, (uint8_t) (offsetof(mii, reserved) + k) }

static const field fields[] = {
	PAD(0x00, 15, 1, 0), F(0x00, 14, 1, sex), F(0x00, 10, 4, birth_month), F(0x00, 5, 5, birth_day),
	F(0x00, 1, 4, fav_color), F(0x00, 0, 1, favorite),
	F(0x20, 13, 3, face_type), F(0x20, 10, 3, face_color), F(0x20, 6, 4, face_tex),
	PAD(0x20, 3, 3, 1), F(0x20, 2, 1, local_only), F(0x20, 0, 2, kind),
	F(0x22, 9, 7, hair_type), F(0x22, 6, 3, hair_color), F(0x22, 5, 1, hair_flip), PAD(0x22, 0, 5, 2),
	F(0x24, 11, 5, brow_type), F(0x24, 6, 5, brow_rotate), PAD(0x24, 0, 6, 3),
	F(0x26, 13, 3, brow_color), F(0x26, 9, 4, brow_scale), F(0x26, 4, 5, brow_y), F(0x26, 0, 4, brow_x),
	F(0x28, 10, 6, eye_type), F(0x28, 5, 5, eye_rotate), F(0x28, 0, 5, eye_y),
	F(0x2A, 13, 3, eye_color), F(0x2A, 9, 4, eye_scale), F(0x2A, 5, 4, eye_x), PAD(0x2A, 0, 5, 4),
	F(0x2C, 12, 4, nose_type), F(0x2C, 8, 4, nose_scale), F(0x2C, 3, 5, nose_y), PAD(0x2C, 0, 3, 5),
	F(0x2E, 11, 5, mouth_type), F(0x2E, 9, 2, mouth_color), F(0x2E, 5, 4, mouth_scale), F(0x2E, 0, 5, mouth_y),
	F(0x30, 12, 4, glass_type), F(0x30, 9, 3, glass_color), F(0x30, 5, 4, glass_scale), F(0x30, 0, 5, glass_y),
	F(0x32, 14, 2, mustache), F(0x32, 12, 2, beard_type), F(0x32, 9, 3, beard_color),
	F(0x32, 5, 4, beard_scale), F(0x32, 0, 5, beard_y),
	F(0x34, 15, 1, mole), F(0x34, 11, 4, mole_scale), F(0x34, 6, 5, mole_y), F(0x34, 1, 5, mole_x),
	PAD(0x34, 0, 1, 6),
};

#define NFIELDS (sizeof(fields) / sizeof(fields[0]))

void mii_unpack(const uint8_t raw[MII_SIZE], mii *m) {
	size_t i;

	memset(m, 0, sizeof(*m));
	for (i = 0; i < NFIELDS; ++i) {
		const field *f = &fields[i];
		((uint8_t *) m)[f->at] = (uint8_t) (get16(raw + f->word) >> f->shift & ((1u << f->bits) - 1));
	}
	for (i = 0; i < MII_NAME_MAX; ++i) {
		m->name[i] = get16(raw + 0x02 + 2 * i);
		m->creator[i] = get16(raw + 0x36 + 2 * i);
	}
	m->height = raw[0x16];
	m->build = raw[0x17];
	memcpy(m->id, raw + 0x18, MII_ID_SIZE);
}

void mii_pack(const mii *m, uint8_t raw[MII_SIZE]) {
	size_t i;

	memset(raw, 0, MII_SIZE);
	for (i = 0; i < NFIELDS; ++i) {
		const field *f = &fields[i];
		uint16_t mask = (uint16_t) ((1u << f->bits) - 1);
		uint16_t w = get16(raw + f->word);

		w |= (uint16_t) ((((const uint8_t *) m)[f->at] & mask) << f->shift);
		put16(raw + f->word, w);
	}
	for (i = 0; i < MII_NAME_MAX; ++i) {
		put16(raw + 0x02 + 2 * i, m->name[i]);
		put16(raw + 0x36 + 2 * i, m->creator[i]);
	}
	raw[0x16] = m->height;
	raw[0x17] = m->build;
	memcpy(raw + 0x18, m->id, MII_ID_SIZE);
}

/* ---- The check ------------------------------------------------------------------
 * RFLiCheckValidInfo (RFL_DataUtility.c), limits from RFLi_FaceConfig.h. */

typedef struct {
	uint8_t at, min, max;
	const char *low, *high;
} limit;

#define L(member, max, what) { (uint8_t) offsetof(mii, member), 0, max, NULL, what " above " #max }

static const limit limits[] = {
	L(sex, 1, "sex"),
	L(face_type, 7, "face type"), L(face_color, 5, "face colour"), L(face_tex, 11, "face features"),
	L(hair_type, 71, "hair type"), L(hair_color, 7, "hair colour"), L(hair_flip, 1, "hair flip"),
	L(eye_type, 47, "eye type"), L(eye_color, 5, "eye colour"), L(eye_scale, 7, "eye scale"),
	L(eye_rotate, 7, "eye rotation"), L(eye_x, 12, "eye X"), L(eye_y, 18, "eye Y"),
	L(brow_type, 23, "eyebrow type"), L(brow_color, 7, "eyebrow colour"), L(brow_scale, 8, "eyebrow scale"),
	L(brow_rotate, 11, "eyebrow rotation"), L(brow_x, 12, "eyebrow X"),
	{ (uint8_t) offsetof(mii, brow_y), 3, 18, "eyebrow Y below 3", "eyebrow Y above 18" },
	L(nose_type, 11, "nose type"), L(nose_scale, 8, "nose scale"), L(nose_y, 18, "nose Y"),
	L(mouth_type, 23, "mouth type"), L(mouth_color, 2, "mouth colour"), L(mouth_scale, 8, "mouth scale"),
	L(mouth_y, 18, "mouth Y"),
	L(mustache, 3, "mustache"), L(beard_type, 3, "beard type"), L(beard_color, 7, "beard colour"),
	L(beard_scale, 8, "beard scale"), L(beard_y, 16, "beard Y"),
	L(glass_type, 8, "glasses type"), L(glass_color, 5, "glasses colour"), L(glass_scale, 7, "glasses scale"),
	L(glass_y, 20, "glasses Y"),
	L(mole, 1, "mole"), L(mole_scale, 8, "mole scale"), L(mole_x, 16, "mole X"), L(mole_y, 30, "mole Y"),
	L(height, 128, "height"), L(build, 128, "build"),
	L(fav_color, 11, "favourite colour"),
};

static int birthday_ok(int month, int day) {
	static const uint8_t days[13] = { 0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if (!month || !day)
		return !month && !day;
	return month <= 12 && day <= days[month];
}

int mii_check(const mii *m, const char **why) {
	const char *bad = NULL;
	size_t i;

	if (!m)
		return MII_E_ARG;
	for (i = 0; i < sizeof(limits) / sizeof(limits[0]) && !bad; ++i) {
		uint8_t v = ((const uint8_t *) m)[limits[i].at];
		if (v < limits[i].min)
			bad = limits[i].low;
		else if (v > limits[i].max)
			bad = limits[i].high;
	}
	if (!bad && !birthday_ok(m->birth_month, m->birth_day))
		bad = "not a real birthday";
	if (!bad && !m->name[0])
		bad = "no name";
	if (!bad && mii_id_empty(m->id))
		bad = "no creation ID (mii_make_id)";
	if (!bad && !(mii_id_flags(m->id) & MII_ID_NORMAL) && !m->local_only)
		bad = "a special creation ID on a Mii that is not local only";
	if (why)
		*why = bad;
	return bad ? MII_E_INVALID : MII_OK;
}

/* ---- Names -------------------------------------------------------------------- */

int mii_set_name(uint16_t name[MII_NAME_MAX], const char *utf8) {
	const uint8_t *s = (const uint8_t *) (utf8 ? utf8 : "");
	int n = 0;

	memset(name, 0, MII_NAME_MAX * sizeof(name[0]));
	while (*s) {
		uint32_t c = *s++;
		int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

		if (c >= 0x80 && c < 0xc0)
			continue;                      /* a stray continuation byte */
		c &= more == 3 ? 0x07 : more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
		while (more-- && (*s & 0xc0) == 0x80)
			c = c << 6 | (*s++ & 0x3f);
		if (c >= 0x10000) {
			if (n + 2 > MII_NAME_MAX)
				break;
			c -= 0x10000;
			name[n++] = (uint16_t) (0xd800 | c >> 10);
			name[n++] = (uint16_t) (0xdc00 | (c & 0x3ff));
		} else {
			if (n + 1 > MII_NAME_MAX)
				break;
			name[n++] = (uint16_t) c;
		}
	}
	return n;
}

int mii_get_name(const uint16_t name[MII_NAME_MAX], char *utf8, size_t size) {
	size_t n = 0;
	int i;

	if (!size)
		return 0;
	for (i = 0; i < MII_NAME_MAX && name[i]; ++i) {
		uint32_t c = name[i];
		uint8_t b[4];
		size_t k, len;

		if (c >= 0xd800 && c < 0xdc00 && i + 1 < MII_NAME_MAX && name[i + 1] >= 0xdc00 &&
				name[i + 1] < 0xe000)
			c = 0x10000 + ((c - 0xd800) << 10) + (name[++i] - 0xdc00);
		if (c < 0x80) {
			b[0] = (uint8_t) c;
			len = 1;
		} else if (c < 0x800) {
			b[0] = (uint8_t) (0xc0 | c >> 6);
			b[1] = (uint8_t) (0x80 | (c & 0x3f));
			len = 2;
		} else if (c < 0x10000) {
			b[0] = (uint8_t) (0xe0 | c >> 12);
			b[1] = (uint8_t) (0x80 | (c >> 6 & 0x3f));
			b[2] = (uint8_t) (0x80 | (c & 0x3f));
			len = 3;
		} else {
			b[0] = (uint8_t) (0xf0 | c >> 18);
			b[1] = (uint8_t) (0x80 | (c >> 12 & 0x3f));
			b[2] = (uint8_t) (0x80 | (c >> 6 & 0x3f));
			b[3] = (uint8_t) (0x80 | (c & 0x3f));
			len = 4;
		}
		if (n + len + 1 > size)
			break;
		for (k = 0; k < len; ++k)
			utf8[n++] = (char) b[k];
	}
	utf8[n] = 0;
	return (int) n;
}

/* ---- New Miis ------------------------------------------------------------------ */

void mii_default(mii *m, int sex) {
	memset(m, 0, sizeof(*m));
	m->sex = sex == MII_FEMALE ? MII_FEMALE : MII_MALE;
	mii_set_name(m->name, "Mii");
	m->height = m->build = 64;
	m->hair_type = m->sex ? 12 : 33;
	m->hair_color = 1;
	m->brow_type = m->sex ? 0 : 6;
	m->brow_color = 1;
	m->brow_scale = 4;
	m->brow_rotate = 6;
	m->brow_x = 2;
	m->brow_y = 10;
	m->eye_type = m->sex ? 4 : 2;
	m->eye_scale = 4;
	m->eye_rotate = m->sex ? 3 : 4;
	m->eye_x = 2;
	m->eye_y = 12;
	m->nose_type = 1;
	m->nose_scale = m->sex ? 3 : 4;
	m->nose_y = 9;
	m->mouth_type = m->sex ? 23 : 1;
	m->mouth_scale = 4;
	m->mouth_y = 13;
	m->glass_scale = 4;
	m->glass_y = 10;
	m->mole_scale = 4;
	m->mole_x = 2;
	m->mole_y = 20;
}

static uint32_t rnd(uint32_t *seed, uint32_t n) {
	uint32_t x = *seed ? *seed : 0x9e3779b9u;

	x ^= x << 13;          /* xorshift32 */
	x ^= x >> 17;
	x ^= x << 5;
	*seed = x;
	return n ? x % n : 0;
}

/* RFL's proportions (RFLi_MakeRandomFace): half men; 40 % children, 40 %
 * adults, 20 % elders; the parts placed as the Mii Channel's defaults are,
 * lower for women and children; beards on a fifth of grown men; glasses
 * rarely on children. The part choices are spread evenly here, not by RFL's
 * per-age tables. */
void mii_random(mii *m, int sex, int age, uint32_t *seed) {
	uint32_t s = 0, r;
	uint8_t low;

	if (!seed)
		seed = &s;
	if (sex != MII_MALE && sex != MII_FEMALE)
		sex = (int) rnd(seed, 2);
	if (age != MII_CHILD && age != MII_ADULT && age != MII_ELDER) {
		r = rnd(seed, 10);
		age = r < 4 ? MII_CHILD : r < 8 ? MII_ADULT : MII_ELDER;
	}
	mii_default(m, sex);
	low = (uint8_t) ((sex == MII_FEMALE ? rnd(seed, 3) : 0) + (age == MII_CHILD ? rnd(seed, 3) : 0));

	m->face_type = (uint8_t) rnd(seed, age == MII_CHILD ? 6 : 8);
	m->face_color = (uint8_t) rnd(seed, 6);
	m->face_tex = (uint8_t) (age == MII_ELDER && rnd(seed, 2) ? 4 + rnd(seed, 4) : rnd(seed, 4) ? 0 : rnd(seed, 12));
	m->hair_type = (uint8_t) rnd(seed, 72);
	m->hair_color = (uint8_t) (age == MII_ELDER && rnd(seed, 10) < 6 ? 4 : rnd(seed, 8));
	m->hair_flip = (uint8_t) rnd(seed, 2);

	m->eye_type = (uint8_t) rnd(seed, 48);
	m->eye_color = (uint8_t) rnd(seed, 6);
	m->eye_y = (uint8_t) (12 + low);

	m->brow_type = (uint8_t) rnd(seed, 24);
	m->brow_color = m->hair_color;
	m->brow_y = (uint8_t) (10 + low);

	m->nose_type = (uint8_t) rnd(seed, 12);
	m->nose_y = (uint8_t) (9 + low);

	m->mouth_type = (uint8_t) rnd(seed, 24);
	m->mouth_color = (uint8_t) (sex == MII_FEMALE ? rnd(seed, 3) : 0);
	m->mouth_y = (uint8_t) (13 + low);

	if (sex == MII_MALE && age != MII_CHILD && rnd(seed, 10) < 2) {
		r = rnd(seed, 3);
		m->mustache = (uint8_t) (r ? 1 + rnd(seed, 3) : 0);
		m->beard_type = (uint8_t) (r != 1 ? 1 + rnd(seed, 3) : 0);
		m->beard_color = m->hair_color;
		m->beard_scale = 4;
		m->beard_y = 10;
	}
	r = rnd(seed, 10);
	if (age == MII_CHILD ? r == 0 : age == MII_ADULT ? r < 3 : r < 5)
		m->glass_type = (uint8_t) (1 + rnd(seed, 8));
	m->glass_y = (uint8_t) (10 + low);

	m->mole_x = (uint8_t) rnd(seed, 17);
	m->mole_y = (uint8_t) rnd(seed, 31);
	m->fav_color = (uint8_t) rnd(seed, 12);
}

/* ---- The creation ID -------------------------------------------------------------- */

int mii_id_empty(const uint8_t id[MII_ID_SIZE]) {
	return !get32(id) && !get32(id + 4);
}

int mii_same_id(const uint8_t a[MII_ID_SIZE], const uint8_t b[MII_ID_SIZE]) {
	return !memcmp(a, b, MII_ID_SIZE);
}

uint32_t mii_id_flags(const uint8_t id[MII_ID_SIZE]) {
	return get32(id) & 0xf0000000u;
}

uint32_t mii_id_time(const uint8_t id[MII_ID_SIZE]) {
	return (get32(id) & 0x0fffffffu) * 4;
}

uint32_t mii_id_home(const uint8_t mac[6]) {
	uint8_t sum = (uint8_t) (mac[0] + mac[1] + mac[2]);

	if (mac[0] != 0x00 || mac[1] != 0x17 || mac[2] != 0xab)
		sum &= 0x7f;           /* not Nintendo's first maker code */
	return (uint32_t) sum << 24 | (uint32_t) mac[3] << 16 | (uint32_t) mac[4] << 8 | mac[5];
}

/* RFLiSetCreateID: OSTicksToSeconds((OSGetTime() - 2006-01-01) >> 2), the
 * time base running at 60.75 MHz. */
#define TICKS_2006 11505369649262175ull
#define TICKS_PER_SEC 60750000u

void mii_id_make(uint8_t id[MII_ID_SIZE], uint64_t ticks, const uint8_t mac[6]) {
	uint64_t since = ticks > TICKS_2006 ? ticks - TICKS_2006 : 0;

	put32(id, (uint32_t) ((since >> 2) / TICKS_PER_SEC & 0x0fffffffu) | MII_ID_NORMAL);
	put32(id + 4, mii_id_home(mac));
}

static int is_home(const uint8_t id[MII_ID_SIZE], uint32_t home) {
	return get32(id + 4) == home && !(get32(id) & MII_ID_DS);
}

/* ---- Files -------------------------------------------------------------------------- */

void mii_store(const uint8_t raw[MII_SIZE], uint8_t out[MII_STORE_SIZE]) {
	memmove(out, raw, MII_SIZE);
	put16(out + MII_SIZE, mii_crc16(out, MII_SIZE));
}

int mii_import(const void *file, size_t size, mii *m) {
	const uint8_t *p = file;

	if (!p || !m || (size != MII_SIZE && size != MII_STORE_SIZE))
		return MII_E_ARG;
	if (size == MII_STORE_SIZE && mii_crc16(p, MII_STORE_SIZE))
		return MII_E_CRC;
	mii_unpack(p, m);
	return MII_OK;
}

/* ---- The database ----------------------------------------------------------------------
 * RFLiDatabase (RFLi_Database.h, RFLi_HiddenDatabase.h), big-endian:
 *   0x00000  "RNOD"
 *   0x00004  100 Miis, 0x4A bytes each
 *   0x01CEC  bit 31: isolation (mingling off)
 *   0x01D00  "RNHD", then the hidden database's head and tail (s16, -1 for
 *            none) and 10,000 table nodes of 12 bytes: a creation ID, then
 *            u16 sex:1 next:15 and u16 pad:1 prev:15 (signed 15-bit indexes;
 *            a free node has ID 0, and next and prev -1)
 *   0x1F1DE  the CRC of 0x00000-0x1F1DD (the only one)
 *   0x1F1E0  10,000 hidden Miis, 0x40 bytes each: a Mii's first 0x36 bytes
 *            without its birthday, then 10 zero bytes. Not in the CRC.
 * The Wii Menu's format leaves the file 0xBE6C0 bytes long. */

#define DB_ISOLATION 0x1CEC
#define DB_HIDDEN 0x1D00
#define DB_HEAD 0x1D04
#define DB_TAIL 0x1D06
#define DB_NODES 0x1D08
#define DB_CRC 0x1F1DE
#define DB_SIZE 0x1F1E0          /* what RFL reads and writes as one: the CRC's range and it */
#define DB_HDATA 0x1F1E0
#define NODE 12
#define HMII 0x40

static uint8_t *slot_at(const mii_db *db, int slot) {
	return db->file + 4 + slot * MII_SIZE;
}

static uint8_t *node_at(const mii_db *db, int i) {
	return db->file + DB_NODES + i * NODE;
}

static int s15(uint16_t w) {
	int v = w & 0x7fff;
	return v & 0x4000 ? v - 0x8000 : v;
}

static int node_next(const mii_db *db, int i) {
	return s15(get16(node_at(db, i) + 8));
}

static int node_prev(const mii_db *db, int i) {
	return s15(get16(node_at(db, i) + 10));
}

static void set_next(mii_db *db, int i, int next) {
	uint8_t *n = node_at(db, i);
	put16(n + 8, (uint16_t) ((get16(n + 8) & 0x8000) | (next & 0x7fff)));
}

static void set_prev(mii_db *db, int i, int prev) {
	uint8_t *n = node_at(db, i);
	put16(n + 10, (uint16_t) ((get16(n + 10) & 0x8000) | (prev & 0x7fff)));
}

static int head(const mii_db *db) {
	return (int16_t) get16(db->file + DB_HEAD);
}

static int tail(const mii_db *db) {
	return (int16_t) get16(db->file + DB_TAIL);
}

static void set_head(mii_db *db, int i) {
	put16(db->file + DB_HEAD, (uint16_t) i);
}

static void set_tail(mii_db *db, int i) {
	put16(db->file + DB_TAIL, (uint16_t) i);
}

static void seal(mii_db *db) {
	put16(db->file + DB_CRC, mii_crc16(db->file, DB_CRC));
}

int mii_db_attach(mii_db *db, uint8_t *file, size_t size) {
	int h, t;

	if (!db || !file)
		return MII_E_ARG;
	memset(db, 0, sizeof(*db));
	if (size != MII_DB_FILE_SIZE || get32(file) != 0x524e4f44 || get32(file + DB_HIDDEN) != 0x524e4844 ||
			mii_crc16(file, DB_SIZE))
		return MII_E_FORMAT;
	db->file = file;
	h = head(db);
	t = tail(db);
	if (h < -1 || h >= MII_HIDDEN_SLOTS || t < -1 || t >= MII_HIDDEN_SLOTS || (h < 0) != (t < 0)) {
		db->file = NULL;
		return MII_E_FORMAT;
	}
	db->loaded_crc = get16(file + DB_CRC);
	db->dirty_lo = db->dirty_hi = 0;
	return MII_OK;
}

static int db_ok(const mii_db *db, int slot) {
	return db && db->file && slot >= 0 && slot < MII_DB_SLOTS;
}

int mii_db_get(const mii_db *db, int slot, mii *m) {
	if (!db_ok(db, slot) || !m)
		return MII_E_ARG;
	if (mii_id_empty(slot_at(db, slot) + 0x18))
		return MII_E_EMPTY;
	mii_unpack(slot_at(db, slot), m);
	return MII_OK;
}

int mii_db_count(const mii_db *db) {
	int i, n = 0;

	if (!db || !db->file)
		return MII_E_ARG;
	for (i = 0; i < MII_DB_SLOTS; ++i)
		n += !mii_id_empty(slot_at(db, i) + 0x18);
	return n;
}

int mii_db_find(const mii_db *db, const uint8_t id[MII_ID_SIZE]) {
	int i;

	if (!db || !db->file || !id)
		return MII_E_ARG;
	if (mii_id_empty(id))
		return MII_E_EMPTY;
	for (i = 0; i < MII_DB_SLOTS; ++i)
		if (mii_same_id(slot_at(db, i) + 0x18, id))
			return i;
	return MII_E_EMPTY;
}

int mii_db_isolated(const mii_db *db) {
	return db && db->file && (get32(db->file + DB_ISOLATION) & 0x80000000u) ? 1 : 0;
}

/* RFLiClearTableData: no ID, and next and prev -1 (0x7FFF), as RFL's format
 * leaves all 10,000. */
static void clear_node(mii_db *db, int i) {
	uint8_t *n = node_at(db, i);

	memset(n, 0, NODE);
	put16(n + 8, 0x7fff);
	put16(n + 10, 0x7fff);
}

static int unlink_hidden(mii_db *db, int i) {
	int prev = node_prev(db, i), next = node_next(db, i);

	if (prev >= 0)
		set_next(db, prev, next);
	if (next >= 0)
		set_prev(db, next, prev);
	if (head(db) == i)
		set_head(db, next);
	if (tail(db) == i)
		set_tail(db, prev);
	clear_node(db, i);
	return MII_OK;
}

static int storable(const mii *m) {
	if (mii_check(m, NULL) < 0 || (mii_id_flags(m->id) & MII_ID_TEMP))
		return MII_E_INVALID;
	return MII_OK;
}

int mii_db_put(mii_db *db, int slot, const mii *m) {
	mii copy;
	int other, h;

	if (!db_ok(db, slot) || !m)
		return MII_E_ARG;
	if (storable(m) < 0)
		return MII_E_INVALID;
	other = mii_db_find(db, m->id);
	if (other >= 0 && other != slot)
		return MII_E_EXISTS;
	copy = *m;
	if (db->home && !is_home(copy.id, db->home))
		copy.local_only = 1;     /* RFL: another Wii's Mii stays on this Wii */
	mii_pack(&copy, slot_at(db, slot));
	if ((h = mii_hidden_find(db, copy.id)) >= 0)
		unlink_hidden(db, h);    /* one Mii, one place */
	seal(db);
	return MII_OK;
}

int mii_db_add(mii_db *db, const mii *m) {
	int i, r;

	if (!db || !db->file || !m)
		return MII_E_ARG;
	if (storable(m) < 0)
		return MII_E_INVALID;
	if (mii_db_find(db, m->id) >= 0)
		return MII_E_EXISTS;
	for (i = 0; i < MII_DB_SLOTS; ++i)
		if (mii_id_empty(slot_at(db, i) + 0x18)) {
			r = mii_db_put(db, i, m);
			return r < 0 ? r : i;
		}
	return MII_E_FULL;
}

int mii_db_remove(mii_db *db, int slot) {
	if (!db_ok(db, slot))
		return MII_E_ARG;
	if (mii_id_empty(slot_at(db, slot) + 0x18))
		return MII_E_EMPTY;
	memset(slot_at(db, slot), 0, MII_SIZE);
	seal(db);
	return MII_OK;
}

/* ---- The hidden database ----------------------------------------------------------- */

static int hidden_ok(const mii_db *db, int i) {
	return db && db->file && i >= 0 && i < MII_HIDDEN_SLOTS && !mii_id_empty(node_at(db, i));
}

int mii_hidden_first(const mii_db *db) {
	return db && db->file ? head(db) : -1;
}

int mii_hidden_next(const mii_db *db, int index) {
	return hidden_ok(db, index) ? node_next(db, index) : -1;
}

int mii_hidden_count(const mii_db *db) {
	int i, n = 0;

	if (!db || !db->file)
		return MII_E_ARG;
	/* Bounded, so a broken chain cannot loop. */
	for (i = head(db); i >= 0 && n < MII_HIDDEN_SLOTS; i = mii_hidden_next(db, i))
		++n;
	return n;
}

int mii_hidden_find(const mii_db *db, const uint8_t id[MII_ID_SIZE]) {
	int i;

	if (!db || !db->file || !id)
		return MII_E_ARG;
	if (mii_id_empty(id))
		return MII_E_EMPTY;
	for (i = 0; i < MII_HIDDEN_SLOTS; ++i)
		if (mii_same_id(node_at(db, i), id))
			return i;
	return MII_E_EMPTY;
}

int mii_hidden_get(const mii_db *db, int index, mii *m) {
	uint8_t raw[MII_SIZE];
	const uint8_t *h;

	if (!db || !db->file || !m || index < 0 || index >= MII_HIDDEN_SLOTS)
		return MII_E_ARG;
	if (!hidden_ok(db, index))
		return MII_E_EMPTY;
	h = db->file + DB_HDATA + index * HMII;
	if (!mii_same_id(h + 0x18, node_at(db, index)))
		return MII_E_FORMAT;     /* the table and the data disagree */
	memset(raw, 0, sizeof(raw));
	memcpy(raw, h, 0x36);
	mii_unpack(raw, m);
	return MII_OK;
}

static void dirty(mii_db *db, uint32_t lo, uint32_t hi) {
	if (db->dirty_hi <= db->dirty_lo) {
		db->dirty_lo = lo;
		db->dirty_hi = hi;
	} else {
		if (lo < db->dirty_lo)
			db->dirty_lo = lo;
		if (hi > db->dirty_hi)
			db->dirty_hi = hi;
	}
}

/* RFLiOneDataToHiddenDB and writeData_ (RFL_HiddenDatabase.c). */
int mii_hidden_put(mii_db *db, const mii *m) {
	uint8_t raw[MII_SIZE], *h, *n;
	int i, t;

	if (!db || !db->file || !m)
		return MII_E_ARG;
	if (storable(m) < 0 || m->local_only)
		return MII_E_INVALID;
	if (mii_db_find(db, m->id) >= 0)
		return MII_E_EXISTS;

	i = mii_hidden_find(db, m->id);
	if (i < 0) {
		for (i = 0; i < MII_HIDDEN_SLOTS && !mii_id_empty(node_at(db, i)); ++i)
			;
		if (i == MII_HIDDEN_SLOTS) {
			/* Full: the oldest makes room, and its place becomes the newest. */
			i = head(db);
			if (i < 0)
				return MII_E_FORMAT;
			unlink_hidden(db, i);
		}
		t = tail(db);
		clear_node(db, i);
		set_prev(db, i, t);
		set_next(db, i, -1);
		if (t >= 0)
			set_next(db, t, i);
		else
			set_head(db, i);
		set_tail(db, i);
	}

	mii_pack(m, raw);
	put16(raw, (uint16_t) (get16(raw) & ~0x3fe1u));    /* no birthday, not favourite */
	h = db->file + DB_HDATA + i * HMII;
	memcpy(h, raw, 0x36);
	memset(h + 0x36, 0, HMII - 0x36);
	dirty(db, DB_HDATA + i * HMII, DB_HDATA + (i + 1) * HMII);

	n = node_at(db, i);
	memcpy(n, m->id, MII_ID_SIZE);
	put16(n + 8, (uint16_t) ((get16(n + 8) & 0x7fff) | (m->sex ? 0x8000 : 0)));
	seal(db);
	return i;
}

int mii_hidden_remove(mii_db *db, int index) {
	if (!db || !db->file || index < 0 || index >= MII_HIDDEN_SLOTS)
		return MII_E_ARG;
	if (!hidden_ok(db, index))
		return MII_E_EMPTY;
	unlink_hidden(db, index);   /* RFL leaves the 0x40 bytes of data */
	seal(db);
	return MII_OK;
}

/* ---- A Wii Remote's Miis ---------------------------------------------------------------
 * RFLiCtrlBuffer (RFLi_Controller.h), twice from 0x0FCA of the Remote's
 * memory:
 *   +0x000  "RNCD"
 *   +0x004  u16: bit i, slot i came from the hidden database
 *   +0x008  10 Miis, 0x4A bytes each
 *   +0x2EC  u16: bit i, slot i deleted (RFL clears these as it writes)
 *   +0x2EE  the CRC of +0x000-+0x2ED */

#define RC_SECRET 0x004
#define RC_SLOTS 0x008
#define RC_DELETED 0x2EC
#define RC_CRC 0x2EE

int mii_remote_parse(const uint8_t *mem, size_t size, mii_remote *r) {
	int c, i;

	if (!mem || !r || size < 2 * MII_REMOTE_BLOCK)
		return MII_E_ARG;
	memset(r, 0, sizeof(*r));
	r->copy = -1;
	for (c = 0; c < 2; ++c) {
		const uint8_t *b = mem + c * MII_REMOTE_BLOCK;
		uint16_t deleted;

		if (get32(b) != 0x524e4344 || mii_crc16(b, MII_REMOTE_BLOCK))
			continue;
		r->copy = (int8_t) c;
		r->secret = get16(b + RC_SECRET) & 0x3ff;
		deleted = get16(b + RC_DELETED) & 0x3ff;
		for (i = 0; i < MII_REMOTE_SLOTS; ++i) {
			mii m;

			memcpy(r->raw[i], b + RC_SLOTS + i * MII_SIZE, MII_SIZE);
			mii_unpack(r->raw[i], &m);
			/* What RFL drops as it reads: deleted slots, and Miis it would
			 * not accept. */
			if ((deleted >> i & 1) || storable(&m) < 0) {
				memset(r->raw[i], 0, MII_SIZE);
				r->secret &= (uint16_t) ~(1u << i);
			}
		}
		return MII_OK;
	}
	return MII_OK;           /* neither copy: an empty set (r->copy -1) */
}

void mii_remote_build(const mii_remote *r, uint8_t block[MII_REMOTE_BLOCK]) {
	int i;

	memset(block, 0, MII_REMOTE_BLOCK);
	put32(block, 0x524e4344);
	put16(block + RC_SECRET, r->secret & 0x3ff);
	for (i = 0; i < MII_REMOTE_SLOTS; ++i)
		memcpy(block + RC_SLOTS + i * MII_SIZE, r->raw[i], MII_SIZE);
	put16(block + RC_CRC, mii_crc16(block, RC_CRC));
}

int mii_remote_get(const mii_remote *r, int slot, mii *m) {
	if (!r || !m || slot < 0 || slot >= MII_REMOTE_SLOTS)
		return MII_E_ARG;
	if (mii_id_empty(r->raw[slot] + 0x18))
		return MII_E_EMPTY;
	mii_unpack(r->raw[slot], m);
	return MII_OK;
}

int mii_remote_put(mii_remote *r, int slot, const mii *m) {
	mii copy;

	if (!r || !m || slot < 0 || slot >= MII_REMOTE_SLOTS)
		return MII_E_ARG;
	if (storable(m) < 0)
		return MII_E_INVALID;
	copy = *m;
	copy.favorite = 0;
	mii_pack(&copy, r->raw[slot]);
	r->secret &= (uint16_t) ~(1u << slot);
	return MII_OK;
}

int mii_remote_remove(mii_remote *r, int slot) {
	if (!r || slot < 0 || slot >= MII_REMOTE_SLOTS)
		return MII_E_ARG;
	if (mii_id_empty(r->raw[slot] + 0x18))
		return MII_E_EMPTY;
	memset(r->raw[slot], 0, MII_SIZE);
	r->secret &= (uint16_t) ~(1u << slot);
	return MII_OK;
}

#ifndef MII_NO_OGC

/* ---- On the Wii ------------------------------------------------------------------------ */

#include <malloc.h>
#include <unistd.h>

#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <wiiuse/wpad.h>

static char db_path[] ATTRIBUTE_ALIGN(32) = "/shared2/menu/FaceLib/RFL_DB.dat";

static int nand_error(s32 r) {
	return r == -102 || r == -1 ? MII_E_DENIED : r == -106 ? MII_E_EMPTY : MII_E_IO;
}

#define BOUNCE (8 * 1024)
static u8 bounce[BOUNCE] ATTRIBUTE_ALIGN(32);

static int nand_read(s32 fd, u32 off, u8 *out, u32 len) {
	while (len) {
		u32 n = len < BOUNCE ? len : BOUNCE;
		if (IOS_Seek(fd, off, SEEK_SET) != (s32) off || IOS_Read(fd, bounce, n) != (s32) n)
			return MII_E_IO;
		memcpy(out, bounce, n);
		out += n;
		off += n;
		len -= n;
	}
	return MII_OK;
}

static int nand_write(s32 fd, u32 off, const u8 *in, u32 len) {
	while (len) {
		u32 n = len < BOUNCE ? len : BOUNCE;
		memcpy(bounce, in, n);
		if (IOS_Seek(fd, off, SEEK_SET) != (s32) off || IOS_Write(fd, bounce, n) != (s32) n)
			return MII_E_IO;
		in += n;
		off += n;
		len -= n;
	}
	return MII_OK;
}

/* The Wi-Fi MAC, which RFL takes for its IDs (NCDGetWirelessMacAddress).
 * Through libogc's net_get_mac_address: the same NCD call (/dev/net/ncd/
 * manage, ioctlv 8), with the 6-byte answer in libogc's MEM2 net heap. With
 * the answer in a MEM1 buffer instead, the bench Wii's IOS returned the
 * last two bytes wrong, every time.
 * Right after an IOS reload (an app just launched from HBC) IOS's network
 * module has not registered /dev/net/ncd yet: it fails with -6 for a few
 * seconds. The MAC does not change, so it is read once. */
int mii_wifi_mac(uint8_t mac[6]) {
	static u8 known[6];
	static bool have;
	u64 end = gettime() + secs_to_ticks(8);
	s32 r;

	if (!have) {
		while ((r = net_get_mac_address(known)) == -6 && gettime() < end)
			usleep(100 * 1000);
		if (r < 0)
			return MII_E_IO;
		have = true;
	}
	memcpy(mac, known, 6);
	return MII_OK;
}

static uint32_t home_word(void) {
	u8 mac[6];
	return mii_wifi_mac(mac) < 0 ? 0 : mii_id_home(mac);
}

int mii_make_id(mii *m) {
	u8 mac[6];
	int r;

	if (!m)
		return MII_E_ARG;
	if ((r = mii_wifi_mac(mac)) < 0)
		return r;
	mii_id_make(m->id, gettime(), mac);
	return MII_OK;
}

int mii_is_home(const mii *m) {
	uint32_t home = home_word();
	return m && home && is_home(m->id, home);
}

int mii_db_load(mii_db *db) {
	u8 *file;
	s32 fd;
	int r;

	if (!db)
		return MII_E_ARG;
	memset(db, 0, sizeof(*db));
	ISFS_Initialize();
	fd = IOS_Open(db_path, IPC_OPEN_READ);
	if (fd < 0)
		return nand_error(fd);
	file = memalign(32, MII_DB_FILE_SIZE + 32);
	if (!file) {
		IOS_Close(fd);
		return MII_E_MEM;
	}
	r = nand_read(fd, 0, file, MII_DB_FILE_SIZE);
	/* One byte more must not be there: the size is part of the format. */
	if (r == MII_OK && IOS_Read(fd, bounce, 32) != 0)
		r = MII_E_FORMAT;
	IOS_Close(fd);
	if (r == MII_OK)
		r = mii_db_attach(db, file, MII_DB_FILE_SIZE);
	if (r < 0) {
		free(file);
		memset(db, 0, sizeof(*db));
		return r;
	}
	db->owned = 1;
	db->home = home_word();
	return MII_OK;
}

static int save(mii_db *db, u8 *check, u32 lo, u32 hi);

int mii_db_save(mii_db *db) {
	u8 *check;
	u32 lo, hi;
	int r;

	if (!db || !db->file)
		return MII_E_ARG;
	lo = db->dirty_lo;
	hi = db->dirty_hi > db->dirty_lo ? db->dirty_hi : 0;
	check = malloc(DB_SIZE);
	if (!check)
		return MII_E_MEM;
	r = save(db, check, lo, hi);
	free(check);
	return r;
}

static int save(mii_db *db, u8 *check, u32 lo, u32 hi) {
	s32 fd;
	int r;

	ISFS_Initialize();
	fd = IOS_Open(db_path, IPC_OPEN_RW);
	if (fd < 0)
		return nand_error(fd);
	/* Nobody else wrote it since we loaded it. */
	r = nand_read(fd, 0, check, DB_SIZE);
	if (r == MII_OK && (mii_crc16(check, DB_SIZE) || get16(check + DB_CRC) != db->loaded_crc))
		r = MII_E_CHANGED;
	if (r == MII_OK && !hi && !memcmp(check, db->file, DB_SIZE)) {
		IOS_Close(fd);
		return MII_OK;           /* nothing to write */
	}
	/* The hidden Miis first, then the table and slots that point to them. */
	if (r == MII_OK && hi)
		r = nand_write(fd, lo, db->file + lo, hi - lo);
	if (r == MII_OK)
		r = nand_write(fd, 0, db->file, DB_SIZE);
	if (IOS_Close(fd) < 0 && r == MII_OK)
		r = MII_E_IO;
	if (r < 0)
		return r;

	/* Read it back. */
	fd = IOS_Open(db_path, IPC_OPEN_READ);
	if (fd < 0)
		return nand_error(fd);
	r = nand_read(fd, 0, check, DB_SIZE);
	if (r == MII_OK && memcmp(check, db->file, DB_SIZE))
		r = MII_E_VERIFY;
	while (r == MII_OK && lo < hi) {
		u32 n = hi - lo < DB_SIZE ? hi - lo : DB_SIZE;
		r = nand_read(fd, lo, check, n);
		if (r == MII_OK && memcmp(check, db->file + lo, n))
			r = MII_E_VERIFY;
		lo += n;
	}
	IOS_Close(fd);
	if (r == MII_OK) {
		db->loaded_crc = get16(db->file + DB_CRC);
		db->dirty_lo = db->dirty_hi = 0;
	}
	return r;
}

void mii_db_free(mii_db *db) {
	if (db && db->owned && db->file)
		free(db->file);
	if (db)
		memset(db, 0, sizeof(*db));
}

int mii_get(int slot, mii *m) {
	mii_db db;
	u8 *head_part;
	s32 fd;
	int r;

	if (slot < 0 || slot >= MII_DB_SLOTS || !m)
		return MII_E_ARG;
	ISFS_Initialize();
	fd = IOS_Open(db_path, IPC_OPEN_READ);
	if (fd < 0)
		return nand_error(fd);
	head_part = malloc(DB_SIZE);
	if (!head_part) {
		IOS_Close(fd);
		return MII_E_MEM;
	}
	r = nand_read(fd, 0, head_part, DB_SIZE);
	IOS_Close(fd);
	if (r == MII_OK && (get32(head_part) != 0x524e4f44 || mii_crc16(head_part, DB_SIZE)))
		r = MII_E_FORMAT;
	if (r == MII_OK) {
		db.file = head_part;     /* the slots are all this reads */
		r = mii_db_get(&db, slot, m);
	}
	free(head_part);
	return r;
}

/* ---- The Remote's memory ---------------------------------------------------------------
 * Through wiiuse, which libogc's WPAD runs on. Its remotes are in a static
 * array that WPAD_Rumble indexes right after loading its address from the
 * small-data area (lwz rA,off(r13); slwi r3,r3,2; lwzx r3,rA,r3); found the
 * way sdk/wiispk finds it, and nothing is used unless every entry is a
 * plausible remote that knows its own channel. */

#define ST_CONNECTED 0x000010

static struct wiimote_t **remotes;
static bool looked;

static bool plausible(const void *p) {
	uintptr_t a = (uintptr_t) p;

	return (a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x94000000);
}

static struct wiimote_t **locate_remotes(void) {
	const u32 *op = (const u32 *) WPAD_Rumble;
	register u32 sda asm("r13");
	int i, chan;

	for (i = 0; i < 48; ++i) {
		bool load_sda = (op[i] >> 26) == 32 && ((op[i] >> 16) & 31) == 13;
		bool shift = op[i + 1] == 0x5463103a;
		bool indexed = (op[i + 2] & 0xfc0007fe) == 0x7c00002e;
		struct wiimote_t **table;

		if (!load_sda || !shift || !indexed)
			continue;
		table = *(struct wiimote_t ***) (sda + (s16) (op[i] & 0xffff));
		if (!plausible(table))
			return NULL;
		for (chan = 0; chan < 4; ++chan)
			if (table[chan] && (!plausible(table[chan]) || table[chan]->unid != chan))
				return NULL;
		return table;
	}
	return NULL;
}

static struct wiimote_t *remote(int chan) {
	if (!looked) {
		remotes = locate_remotes();
		looked = true;
	}
	if (!remotes || chan < 0 || chan > 3 || !remotes[chan])
		return NULL;
	return (remotes[chan]->state & ST_CONNECTED) ? remotes[chan] : NULL;
}

/* One transfer at a time, each waited for. A transfer that times out may
 * still finish later into these buffers, so they are static, and the next
 * call waits for the Remote's queue to empty first. */
static u8 rm_mem[2 * MII_REMOTE_BLOCK] ATTRIBUTE_ALIGN(32);
static u8 rm_block[MII_REMOTE_BLOCK] ATTRIBUTE_ALIGN(32);
static volatile int rm_done;
static volatile u32 rm_len;

static void rm_cb(struct wiimote_t *wm, ubyte *data, uword len) {
	(void) wm;
	(void) data;
	rm_len = len;
	rm_done = 1;
}

static bool rm_idle(struct wiimote_t *wm) {
	return *(void *volatile *) &wm->cmd_head == NULL;
}

static int rm_wait(struct wiimote_t *wm, u32 ms) {
	u64 end = gettime() + millisecs_to_ticks(ms);

	while (!rm_done) {
		if (!(wm->state & ST_CONNECTED))
			return MII_E_NO_REMOTE;
		if (gettime() > end)
			return MII_E_TIMEOUT;
		usleep(1000);
	}
	return MII_OK;
}

static int rm_start(struct wiimote_t *wm) {
	u64 end = gettime() + millisecs_to_ticks(2000);

	while (!rm_idle(wm)) {          /* the app's own reports, or a late one of ours */
		if (gettime() > end)
			return MII_E_TIMEOUT;
		usleep(1000);
	}
	return MII_OK;
}

static int rm_read(struct wiimote_t *wm, u32 addr, u8 *buf, u16 len) {
	u32 level;
	int ok, r;

	if ((r = rm_start(wm)) < 0)
		return r;
	rm_done = 0;
	rm_len = 0;
	_CPU_ISR_Disable(level);
	ok = wiiuse_read_data(wm, buf, addr, len, rm_cb);
	_CPU_ISR_Restore(level);
	if (!ok)
		return MII_E_IO;
	if ((r = rm_wait(wm, 3000)) < 0)
		return r;
	return rm_len == len ? MII_OK : MII_E_IO;
}

/* A write report carries up to 16 bytes, but libogc 3's wiiuse_write_data
 * keeps only the low 4 bits of the length: 16 goes out as 0, which a Remote
 * ignores without an answer. 8 works with every libogc, and 0x2F0 is 94 of
 * them. */
#define RM_WRITE 8

static int rm_write(struct wiimote_t *wm, u32 addr, const u8 *data, u32 len) {
	while (len) {
		u8 n = len < RM_WRITE ? (u8) len : RM_WRITE;
		u32 level;
		int ok, r;

		if ((r = rm_start(wm)) < 0)
			return r;
		rm_done = 0;
		_CPU_ISR_Disable(level);
		ok = wiiuse_write_data(wm, addr, (ubyte *) data, n, rm_cb);
		_CPU_ISR_Restore(level);
		if (!ok)
			return MII_E_IO;
		if ((r = rm_wait(wm, 1000)) < 0)
			return r;
		addr += n;
		data += n;
		len -= n;
	}
	return MII_OK;
}

int mii_remote_read_raw(int chan, uint8_t mem[2 * MII_REMOTE_BLOCK]) {
	struct wiimote_t *wm = remote(chan);
	int res;

	if (!mem || chan < 0 || chan > 3)
		return MII_E_ARG;
	if (!wm)
		return MII_E_NO_REMOTE;
	if ((res = rm_read(wm, MII_REMOTE_ADDR, rm_mem, sizeof(rm_mem))) < 0)
		return res;
	memcpy(mem, rm_mem, sizeof(rm_mem));
	return MII_OK;
}

int mii_remote_read(int chan, mii_remote *r) {
	static u8 mem[2 * MII_REMOTE_BLOCK];
	int res;

	if (!r)
		return MII_E_ARG;
	if ((res = mii_remote_read_raw(chan, mem)) < 0)
		return res;
	return mii_remote_parse(mem, sizeof(mem), r);
}

/* Each copy written, then read back, before the next is touched: whatever
 * happens part way, one copy is whole (old or new) and a reader takes it. */
int mii_remote_write(int chan, const mii_remote *r) {
	struct wiimote_t *wm = remote(chan);
	int c, res;

	if (!r || chan < 0 || chan > 3)
		return MII_E_ARG;
	if (!wm)
		return MII_E_NO_REMOTE;
	mii_remote_build(r, rm_block);
	for (c = 0; c < 2; ++c) {
		u32 addr = MII_REMOTE_ADDR + c * MII_REMOTE_BLOCK;

		if ((res = rm_write(wm, addr, rm_block, MII_REMOTE_BLOCK)) < 0)
			return res;
		if ((res = rm_read(wm, addr, rm_mem, MII_REMOTE_BLOCK)) < 0)
			return res;
		if (memcmp(rm_mem, rm_block, MII_REMOTE_BLOCK))
			return MII_E_VERIFY;
	}
	return MII_OK;
}

#endif /* MII_NO_OGC */
