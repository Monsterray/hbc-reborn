// "Save Mii faces": copies the Wii Menu's Mii face resource, RFL_Res.dat, to
// sd:/mii/RFL_Res.dat for apps that draw Miis (docs/research/mii-rendering.md,
// sdk/mii). The file is Nintendo's and stays on the user's own SD card; it
// is never shipped with an app.
//
// The Wii Menu (title 1-2) keeps it in a U8 archive inside one of its content
// files, and an app cannot read any of them: ES_OpenTitleContent refuses
// another title's contents (-1026), and ISFS refuses /shared1 and the
// title's content folder (-102), as tests/mii_res_probe showed on the bench
// Wii. So HBC lifts IOS's NAND permission check for the reads (nandperm.c,
// as for the play log), reads only what it needs of each content (the U8
// header and node table, then the one file) and puts the check back.

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
#define U8_MAGIC 0x55AA382D
#define MAX_TABLE (256 * 1024)    // a U8 node and name table this big is not one
#define MAX_RES (4 * 1024 * 1024)
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

// Looks for RFL_Res.dat in the U8 archive at NAND path; on finding it,
// returns its bytes in *out (memalign'd) and its size.
static s32 find_in(const char *path, u8 **out, u32 *size) {
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
		u32 name = (be32(n) & 0xffffff) + count * 12, off = be32(n + 4), len = be32(n + 8);

		if (n[0] != 0 || name >= table_len ||
				strncasecmp((const char *) table + name, "RFL_Res.dat", table_len - name))
			continue;
		if (len < 4 + 18 * 4 || len > MAX_RES) {
			res = -EINVAL;
			break;
		}
		*out = memalign(32, (len + 31) & ~31);
		if (!*out) {
			res = -ENOMEM;
			break;
		}
		if (!read_at(fd, off, *out, len) || !res_ok(*out, len)) {
			free(*out);
			*out = NULL;
			res = -EINVAL;
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

// Each of the System Menu's contents, shared ones (TMD type 0x8000) by the
// name /shared1/content.map gives their SHA-1, until one holds the file.
static s32 find(u8 **out, u32 *size, char *where, size_t where_len) {
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
		r = find_in(path, out, size);
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

static s32 save(const u8 *data, u32 size) {
	FILE *f;
	size_t n;

	app_entry_mount_for(DEST);
	mkdir(DEST_DIR, 0777);
	f = fopen(DEST DEST_TMP_SUFFIX, "wb");
	if (!f)
		return errno ? -errno : -EIO;
	n = fwrite(data, 1, size, f);
	if (fclose(f) || n != size) {
		unlink(DEST DEST_TMP_SUFFIX);
		return -EIO;
	}
	unlink(DEST);
	if (rename(DEST DEST_TMP_SUFFIX, DEST)) {
		unlink(DEST DEST_TMP_SUFFIX);
		return -EIO;
	}
	return 0;
}

s32 miires_save(char *msg, size_t len) {
	char where[48] = "";
	u8 *data = NULL;
	u32 size = 0, level;
	s32 res;

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
		res = find(&data, &size, where, sizeof(where));
		nand_relock();
		if (res == -ENOENT)
			snprintf(msg, len, "No Mii faces in the System Menu");
		else if (res)
			snprintf(msg, len, "Mii faces: could not read the System Menu (%d)", (int) res);
		else if ((res = save(data, size)))
			snprintf(msg, len, "Mii faces: could not write " DEST " (%d)", (int) res);
		else
			snprintf(msg, len, "Saved Mii faces to " DEST " (%u KB)", (unsigned) (size + 1023) / 1024);
	}
	hlog("Mii faces: %s%s%s\n", msg, where[0] ? ", from " : "", where);
	free(data);
	LWP_MutexUnlock(lock);
	return res;
}
