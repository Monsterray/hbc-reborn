// "Save Mii faces": copies the Wii's own Mii models to sd:/mii for apps that
// draw Miis (docs/research/mii-rendering.md, docs/research/mii-bodies.md,
// sdk/mii): the faces, RFL_Res.dat, from the Wii Menu, and the bodies,
// maleBody.brres and femaleBody.brres, from the Mii Channel. The files are
// Nintendo's and stay on the user's own SD card; they are never shipped with
// an app.
//
// Both titles keep them in U8 archives inside their content files, and an
// app cannot read any of them: ES_OpenTitleContent refuses another title's
// contents (-1026), and ISFS refuses /shared1 and the titles' content
// folders (-102), as tests/mii_res_probe showed on the bench Wii. So HBC
// lifts IOS's NAND permission check for the reads (nandperm.c, as for the
// play log), reads only what it needs of each content (the U8 header and
// node table, then the one file) and puts the check back.

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>

#include "../config.h"
#include "appentry.h"
#include "nandperm.h"
#include "miires.h"

#define MENU 0x0000000100000002ull
#define MII_CHANNEL 0x0001000248414300ull   // HAC + the region letter
#define U8_MAGIC 0x55AA382D
#define MAX_TABLE (256 * 1024)    // a U8 node and name table this big is not one
#define MAX_RES (4 * 1024 * 1024)
#define MAX_SZS (2 * 1024 * 1024)
#define DEST_DIR "sd:/mii"
#define DEST DEST_DIR "/RFL_Res.dat"
#define DEST_TMP_SUFFIX ".part"   // written whole, then renamed

static mutex_t lock = LWP_MUTEX_NULL;

