/*
 * mboard.c - the Wii Message Board from homebrew; see mboard.h and
 * docs/mboard.md. Public domain.
 *
 * In order: bytes, the VFF (FAT16 and FAT12 volumes in NAND files), the play
 * history (cdb.vff), the mailboxes (posts and sends), the Address Book,
 * text, and the libogc half (IOS, KD, the settings), which MBOARD_NO_OGC
 * leaves out.
 *
 * A post writes what the Revolution SDK's NWC24CommitMsg() writes for a
 * message a game addresses to its own Wii (its loop-back delivery), and a
 * send what it writes for one to another Wii, field for field. The
 * Address Book is written as NWC24WriteFriendInfo(), UpdateFriendInfo() and
 * DeleteFriendInfo() write it. The play-history writer is the one the
 * Homebrew Channel logs with (playtime.c builds this file, its cores only),
 * checked byte for byte against the Wii Menu's accepted reference
 * (tests/test_cdblog.py, tests/test_mboard.py).
 */

#include <stdlib.h>
#include <string.h>

#include "mboard.h"

/* ---- Bytes ------------------------------------------------------------- */

static uint32_t get32(const uint8_t *p) {
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint64_t get64(const uint8_t *p) {
	return (uint64_t) get32(p) << 32 | get32(p + 4);
}

static void put64(uint8_t *p, uint64_t v) {
	put32(p, v >> 32);
	put32(p + 4, (uint32_t) v);
}

static void put16(uint8_t *p, uint16_t v) {
	p[0] = v >> 8;
	p[1] = (uint8_t) v;
}

static uint16_t le16(const uint8_t *p) {
	return p[0] | p[1] << 8;
}

static void put_le16(uint8_t *p, uint16_t v) {
	p[0] = v;
	p[1] = v >> 8;
}

static void put_le32(uint8_t *p, uint32_t v) {
	put_le16(p, v);
	put_le16(p + 2, v >> 16);
}

/* zlib's CRC-32, bitwise: it runs over 0x140 bytes once per write. */
static uint32_t crc32(const uint8_t *p, uint32_t n) {
	uint32_t c = 0xffffffff;
	int k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; ++k)
			c = c & 1 ? (c >> 1) ^ 0xedb88320 : c >> 1;
	}
	return ~c;
}

/* ---- VFF: the Wii's FAT volume in a NAND file --------------------------
 * A 0x20-byte header, two FATs, a 0x1000-byte root directory, then 512-byte
 * clusters from cluster 2. FAT16 for the big volumes (cdb.vff,
 * wc24recv.mbx), FAT12 below 4085 clusters (wc24send.mbx, 2 MiB). Either
 * way each FAT is as long as a FAT16 one would be (two bytes a cluster):
 * the bench Wii's send box has its second FAT at 0x2020 and its root at
 * 0x4020, with FAT12 entries packed at the start. */

#define CL 0x200u
#define ROOT_LEN 0x1000u
#define FAT_MAX 0x14000u
#define DIR_MAX 128  /* entries a directory may have: 8 clusters */

typedef struct {
	const mboard_io *io;
	uint8_t *fat;
	uint32_t fat_len, fat1, fat2, root, data, nclusters;
	int fat12;
	uint16_t eoc;     /* the end of a chain: 0xfff, or 0xffff */
	uint8_t dirty[FAT_MAX / 512];
	int err;
} vff;

typedef struct {
	uint16_t t, d;
} stamp;

static int rd(vff *v, uint32_t off, void *buf, uint32_t len) {
	if (!v->err && v->io->read(v->io->ctx, off, buf, len))
		v->err = MBOARD_E_IO;
	return v->err;
}

static int wr(vff *v, uint32_t off, const void *buf, uint32_t len) {
	if (!v->err && v->io->write(v->io->ctx, off, buf, len))
		v->err = MBOARD_E_IO;
	return v->err;
}

static int vff_open(vff *v, const mboard_io *io, uint32_t size) {
	static const uint8_t fat_head[4] = { 0xf0, 0xff, 0xff, 0xff };
	uint8_t head[16];
	uint32_t clusters;

	memset(v, 0, sizeof(*v));
	v->io = io;
	if (rd(v, 0, head, sizeof(head)))
		return v->err;
	if (memcmp(head, "VFF ", 4) || head[4] != 0xfe || head[5] != 0xff || get32(head + 8) != size ||
			(uint32_t) (head[12] << 8 | head[13]) * 16 != CL)
		return v->err = MBOARD_E_FORMAT;
	clusters = size / CL;
	v->fat_len = (clusters * 2 + 511) & ~511u;
	if (clusters < 64 || v->fat_len > FAT_MAX)
		return v->err = MBOARD_E_FORMAT;
	v->fat1 = 0x20;
	v->fat2 = v->fat1 + v->fat_len;
	v->root = v->fat2 + v->fat_len;
	v->data = v->root + ROOT_LEN;
	v->nclusters = (size - v->data) / CL + 2;
	/* FAT12 by the data clusters' count, as FAT itself decides it. */
	v->fat12 = v->nclusters - 2 < 4085;
	v->eoc = v->fat12 ? 0xfff : 0xffff;
	v->fat = malloc(v->fat_len);
	if (!v->fat)
		return v->err = MBOARD_E_MEM;
	/* The media byte and the reserved entry: f0 ff ff ff, or f0 ff ff for FAT12. */
	if (!rd(v, v->fat1, v->fat, v->fat_len) && memcmp(v->fat, fat_head, v->fat12 ? 3 : 4))
		v->err = MBOARD_E_FORMAT;
	return v->err;
}

static void vff_close(vff *v) {
	free(v->fat);
	v->fat = NULL;
}

/* Both FAT copies, only the sectors that changed. */
static int vff_flush(vff *v) {
	uint32_t i;

	for (i = 0; i < v->fat_len / 512; ++i)
		if (v->dirty[i]) {
			wr(v, v->fat1 + 512 * i, v->fat + 512 * i, 512);
			wr(v, v->fat2 + 512 * i, v->fat + 512 * i, 512);
			v->dirty[i] = 0;
		}
	return v->err;
}

static uint32_t at(vff *v, uint16_t c) {
	return v->data + (uint32_t) (c - 2) * CL;
}

/* FAT12 packs two entries in three bytes: an even cluster's in the low 12
 * bits of its two bytes, an odd one's in the high 12. */
static uint16_t fat_get(vff *v, uint16_t c) {
	uint16_t w;

	if (!v->fat12)
		return le16(v->fat + 2 * c);
	w = le16(v->fat + c + c / 2);
	return c & 1 ? w >> 4 : w & 0xfff;
}

static void fat_set(vff *v, uint16_t c, uint16_t val) {
	uint32_t off;
	uint16_t w;

	if (!v->fat12) {
		put_le16(v->fat + 2 * c, val);
		v->dirty[2 * c / 512] = 1;
		return;
	}
	off = c + c / 2u;
	w = le16(v->fat + off);
	w = c & 1 ? (uint16_t) ((w & 0x000f) | val << 4) : (uint16_t) ((w & 0xf000) | (val & 0xfff));
	put_le16(v->fat + off, w);
	v->dirty[off / 512] = 1;
	v->dirty[(off + 1) / 512] = 1;
}

static int is_cluster(vff *v, uint16_t c) {
	return c >= 2 && c < (v->fat12 ? 0xff8 : 0xfff8) && c < v->nclusters;
}

static uint32_t free_clusters(vff *v) {
	uint32_t c, n = 0;

	for (c = 2; c < v->nclusters; ++c)
		n += !fat_get(v, (uint16_t) c);
	return n;
}

/* A free cluster, linked after `after` (0: a new chain). Directory clusters
 * are zeroed; file clusters are written whole by the caller. */
static uint16_t alloc(vff *v, uint16_t after, int zero) {
	static const uint8_t zeros[CL];
	uint16_t c;

	for (c = 2; c < v->nclusters; ++c)
		if (!fat_get(v, c))
			break;
	if (c >= v->nclusters) {
		v->err = MBOARD_E_FULL;
		return 0;
	}
	fat_set(v, c, v->eoc);
	if (after)
		fat_set(v, after, c);
	if (zero)
		wr(v, at(v, c), zeros, CL);
	return c;
}

/* The byte offsets of a directory's entries (the root is a fixed area). */
static int slots(vff *v, uint16_t start, uint32_t *offs) {
	int n = 0, i, guard = 0;

	if (!start) {
		for (i = 0; i < (int) (ROOT_LEN / 32); ++i)
			offs[n++] = v->root + 32 * i;
		return n;
	}
	for (; is_cluster(v, start) && guard < DIR_MAX / 16; start = fat_get(v, start), ++guard)
		for (i = 0; i < (int) (CL / 32); ++i)
			offs[n++] = at(v, start) + 32 * i;
	return n;
}

static uint32_t find(vff *v, uint16_t dir, const char *name11) {
	uint32_t offs[DIR_MAX];
	uint8_t e[32];
	int n = slots(v, dir, offs), i;

	for (i = 0; i < n && !rd(v, offs[i], e, 32); ++i) {
		if (!e[0])
			break;
		if (e[0] != 0xe5 && e[11] != 0x0f && !memcmp(e, name11, 11))
			return offs[i];
	}
	return 0;
}

static uint16_t entry_cluster(vff *v, uint32_t off) {
	uint8_t e[32];

	if (rd(v, off, e, 32))
		return 0;
	return le16(e + 26);
}

/* n consecutive free entries, growing the directory when it has none. */
static int free_slots(vff *v, uint16_t dir, int need, uint32_t *out) {
	uint32_t offs[DIR_MAX];
	uint8_t e[32];
	int n, i, run, tries;

	for (tries = 0; tries < 4 && !v->err; ++tries) {
		n = slots(v, dir, offs);
		for (i = 0, run = 0; i < n && !rd(v, offs[i], e, 32); ++i) {
			if (e[0] == 0 || e[0] == 0xe5) {
				out[run++] = offs[i];
				if (run == need)
					return 0;
			} else {
				run = 0;
			}
		}
		if (!dir || v->err)
			break;
		{
			uint16_t last = dir;
			while (is_cluster(v, fat_get(v, last)))
				last = fat_get(v, last);
			if (!alloc(v, last, 1))
				break;
		}
	}
	if (!v->err)
		v->err = MBOARD_E_FULL;
	return v->err;
}

