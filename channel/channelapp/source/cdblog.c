// The Message Board's play log writer; see cdblog.h and docs/messageboard.md.
// It follows tools/msgboard/cdb_log.py step for step (tests/cdblog_host
// compares the two), down to the order clusters are taken in.

#include <stdlib.h>
#include <string.h>

#include "cdblog.h"

#define FAT1 0x20u
#define FAT2 0x14020u
#define FAT_LEN 0x14000u
#define ROOT 0x28020u
#define ROOT_LEN 0x1000u
#define DATA 0x29020u
#define CL 0x200u
#define VFF_SIZE 0x01400000u

#define TEXT_AT 0x548u
#define LIST_SIZE 0x668u
#define TAIL 0x18u
#define ENTRY 0x88u
#define MAX_TITLES 12
#define MSG_MAX 8192u
#define DIR_MAX 128  // entries a directory may have: 8 clusters (a month has 33)

typedef struct {
	const cdblog_io *io;
	uint8_t *fat;
	uint8_t dirty[FAT_LEN / 512];
	uint32_t nclusters;
	int err;
} vff;

typedef struct {
	uint16_t t, d;
} stamp;

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

// zlib's CRC-32, bitwise: it runs over 0x140 bytes once per write.
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

static int rd(vff *v, uint32_t off, void *buf, uint32_t len) {
	if (!v->err && v->io->read(v->io->ctx, off, buf, len))
		v->err = CDBLOG_E_IO;
	return v->err;
}

static int wr(vff *v, uint32_t off, const void *buf, uint32_t len) {
	if (!v->err && v->io->write(v->io->ctx, off, buf, len))
		v->err = CDBLOG_E_IO;
	return v->err;
}

static uint32_t at(uint16_t c) {
	return DATA + (uint32_t) (c - 2) * CL;
}

static uint16_t fat_get(vff *v, uint16_t c) {
	return le16(v->fat + 2 * c);
}

static void fat_set(vff *v, uint16_t c, uint16_t val) {
	put_le16(v->fat + 2 * c, val);
	v->dirty[2 * c / 512] = 1;
}

static int is_cluster(vff *v, uint16_t c) {
	return c >= 2 && c < 0xfff8 && c < v->nclusters;
}

// A free cluster, linked after `after` (0: a new chain). Directory clusters
// are zeroed; file clusters are written whole by the caller.
static uint16_t alloc(vff *v, uint16_t after, int zero) {
	static const uint8_t zeros[CL];
	uint16_t c;

	for (c = 2; c < v->nclusters; ++c)
		if (!fat_get(v, c))
			break;
	if (c >= v->nclusters) {
		v->err = CDBLOG_E_FULL;
		return 0;
	}
	fat_set(v, c, 0xffff);
	if (after)
		fat_set(v, after, c);
	if (zero)
		wr(v, at(c), zeros, CL);
	return c;
}

