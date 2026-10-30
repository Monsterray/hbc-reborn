/*
 * miinand.c - the Wii's own Mii faces and bodies, read from NAND into memory
 * (miidraw.h), with nothing saved to SD. Public domain.
 */

#ifndef MII_NO_OGC

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <gccore.h>
#include <ogc/machine/processor.h>

#include "miidraw.h"

#define HW_AHBPROT 0x0d800064
#define MEM2_PROT 0x0d8b420a
#define IOS_MEM2_START ((u8 *) *(u32 *) 0x80003134)
#define IOS_MEM2_END ((u8 *) 0x94000000)
#define ISFS_DENIED -102
#define ISFS_MISSING -106
#define U8_MAGIC 0x55aa382d
#define MENU_TITLE 0x0000000100000002ull
#define MII_CHANNEL 0x0001000248414300ull   /* "HAC" and a region letter */
#define MAX_TABLE (256 * 1024)
#define MAX_PART (8 * 1024 * 1024)
#define MAX_SITES 8

/* IOS's FS module checks each open with `cmp r3, r1; beq allowed`. Turning
 * the beq into b lets the open through; other homebrew patches the same bytes. */
static const u8 perm_check[] = { 0x42, 0x8b, 0xd0, 0x01, 0x25, 0x66 };

typedef struct {
	u8 *site[MAX_SITES];
	int sites;
	u16 mem2_prot;
} lift;

static mutex_t nand_lock = LWP_MUTEX_NULL;
static u8 tmd_buf[MAX_SIGNED_TMD_SIZE] ATTRIBUTE_ALIGN(32);
static u8 head[32] ATTRIBUTE_ALIGN(32);
static fstats stats ATTRIBUTE_ALIGN(32);