static void put_entry(vff *v, uint32_t off, const char *name11, uint8_t attr, uint16_t cluster,
					  uint32_t size, stamp s) {
	uint8_t e[32];

	memset(e, 0, sizeof(e));
	memcpy(e, name11, 11);
	e[11] = attr;
	e[13] = 1;
	put_le16(e + 14, s.t);
	put_le16(e + 16, s.d);
	put_le16(e + 18, s.d);
	put_le16(e + 22, s.t);
	put_le16(e + 24, s.d);
	put_le16(e + 26, cluster);
	put_le32(e + 28, size);
	wr(v, off, e, 32);
}

/* The one long-name entry the Wii's VF library writes for a lower-case
 * name of up to 13 characters (the Wii Menu's "log", the mailboxes' "mb"). */
static void put_lfn(vff *v, uint32_t off, const char *name11, const char *lfn) {
	uint8_t e[32], sum = 0, units[26];
	size_t k, len = strlen(lfn);
	int i;

	for (i = 0; i < 11; ++i)
		sum = (uint8_t) ((((sum & 1) << 7) | (sum >> 1)) + (uint8_t) name11[i]);
	memset(units, 0xff, sizeof(units));
	for (k = 0; k <= len && k < 13; ++k)
		put_le16(units + 2 * k, k < len ? (uint8_t) lfn[k] : 0);
	memset(e, 0, 32);
	e[0] = 0x41;
	e[11] = 0x0f;
	e[13] = sum;
	memcpy(e + 1, units, 10);
	memcpy(e + 14, units + 10, 12);
	memcpy(e + 28, units + 22, 4);
	wr(v, off, e, 32);
}

static uint16_t mkdir_in(vff *v, uint16_t parent, const char *name11, stamp s, const char *lfn) {
	uint32_t where[2];
	uint16_t c = alloc(v, 0, 1);

	if (!c || free_slots(v, parent, lfn ? 2 : 1, where))
		return 0;
	if (lfn)
		put_lfn(v, where[0], name11, lfn);
	put_entry(v, where[lfn ? 1 : 0], name11, 0x10, c, 0, s);
	put_entry(v, at(v, c), ".          ", 0x10, c, 0, s);
	put_entry(v, at(v, c) + 32, "..         ", 0x10, parent, 0, s);
	return v->err ? 0 : c;
}

static uint16_t subdir(vff *v, uint16_t parent, const char *name11, stamp s, const char *lfn) {
	uint32_t off = find(v, parent, name11);

	if (v->err)
		return 0;
	if (off)
		return entry_cluster(v, off);
	return mkdir_in(v, parent, name11, s, lfn);
}

#define MSG_MAX 8192u

/* A file's contents, up to MSG_MAX bytes. */
static int read_file(vff *v, uint32_t off, uint8_t *buf, uint32_t *size) {
	uint8_t e[32];
	uint32_t got = 0, len;
	uint16_t c;

	if (rd(v, off, e, 32))
		return v->err;
	len = e[28] | e[29] << 8 | e[30] << 16 | (uint32_t) e[31] << 24;
	if (len > MSG_MAX)
		return v->err = MBOARD_E_FORMAT;
	for (c = le16(e + 26); got < len && is_cluster(v, c); c = fat_get(v, c), got += CL)
		if (rd(v, at(v, c), buf + got, len - got < CL ? len - got : CL))
			return v->err;
	if (got < len)
		return v->err = MBOARD_E_FORMAT;
	*size = len;
	return 0;
}

/* Rewrite a file in place: its chain grows or shrinks to fit. With
 * entry_last, its directory entry is written only after the FAT: then a
 * failure part way leaves at most clusters no entry points to, never an
 * entry pointing at clusters the FAT calls free. */
static int write_file(vff *v, uint32_t off, const uint8_t *data, uint32_t len, stamp s,
					  int entry_last) {
	uint16_t chain[0x40000 / CL + 2], c;
	uint8_t e[32], block[CL];
	uint32_t need = len ? (len + CL - 1) / CL : 1, n = 0, i;

	if (need > sizeof(chain) / sizeof(chain[0]) - 2)
		return v->err = MBOARD_E_FULL;
	if (rd(v, off, e, 32))
		return v->err;
	for (c = le16(e + 26); is_cluster(v, c) && n < need + 2; c = fat_get(v, c))
		chain[n++] = c;
	while (n < need) {
		c = alloc(v, n ? chain[n - 1] : 0, 0);
		if (!c)
			return v->err;
		chain[n++] = c;
	}
	if (!le16(e + 26))
		put_le16(e + 26, chain[0]);
	for (i = need; i < n; ++i)
		fat_set(v, chain[i], 0);
	fat_set(v, chain[need - 1], v->eoc);
	for (i = 0; i < need; ++i) {
		uint32_t part = len - i * CL < CL ? len - i * CL : CL;
		memset(block, 0, CL);
		memcpy(block, data + i * CL, part);
		wr(v, at(v, chain[i]), block, CL);
	}
	put_le16(e + 22, s.t);
	put_le16(e + 24, s.d);
	put_le32(e + 28, len);
	if (entry_last)
		vff_flush(v);
	return wr(v, off, e, 32);
}

/* ---- Dates --------------------------------------------------------------- */

typedef struct {
	int y, mo, d, h, mi, s;  /* month 1-12 */
} civil;

static civil civil_from_secs(uint64_t secs) {  /* since 2000-01-01 */
	int64_t z = (int64_t) (secs / 86400) + 10957 + 719468;  /* days since 0000-03-01 */
	int64_t era = z / 146097, doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	civil c;

	c.d = (int) (doy - (153 * mp + 2) / 5 + 1);
	c.mo = (int) (mp < 10 ? mp + 3 : mp - 9);
	c.y = (int) (yoe + era * 400 + (c.mo <= 2));
	c.h = (int) (secs % 86400 / 3600);
	c.mi = (int) (secs % 3600 / 60);
	c.s = (int) (secs % 60);
	return c;
}

static civil to_civil(uint64_t ticks) {
	return civil_from_secs(ticks / MBOARD_TICKS_PER_SEC);
}

static stamp fat_stamp(uint64_t ticks) {
	civil c = to_civil(ticks);
	stamp s;

	s.t = (uint16_t) (c.h << 11 | c.mi << 5 | c.s / 2);
	s.d = (uint16_t) ((c.y - 1980) << 9 | c.mo << 5 | c.d);
	return s;
}

/* "NN" or "NNNN", space-padded to an 8.3 name. */
static void name83(char *out, const char *base, const char *ext) {
	size_t i;

	memset(out, ' ', 11);
	for (i = 0; base[i] && i < 8; ++i)
		out[i] = base[i];
	for (i = 0; ext && ext[i] && i < 3; ++i)
		out[8 + i] = ext[i];
}

static void num83(char *out, int v, int digits) {
	char b[8];
	int i;

	for (i = digits - 1; i >= 0; --i, v /= 10)
		b[i] = (char) ('0' + v % 10);
	b[digits] = 0;
	name83(out, b, NULL);
}

/* ---- Today's Play History (cdb.vff) ---------------------------------------
 * docs/messageboard.md has the format. */

#define CDB_SIZE 0x01400000u
#define TEXT_AT 0x548u
#define LIST_SIZE 0x668u
#define TAIL 0x18u
#define ENTRY 0x88u
#define MAX_TITLES 12

static void record(uint8_t *r, const uint8_t *name84, const char *id, uint64_t start,
				   uint64_t end) {
	uint32_t sum = 0;
	int i;

	memset(r, 0, 128);
	memcpy(r + 4, name84, 84);
	put64(r + 0x58, start);
	put64(r + 0x60, end);
	memcpy(r + 0x68, id, 6);
	for (i = 1; i < 32; ++i)
		sum += get32(r + 4 * i);
	put32(r, sum);
}

typedef struct {
	uint8_t *p;
	uint32_t n, max;
} text;

static void put_unit(text *t, uint16_t u) {
	if (t->n + 2 <= t->max) {
		t->p[t->n++] = u >> 8;
		t->p[t->n++] = (uint8_t) u;
	}
}

static void put_ascii(text *t, const char *s) {
	while (*s)
		put_unit(t, (uint8_t) *s++);
}

static void put_hhmm(text *t, uint64_t ticks) {
	uint64_t m = ticks / MBOARD_TICKS_PER_SEC / 60;
	char b[12];
	int i = 0;
	uint64_t h = m / 60;

	if (h >= 100)
		b[i++] = (char) ('0' + h / 100 % 10);
	b[i++] = (char) ('0' + h / 10 % 10);
	b[i++] = (char) ('0' + h % 10);
	b[i++] = ':';
	b[i++] = (char) ('0' + m % 60 / 10);
	b[i++] = (char) ('0' + m % 10);
	b[i] = 0;
	put_ascii(t, b);
}

/* Rebuild the message from its header and list (English, as the Wii Menu
 * writes it; the Wii Menu regenerates the text in its own language when it
 * next adds a record). */
static uint32_t build(uint8_t *out, const uint8_t *header, const uint8_t *entries, int count,
					  uint64_t now) {
	text t;
	uint64_t total = 0, newest = 0;
	uint32_t list_at, i;
	int k;

	memset(out, 0, MSG_MAX);
	memcpy(out, header, TEXT_AT);
	t.p = out + TEXT_AT;
	t.n = 0;
	t.max = MSG_MAX - TEXT_AT - LIST_SIZE - TAIL - 32;
	put_ascii(&t, "Today's Accomplishments");
	put_unit(&t, 0);
	put_ascii(&t, "Today's Play History\n\n");
	for (k = 0; k < count; ++k) {
		const uint8_t *r = entries + k * ENTRY + 8;
		uint64_t b = get64(r + 0x58), l = get64(r + 0x60);

		if (k)
			put_ascii(&t, "\n\n");
		for (i = 0; i < 40; ++i) {
			uint16_t u = (uint16_t) (r[4 + 2 * i] << 8 | r[5 + 2 * i]);
			if (!u)
				break;
			put_unit(&t, u);
		}
		put_ascii(&t, "\n     ");
		put_hhmm(&t, l - b);
		total += l - b;
		if (l > newest)
			newest = l;
	}
	put_ascii(&t, "\n\nTotal Play Time\n     ");
	put_hhmm(&t, total);
	put_unit(&t, 0);

	list_at = (TEXT_AT + t.n + 0x1f) & ~0x1fu;
	memcpy(out + list_at, "03_0", 4);
	memcpy(out + list_at + 8, entries, (uint32_t) count * ENTRY);
	put32(out + 0x74, (uint32_t) count);
	put32(out + 0x7c, (uint32_t) (newest / MBOARD_TICKS_PER_SEC));
	put64(out + 0x410, now);
	put32(out + 0x52c, list_at - 0x400);
	put32(out + 0x540, crc32(out + 0x400, 0x140));
	return list_at + LIST_SIZE + TAIL;
}