// The byte offsets of a directory's entries (the root is a fixed area).
static int slots(vff *v, uint16_t start, uint32_t *offs) {
	int n = 0, i, guard = 0;

	if (!start) {
		for (i = 0; i < (int) (ROOT_LEN / 32); ++i)
			offs[n++] = ROOT + 32 * i;
		return n;
	}
	for (; is_cluster(v, start) && guard < DIR_MAX / 16; start = fat_get(v, start), ++guard)
		for (i = 0; i < (int) (CL / 32); ++i)
			offs[n++] = at(start) + 32 * i;
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

// n consecutive free entries, growing the directory when it has none.
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
		v->err = CDBLOG_E_FULL;
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

static uint16_t mkdir_in(vff *v, uint16_t parent, const char *name11, stamp s, const char *lfn) {
	uint32_t where[2];
	uint8_t e[64];
	uint16_t c = alloc(v, 0, 1);
	int i;

	if (!c || free_slots(v, parent, lfn ? 2 : 1, where))
		return 0;
	if (lfn) {
		uint8_t sum = 0, units[26];
		size_t k, len = strlen(lfn);

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
		wr(v, where[0], e, 32);
	}
	put_entry(v, where[lfn ? 1 : 0], name11, 0x10, c, 0, s);
	put_entry(v, at(c), ".          ", 0x10, c, 0, s);
	put_entry(v, at(c) + 32, "..         ", 0x10, parent, 0, s);
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

// A file's contents, up to MSG_MAX bytes.
static int read_file(vff *v, uint32_t off, uint8_t *buf, uint32_t *size) {
	uint8_t e[32];
	uint32_t got = 0, len;
	uint16_t c;

	if (rd(v, off, e, 32))
		return v->err;
	len = e[28] | e[29] << 8 | e[30] << 16 | (uint32_t) e[31] << 24;
	if (len > MSG_MAX)
		return v->err = CDBLOG_E_FORMAT;
	for (c = le16(e + 26); got < len && is_cluster(v, c); c = fat_get(v, c), got += CL)
		if (rd(v, at(c), buf + got, len - got < CL ? len - got : CL))
			return v->err;
	if (got < len)
		return v->err = CDBLOG_E_FORMAT;
	*size = len;
	return 0;
}

static int write_file(vff *v, uint32_t off, const uint8_t *data, uint32_t len, stamp s) {
	uint16_t chain[MSG_MAX / CL + 2], c;
	uint8_t e[32], block[CL];
	uint32_t need = len ? (len + CL - 1) / CL : 1, n = 0, i;

	if (rd(v, off, e, 32))
		return v->err;
	for (c = le16(e + 26); is_cluster(v, c) && n < MSG_MAX / CL + 2; c = fat_get(v, c))
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
	fat_set(v, chain[need - 1], 0xffff);
	for (i = 0; i < need; ++i) {
		uint32_t part = len - i * CL < CL ? len - i * CL : CL;
		memset(block, 0, CL);
		memcpy(block, data + i * CL, part);
		wr(v, at(chain[i]), block, CL);
	}
	put_le16(e + 22, s.t);
	put_le16(e + 24, s.d);
	put_le32(e + 28, len);
	return wr(v, off, e, 32);
}

// ---- Dates ----------------------------------------------------------------

typedef struct {
	int y, mo, d, h, mi, s;  // month 1-12
} civil;

static civil to_civil(uint64_t ticks) {
	uint64_t secs = ticks / CDBLOG_TICKS_PER_SEC;
	int64_t z = (int64_t) (secs / 86400) + 10957 + 719468;  // days since 0000-03-01
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

static stamp fat_stamp(uint64_t ticks) {
	civil c = to_civil(ticks);
	stamp s;

	s.t = (uint16_t) (c.h << 11 | c.mi << 5 | c.s / 2);
	s.d = (uint16_t) ((c.y - 1980) << 9 | c.mo << 5 | c.d);
	return s;
}

// "NN" or "NNNN", space-padded to an 8.3 name.
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

// ---- The message ----------------------------------------------------------

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
	uint64_t m = ticks / CDBLOG_TICKS_PER_SEC / 60;
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

// Rebuild the message from its header and list (English, as the Wii Menu
// writes it; the Wii Menu regenerates the text in its own language when it
// next adds a record).
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
	put32(out + 0x7c, (uint32_t) (newest / CDBLOG_TICKS_PER_SEC));
	put64(out + 0x410, now);
	put32(out + 0x52c, list_at - 0x400);
	put32(out + 0x540, crc32(out + 0x400, 0x140));
	return list_at + LIST_SIZE + TAIL;
}

static void new_header(uint8_t *h, const uint8_t *wiiid, uint32_t number) {
	memset(h, 0, TEXT_AT);
	memcpy(h, "CDBFILE\x02", 8);
	memcpy(h + 8, wiiid, 8);
	put_le32(h + 0x10, 12);  // little-endian, unlike the rest
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

// The last playtimelog message under the day's folder, or 0.
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
				if (!rd(v, at(le16(e + 26)), head, sizeof(head)) &&
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
		if (!fat_get(v, c) || rd(v, at(c), head, 16))
			continue;
		if (!memcmp(head, "CDBFILE\x02", 8)) {
			memcpy(id, head + 8, 8);
			return 0;
		}
	}
	return -1;
}

static int add(vff *v, const cdblog_session *s, uint64_t now, const uint8_t *wiiid,
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
			return CDBLOG_E_FORMAT;
		memcpy(entries, msg + list + 8, (uint32_t) count * ENTRY);
		for (i = 0; i < count; ++i) {
			uint8_t *e = entries + i * ENTRY;
			if (!memcmp(e + 8 + 4, rec + 4, 84) && !memcmp(e + 8 + 0x68, rec + 0x68, 6)) {
				uint64_t b = get64(e + 8 + 0x58), l = get64(e + 8 + 0x60);
				char old_id[6];
				uint8_t old_name[84];

				// record() clears its target first: copy what it reads.
				memcpy(old_name, e + 8 + 4, 84);
				memcpy(old_id, e + 8 + 0x68, 6);
				record(e + 8, old_name, old_id, b, b + (l - b) + (s->end - s->start));
				len = build(out, msg, entries, count, now);
				write_file(v, foff, out, len, st);
				return v->err ? v->err : CDBLOG_COMBINED;
			}
		}
		if (count >= MAX_TITLES)
			return CDBLOG_E_FULL;
		put_le32(entries + count * ENTRY, 1);
		put_le32(entries + count * ENTRY + 4, 0);
		memcpy(entries + count * ENTRY + 8, rec, 128);
		len = build(out, msg, entries, count + 1, now);
		write_file(v, foff, out, len, st);
		return v->err ? v->err : CDBLOG_ADDED;
	}

	// A new message: the next number after cdb.conf's, folders from END.
	conf = find(v, 0, "CDB~1   CON");
	if (!conf || read_file(v, conf, msg, &size) || size < 4)
		return v->err ? v->err : CDBLOG_E_FORMAT;
	number = get32(msg) + 1;
	if (wiiid)
		memcpy(id, wiiid, 8);
	else if (scan_wiiid(v, id))
		return v->err ? v->err : CDBLOG_E_FORMAT;

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
		return v->err ? v->err : CDBLOG_E_FULL;
	{
		static const char digits[] = "0123456789ABCDEF";
		uint32_t secs = (uint32_t) (s->end / CDBLOG_TICKS_PER_SEC);
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
	write_file(v, where, out, len, st);
	put32(msg, number);
	write_file(v, conf, msg, 4, st);
	return v->err ? v->err : CDBLOG_CREATED;
}

int cdblog_add(const cdblog_io *io, const cdblog_session *s, uint64_t now,
			   const uint8_t *wiiid) {
	uint8_t head[16], *msg, *out;
	vff v;
	int res, i;

	memset(&v, 0, sizeof(v));
	v.io = io;
	if (s->end < s->start)
		return CDBLOG_E_FORMAT;
	if (rd(&v, 0, head, sizeof(head)))
		return v.err;
	if (memcmp(head, "VFF ", 4) || get32(head + 8) != VFF_SIZE)
		return CDBLOG_E_FORMAT;
	v.nclusters = (VFF_SIZE - DATA) / CL + 2;
	v.fat = malloc(FAT_LEN);
	msg = malloc(MSG_MAX);
	out = malloc(MSG_MAX);
	if (!v.fat || !msg || !out) {
		free(v.fat);
		free(msg);
		free(out);
		return CDBLOG_E_MEM;
	}
	if (!rd(&v, FAT1, v.fat, FAT_LEN))
		res = add(&v, s, now, wiiid, msg, out);
	else
		res = v.err;

	// The FAT last, both copies, only the sectors that changed. The message
	// itself is rewritten in place first, so a failure part way can leave a
	// half-written message, but never a FAT that points at unwritten clusters.
	if (res >= 0)
		for (i = 0; i < (int) (FAT_LEN / 512); ++i)
			if (v.dirty[i]) {
				wr(&v, FAT1 + 512 * i, v.fat + 512 * i, 512);
				wr(&v, FAT2 + 512 * i, v.fat + 512 * i, 512);
			}
	if (v.err)
		res = v.err;
	free(v.fat);
	free(msg);
	free(out);
	return res;
}