static u32 be32(const u8 *p) {
	return (u32) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static bool read_at(s32 fd, u32 off, void *buf, u32 len) {
	return ISFS_Seek(fd, off, SEEK_SET) >= 0 && ISFS_Read(fd, buf, len) == (s32) len;
}

// RFL_Res.dat's own header: 18 archives, a version RFL accepts, each archive
// inside the file.
static bool res_ok(const u8 *d, u32 size) {
	u32 i;

	if (size < 4 + 18 * 4 || (d[0] << 8 | d[1]) != 18 || (d[2] << 8 | d[3]) < 0x39D)
		return false;
	for (i = 0; i < 18; ++i)
		if (be32(d + 4 + 4 * i) + 4 > size)
			return false;
	return true;
}

// The file called `name` in the U8 archive at NAND `path` (at any depth);
// on finding it, its bytes in *out (memalign'd) and its size.
static s32 find_in(const char *path, const char *name, u32 max, u8 **out, u32 *size) {
	static u8 head[32] ATTRIBUTE_ALIGN(32);
	u8 *table = NULL;
	u32 root, table_len, count, i;
	s32 fd = ISFS_Open(path, ISFS_OPEN_READ), res = -ENOENT;

	if (fd < 0)
		return fd;
	if (!read_at(fd, 0, head, sizeof(head)) || be32(head) != U8_MAGIC)
		goto done;
	root = be32(head + 4);
	table_len = be32(head + 8);   // the nodes and their names
	if (root < 0x20 || table_len < 12 || table_len > MAX_TABLE)
		goto done;
	table = memalign(32, (table_len + 31) & ~31);
	if (!table) {
		res = -ENOMEM;
		goto done;
	}
	if (!read_at(fd, root, table, table_len))
		goto done;
	count = be32(table + 8);
	if (count < 2 || count * 12 > table_len)
		goto done;
	for (i = 1; i < count; ++i) {
		const u8 *n = table + i * 12;
		u32 at = (be32(n) & 0xffffff) + count * 12, off = be32(n + 4), len = be32(n + 8);

		if (n[0] != 0 || at >= table_len || strncasecmp((const char *) table + at, name, table_len - at))
			continue;
		if (len < 16 || len > max) {
			res = -EINVAL;
			break;
		}
		*out = memalign(32, (len + 31) & ~31);
		if (!*out) {
			res = -ENOMEM;
			break;
		}
		if (!read_at(fd, off, *out, len)) {
			free(*out);
			*out = NULL;
			res = -EIO;
			break;
		}
		*size = len;
		res = 0;
		break;
	}
done:
	free(table);
	ISFS_Close(fd);
	return res;
}

// The file called `name` in a U8 archive in memory: its offset and size.
static bool find_mem(const u8 *d, u32 size, const char *name, u32 *off, u32 *len) {
	u32 root, count, i, names;

	if (size < 0x20 || be32(d) != U8_MAGIC)
		return false;
	root = be32(d + 4);
	if (root >= size || size - root < 12)
		return false;
	count = be32(d + root + 8);
	if (count < 2 || count > (size - root) / 12)
		return false;
	names = root + count * 12;
	for (i = 1; i < count; ++i) {
		const u8 *n = d + root + i * 12;
		u32 at = names + (be32(n) & 0xffffff);

		if (n[0] != 0 || at >= size || strncasecmp((const char *) d + at, name, size - at))
			continue;
		*off = be32(n + 4);
		*len = be32(n + 8);
		return *off <= size && *len <= size - *off;
	}
	return false;
}

// Yaz0 (an SZS file): the decompressed bytes (memalign'd), or NULL.
static u8 *yaz0(const u8 *d, u32 size, u32 *out_size) {
	u32 n, at = 16, o = 0;
	u8 *out;

	if (size < 16 || memcmp(d, "Yaz0", 4))
		return NULL;
	n = be32(d + 4);
	if (!n || n > MAX_SZS)
		return NULL;
	out = memalign(32, (n + 31) & ~31);
	if (!out)
		return NULL;
	while (o < n) {
		u8 flags;
		int bit;

		if (at >= size)
			goto bad;
		flags = d[at++];
		for (bit = 0; bit < 8 && o < n; ++bit) {
			if (flags & (0x80 >> bit)) {
				if (at >= size)
					goto bad;
				out[o++] = d[at++];
			} else {
				u32 dist, len;

				if (at + 2 > size)
					goto bad;
				dist = ((d[at] & 15) << 8 | d[at + 1]) + 1;
				len = d[at] >> 4;
				at += 2;
				if (!len) {
					if (at >= size)
						goto bad;
					len = d[at++] + 0x12;
				} else {
					len += 2;
				}
				if (dist > o || len > n - o)
					goto bad;
				while (len--) {
					out[o] = out[o - dist];
					++o;
				}
			}
		}
	}
	*out_size = n;
	return out;
bad:
	free(out);
	return NULL;
}

// Each of the System Menu's contents, shared ones (TMD type 0x8000) by the
// name /shared1/content.map gives their SHA-1, until one holds the faces.
static s32 find_faces(u8 **out, u32 *size, char *where, size_t where_len) {
	static u8 tmd_buf[MAX_SIGNED_TMD_SIZE] ATTRIBUTE_ALIGN(32);
	static fstats st ATTRIBUTE_ALIGN(32);
	char path[64];
	u8 *map = NULL;
	u32 tmd_size = 0, map_len = 0, i, e;
	s32 r, res = -ENOENT;
	tmd *t;

	if ((r = ES_GetStoredTMDSize(MENU, &tmd_size)) < 0)
		return r;
	if (tmd_size > sizeof(tmd_buf))
		return -EINVAL;
	if ((r = ES_GetStoredTMD(MENU, (signed_blob *) tmd_buf, tmd_size)) < 0)
		return r;
	t = SIGNATURE_PAYLOAD((signed_blob *) tmd_buf);

	r = ISFS_Open("/shared1/content.map", ISFS_OPEN_READ);
	if (r >= 0) {
		if (ISFS_GetFileStats(r, &st) >= 0 && st.file_length <= 64 * 1024 &&
				(map = memalign(32, (st.file_length + 31) & ~31))) {
			map_len = st.file_length;
			if (ISFS_Read(r, map, map_len) != (s32) map_len)
				map_len = 0;
		}
		ISFS_Close(r);
	}

	for (i = 0; i < t->num_contents && res; ++i) {
		tmd_content *c = &t->contents[i];

		path[0] = 0;
		if (c->type & 0x8000) {
			for (e = 0; e + 28 <= map_len; e += 28)
				if (!memcmp(map + e + 8, c->hash, 20)) {
					snprintf(path, sizeof(path), "/shared1/%.8s.app", (const char *) map + e);
					break;
				}
		} else {
			snprintf(path, sizeof(path), "/title/00000001/00000002/content/%08x.app",
					 (unsigned) c->cid);
		}
		if (!path[0])
			continue;
		r = find_in(path, "RFL_Res.dat", MAX_RES, out, size);
		if (!r && !res_ok(*out, *size)) {
			free(*out);
			*out = NULL;
			r = -EINVAL;
		}
		if (!r) {
			snprintf(where, where_len, "System Menu v%u, content %u", (unsigned) t->title_version,
					 (unsigned) c->index);
			res = 0;
		} else if (r == -ENOMEM) {
			res = r;
		}
	}
	free(map);
	return res;
}

static s32 save(const char *dest, const u8 *data, u32 size) {
	char part[64];
	FILE *f;
	size_t n;

	snprintf(part, sizeof(part), "%s" DEST_TMP_SUFFIX, dest);
	app_entry_mount_for(dest);
	mkdir(DEST_DIR, 0777);
	f = fopen(part, "wb");
	if (!f)
		return errno ? -errno : -EIO;
	n = fwrite(data, 1, size, f);
	if (fclose(f) || n != size) {
		unlink(part);
		return -EIO;
	}
	unlink(dest);
	if (rename(part, dest)) {
		unlink(part);
		return -EIO;
	}
	return 0;
}

// The Mii Channel's Character.szs (in a U8 archive in one of its contents):
// its male and female bodies, to sd:/mii. 0, or why not.
static s32 save_bodies(char *where, size_t where_len) {
	static const char regions[] = "AEJPK";
	static const char *files[] = { "maleBody.brres", "femaleBody.brres" };
	static u8 tmd_buf[MAX_SIGNED_TMD_SIZE] ATTRIBUTE_ALIGN(32);
	char path[64];
	u32 tmd_size = 0, i, szs_size = 0, arc_size = 0;
	u8 *szs = NULL, *arc = NULL;
	u64 title = 0;
	s32 r = -ENOENT;
	tmd *t;
	int k;

	for (k = 0; regions[k]; ++k)
		if (ES_GetStoredTMDSize(MII_CHANNEL | (u8) regions[k], &tmd_size) >= 0) {
			title = MII_CHANNEL | (u8) regions[k];
			break;
		}
	if (!title || tmd_size > sizeof(tmd_buf) ||
			ES_GetStoredTMD(title, (signed_blob *) tmd_buf, tmd_size) < 0)
		return -ENOENT;
	t = SIGNATURE_PAYLOAD((signed_blob *) tmd_buf);
	for (i = 0; i < t->num_contents && r; ++i) {
		if (t->contents[i].type & 0x8000)
			continue;
		snprintf(path, sizeof(path), "/title/%08x/%08x/content/%08x.app", (unsigned) (title >> 32),
				 (unsigned) title, (unsigned) t->contents[i].cid);
		r = find_in(path, "Character.szs", MAX_SZS, &szs, &szs_size);
		if (!r)
			snprintf(where, where_len, "Mii Channel v%u, content %u", (unsigned) t->title_version,
					 (unsigned) t->contents[i].index);
	}
	if (r)
		return r;
	arc = yaz0(szs, szs_size, &arc_size);
	free(szs);
	if (!arc)
		return -EINVAL;
	for (k = 0; k < 2 && !r; ++k) {
		u32 off, len;
		char dest[48];

		if (!find_mem(arc, arc_size, files[k], &off, &len) || len < 16 || memcmp(arc + off, "bres", 4)) {
			r = -EINVAL;
			break;
		}
		snprintf(dest, sizeof(dest), DEST_DIR "/%s", files[k]);
		r = save(dest, arc + off, len);
	}
	free(arc);
	return r;
}

s32 miires_save(char *msg, size_t len) {
	char where[48] = "", body_where[48] = "";
	u8 *data = NULL;
	u32 size = 0, level;
	s32 res, bodies = -ENOENT;

	_CPU_ISR_Disable(level);
	if (lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&lock, false);
	_CPU_ISR_Restore(level);
	if (LWP_MutexTryLock(lock)) {
		snprintf(msg, len, "Already saving the Mii faces");
		return -EBUSY;
	}
	if (!nand_unlock()) {
		snprintf(msg, len, "Mii faces: needs AHBPROT (HBC's own IOS)");
		res = -EACCES;
	} else {
		res = find_faces(&data, &size, where, sizeof(where));
		if (!res)
			bodies = save_bodies(body_where, sizeof(body_where));
		nand_relock();
		if (res == -ENOENT)
			snprintf(msg, len, "No Mii faces in the System Menu");
		else if (res)
			snprintf(msg, len, "Mii faces: could not read the System Menu (%d)", (int) res);
		else if ((res = save(DEST, data, size)))
			snprintf(msg, len, "Mii faces: could not write " DEST " (%d)", (int) res);
		else if (bodies)
			snprintf(msg, len, "Saved Mii faces to " DEST_DIR "; no Mii Channel bodies (%d)", (int) bodies);
		else
			snprintf(msg, len, "Saved Mii faces and bodies to " DEST_DIR);
	}
	hlog("Mii faces: %s%s%s%s%s\n", msg, where[0] ? ", from " : "", where, body_where[0] ? " and " : "",
		 body_where);
	free(data);
	LWP_MutexUnlock(lock);
	return res;
}