static void new_header(uint8_t *h, const uint8_t *wiiid, uint32_t number) {
	memset(h, 0, TEXT_AT);
	memcpy(h, "CDBFILE\x02", 8);
	memcpy(h + 8, wiiid, 8);
	put_le32(h + 0x10, 12);  /* little-endian, unlike the rest */
	memcpy(h + 0x14, "playtimelog", 12);
	put32(h + 0x70, number);
	memcpy(h + 0x400, "RI_5", 4);
	put32(h + 0x40c, 2);
	put32(h + 0x518, 1);
	put32(h + 0x51c, 0x148);
	put32(h + 0x520, 0x178);
	put32(h + 0x528, 3);
	put32(h + 0x530, LIST_SIZE);
}

static int is_dir_entry(const uint8_t *e) {
	return e[0] && e[0] != 0xe5 && e[0] != '.' && e[11] == 0x10;
}

/* The last playtimelog message under the day's folder, or 0. */
static uint32_t find_day(vff *v, const civil *c) {
	uint32_t hours[DIR_MAX], mins[DIR_MAX], files[DIR_MAX], found = 0;
	uint8_t e[32], head[0x20];
	char n11[11];
	uint16_t dir = 0;
	int nh, nm, nf, i, j, k;

	num83(n11, c->y, 4);
	if (!(dir = find(v, dir, n11) ? entry_cluster(v, find(v, dir, n11)) : 0))
		return 0;
	num83(n11, c->mo - 1, 2);
	if (!(dir = find(v, dir, n11) ? entry_cluster(v, find(v, dir, n11)) : 0))
		return 0;
	num83(n11, c->d, 2);
	if (!(dir = find(v, dir, n11) ? entry_cluster(v, find(v, dir, n11)) : 0))
		return 0;
	nh = slots(v, dir, hours);
	for (i = 0; i < nh && !rd(v, hours[i], e, 32) && e[0]; ++i) {
		if (!is_dir_entry(e))
			continue;
		nm = slots(v, le16(e + 26), mins);
		for (j = 0; j < nm && !rd(v, mins[j], e, 32) && e[0]; ++j) {
			uint16_t cur;
			uint32_t off;

			if (!is_dir_entry(e))
				continue;
			cur = le16(e + 26);
			off = find(v, cur, "HAEA_#1    ");
			cur = off ? entry_cluster(v, off) : 0;
			off = cur ? find(v, cur, "LOG        ") : 0;
			cur = off ? entry_cluster(v, off) : 0;
			if (!cur)
				continue;
			nf = slots(v, cur, files);
			for (k = 0; k < nf && !rd(v, files[k], e, 32) && e[0]; ++k) {
				if (e[0] == 0xe5 || e[11] & 0x18 || !is_cluster(v, le16(e + 26)))
					continue;
				if (!rd(v, at(v, le16(e + 26)), head, sizeof(head)) &&
						!memcmp(head, "CDBFILE\x02", 8) && !memcmp(head + 0x14, "playtimelog", 12))
					found = files[k];
			}
		}
	}
	return found;
}

static int scan_wiiid(vff *v, uint8_t *id) {
	uint8_t head[16];
	uint16_t c;

	for (c = 2; c < v->nclusters && !v->err; ++c) {
		if (!fat_get(v, c) || rd(v, at(v, c), head, 16))
			continue;
		if (!memcmp(head, "CDBFILE\x02", 8)) {
			memcpy(id, head + 8, 8);
			return 0;
		}
	}
	return -1;
}

static int play_add(vff *v, const mboard_session *s, uint64_t now, const uint8_t *wiiid,
					uint8_t *msg, uint8_t *out) {
	uint8_t name84[84], entries[MAX_TITLES * ENTRY], rec[128], id[8];
	civil end = to_civil(s->end);
	stamp st = fat_stamp(now);
	uint32_t foff, size, list, len, conf, number, where;
	char n11[11], hex[9];
	uint16_t dir;
	int count, i;

	memset(name84, 0, sizeof(name84));
	for (i = 0; i < 40 && s->name[i]; ++i) {
		name84[2 * i] = s->name[i] >> 8;
		name84[2 * i + 1] = (uint8_t) s->name[i];
	}
	record(rec, name84, s->id, s->start, s->end);

	foff = find_day(v, &end);
	if (v->err)
		return v->err;
	if (foff) {
		if (read_file(v, foff, msg, &size))
			return v->err;
		count = (int) get32(msg + 0x74);
		list = get32(msg + 0x52c) + 0x400;
		if (count < 0 || count > MAX_TITLES || list + 8 + (uint32_t) count * ENTRY > size ||
				memcmp(msg + list, "03_0", 4))
			return MBOARD_E_FORMAT;
		memcpy(entries, msg + list + 8, (uint32_t) count * ENTRY);
		for (i = 0; i < count; ++i) {
			uint8_t *e = entries + i * ENTRY;
			if (!memcmp(e + 8 + 4, rec + 4, 84) && !memcmp(e + 8 + 0x68, rec + 0x68, 6)) {
				uint64_t b = get64(e + 8 + 0x58), l = get64(e + 8 + 0x60);
				char old_id[6];
				uint8_t old_name[84];

				/* record() clears its target first: copy what it reads. */
				memcpy(old_name, e + 8 + 4, 84);
				memcpy(old_id, e + 8 + 0x68, 6);
				record(e + 8, old_name, old_id, b, b + (l - b) + (s->end - s->start));
				len = build(out, msg, entries, count, now);
				write_file(v, foff, out, len, st, 0);
				return v->err ? v->err : MBOARD_COMBINED;
			}
		}
		if (count >= MAX_TITLES)
			return MBOARD_E_FULL;
		put_le32(entries + count * ENTRY, 1);
		put_le32(entries + count * ENTRY + 4, 0);
		memcpy(entries + count * ENTRY + 8, rec, 128);
		len = build(out, msg, entries, count + 1, now);
		write_file(v, foff, out, len, st, 0);
		return v->err ? v->err : MBOARD_OK;
	}

	/* A new message: the next number after cdb.conf's, folders from END. */
	conf = find(v, 0, "CDB~1   CON");
	if (!conf || read_file(v, conf, msg, &size) || size < 4)
		return v->err ? v->err : MBOARD_E_FORMAT;
	number = get32(msg) + 1;
	if (wiiid)
		memcpy(id, wiiid, 8);
	else if (scan_wiiid(v, id))
		return v->err ? v->err : MBOARD_E_FORMAT;

	dir = 0;
	num83(n11, end.y, 4);
	dir = subdir(v, dir, n11, st, NULL);
	num83(n11, end.mo - 1, 2);
	if (dir)
		dir = subdir(v, dir, n11, st, NULL);
	num83(n11, end.d, 2);
	if (dir)
		dir = subdir(v, dir, n11, st, NULL);
	num83(n11, end.h, 2);
	if (dir)
		dir = subdir(v, dir, n11, st, NULL);
	num83(n11, end.mi, 2);
	if (dir)
		dir = subdir(v, dir, n11, st, NULL);
	if (dir)
		dir = subdir(v, dir, "HAEA_#1    ", st, NULL);
	if (dir)
		dir = subdir(v, dir, "LOG        ", st, "log");
	if (!dir)
		return v->err ? v->err : MBOARD_E_FULL;
	{
		static const char digits[] = "0123456789ABCDEF";
		uint32_t secs = (uint32_t) (s->end / MBOARD_TICKS_PER_SEC);
		for (i = 7; i >= 0; --i, secs >>= 4)
			hex[i] = digits[secs & 15];
		hex[8] = 0;
	}
	name83(n11, hex, "000");
	if (free_slots(v, dir, 1, &where))
		return v->err;
	put_entry(v, where, n11, 0x20, 0, 0, st);
	new_header(msg, id, number);
	put_le32(entries, 1);
	put_le32(entries + 4, 0);
	memcpy(entries + 8, rec, 128);
	len = build(out, msg, entries, 1, now);
	write_file(v, where, out, len, st, 0);
	put32(msg, number);
	write_file(v, conf, msg, 4, st, 0);
	return v->err ? v->err : MBOARD_CREATED;
}

int mboard_play_vff(const mboard_io *cdb, const mboard_session *s, uint64_t now,
					const uint8_t *wiiid) {
	uint8_t *msg, *out;
	vff v;
	int res;

	if (s->end < s->start)
		return MBOARD_E_FORMAT;
	if (vff_open(&v, cdb, CDB_SIZE)) {
		vff_close(&v);
		return v.err;
	}
	msg = malloc(MSG_MAX);
	out = malloc(MSG_MAX);
	if (!msg || !out)
		res = MBOARD_E_MEM;
	else
		res = play_add(&v, s, now, wiiid, msg, out);

	/* The FAT last: the message is rewritten in place first, so a failure
	 * part way can leave a half-written message, but never a FAT that
	 * points at unwritten clusters. */
	if (res >= 0)
		vff_flush(&v);
	if (v.err)
		res = v.err;
	free(msg);
	free(out);
	vff_close(&v);
	return res;
}

/* ---- The receive box: a game's message to its own Wii ---------------------
 * wc24recv.ctl: a 0x80-byte header, then 255 entries of 0x80 bytes; free
 * entries are chained through their app_id word. wc24recv.mbx: a 7 MiB
 * FAT16 VFF with the messages as /mb/rNNNNNNN.msg. */

/* The two mailboxes: wc24recv.* (a message to this Wii, and the SDK's
 * loop-back) and wc24send.* (what KD uploads to the mail server). */