static u32 be32(const u8 *p) {
	return (u32) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static int nand_error(s32 r) {
	return r == ISFS_DENIED ? MII_E_DENIED : r == ISFS_MISSING ? MII_E_NO_FILE : MII_E_IO;
}

static bool lift_check(lift *l) {
	u8 *p = IOS_MEM2_START, *end = IOS_MEM2_END - sizeof(perm_check);

	l->sites = 0;
	if (read32(HW_AHBPROT) != 0xffffffff || (u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return false;
	l->mem2_prot = read16(MEM2_PROT);
	write16(MEM2_PROT, 2);
	for (; p < end && l->sites < MAX_SITES; ++p)
		if (p[0] == perm_check[0] && !memcmp(p, perm_check, sizeof(perm_check))) {
			p[2] = 0xe0;
			DCFlushRange(p, sizeof(perm_check));
			l->site[l->sites++] = p;
		}
	/* Left writable, MEM2 outlived the app and hung HBC's next run (REVIEW.md, 1.10.22). */
	if (!l->sites)
		write16(MEM2_PROT, l->mem2_prot);
	return l->sites > 0;
}

/* Puts back only the sites lift_check changed, so an app's own lasting patch stays. */
static void restore_check(lift *l) {
	int i;

	for (i = 0; i < l->sites; ++i) {
		l->site[i][2] = perm_check[2];
		DCFlushRange(l->site[i], sizeof(perm_check));
	}
	if (l->sites)
		write16(MEM2_PROT, l->mem2_prot);
	l->sites = 0;
}

static int nand_begin(lift *l) {
	u32 level;
	s32 fd;

	_CPU_ISR_Disable(level);
	if (nand_lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&nand_lock, false);
	_CPU_ISR_Restore(level);
	LWP_MutexLock(nand_lock);
	l->sites = 0;
	ISFS_Initialize();
	/* Dolphin and IOSes patched for good let the reads through as they are. */
	fd = ISFS_Open("/shared1/content.map", ISFS_OPEN_READ);
	if (fd >= 0)
		ISFS_Close(fd);
	else if (fd == ISFS_DENIED && !lift_check(l)) {
		LWP_MutexUnlock(nand_lock);
		return MII_E_DENIED;
	}
	return MII_OK;
}

static void nand_end(lift *l) {
	restore_check(l);
	LWP_MutexUnlock(nand_lock);
}

static bool read_at(s32 fd, u32 off, void *buf, u32 len) {
	return ISFS_Seek(fd, off, SEEK_SET) >= 0 && ISFS_Read(fd, buf, len) == (s32) len;
}

static int read_whole(const char *path, u32 max, u8 **out, u32 *len) {
	s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
	int r = MII_E_IO;

	*out = NULL;
	if (fd < 0)
		return nand_error(fd);
	if (ISFS_GetFileStats(fd, &stats) < 0 || !stats.file_length || stats.file_length > max)
		r = MII_E_FORMAT;
	else if (!(*out = memalign(32, (stats.file_length + 31) & ~31)))
		r = MII_E_MEM;
	else if (read_at(fd, 0, *out, stats.file_length)) {
		*len = stats.file_length;
		r = MII_OK;
	} else {
		free(*out);
		*out = NULL;
	}
	ISFS_Close(fd);
	return r;
}

/* The file `name`, at any depth, among a U8 archive's nodes and names (`table`,
 * from the root node on): its offset in the archive and its size. */
static bool u8_find(const u8 *table, u32 table_len, const char *name, u32 *off, u32 *len) {
	u32 count, names, i, name_len = strlen(name);

	if (table_len < 12)
		return false;
	count = be32(table + 8);
	if (count < 2 || count > table_len / 12)
		return false;
	names = count * 12;
	for (i = 1; i < count; ++i) {
		const u8 *n = table + i * 12;
		u32 at = names + (be32(n) & 0xffffff);

		if (!n[0] && at < table_len && table_len - at > name_len &&
				!strncasecmp((const char *) table + at, name, name_len + 1)) {
			*off = be32(n + 4);
			*len = be32(n + 8);
			return true;
		}
	}
	return false;
}

/* Reads only the archive's header, its node table and the one file. */
static int nand_u8_file(const char *path, const char *name, u8 **out, u32 *len) {
	u8 *table = NULL;
	u32 root, table_len, off, size;
	s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
	int r = MII_E_NO_FILE;

	*out = NULL;
	if (fd < 0)
		return nand_error(fd);
	if (!read_at(fd, 0, head, sizeof(head)) || be32(head) != U8_MAGIC)
		goto done;
	root = be32(head + 4);
	table_len = be32(head + 8);
	if (table_len < 12 || table_len > MAX_TABLE)
		goto done;
	if (!(table = memalign(32, (table_len + 31) & ~31))) {
		r = MII_E_MEM;
		goto done;
	}
	if (!read_at(fd, root, table, table_len) || !u8_find(table, table_len, name, &off, &size))
		goto done;
	if (!size || size > MAX_PART) {
		r = MII_E_FORMAT;
		goto done;
	}
	if (!(*out = memalign(32, (size + 31) & ~31))) {
		r = MII_E_MEM;
		goto done;
	}
	if (!read_at(fd, off, *out, size)) {
		free(*out);
		*out = NULL;
		r = MII_E_IO;
		goto done;
	}
	*len = size;
	r = MII_OK;
done:
	free(table);
	ISFS_Close(fd);
	return r;
}

static tmd *stored_tmd(u64 title) {
	u32 size = 0;

	if (ES_GetStoredTMDSize(title, &size) < 0 || size > sizeof(tmd_buf) ||
			ES_GetStoredTMD(title, (signed_blob *) tmd_buf, size) < 0)
		return NULL;
	return SIGNATURE_PAYLOAD((signed_blob *) tmd_buf);
}

/* A shared content lives in /shared1 under the name content.map gives its hash. */
static bool content_path(u64 title, const tmd_content *c, const u8 *map, u32 map_len, char *path, size_t path_len) {
	u32 e;

	if (!(c->type & 0x8000)) {
		snprintf(path, path_len, "/title/%08x/%08x/content/%08x.app", (unsigned) (title >> 32),
				 (unsigned) title, (unsigned) c->cid);
		return true;
	}
	for (e = 0; e + 28 <= map_len; e += 28)
		if (!memcmp(map + e + 8, c->hash, 20)) {
			snprintf(path, path_len, "/shared1/%.8s.app", (const char *) map + e);
			return true;
		}
	return false;
}

static int find_faces(mii_res *res) {
	char path[64];
	u8 *map = NULL, *data;
	u32 map_len = 0, size, i;
	int r = MII_E_NO_FILE, found;
	tmd *t = stored_tmd(MENU_TITLE);

	if (!t)
		return MII_E_NO_FILE;
	if (read_whole("/shared1/content.map", 64 * 1024, &map, &map_len) < 0)
		map_len = 0;
	for (i = 0; i < t->num_contents && r < 0; ++i) {
		if (!content_path(MENU_TITLE, &t->contents[i], map, map_len, path, sizeof(path)))
			continue;
		found = nand_u8_file(path, "RFL_Res.dat", &data, &size);
		if (found == MII_E_MEM || found == MII_E_DENIED) {
			r = found;
			break;
		}
		if (found < 0)
			continue;
		if ((r = mii_res_attach(res, data, size)) < 0)
			free(data);
		else
			res->owned = data;
	}
	free(map);
	return r;
}

/* Yaz0 (an SZS file), decoded into a new buffer, or NULL. */
static u8 *yaz0_decode(const u8 *in, u32 in_len, u32 *out_len) {
	u32 n, i = 16, o = 0, dist, count;
	u8 *out, code;
	int b;

	if (in_len < 16 || memcmp(in, "Yaz0", 4))
		return NULL;
	n = be32(in + 4);
	if (!n || n > MAX_PART || !(out = memalign(32, (n + 31) & ~31)))
		return NULL;
	while (o < n) {
		if (i >= in_len)
			goto bad;
		code = in[i++];
		for (b = 7; b >= 0 && o < n; --b) {
			if (code >> b & 1) {
				if (i >= in_len)
					goto bad;
				out[o++] = in[i++];
				continue;
			}
			if (i + 2 > in_len)
				goto bad;
			dist = ((in[i] & 0x0f) << 8 | in[i + 1]) + 1;
			count = in[i] >> 4;
			i += 2;
			if (count)
				count += 2;
			else if (i < in_len)
				count = in[i++] + 0x12;
			else
				goto bad;
			if (dist > o || count > n - o)
				goto bad;
			for (; count; --count, ++o)
				out[o] = out[o - dist];
		}
	}
	*out_len = n;
	return out;
bad:
	free(out);
	return NULL;
}

static u8 *find_characters(u32 *len) {
	static const char regions[] = "AEJPK";
	char path[64];
	u8 *szs = NULL, *arc = NULL;
	u32 szs_len, i;
	u64 title = 0;
	tmd *t = NULL;
	int k;

	for (k = 0; regions[k] && !t; ++k) {
		title = MII_CHANNEL | (u8) regions[k];
		t = stored_tmd(title);
	}
	for (i = 0; t && i < t->num_contents && !arc; ++i) {
		if (!content_path(title, &t->contents[i], NULL, 0, path, sizeof(path)) ||
				nand_u8_file(path, "Character.szs", &szs, &szs_len) < 0)
			continue;
		arc = yaz0_decode(szs, szs_len, len);
		free(szs);
	}
	return arc;
}

static int attach_body(mii_body *body, const u8 *arc, u32 arc_len, const char *name) {
	u32 root = be32(arc + 4), off, len;
	u8 *copy;
	int r;

	if (root >= arc_len || !u8_find(arc + root, arc_len - root, name, &off, &len) ||
			off > arc_len || len > arc_len - off)
		return MII_E_FORMAT;
	if (!(copy = memalign(32, (len + 31) & ~31)))
		return MII_E_MEM;
	memcpy(copy, arc + off, len);
	if ((r = mii_body_attach(body, copy, len)) < 0)
		free(copy);
	return r;
}

int mii_res_load_nand(mii_res *res) {
	lift l;
	int r;

	if (!res)
		return MII_E_ARG;
	memset(res, 0, sizeof(*res));
	if ((r = nand_begin(&l)) < 0)
		return r;
	r = find_faces(res);
	nand_end(&l);
	return r;
}

int mii_bodies_load_nand(mii_body *male, mii_body *female) {
	u8 *arc;
	u32 len = 0;
	lift l;
	int r;

	if (!male && !female)
		return MII_E_ARG;
	if ((r = nand_begin(&l)) < 0)
		return r;
	arc = find_characters(&len);
	nand_end(&l);
	if (!arc)
		return MII_E_NO_FILE;
	if (be32(arc) != U8_MAGIC || len < 0x20) {
		free(arc);
		return MII_E_FORMAT;
	}
	r = male ? attach_body(male, arc, len, "maleBody.brres") : MII_OK;
	if (r >= 0 && female && (r = attach_body(female, arc, len, "femaleBody.brres")) < 0 && male)
		mii_body_free(male);
	free(arc);
	return r;
}

#endif