typedef struct {
	uint32_t ctl_size, capacity, mbx_size;
	char letter;      /* the message files: /mb/r0000001.msg, /mb/s0000001.msg */
	uint32_t flag;    /* NWC24_MSGOBJ_TO_RECV or TO_SEND */
} box_kind;
#define BOX_ENTRY 0x80u
#define BOX_MAGIC 0x57635466u  /* 'WcTf' */
#define MSG_ID_MAX 1000000u
#define MSG_LIMIT 0x31c00u
#define STRING_WORK 1024u      /* NWC24i_STRING_WORK_SIZE */
#define B64_STEP 57            /* input bytes per 76-character line */

/* NWC24iMsgObj flags, as NWC24InitMsgObj(NWC24_MSGTYPE_WII_MENU) and
 * NWC24CommitMsg() leave them for a message delivered to the receive box. */
#define F_FOR_RECIPIENT 0x1u
#define F_FOR_MENU 0x8u
#define F_TO_SEND 0x10u
#define F_TO_RECV 0x20u
#define F_INITIALIZED 0x100u
#define F_DELIVERING 0x200u
#define F_21 0x200000u
#define LED_DEFAULT 0x00040001u    /* NWC24SetMsgLedPattern(NWC24_LED_APP_DEFAULT) */
#define TXT_PLAIN 0x00010000u      /* an unused attachment's type */
#define ICON_NEW_NONE 0x80000000u

static const box_kind RECV_BOX = { MBOARD_RECV_CTL_SIZE, 255, 0x700000, 'r', F_TO_RECV };
static const box_kind SEND_BOX = { MBOARD_SEND_CTL_SIZE, 127, 0x200000, 's', F_TO_SEND };

typedef struct {
	uint8_t *p;
	uint32_t n, max;
	int over;
} buf;

static void put_bytes(buf *b, const void *data, uint32_t len) {
	if (b->n + len > b->max) {
		b->over = 1;
		return;
	}
	memcpy(b->p + b->n, data, len);
	b->n += len;
}

static void put_str(buf *b, const char *s) {
	put_bytes(b, s, (uint32_t) strlen(s));
}

static void put_hex(buf *b, uint32_t v, int digits) {
	static const char hex[] = "0123456789ABCDEF";
	char t[8];
	int i;

	for (i = digits - 1; i >= 0; --i, v >>= 4)
		t[i] = hex[v & 15];
	put_bytes(b, t, (uint32_t) digits);
}

static void put_dec(buf *b, uint32_t v, int width) {
	char t[12];
	int n = 0, i;

	do {
		t[n++] = (char) ('0' + v % 10);
		v /= 10;
	} while (v);
	while (n < width)
		t[n++] = '0';
	for (i = n - 1; i >= 0; --i)
		put_bytes(b, &t[i], 1);
}

static void put_base64(buf *b, const uint8_t *p, uint32_t n) {
	static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char q[4];

	for (; n; p += n >= 3 ? 3 : n, n -= n >= 3 ? 3 : n) {
		uint32_t v = (uint32_t) p[0] << 16 | (n > 1 ? p[1] << 8 : 0) | (n > 2 ? p[2] : 0);
		q[0] = tab[v >> 18];
		q[1] = tab[v >> 12 & 63];
		q[2] = n > 1 ? tab[v >> 6 & 63] : '=';
		q[3] = n > 2 ? tab[v & 63] : '=';
		put_bytes(b, q, 4);
	}
}

/* "w" + a Wii number's 16 digits + the account domain (the SDK's
 * SynthesizeAddrStr(): every Wii address takes this Wii's domain). */
static void put_addr(buf *b, const mboard_env *env, uint64_t id) {
	char t[17];
	int i;

	for (i = 15; i >= 0; --i, id /= 10)
		t[i] = (char) ('0' + id % 10);
	put_str(b, "w");
	put_bytes(b, t, 16);
	put_str(b, env->domain);
}

static uint32_t pack(uint32_t off, uint32_t size) {
	return (off & 0x000fffff) | (size << 20 & 0xfff00000);
}

static int compare_id(uint32_t a, uint32_t b) {  /* CompareMsgId(): ids wrap at 1000000 */
	if (a == b)
		return 0;
	if (a > 900000 && b < 100000)
		return -1;
	if (a < 100000 && b > 900000)
		return 1;
	return a > b ? 1 : -1;
}

#define F_MULTIPART 0x10000u       /* attachments follow the text */
#define F_DESIGNATED 0x1000000u    /* NWC24SetMsgDesignatedTime(): held until 0x2C */
#define T_JPEG 0x00020000u
#define T_MSGBOARD 0x00030001u
#define ATTACH_MAX 2

typedef struct {
	uint32_t from, to, ctype, txenc, start, body, body_len, body_raw;
	uint32_t att[ATTACH_MAX], att_len[ATTACH_MAX];
} offsets;

/* What one post writes: the text in UTF-16, the rest as given. */
typedef struct {
	const uint16_t *title, *body;
	int title_len, body_len;
	const uint8_t *face;
	int reply, delay_hours;
	int b64_text;                /* to another Wii: the text part in base64 */
	uint16_t reg_date;           /* the SDK's: (year - 2000) << 9 | month << 5 | day */
	int n_att;
	const uint8_t *att[ATTACH_MAX];
	uint32_t att_size[ATTACH_MAX], att_type[ATTACH_MAX];
} parts;

/* A header of base64 lines, 42 bytes to a line: the alt name, the face. */
static void put_b64_field(buf *b, const char *key, const uint8_t *p, uint32_t n) {
	uint32_t at;

	put_str(b, key);
	for (at = 0; at < n; at += 42) {
		put_str(b, " ");
		put_base64(b, p + at, n - at < 42 ? n - at : 42);
		put_str(b, "\r\n");
	}
}

static void put_boundary(buf *b, uint32_t minutes, uint32_t msg_id, int end) {
	put_str(b, "\r\n--Boundary-NWC24-");
	put_hex(b, minutes, 8);
	put_hex(b, msg_id, 5);
	put_str(b, end ? "--\r\n" : "\r\n");
}

/* "a0000007.jpg": the part's letter, the message id, its type's suffix. */
static void put_part_name(buf *b, int i, uint32_t msg_id, uint32_t type) {
	char letter = (char) ('a' + i);

	put_bytes(b, &letter, 1);
	put_dec(b, msg_id, 7);
	put_str(b, type == T_JPEG ? ".jpg" : ".wii");
}

/* An attachment's data, as WriteBase64Data() writes it: 57 bytes (76
 * characters) to a line, then the rest and an empty line, even when the
 * rest is nothing. */
static void put_b64_body(buf *b, const uint8_t *p, uint32_t n) {
	for (; n >= B64_STEP; p += B64_STEP, n -= B64_STEP) {
		put_base64(b, p, B64_STEP);
		put_str(b, "\r\n");
	}
	put_base64(b, p, n);
	put_str(b, "\r\n\r\n");
}

/* The message, byte for byte as NWC24CommitMsg() writes it to the receive
 * box for a game's NWC24_MSGTYPE_WII_MENU message to itself: an alt name,
 * its text as UTF-16 in 8 bits and an LED pattern (Super Mario Galaxy's
 * letters), and what else the game set: a face, the board's Reply, filing
 * date and delay, and attachments. No SMTP envelope: it is not sent. */
static uint32_t compose(buf *b, offsets *o, const mboard_env *env, uint64_t to, uint32_t msg_id,
						uint32_t minutes, const parts *p) {
	static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
										  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	civil c = civil_from_secs((uint64_t) (env->utc - 946684800));
	uint8_t alt[2 * (MBOARD_TITLE_MAX + 1)];
	unsigned tl;
	int i;

	/* For the send box, the SMTP envelope KD hands the server (the SDK's
	 * WriteSMTP_MAILFROM, _RCPTTO, _DATA); the offsets below count it. */
	if (to != env->wii_number) {
		put_str(b, "MAIL FROM: ");
		put_addr(b, env, env->wii_number);
		put_str(b, "\r\nRCPT TO: ");
		put_addr(b, env, to);
		put_str(b, "\r\nDATA\r\n");
	}
	put_str(b, "Date: ");
	put_dec(b, (uint32_t) c.d, 2);
	put_str(b, " ");
	put_str(b, months[c.mo - 1]);
	put_str(b, " ");
	put_dec(b, (uint32_t) c.y, 1);
	put_str(b, " ");
	put_dec(b, (uint32_t) c.h, 2);
	put_str(b, ":");
	put_dec(b, (uint32_t) c.mi, 2);
	put_str(b, ":");
	put_dec(b, (uint32_t) c.s, 2);
	put_str(b, " -0000\r\n");

	put_str(b, "From: ");
	o->from = b->n;
	put_addr(b, env, env->wii_number);
	o->from = pack(o->from, b->n - o->from);
	put_str(b, "\r\nTo: ");
	o->to = b->n;
	put_addr(b, env, to);
	o->to = pack(o->to, b->n - o->to);
	put_str(b, "\r\n");

	put_str(b, "Message-Id: <");
	put_hex(b, msg_id, 5);
	put_hex(b, (uint32_t) (env->wii_number >> 32), 8);
	put_hex(b, (uint32_t) env->wii_number, 8);
	put_hex(b, minutes, 8);
	put_str(b, env->domain);
	put_str(b, ">\r\n");

	put_str(b, "X-Wii-AppId: 2-");
	put_hex(b, env->app_id, 8);
	put_str(b, "-");
	put_hex(b, env->group_id, 4);
	put_str(b, "\r\nX-Wii-Cmd: ");
	put_hex(b, LED_DEFAULT, 8);
	put_str(b, "\r\n");

	/* The alt name with its terminating NUL, then the face. */
	tl = (unsigned) p->title_len < MBOARD_TITLE_MAX ? (unsigned) p->title_len : MBOARD_TITLE_MAX;
	for (i = 0; i < (int) tl; ++i)
		put16(alt + 2 * i, p->title[i]);
	put16(alt + 2 * tl, 0);
	put_b64_field(b, "X-Wii-AltName:", alt, (uint32_t) (2 * (tl + 1)));
	if (p->face)
		put_b64_field(b, "X-WiiFace:", p->face, MBOARD_FACE_SIZE);
	if (!p->reply)
		put_str(b, "X-Wii-MB-NoReply: -\r\n");
	if (p->reg_date) {
		put_str(b, "X-Wii-MB-RegDate: ");
		put_hex(b, p->reg_date, 4);
		put_str(b, "\r\n");
	}
	if (p->delay_hours) {
		put_str(b, "X-Wii-MB-Delay: ");
		put_hex(b, (uint32_t) p->delay_hours, 2);
		put_str(b, "\r\n");
	}

	put_str(b, "MIME-Version: 1.0\r\n");
	if (p->n_att) {
		put_str(b, "Content-Type: multipart/mixed;\r\n boundary=\"Boundary-NWC24-");
		put_hex(b, minutes, 8);
		put_hex(b, msg_id, 5);
		put_str(b, "\"\r\n");
		o->start = b->n;
		put_boundary(b, minutes, msg_id, 0);
	}
	put_str(b, "Content-Type: text/plain; charset=");
	o->ctype = pack(b->n, 9);  /* "utf-16be" and its NUL */
	/* To another Wii the text goes base64, as the Wii Menu's own letters
	 * do: a mail server rewrites line ends, and in raw UTF-16 a line end is
	 * the bytes 00 0A (WiiLink's makes it 00 0D 0A, and every character
	 * after it lands a byte off). The loop-back never meets a server. */
	put_str(b, "utf-16be\r\nContent-Transfer-Encoding: ");
	o->txenc = pack(b->n, p->b64_text ? 7 : 5);  /* "base64" or "8bit", and the NUL */
	put_str(b, p->b64_text ? "base64\r\n\r\n" : "8bit\r\n\r\n");
	if (!p->n_att)
		o->start = b->n;

	o->body = b->n;
	{
		uint8_t raw[2 * MBOARD_BODY_MAX];
		uint32_t n = 0;

		for (i = 0; i < p->body_len && i < MBOARD_BODY_MAX; ++i, n += 2)
			put16(raw + n, p->body[i]);
		if (p->b64_text)
			put_b64_body(b, raw, n);
		else
			put_bytes(b, raw, n);
		o->body_raw = n;
	}
	o->body_len = b->n - o->body;

	for (i = 0; i < p->n_att; ++i) {
		const char *type = p->att_type[i] == T_JPEG ? "image/jpeg" : "application/x-wii-msgboard";

		put_boundary(b, minutes, msg_id, 0);
		put_str(b, "Content-Type: ");
		put_str(b, type);
		put_str(b, ";\r\n name=");
		put_part_name(b, i, msg_id, p->att_type[i]);
		put_str(b, "\r\nContent-Transfer-Encoding: base64\r\n"
				   "Content-Disposition: attachment;\r\n filename=");
		put_part_name(b, i, msg_id, p->att_type[i]);
		put_str(b, "\r\n\r\n");
		o->att[i] = b->n;
		put_b64_body(b, p->att[i], p->att_size[i]);
		o->att_len[i] = b->n - o->att[i];
		if (i == p->n_att - 1)
			put_boundary(b, minutes, msg_id, 1);
	}
	return b->n;
}

/* The receive box's message file, written as the VF library does: one
 * long-name entry ("r0000001.msg") and its 8.3 entry, in /mb. */
static int box_file(vff *v, char letter, uint32_t msg_id, const uint8_t *data, uint32_t len,
					stamp st) {
	char lfn[13], n11[11];
	uint32_t where[2], off;
	uint16_t mb;
	int i;

	mb = subdir(v, 0, "MB         ", st, "mb");
	if (!mb)
		return v->err ? v->err : MBOARD_E_FULL;
	lfn[0] = letter;
	for (i = 7; i >= 1; --i, msg_id /= 10)
		lfn[i] = (char) ('0' + msg_id % 10);
	memcpy(lfn + 8, ".msg", 5);
	memset(n11, ' ', 11);
	n11[0] = (char) (letter - 'a' + 'A');
	memcpy(n11 + 1, lfn + 1, 7);
	memcpy(n11 + 8, "MSG", 3);

	/* A file left by a post that failed after writing it (its control
	 * entry never written) is reused. */
	off = find(v, mb, n11);
	if (v->err)
		return v->err;
	if (!off) {
		if (free_slots(v, mb, 2, where))
			return v->err;
		/* Data, then the FAT, then the long name and the entry: a failure
		 * part way leaves at most clusters nothing points to. */
		{
			uint16_t c, prev = 0, first = 0;
			uint32_t n, need = (len + CL - 1) / CL;
			uint8_t block[CL];

			for (n = 0; n < need; ++n) {
				uint32_t part = len - n * CL < CL ? len - n * CL : CL;
				c = alloc(v, prev, 0);
				if (!c)
					return v->err;
				if (!first)
					first = c;
				memset(block, 0, CL);
				memcpy(block, data + n * CL, part);
				wr(v, at(v, c), block, CL);
				prev = c;
			}
			if (vff_flush(v))
				return v->err;
			put_lfn(v, where[0], n11, lfn);
			put_entry(v, where[1], n11, 0x20, first, len, st);
		}
		return v->err;
	}
	return write_file(v, off, data, len, st, 1);
}

static int leap(int y) {
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* The SDK's checks (NWC24SetMsg*), and the text in UTF-16. */
static int check_msg(const mboard_msg *m, parts *p, uint16_t *title, uint16_t *body) {
	static const uint8_t days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	uint32_t total = 0;
	int i;

	memset(p, 0, sizeof(*p));
	if (!m || !m->title || !m->body)
		return MBOARD_E_ARG;
	p->title = title;
	p->body = body;
	p->title_len = mboard_utf16(title, MBOARD_TITLE_MAX, m->title);
	p->body_len = mboard_utf16(body, MBOARD_BODY_MAX, m->body);
	if (!p->title_len || !p->body_len)
		return MBOARD_E_ARG;
	/* A Mii with no creation ID is no Mii: the Wii Menu would drop it. */
	if (m->face) {
		for (i = 0x18; i < 0x20 && !m->face[i]; ++i)
			;
		if (i == 0x20)
			return MBOARD_E_ARG;
		p->face = m->face;
	}
	p->reply = m->reply != 0;
	if (m->delay_hours < 0 || m->delay_hours > MBOARD_DELAY_MAX)
		return MBOARD_E_ARG;
	p->delay_hours = m->delay_hours;
	if (m->file_year) {
		if (m->file_year < 2000 || m->file_year > 2035 || m->file_month < 1 ||
				m->file_month > 12 || m->file_day < 1 ||
				m->file_day > days[m->file_month - 1] + (m->file_month == 2 && leap(m->file_year)))
			return MBOARD_E_ARG;
		p->reg_date = (uint16_t) ((m->file_year - 2000) << 9 | m->file_month << 5 | m->file_day);
	}
	if (m->jpeg) {
		const uint8_t *j = m->jpeg;
		if (m->jpeg_size < 4 || j[0] != 0xff || j[1] != 0xd8 || j[2] != 0xff)
			return MBOARD_E_ARG;
		p->att[p->n_att] = j;
		p->att_size[p->n_att] = m->jpeg_size;
		p->att_type[p->n_att++] = T_JPEG;
	}
	if (m->board) {
		const uint8_t *a = m->board;
		if (m->board_size < 0x20 || get32(a) != 0x55aa382du)   /* a U8 archive */
			return MBOARD_E_ARG;
		if (m->board_size > MBOARD_BOARD_MAX)
			return MBOARD_E_TOO_BIG;
		p->att[p->n_att] = a;
		p->att_size[p->n_att] = m->board_size;
		p->att_type[p->n_att++] = T_MSGBOARD;
	}
	for (i = 0; i < p->n_att; ++i)
		total += p->att_size[i];
	if (total >= MBOARD_ATTACH_TOTAL)
		return MBOARD_E_TOO_BIG;
	return MBOARD_OK;
}

/* One message into a mailbox: to this Wii's own number, the receive box (as
 * the SDK's loop-back); to another's, the send box. */
static int box_commit(uint8_t *ctl, const mboard_io *mbx, const mboard_env *env, uint64_t to,
					  const mboard_msg *m, const box_kind *k) {
	static uint16_t title[MBOARD_TITLE_MAX], body[MBOARD_BODY_MAX];
	uint32_t num, cap, next_id, free_at, next_free, free_space, msg_id, minutes, len, off, oldest;
	uint32_t room, flags;
	uint8_t *e, *msg;
	offsets o;
	parts p;
	buf b;
	vff v;
	int res, i;

	if ((res = check_msg(m, &p, title, body)) < 0)
		return res;
	if (!env->wii_number || env->wii_number > 9999999999999999ull || !env->domain ||
			env->domain[0] != '@' || strlen(env->domain) > 63)
		return MBOARD_E_NO_ID;
	if (!to || to > 9999999999999999ull)
		return MBOARD_E_ARG;
	p.b64_text = to != env->wii_number;

	/* The control file, as GetCachedMBCHeader() and AddMBCEntry() check it. */
	num = get32(ctl + 0x08);
	cap = get32(ctl + 0x0c);
	next_id = get32(ctl + 0x18);
	free_at = get32(ctl + 0x1c);
	free_space = get32(ctl + 0x24);
	if (get32(ctl) != BOX_MAGIC || get32(ctl + 4) != 4 || cap != k->capacity ||
			get32(ctl + 0x14) != k->ctl_size)
		return MBOARD_E_FORMAT;
	if (num >= cap || !free_at)
		return MBOARD_E_FULL;
	if (free_at < BOX_ENTRY || free_at >= k->ctl_size || (free_at - BOX_ENTRY) % BOX_ENTRY)
		return MBOARD_E_FORMAT;
	e = ctl + free_at;
	next_free = get32(e + 0x0c);
	if (get32(e) != 0 || (next_free && (next_free < BOX_ENTRY || next_free >= k->ctl_size ||
										(next_free - BOX_ENTRY) % BOX_ENTRY)))
		return MBOARD_E_FORMAT;
	msg_id = next_id >= 1 && next_id <= MSG_ID_MAX ? next_id : 1;
	for (off = BOX_ENTRY; off < k->ctl_size; off += BOX_ENTRY)
		if (get32(ctl + off) == msg_id)
			return MBOARD_E_FORMAT;  /* a message already has the next id */

	/* CheckMsgBoxSpace(): the encoded attachments and the text, under the
	 * box's largest message. */
	room = (uint32_t) (2 * p.body_len);
	if (p.b64_text)
		room = (room * 4 + 2) / 3 + room / B64_STEP * 2 + 4;
	for (i = 0; i < p.n_att; ++i)
		room += (p.att_size[i] * 4 + 2) / 3 + p.att_size[i] / B64_STEP * 2 + 4;
	if (room >= MSG_LIMIT)
		return MBOARD_E_TOO_BIG;

	minutes = (uint32_t) ((env->utc + 2208988800ll) / 60);
	b.max = room + 2048 + (uint32_t) p.n_att * 512;   /* + the headers */
	msg = malloc(b.max + 32);                          /* + the padding */
	if (!msg)
		return MBOARD_E_MEM;
	b.p = msg;
	b.n = 0;
	b.over = 0;
	memset(&o, 0, sizeof(o));
	len = compose(&b, &o, env, to, msg_id, minutes, &p);
	if (b.over) {
		free(msg);
		return MBOARD_E_TOO_BIG;
	}
	/* NWC24iMBoxCheck(): room for the message, the work area and 16 KiB.
	 * The SDK deletes the oldest messages to make room; this never does. */
	if (len >= MSG_LIMIT || free_space <= len + STRING_WORK + 0x4000) {
		free(msg);
		return len >= MSG_LIMIT ? MBOARD_E_TOO_BIG : MBOARD_E_FULL;
	}

	/* The file is zero-padded to 32 bytes, as the SDK's buffered VF writes
	 * leave it: the readers' streams read whole 32-byte blocks, and fail
	 * with NWC24_ERR_FILE_READ on a file that ends inside one. The control
	 * entry keeps the exact length. */
	memset(msg + len, 0, ((len + 31) & ~31u) - len);
	res = vff_open(&v, mbx, k->mbx_size);
	/* The VF library leaves the volume's last cluster unused: the free
	 * space it records is one cluster short of what the FAT shows free. */
	v.nclusters -= 1;
	if (!res)
		res = box_file(&v, k->letter, msg_id, msg, (len + 31) & ~31u, fat_stamp(env->local));
	if (!res && v.err)
		res = v.err;
	if (!res) {
		free_space = free_clusters(&v) * CL;

		/* CopyMsgObjToMBCFmt(), then AddMBCEntry()'s header update. */
		flags = F_FOR_RECIPIENT | F_FOR_MENU | k->flag | F_INITIALIZED | F_DELIVERING | F_21;
		if (p.n_att)
			flags |= F_MULTIPART;
		if (p.delay_hours)
			flags |= F_DESIGNATED;
		memset(e, 0, BOX_ENTRY);
		put32(e + 0x00, msg_id);
		put32(e + 0x04, flags);
		put32(e + 0x08, len);
		put32(e + 0x0c, env->app_id);
		put32(e + 0x10, o.start);         /* the first boundary, or the text */
		put32(e + 0x18, LED_DEFAULT);
		put64(e + 0x20, env->wii_number);  /* from */
		put32(e + 0x28, minutes);
		/* CorrectHeadersForMBDelay(): the time it may be shown, in minutes. */
		if (p.delay_hours)
			put32(e + 0x2c, minutes + (uint32_t) p.delay_hours * 60);
		e[0x30] = 1;                        /* one recipient */
		e[0x31] = (uint8_t) p.n_att;
		put16(e + 0x32, env->group_id);
		put32(e + 0x34, o.from);
		put32(e + 0x38, o.to);
		put32(e + 0x40, o.ctype);
		put32(e + 0x44, o.txenc);
		put32(e + 0x48, o.body);
		put32(e + 0x4c, o.body_len);
		for (i = 0; i < ATTACH_MAX; ++i) {
			put32(e + 0x50 + 8 * i, i < p.n_att ? o.att[i] : 0);
			put32(e + 0x54 + 8 * i, i < p.n_att ? o.att_len[i] : 0);
			put32(e + 0x60 + 4 * i, i < p.n_att ? p.att_size[i] : 0);
			put32(e + 0x68 + 4 * i, i < p.n_att ? p.att_type[i] : TXT_PLAIN);
		}
		put32(e + 0x70, o.body_raw);        /* the text's size before encoding */
		put32(e + 0x78, ICON_NEW_NONE);

		/* DuplicationCheck() recomputes the oldest id. */
		oldest = 0;
		for (off = BOX_ENTRY; off < k->ctl_size; off += BOX_ENTRY) {
			uint32_t id = get32(ctl + off);
			if (id && (!oldest || compare_id(oldest, id) > 0))
				oldest = id;
		}
		put32(ctl + 0x08, num + 1);
		put32(ctl + 0x10, get32(ctl + 0x10) + len);
		put32(ctl + 0x18, msg_id + 1);
		put32(ctl + 0x1c, next_free);
		put32(ctl + 0x20, oldest);
		put32(ctl + 0x24, free_space);
	}
	vff_close(&v);
	free(msg);
	return res;
}

int mboard_post_box(uint8_t *ctl, const mboard_io *mbx, const mboard_env *env,
					const mboard_msg *msg) {
	return box_commit(ctl, mbx, env, env->wii_number, msg, &RECV_BOX);
}

int mboard_send_box(uint8_t *ctl, const mboard_io *mbx, const mboard_env *env, uint64_t to,
					const mboard_msg *msg) {
	/* To this Wii itself, the SDK would loop it back: use mboard_post_box. */
	if (to == env->wii_number)
		return MBOARD_E_ARG;
	return box_commit(ctl, mbx, env, to, msg, &SEND_BOX);
}

/* ---- The address book ------------------------------------------------------ */

#define FL_HEAD 0x360u
#define FL_ENTRY 0x140u
#define FL_MAGIC 0x5763466cu  /* 'WcFl' */

static int fl_ok(const uint8_t *fl) {
	return get32(fl) == FL_MAGIC && get32(fl + 4) == 2 && get32(fl + 8) == MBOARD_FRIENDS;
}

static int fl_bit(const uint8_t *fl, int slot) {
	return get32(fl + 0x10 + 4 * (slot >> 5)) >> (slot & 31) & 1;
}

static void fl_set_bit(uint8_t *fl, int slot, int on) {
	uint32_t w = get32(fl + 0x10 + 4 * (slot >> 5));

	w = (w & ~(1u << (slot & 31))) | (uint32_t) on << (slot & 31);
	put32(fl + 0x10 + 4 * (slot >> 5), w);
}

int mboard_book_parse(const uint8_t *fl, mboard_friend *out, int max) {
	int slot, n = 0, i;

	if (!fl_ok(fl))
		return MBOARD_E_FORMAT;
	for (slot = 0; slot < MBOARD_FRIENDS && n < max; ++slot) {
		const uint8_t *e = fl + FL_HEAD + FL_ENTRY * slot;
		mboard_friend *f = &out[n];

		if (!get64(fl + 0x40 + 8 * slot))
			continue;
		memset(f, 0, sizeof(*f));
		f->slot = slot;
		f->type = (int) get32(e);
		/* KD's bit says confirmed; the entry's own status is not updated. */
		f->confirmed = fl_bit(fl, slot);
		for (i = 0; i < MBOARD_NICK_MAX; ++i)
			f->nick[i] = (uint16_t) (e[8 + 2 * i] << 8 | e[9 + 2 * i]);
		memcpy(f->mii_id, e + 0x20, 8);
		if (f->type == 1)
			f->wii_number = get64(e + 0x40);
		else
			memcpy(f->email, e + 0x40, sizeof(f->email) - 1);
		++n;
	}
	return n;
}

static int nick_ok(const uint16_t *nick, int len) {
	return nick && len >= 1 && len <= MBOARD_NICK_MAX;
}

static void put_nick(uint8_t *e, const uint16_t *nick, int len) {
	int i;

	memset(e + 8, 0, 24);
	for (i = 0; i < len; ++i)
		put16(e + 8 + 2 * i, nick[i]);
}

int mboard_book_add(uint8_t *fl, uint64_t number, const uint16_t *nick, int nick_len,
					   const uint8_t *mii_id) {
	int slot;
	uint8_t *e;

	if (!fl_ok(fl))
		return MBOARD_E_FORMAT;
	if (!number || number > 9999999999999999ull || !nick_ok(nick, nick_len))
		return MBOARD_E_ARG;
	for (slot = 0; slot < MBOARD_FRIENDS; ++slot)
		if (get64(fl + 0x40 + 8 * slot) == number)
			return MBOARD_E_ARG;          /* already in the book */
	for (slot = 0; slot < MBOARD_FRIENDS && get64(fl + 0x40 + 8 * slot); ++slot)
		;
	if (slot == MBOARD_FRIENDS)
		return MBOARD_E_FULL;
	/* NWC24WriteFriendInfo(): a Wii, pending, the address; then the header's
	 * slot, count, its bit cleared and the changed flag set. */
	e = fl + FL_HEAD + FL_ENTRY * slot;
	memset(e, 0, FL_ENTRY);
	put32(e, 1);
	put32(e + 4, 1);
	put_nick(e, nick, nick_len);
	if (mii_id)
		memcpy(e + 0x20, mii_id, 8);
	put64(e + 0x40, number);
	put64(fl + 0x40 + 8 * slot, number);
	put32(fl + 0x0c, get32(fl + 0x0c) + 1);
	fl_set_bit(fl, slot, 0);
	put32(fl + 0x3c, get32(fl + 0x3c) | 1);
	return slot;
}

int mboard_book_set(uint8_t *fl, int slot, const uint16_t *nick, int nick_len,
					   const uint8_t *mii_id) {
	uint8_t *e;

	if (!fl_ok(fl))
		return MBOARD_E_FORMAT;
	if (slot < 0 || slot >= MBOARD_FRIENDS || !get64(fl + 0x40 + 8 * slot) ||
			(nick && !nick_ok(nick, nick_len)))
		return MBOARD_E_ARG;
	/* NWC24UpdateFriendInfo() changes only these two. */
	e = fl + FL_HEAD + FL_ENTRY * slot;
	if (nick)
		put_nick(e, nick, nick_len);
	if (mii_id)
		memcpy(e + 0x20, mii_id, 8);
	return slot;
}

int mboard_book_remove(uint8_t *fl, int slot) {
	if (!fl_ok(fl))
		return MBOARD_E_FORMAT;
	if (slot < 0 || slot >= MBOARD_FRIENDS || !get64(fl + 0x40 + 8 * slot))
		return MBOARD_E_ARG;
	/* NWC24DeleteFriendInfo(). */
	memset(fl + FL_HEAD + FL_ENTRY * slot, 0, FL_ENTRY);
	put64(fl + 0x40 + 8 * slot, 0);
	put32(fl + 0x0c, get32(fl + 0x0c) - 1);
	fl_set_bit(fl, slot, 0);
	return MBOARD_OK;
}

/* ---- Text ------------------------------------------------------------------ */

/* UTF-8 to UTF-16, at most max units; a bad sequence becomes U+FFFD. */
int mboard_utf16(uint16_t *out, int max, const char *in) {
	const uint8_t *p = (const uint8_t *) in;
	int n = 0;

	while (*p && n < max) {
		uint32_t c = *p++, need = 0, min = 0;

		if (c >= 0xf0 && c < 0xf5) {
			c &= 7;
			need = 3;
			min = 0x10000;
		} else if (c >= 0xe0) {
			c &= 15;
			need = 2;
			min = 0x800;
		} else if (c >= 0xc2 && c < 0xe0) {
			c &= 31;
			need = 1;
			min = 0x80;
		} else if (c >= 0x80) {
			c = 0xfffd;
		}
		for (; need && (*p & 0xc0) == 0x80; --need)
			c = c << 6 | (*p++ & 63);
		if (need || c < min || c > 0x10ffff || (c >= 0xd800 && c < 0xe000))
			c = 0xfffd;
		if (c >= 0x10000) {
			if (n + 2 > max)
				break;
			c -= 0x10000;
			out[n++] = (uint16_t) (0xd800 | c >> 10);
			out[n++] = (uint16_t) (0xdc00 | (c & 0x3ff));
		} else {
			out[n++] = (uint16_t) c;
		}
	}
	return n;
}

const char *mboard_strerror(int result) {
	switch (result) {
	case MBOARD_OK: return "posted";
	case MBOARD_COMBINED: return "added to the title's line";
	case MBOARD_CREATED: return "today's first line";
	case MBOARD_E_IO: return "NAND error";
	case MBOARD_E_FORMAT: return "unexpected file format, nothing written";
	case MBOARD_E_FULL: return "full";
	case MBOARD_E_MEM: return "out of memory";
	case MBOARD_E_ARG: return "bad text or ID";
	case MBOARD_E_BUSY: return "WiiConnect24 busy, try again";
	case MBOARD_E_NO_ID: return "no Wii number";
	case MBOARD_E_DENIED: return "permission denied";
	case MBOARD_E_TOO_BIG: return "photo, board data or message too big";
	case MBOARD_E_NO_MII: return "no Mii in that slot";
	case MBOARD_QUEUED: return "queued; WiiConnect24 sends it on its next round";
	default: return "unknown error";
	}
}

#ifndef MBOARD_NO_OGC

/* ---- On the Wii ------------------------------------------------------------ */

#include <malloc.h>
#include <stdio.h>

#include <gccore.h>
#include <ogc/conf.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

static char cfg_path[] ATTRIBUTE_ALIGN(32) = "/shared2/wc24/nwc24msg.cfg";
static char cbk_path[] ATTRIBUTE_ALIGN(32) = "/shared2/wc24/nwc24msg.cbk";
static char ctl_path[] ATTRIBUTE_ALIGN(32) = "/shared2/wc24/mbox/wc24recv.ctl";
static char mbx_path[] ATTRIBUTE_ALIGN(32) = "/shared2/wc24/mbox/wc24recv.mbx";
static char send_ctl_path[] ATTRIBUTE_ALIGN(32) = "/shared2/wc24/mbox/wc24send.ctl";
static char send_mbx_path[] ATTRIBUTE_ALIGN(32) = "/shared2/wc24/mbox/wc24send.mbx";
static char kd_path[] ATTRIBUTE_ALIGN(32) = "/dev/net/kd/request";
static char kd_time_path[] ATTRIBUTE_ALIGN(32) = "/dev/net/kd/time";
static char cdb_path[] ATTRIBUTE_ALIGN(32) = "/title/00000001/00000002/data/cdb.vff";
static char wiiid_path[] ATTRIBUTE_ALIGN(32) = "/title/00000001/00000002/data/nocopy/cdbwiiid.dat";

#define KD_TRY_SUSPEND 2
#define KD_RESUME 3
#define KD_SEND_MAIL_NOW 0x0b
#define KD_GET_TIME_DIFF 24

static int nand_error(s32 r) {
	return r == -102 || r == -1 ? MBOARD_E_DENIED : r == -106 ? MBOARD_E_FORMAT : MBOARD_E_IO;
}

#define BOUNCE (8 * 1024)
static u8 bounce[BOUNCE] ATTRIBUTE_ALIGN(32);

static int ios_read(void *ctx, uint32_t off, void *out, uint32_t len) {
	s32 fd = (s32) ctx;
	u8 *p = out;

	while (len) {
		u32 n = len < BOUNCE ? len : BOUNCE;
		if (IOS_Seek(fd, off, SEEK_SET) != (s32) off || IOS_Read(fd, bounce, n) != (s32) n)
			return -1;
		memcpy(p, bounce, n);
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int ios_write(void *ctx, uint32_t off, const void *in, uint32_t len) {
	s32 fd = (s32) ctx;
	const u8 *p = in;

	while (len) {
		u32 n = len < BOUNCE ? len : BOUNCE;
		memcpy(bounce, p, n);
		if (IOS_Seek(fd, off, SEEK_SET) != (s32) off || IOS_Write(fd, bounce, n) != (s32) n)
			return -1;
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

/* nwc24msg.cfg (or its backup), as the SDK's CheckConfig() accepts it. */
static int read_config(u64 *id, char *domain) {
	static u8 cfg[0x400] ATTRIBUTE_ALIGN(32);
	char *paths[2] = { cfg_path, cbk_path };
	int i, k;

	for (i = 0; i < 2; ++i) {
		s32 fd = IOS_Open(paths[i], IPC_OPEN_READ), n;
		u32 sum = 0;

		if (fd < 0)
			continue;
		n = IOS_Read(fd, cfg, sizeof(cfg));
		IOS_Close(fd);
		if (n != (s32) sizeof(cfg))
			continue;
		for (k = 0; k < 0x3fc; k += 4)
			sum += get32(cfg + k);
		if (get32(cfg) != 0x57634366 || get32(cfg + 4) != 8 || get32(cfg + 0x3fc) != sum ||
				get32(cfg + 0x10) >= 32)
			continue;
		*id = get64(cfg + 8);
		memcpy(domain, cfg + 0x18, 63);
		domain[63] = 0;
		return *id && domain[0] == '@' ? 0 : MBOARD_E_NO_ID;
	}
	return MBOARD_E_NO_ID;
}

static s32 kd_call(s32 fd, s32 cmd) {
	static u8 in[32] ATTRIBUTE_ALIGN(32);
	static u8 out[32] ATTRIBUTE_ALIGN(32);
	s32 r;

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));
	r = cmd == KD_TRY_SUSPEND ? IOS_Ioctl(fd, cmd, in, sizeof(in), out, sizeof(out))
							  : IOS_Ioctl(fd, cmd, NULL, 0, out, sizeof(out));
	return r < 0 ? r : (s32) get32(out);
}

/* UTC as the SDK has it (KD's difference plus the RTC). When KD has none, or
 * one further from the local clock than any time zone is (UTC-12 to UTC+14;
 * the bench Wii's was a day behind), the local clock. */
static s64 utc_now(u64 local) {
	static u8 out[32] ATTRIBUTE_ALIGN(32);
	s64 local_secs = (s64) (local / MBOARD_TICKS_PER_SEC), utc = local_secs + 946684800;
	u32 bias = 0;
	s32 fd = IOS_Open(kd_time_path, 0);

	if (fd >= 0) {
		memset(out, 0, sizeof(out));
		if (IOS_Ioctl(fd, KD_GET_TIME_DIFF, NULL, 0, out, sizeof(out)) >= 0 && !get32(out) &&
				CONF_GetCounterBias(&bias) >= 0) {
			s64 kd = (s64) get64(out + 4) + (local_secs - (s64) bias);
			if (kd - utc <= 12 * 3600 && utc - kd <= 14 * 3600)
				utc = kd;
		}
		IOS_Close(fd);
	}
	return utc;
}

static void app_identity(u32 *app, u16 *group) {
	static char path[64] ATTRIBUTE_ALIGN(32);
	u64 tid = 0;
	u32 owner;
	u8 attr, own, grp, oth;

	*app = 0x48424c57;  /* 'HBLW' when ES cannot say */
	*group = 0x3031;
	if (ES_GetTitleID(&tid) < 0)
		return;
	*app = (u32) tid;
	snprintf(path, sizeof(path), "/title/%08x/%08x/data", (u32) (tid >> 32), (u32) tid);
	ISFS_GetAttr(path, &owner, group, &attr, &own, &grp, &oth);
}

static int commit(uint64_t to, const mboard_msg *msg);

int mboard_post(const char *title_utf8, const char *body_utf8) {
	mboard_msg m;

	memset(&m, 0, sizeof(m));
	m.title = title_utf8;
	m.body = body_utf8;
	return mboard_post_msg(&m);
}

int mboard_post_msg(const mboard_msg *msg) {
	return commit(0, msg);
}

int mboard_send_msg(uint64_t to, const mboard_msg *msg) {
	return to ? commit(to, msg) : MBOARD_E_ARG;
}

/* to: 0 for the receive box (this Wii's board), else the send box. */
static int commit(uint64_t to, const mboard_msg *msg) {
	static char domain[64];
	const box_kind *k = to ? &SEND_BOX : &RECV_BOX;
	char *ctl_file = to ? send_ctl_path : ctl_path, *mbx_file = to ? send_mbx_path : mbx_path;
	u8 *ctl = NULL;
	mboard_env env;
	mboard_io io;
	s32 kd, fd, r;
	int res;

	ISFS_Initialize();
	memset(&env, 0, sizeof(env));
	if ((res = read_config(&env.wii_number, domain)) < 0)
		return res;
	env.domain = domain;
	app_identity(&env.app_id, &env.group_id);
	env.local = gettime();
	env.utc = utc_now(env.local);

	/* NWC24OpenLib(): KD must let go of the mailboxes first. */
	kd = IOS_Open(kd_path, 0);
	if (kd < 0)
		return MBOARD_E_BUSY;
	if (kd_call(kd, KD_TRY_SUSPEND) < 0) {
		IOS_Close(kd);
		return MBOARD_E_BUSY;
	}

	ctl = memalign(32, k->ctl_size);
	fd = IOS_Open(ctl_file, IPC_OPEN_RW);
	if (!ctl) {
		res = MBOARD_E_MEM;
	} else if (fd < 0) {
		res = nand_error(fd);
	} else if (IOS_Read(fd, ctl, k->ctl_size) != (s32) k->ctl_size) {
		res = MBOARD_E_FORMAT;
	} else {
		s32 box = IOS_Open(mbx_file, IPC_OPEN_RW);
		if (box < 0) {
			res = nand_error(box);
		} else {
			io.ctx = (void *) box;
			io.read = ios_read;
			io.write = ios_write;
			res = to ? mboard_send_box(ctl, &io, &env, to, msg) : mboard_post_box(ctl, &io, &env, msg);
			IOS_Close(box);
		}
		/* The control entry last: it is what makes the message exist. */
		if (res == MBOARD_OK && (IOS_Seek(fd, 0, SEEK_SET) != 0 ||
				IOS_Write(fd, ctl, k->ctl_size) != (s32) k->ctl_size))
			res = MBOARD_E_IO;
	}
	if (fd >= 0)
		IOS_Close(fd);
	free(ctl);
	r = kd_call(kd, KD_RESUME);
	(void) r;
	/* KD uploads the send box on its own rounds; ask for one now. If it
	 * will not, the message waits for the next (MBOARD_QUEUED). */
	if (to && res == MBOARD_OK && kd_call(kd, KD_SEND_MAIL_NOW) < 0)
		res = MBOARD_QUEUED;
	IOS_Close(kd);
	return res;
}

/* The Mii database: "RNOD", then 100 Miis of 74 bytes (RFLCharData). Games
 * read it through RFL; any title may. */
static char rfl_path[] ATTRIBUTE_ALIGN(32) = "/shared2/menu/FaceLib/RFL_DB.dat";

int mboard_mii(int slot, uint8_t face[MBOARD_FACE_SIZE]) {
	static u8 head[4] ATTRIBUTE_ALIGN(32), mii[MBOARD_FACE_SIZE + 6] ATTRIBUTE_ALIGN(32);
	s32 fd, got;
	int i;

	if (slot < 0 || slot >= 100 || !face)
		return MBOARD_E_ARG;
	ISFS_Initialize();
	fd = IOS_Open(rfl_path, IPC_OPEN_READ);
	if (fd < 0)
		return nand_error(fd);
	got = IOS_Read(fd, head, 4) == 4 && !memcmp(head, "RNOD", 4) &&
		  IOS_Seek(fd, 4 + slot * MBOARD_FACE_SIZE, SEEK_SET) == 4 + slot * MBOARD_FACE_SIZE &&
		  IOS_Read(fd, mii, MBOARD_FACE_SIZE) == MBOARD_FACE_SIZE;
	IOS_Close(fd);
	if (!got)
		return MBOARD_E_FORMAT;
	/* An empty slot has no creation ID. */
	for (i = 0x18; i < 0x20 && !mii[i]; ++i)
		;
	if (i == 0x20)
		return MBOARD_E_NO_MII;
	memcpy(face, mii, MBOARD_FACE_SIZE);
	return MBOARD_OK;
}

/* ---- IOS's NAND permission check -------------------------------------------
 * In IOS's FS module: `cmp r3, r1; beq allowed`. The patch makes the branch
 * unconditional (beq -> b) for the moment cdb.vff is written, then puts it
 * back: the Homebrew Channel's play log does the same (playtime.c). */

static const u8 perm_check[] = { 0x42, 0x8B, 0xD0, 0x01, 0x25, 0x66 };

static int ahbprot(void) {
	return read32(0x0d800064) == 0xffffffff;
}

static int fs_permissions(int lifted) {
	u8 *p = (u8 *) *(u32 *) 0x80003134, *end = (u8 *) 0x94000000;
	u8 from = lifted ? 0xD0 : 0xE0, to = lifted ? 0xE0 : 0xD0;
	int n = 0;

	if (!ahbprot() || (u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return 0;
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

int mboard_log_play(const char *name_utf8, const char *id, uint32_t seconds) {
	static u8 wiiid[32] ATTRIBUTE_ALIGN(32);
	mboard_session s;
	mboard_io io;
	size_t i, n;
	int patched, have_id = 0, res;
	u64 now;
	s32 fd;

	if (!name_utf8 || !id || !(n = strlen(id)) || n > 6)
		return MBOARD_E_ARG;
	memset(&s, 0, sizeof(s));
	for (i = 0; i < n; ++i) {
		char c = id[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
			return MBOARD_E_ARG;
		s.id[i] = c;
	}
	if (!mboard_utf16(s.name, 40, name_utf8))
		return MBOARD_E_ARG;
	if (!ahbprot())
		return MBOARD_E_DENIED;
	now = gettime();
	s.end = now;
	s.start = now - (u64) seconds * MBOARD_TICKS_PER_SEC;

	patched = fs_permissions(1);
	fd = IOS_Open(wiiid_path, IPC_OPEN_READ);
	if (fd >= 0) {
		have_id = IOS_Read(fd, wiiid, sizeof(wiiid)) >= 8;
		IOS_Close(fd);
	}
	fd = IOS_Open(cdb_path, IPC_OPEN_RW);
	if (fd < 0) {
		res = nand_error(fd);
	} else {
		io.ctx = (void *) fd;
		io.read = ios_read;
		io.write = ios_write;
		res = mboard_play_vff(&io, &s, now, have_id ? wiiid : NULL);
		IOS_Close(fd);
	}
	if (patched)
		fs_permissions(0);
	return res;
}

/* ---- The address book, on the Wii ---------------------------------------- */

static char fl_path[] ATTRIBUTE_ALIGN(32) = "/shared2/wc24/nwc24fl.bin";

/* The whole file, changed by `change` (NULL: only read), with KD paused, as
 * NWC24OpenLib() would have it: KD reads and writes this file itself. */
static int fl_do(int (*change)(uint8_t *fl, void *arg), void *arg, mboard_friend *out, int max) {
	u8 *fl = memalign(32, MBOARD_FRIENDS_SIZE);
	s32 kd, fd;
	int res;

	if (!fl)
		return MBOARD_E_MEM;
	ISFS_Initialize();
	kd = IOS_Open(kd_path, 0);
	if (kd < 0 || kd_call(kd, KD_TRY_SUSPEND) < 0) {
		if (kd >= 0)
			IOS_Close(kd);
		free(fl);
		return MBOARD_E_BUSY;
	}
	fd = IOS_Open(fl_path, change ? IPC_OPEN_RW : IPC_OPEN_READ);
	if (fd < 0) {
		res = nand_error(fd);
	} else if (IOS_Read(fd, fl, MBOARD_FRIENDS_SIZE) != (s32) MBOARD_FRIENDS_SIZE) {
		res = MBOARD_E_FORMAT;
	} else {
		res = change ? change(fl, arg) : mboard_book_parse(fl, out, max);
		if (change && res >= 0 && (IOS_Seek(fd, 0, SEEK_SET) != 0 ||
				IOS_Write(fd, fl, MBOARD_FRIENDS_SIZE) != (s32) MBOARD_FRIENDS_SIZE))
			res = MBOARD_E_IO;
	}
	if (fd >= 0)
		IOS_Close(fd);
	kd_call(kd, KD_RESUME);
	IOS_Close(kd);
	free(fl);
	return res;
}

typedef struct {
	int op, slot;
	uint64_t number;
	uint16_t nick[MBOARD_NICK_MAX];
	int nick_len;            /* -1: keep it */
	const uint8_t *mii;
} fl_change;

static int fl_apply(uint8_t *fl, void *arg) {
	fl_change *c = arg;
	const uint8_t *id = c->mii ? c->mii + 0x18 : NULL;   /* its creation ID */

	if (c->op == 0)
		return mboard_book_add(fl, c->number, c->nick, c->nick_len, id);
	if (c->op == 1)
		return mboard_book_set(fl, c->slot, c->nick_len < 0 ? NULL : c->nick, c->nick_len, id);
	return mboard_book_remove(fl, c->slot);
}

int mboard_friends(mboard_friend *out, int max) {
	return fl_do(NULL, NULL, out, max);
}

static int nick_utf16(fl_change *c, const char *nick) {
	uint16_t big[MBOARD_NICK_MAX + 1];

	c->nick_len = -1;
	if (!nick)
		return 0;
	/* More than 11 units is refused, not cut. */
	if (mboard_utf16(big, MBOARD_NICK_MAX + 1, nick) > MBOARD_NICK_MAX)
		return MBOARD_E_ARG;
	c->nick_len = mboard_utf16(c->nick, MBOARD_NICK_MAX, nick);
	return c->nick_len ? 0 : MBOARD_E_ARG;
}

int mboard_friend_add(uint64_t number, const char *nick, const uint8_t *mii) {
	fl_change c;

	memset(&c, 0, sizeof(c));
	c.op = 0;
	c.number = number;
	c.mii = mii;
	if (!nick || nick_utf16(&c, nick) < 0)
		return MBOARD_E_ARG;
	return fl_do(fl_apply, &c, NULL, 0);
}

int mboard_friend_set(int slot, const char *nick, const uint8_t *mii) {
	fl_change c;

	memset(&c, 0, sizeof(c));
	c.op = 1;
	c.slot = slot;
	c.mii = mii;
	if (nick_utf16(&c, nick) < 0)
		return MBOARD_E_ARG;
	return fl_do(fl_apply, &c, NULL, 0);
}

int mboard_friend_remove(int slot) {
	fl_change c;

	memset(&c, 0, sizeof(c));
	c.op = 2;
	c.slot = slot;
	return fl_do(fl_apply, &c, NULL, 0);
}

#endif
