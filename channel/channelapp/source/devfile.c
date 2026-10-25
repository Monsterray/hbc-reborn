// File requests of the developer protocol (HBCF, docs/devnet.md), shared by
// HBC's loader thread and the in-app agent (sdk/hbc_agent). Framed transfers
// run in devstream.c; this file validates paths and runs the other ops.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <network.h>
#include <zlib.h>

#include "../config.h"
#include "devfile.h"
#include "devstream.h"
#include "tcp.h"

#define DEVFILE_PATH_MAX 256
#define DEVFILE_PUT_MAX (512 * 1024 * 1024)
#define DEVFILE_LIST_MAX (256 * 1024)
#define DEVFILE_CHUNK (32 * 1024)

static const char *device_names[] = { "sd", "usb", "carda", "cardb", "usb2" };
#define DEVICES (sizeof(device_names) / sizeof(device_names[0]))

// The raw ops' buffer, allocated per request so an idle agent holds none.
static u8 *chunk;

devfile_transfer devfile_last;

// Request state, for aborting before an app launch unmounts the card.
static volatile bool busy, abort_requested;
// The file a transfer moves, for devfile_progress().
static char moving[48];
static volatile s32 active_socket = -1;
static s32 request_prio;

static u16 get_u16(const u8 *p) {
	return (p[0] << 8) | p[1];
}

static u32 get_u32(const u8 *p) {
	return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void put_u32(u8 *p, u32 v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

// On a non-blocking socket whose send buffer fills, IOS can report a whole
// block as sent after queueing only part of it (seen on a Wii at the 4 KiB
// mark; Dolphin uses host sockets and does not). Send blocking instead.
bool devfile_send_all(s32 s, const void *data, u32 len) {
	s32 flags = net_fcntl(s, F_GETFL, 0);
	bool ok;

	if (flags >= 0)
		net_fcntl(s, F_SETFL, flags & ~4);
	ok = tcp_write(s, data, len, NULL, NULL);
	if (flags >= 0)
		net_fcntl(s, F_SETFL, flags);
	return ok;
}

// Reply header: s32 status (0 or -errno), u32 payload length.
bool devfile_reply(s32 s, s32 status, const void *data, u32 len) {
	u8 hdr[8];

	put_u32(hdr, status);
	put_u32(hdr + 4, len);
	if (!devfile_send_all(s, hdr, sizeof(hdr)))
		return false;
	return !len || devfile_send_all(s, data, len);
}

// Accept only "<device>:/<path>" on a known device, without "..", "//",
// backslashes, or control characters.
void (*devfile_mount_hook)(const char *path);

static bool valid_path(const char *path) {
	const char *p = strchr(path, ':');
	size_t len;
	u32 i;

	if (!p || p[1] != '/')
		return false;

	len = p - path;
	for (i = 0; i < DEVICES; ++i)
		if (strlen(device_names[i]) == len && !strncmp(path, device_names[i], len))
			break;
	if (i == DEVICES)
		return false;

	if (strstr(path, "..") || strstr(path, "//") || strchr(path, '\\') ||
			strchr(p + 1, ':'))
		return false;

	for (p = path; *p; ++p)
		if ((unsigned char) *p < 0x20)
			return false;

	return true;
}

// ---- FAT attributes --------------------------------------------------------
// Read-only, hidden, system and archive, as a FAT volume keeps them, for the
// listing and op A. libfat's FAT_getAttr reads them on every libogc.
//
// FAT_setAttr does not work alike. libogc2's and libogc 1.x's libfat write
// the attribute byte whole (so the directory bit must go back unchanged).
// libogc 3's calls FatFs as f_chmod(fs, path, 0xff, attr): attr is taken as
// the mask, so it can set bits but never clear one (FAT_setAttr(path, 0)
// changes nothing; disassembled from libogc 3.1.0's libfat). There op A
// calls f_chmod itself, with the volume FAT_setAttr would use: the FatFs
// object at a fixed offset in the device's data, read from FAT_setAttr's
// own code (lwz rX,deviceData(r3); addi rY,rX,OFF) and used only when that
// code also calls f_chmod. Otherwise clearing a bit is ENOTSUP.

#define FA_RO 0x01
#define FA_HID 0x02
#define FA_SYS 0x04
#define FA_DIR 0x10
#define FA_ARC 0x20
#define FA_SETTABLE (FA_RO | FA_HID | FA_SYS | FA_ARC)

int FAT_getAttr(const char *path);
int FAT_setAttr(const char *path, unsigned attr);   // libfat's takes a uint8_t: the same register

#if __has_include(<tuxedo/ppc/exception.h>)
#include <sys/iosupport.h>
int f_chmod(void *fs, const char *path, unsigned char attr, unsigned char mask);

// The offset of the FatFs object in a FAT device's data, or -1.
static int fatfs_offset(void) {
	static int found = -2;
	const u32 *op = (const u32 *) FAT_setAttr;
	u32 want_lwz = (u32) offsetof(devoptab_t, deviceData);
	int i, j, off = -1, calls = 0;

	if (found != -2)
		return found;
	for (i = 0; i < 64 && op[i] != 0x4e800020; ++i) {   // to its blr
		u32 in = op[i];
		// lwz rX,deviceData(r3)
		if ((in >> 26) == 32 && ((in >> 16) & 31) == 3 && (in & 0xffff) == want_lwz && off < 0) {
			u32 rx = (in >> 21) & 31;
			for (j = i + 1; j < i + 6; ++j)
				if ((op[j] >> 26) == 14 && ((op[j] >> 16) & 31) == rx) {   // addi rY,rX,OFF
					off = (s16) (op[j] & 0xffff);
					break;
				}
		}
		// bl f_chmod
		if ((in >> 26) == 18 && (in & 3) == 1) {
			s32 li = (s32) (in << 6) >> 6 & ~3;
			if ((const u8 *) &op[i] + li == (const u8 *) f_chmod)
				++calls;
		}
	}
	found = off >= 0 && off < 4096 && calls == 1 ? off : -1;
	return found;
}
#endif

// The attributes as letters, in the order r h s a; "-" for none.
static void attr_letters(int a, char out[5]) {
	char *p = out;

	if (a & FA_RO)
		*p++ = 'r';
	if (a & FA_HID)
		*p++ = 'h';
	if (a & FA_SYS)
		*p++ = 's';
	if (a & FA_ARC)
		*p++ = 'a';
	if (p == out)
		*p++ = '-';
	*p = 0;
}

// A path's attributes: FAT's own, or (on a volume that has none, or a
// device root) read-only from stat.
static int path_attr(const char *path, const struct stat *st) {
	int a = FAT_getAttr(path);

	if (a < 0)
		a = (st->st_mode & S_IWUSR ? 0 : FA_RO) | (S_ISDIR(st->st_mode) ? FA_DIR : 0);
	return a;
}

// Set the bits of mask (FA_SETTABLE only) to those of value. 0, or -errno.
static s32 set_attr(const char *path, int value, int mask) {
	int cur = FAT_getAttr(path), want;

	if (cur < 0)
		return -ENOTSUP;           // not FAT, or a device root
	want = (cur & ~mask) | (value & mask);
	if (want == cur)
		return 0;
#if __has_include(<tuxedo/ppc/exception.h>)
	{
		const devoptab_t *d = GetDeviceOpTab(path);
		const char *rel = strchr(path, ':');
		int off = fatfs_offset();

		if (!d || !d->deviceData || !rel || off < 0)
			return -ENOTSUP;
		if (f_chmod((u8 *) d->deviceData + off, rel + 1, (unsigned char) want,
					FA_SETTABLE))
			return -EIO;
	}
#else
	if (FAT_setAttr(path, (unsigned) want) < 0)   // the whole byte: the directory bit too
		return errno ? -errno : -EIO;
#endif
	return FAT_getAttr(path) == want ? 0 : -EIO;
}

// Create each parent directory of path.
// Whether the card has room for size bytes: the upload is written beside
// the file it replaces, so all of it, and one cluster for the rounding.
// Refused before any data arrives, so a big upload to a full card fails at
// once instead of after the whole transfer. 0, or -ENOSPC.
static s32 room_for(const char *path, u32 size) {
	char dev[16];
	const char *colon = strchr(path, ':');
	struct statvfs v;

	if (!colon || colon - path + 3 > (int) sizeof(dev))
		return 0;
	memcpy(dev, path, colon - path + 1);
	strcpy(dev + (colon - path + 1), "/");
	if (statvfs(dev, &v) || !v.f_frsize)
		return 0;   // unknown: the write itself says
	return (u64) size + v.f_frsize > (u64) v.f_bavail * v.f_frsize ? -ENOSPC : 0;
}

static void make_parents(char *path) {
	char *p = strchr(path, '/');

	while (p && (p = strchr(p + 1, '/'))) {
		*p = 0;
		mkdir(path, 0777);
		*p = '/';
	}
}

// Protocol 1 upload: raw bytes, no checksum. Kept for older clients.
static s32 file_put_raw(s32 s, const char *path, const char *part, u32 size) {
	devstream_stats *st = &devfile_last.st;
	FILE *f;
	u32 left = size;
	s32 err = 0;

	f = fopen(part, "wb");
	if (!f)
		err = -errno;

	// Read the whole upload even after an error, so the reply stays in sync.
	while (left) {
		u32 block = left > DEVFILE_CHUNK ? DEVFILE_CHUNK : left;
		u64 t0 = gettime();

		if (abort_requested) {
			err = -EINTR;
			break;
		}
		if (!tcp_read(s, chunk, block, NULL, NULL)) {
			err = -EIO;
			break;
		}
		u64 t1 = gettime();
		if (f && !err && fwrite(chunk, 1, block, f) != block)
			err = errno ? -errno : -ENOSPC;
		st->net += diff_ticks(t0, t1);
		st->disk += diff_ticks(t1, gettime());
		st->wire += block;
		st->raw += block;
		left -= block;
	}

	if (f && fclose(f) && !err)
		err = -errno;

	if (!err) {
		unlink(path);
		if (rename(part, path))
			err = -errno;
	} else if (f) {
		unlink(part);
	}

	return err;
}

// Protocol 1 download: reply header, then raw bytes.
static void file_get_raw(s32 s, const char *path) {
	devstream_stats *st = &devfile_last.st;
	struct stat sb;
	u8 hdr[8];
	FILE *f;
	u32 left;

	if (stat(path, &sb) || !S_ISREG(sb.st_mode)) {
		devfile_reply(s, -ENOENT, NULL, 0);
		return;
	}

	f = fopen(path, "rb");
	if (!f) {
		devfile_reply(s, -errno, NULL, 0);
		return;
	}

	left = sb.st_size;
	st->size = left;
	put_u32(hdr, 0);
	put_u32(hdr + 4, left);
	if (devfile_send_all(s, hdr, sizeof(hdr))) {
		while (left) {
			u32 block = left > DEVFILE_CHUNK ? DEVFILE_CHUNK : left;
			u64 t0 = gettime();

			// A short read means the file changed; pad so the length holds.
			size_t got = fread(chunk, 1, block, f);
			if (got < block)
				memset(chunk + got, 0, block - got);
			u64 t1 = gettime();
			bool ok = !abort_requested && devfile_send_all(s, chunk, block);
			st->disk += diff_ticks(t0, t1);
			st->net += diff_ticks(t1, gettime());
			st->wire += block;
			st->raw += block;
			if (!ok)
				break;
			left -= block;
		}
	}
	fclose(f);
}

// One entry of a listing or a stat: "<d|f> <size> <mtime> <attrs> <name>".
// mtime is seconds since 1970 by the Wii's clock (FAT keeps local time
// without a zone; HBCS "time" reads the same clock, so a client can line
// them up); attrs are the FAT attributes as letters, r (read-only), h
// (hidden), s (system) and a (archive), in that order, or "-" for none. A
// directory's size is 0.
static int entry_line(char *out, size_t size, const struct stat *st, const char *path,
					  const char *name) {
	bool dir = S_ISDIR(st->st_mode);
	char attrs[5];

	attr_letters(path_attr(path, st), attrs);
	return snprintf(out, size, "%c %lld %lld %s %s\n", dir ? 'd' : 'f',
					dir ? 0LL : (long long) st->st_size, (long long) st->st_mtime, attrs, name);
}

// Op S: the entry line for one path.
static void file_stat(s32 s, const char *path) {
	char line[DEVFILE_PATH_MAX + 64];
	const char *base = strrchr(path, '/');
	struct stat st;

	if (stat(path, &st)) {
		devfile_reply(s, -errno, NULL, 0);
		return;
	}
	devfile_reply(s, 0, line, entry_line(line, sizeof(line), &st, path, base ? base + 1 : path));
}

static void file_list(s32 s, const char *path) {
	char full[DEVFILE_PATH_MAX * 2];
	struct dirent *de;
	struct stat st;
	char *buf;
	size_t n = 0;
	DIR *d;

	d = opendir(path);
	if (!d) {
		devfile_reply(s, -errno, NULL, 0);
		return;
	}

	buf = malloc(DEVFILE_LIST_MAX);
	if (!buf) {
		closedir(d);
		devfile_reply(s, -ENOMEM, NULL, 0);
		return;
	}

	// "#list 2", then one entry_line() per entry. A listing that reaches the
	// buffer limit ends with "! truncated".
	n = snprintf(buf, DEVFILE_LIST_MAX, "#list 2\n");
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (n >= DEVFILE_LIST_MAX - DEVFILE_PATH_MAX - 64) {
			n += snprintf(buf + n, DEVFILE_LIST_MAX - n, "! truncated\n");
			break;
		}
		// A name too long for the path buffer cannot be stat'ed: skip it.
		if (snprintf(full, sizeof(full), "%s/%s", path, de->d_name) >= (int) sizeof(full) ||
				stat(full, &st))
			continue;
		n += entry_line(buf + n, DEVFILE_LIST_MAX - n, &st, full, de->d_name);
	}
	closedir(d);

	devfile_reply(s, 0, buf, n);
	free(buf);
}

// Reply with the file's size and CRC-32, so clients can skip unchanged files.
static void file_checksum(s32 s, const char *path) {
	struct stat st;
	u8 out[8];
	uLong crc = crc32(0, Z_NULL, 0);
	size_t got;
	FILE *f;

	if (stat(path, &st)) {
		devfile_reply(s, -ENOENT, NULL, 0);
		return;
	}
	if (S_ISDIR(st.st_mode)) {
		devfile_reply(s, -EISDIR, NULL, 0);
		return;
	}

	f = fopen(path, "rb");
	if (!f) {
		devfile_reply(s, -errno, NULL, 0);
		return;
	}
	while ((got = fread(chunk, 1, DEVFILE_CHUNK, f)) > 0 && !abort_requested)
		crc = crc32(crc, chunk, got);
	fclose(f);

	put_u32(out, st.st_size);
	put_u32(out + 4, crc);
	devfile_reply(s, abort_requested ? -EINTR : 0, out, sizeof(out));
}

static bool dir_empty(const char *path) {
	DIR *d = opendir(path);
	struct dirent *de;
	bool empty = true;

	if (!d)
		return true;            // the delete itself says
	while (empty && (de = readdir(d)))
		empty = !strcmp(de->d_name, ".") || !strcmp(de->d_name, "..");
	closedir(d);
	return empty;
}

// Op A: change attributes. size's low byte holds the values, its next byte
// the mask of those to change: r 0x01, h 0x02, s 0x04, a 0x20 (FAT's own
// bits; no others). The reply is the attributes after, as letters.
static void file_attr(s32 s, const char *path, u32 size) {
	int value = size & 0xff, mask = (size >> 8) & 0xff;
	struct stat st;
	char attrs[5];
	s32 err;

	if ((mask & ~FA_SETTABLE) || (size >> 16)) {
		devfile_reply(s, -EINVAL, NULL, 0);
		return;
	}
	if (stat(path, &st)) {
		devfile_reply(s, -ENOENT, NULL, 0);
		return;
	}
	err = mask ? set_attr(path, value, mask) : 0;
	if (err) {
		devfile_reply(s, err, NULL, 0);
		return;
	}
	attr_letters(path_attr(path, &st), attrs);
	devfile_reply(s, 0, attrs, strlen(attrs));
}

// Op R: rename or move. The request's path is the source; size is the length
// of the destination path, which follows it. A destination ending in "/", or
// one that is a folder already, means "into that folder", keeping the name.
// Missing folders above the destination are made. Both must be on the same
// device (EXDEV: a move between devices is a copy and a delete, which the
// client does). An existing destination is EEXIST unless flags bit 1 asks to
// replace it, and only a file is replaced. A folder cannot move into itself.
// The reply's payload is the destination as it ended up.
static void file_move(s32 s, const char *src, u32 size, u8 flags, devfile_change_fn changed) {
	char dst[DEVFILE_PATH_MAX];
	const char *colon = strchr(src, ':');
	const char *base = strrchr(src, '/');
	struct stat from, to;
	size_t n, srclen = strlen(src);
	bool into;
	s32 err = 0;

	if (!size || size >= sizeof(dst) || !tcp_read(s, (u8 *) dst, size, NULL, NULL)) {
		devfile_reply(s, -EINVAL, NULL, 0);
		return;
	}
	dst[size] = 0;

	// A device root ("sd:/") is not a thing to move.
	if (!colon || !colon[2]) {
		devfile_reply(s, -EINVAL, NULL, 0);
		return;
	}
	if (stat(src, &from)) {
		devfile_reply(s, -ENOENT, NULL, 0);
		return;
	}

	n = strlen(dst);
	into = n > 0 && dst[n - 1] == '/';
	if (into && n > 1 && dst[n - 2] != ':')
		dst[--n] = 0;               // "sd:/apps/x/" -> "sd:/apps/x", then the name
	if (valid_path(dst) && devfile_mount_hook)
		devfile_mount_hook(dst);
	if (!valid_path(dst)) {
		devfile_reply(s, -EINVAL, NULL, 0);
		return;
	}
	// Into a folder: an explicit "/", or a folder that is there (and is not
	// the source itself under another case: that is a rename).
	if (into || (!stat(dst, &to) && S_ISDIR(to.st_mode) && strcasecmp(dst, src))) {
		if (n + 1 + strlen(base + 1) >= sizeof(dst)) {
			devfile_reply(s, -ENAMETOOLONG, NULL, 0);
			return;
		}
		if (dst[n - 1] != '/')
			dst[n++] = '/';
		strcpy(dst + n, base + 1);
		if (!valid_path(dst)) {
			devfile_reply(s, -EINVAL, NULL, 0);
			return;
		}
	}

	if (strncmp(src, dst, colon - src + 1))
		err = -EXDEV;
	else if (S_ISDIR(from.st_mode) && !strncasecmp(dst, src, srclen) && dst[srclen] == '/')
		err = -EINVAL;              // a folder into itself
	else if (!strcmp(src, dst))
		err = 0;                    // nothing to do
	else if (strcasecmp(src, dst) && !stat(dst, &to)) {
		// Something else is there. A case-only rename (FAT does not tell
		// "Boot.dol" from "boot.dol") finds the source itself, and goes on.
		if (!(flags & DEVNET_FLAG_REPLACE))
			err = -EEXIST;
		else if (S_ISDIR(to.st_mode))
			err = -EISDIR;
		else if (S_ISDIR(from.st_mode))
			err = -ENOTDIR;
		else if (unlink(dst))
			err = -errno;
	}
	if (!err && strcmp(src, dst)) {
		make_parents(dst);
		if (rename(src, dst))
			err = -errno;
	}
	if (!err && changed) {
		changed(src);
		changed(dst);
	}
	gprintf("devnet: R %s -> %s: %d\n", src, dst, err);
	devfile_reply(s, err, err ? NULL : dst, err ? 0 : strlen(dst));
}

static void file_request(s32 s, const u8 *hdr, devfile_change_fn changed) {
	char path[DEVFILE_PATH_MAX];
	char part[DEVFILE_PATH_MAX + 8];
	u8 op = hdr[4];
	u16 path_len = get_u16(hdr + 6);
	u32 size = get_u32(hdr + 8);
	bool trailing_slash;
	struct stat st;
	s32 err;

	if (!path_len || path_len >= sizeof(path) ||
			!tcp_read(s, (u8 *) path, path_len, NULL, NULL)) {
		devfile_reply(s, -EINVAL, NULL, 0);
		return;
	}
	path[path_len] = 0;
	trailing_slash = path[path_len - 1] == '/';

	// Strip one trailing slash so "sd:/apps/" lists like "sd:/apps".
	if (path_len > 4 && path[path_len - 1] == '/' && path[path_len - 2] != ':')
		path[path_len - 1] = 0;

	if (valid_path(path) && devfile_mount_hook)
		devfile_mount_hook(path);
	if (!valid_path(path)) {
		devfile_reply(s, -EINVAL, NULL, 0);
		return;
	}

	gprintf("devnet: %c %s (%u)\n", op, path, size);
	if (strchr("pPgG", op)) {
		const char *base = strrchr(path, '/');
		const char *name = base ? base + 1 : path;
		size_t len = strlen(name);

		if (len >= sizeof(moving))   // a long name is cut short
			len = sizeof(moving) - 1;
		memcpy(moving, name, len);
		moving[len] = 0;
		devfile_last.st.size = op == 'p' || op == 'P' ? size : 0;
	}

	switch (op) {
	case 'P':
	case 'p':
		// An oversized upload cannot be drained sensibly; the reply closes it.
		if (size > DEVFILE_PUT_MAX) {
			devfile_reply(s, -EFBIG, NULL, 0);
			break;
		}
		// "sd:/apps/new/" names a directory, not a file to create.
		if (trailing_slash) {
			devfile_reply(s, -EISDIR, NULL, 0);
			break;
		}
		// Over a read-only file: refused before any data, as a normal FS does.
		err = !stat(path, &st) && (path_attr(path, &st) & FA_RO) ? -EACCES : room_for(path, size);
		if (err) {
			devfile_reply(s, err, NULL, 0);
			break;
		}
		make_parents(path);
		snprintf(part, sizeof(part), "%s.part", path);
		err = op == 'p' ? devstream_put(s, path, part, size, &devfile_last.st)
						: file_put_raw(s, path, part, size);
		if (!err && changed)
			changed(path);
		devfile_reply(s, err, NULL, 0);
		break;
	case 'C':
		file_checksum(s, path);
		break;
	case 'G':
		file_get_raw(s, path);
		break;
	case 'g':
		devstream_get(s, path, hdr[5] & DEVNET_FLAG_COMPRESS, &devfile_last.st);
		break;
	case 'L':
		file_list(s, path);
		break;
	case 'S':
		file_stat(s, path);
		break;
	case 'D':
		// FatFs answers a folder that is not empty and a read-only entry
		// alike (FR_DENIED, EACCES): say which, as a normal FS does.
		if (stat(path, &st))
			err = -ENOENT;
		else if (S_ISDIR(st.st_mode) && !dir_empty(path))
			err = -ENOTEMPTY;
		else if (path_attr(path, &st) & FA_RO)
			err = -EACCES;
		else if (S_ISDIR(st.st_mode))
			err = rmdir(path) ? -errno : 0;
		else
			err = unlink(path) ? -errno : 0;
		if (!err && changed)
			changed(path);
		devfile_reply(s, err, NULL, 0);
		break;
	case 'M':
		make_parents(path);
		err = mkdir(path, 0777) && errno != EEXIST ? -errno : 0;
		devfile_reply(s, err, NULL, 0);
		break;
	case 'R':
		file_move(s, path, size, hdr[5], changed);
		break;
	case 'A':
		file_attr(s, path, size);
		break;
	default:
		devfile_reply(s, -ENOSYS, NULL, 0);
		break;
	}
}

void devfile_handle(s32 s, const u8 *hdr, s32 prio, s32 restore_prio,
					devfile_change_fn changed) {
	u64 start = gettime();

	memset(&devfile_last, 0, sizeof(devfile_last));
	devfile_last.op = hdr[4];
	devfile_last.bytes = get_u32(hdr + 8);

	chunk = memalign(32, DEVFILE_CHUNK);
	if (!chunk) {
		devfile_reply(s, -ENOMEM, NULL, 0);
		return;
	}

	abort_requested = false;
	active_socket = s;
	request_prio = prio;
	busy = true;
	LWP_SetThreadPriority(LWP_GetSelf(), prio);
	file_request(s, hdr, changed);
	LWP_SetThreadPriority(LWP_GetSelf(), restore_prio);
	active_socket = -1;
	busy = false;

	free(chunk);
	chunk = NULL;
	devfile_last.total = diff_ticks(start, gettime());
}

char devfile_progress(const char **name, u32 *done, u32 *total) {
	char op = devfile_last.op;

	if (!busy || !op || !strchr("pPgG", op))
		return 0;
	*name = moving;
	*done = devfile_last.st.raw;
	*total = devfile_last.st.size;
	return op == 'p' || op == 'P' ? 'p' : 'g';
}

bool devfile_aborted(void) {
	return abort_requested;
}

s32 devfile_prio(void) {
	return request_prio;
}

void devfile_abort(void) {
	s32 sock = active_socket;
	int i;

	if (!busy)
		return;

	gprintf("devnet: aborting the active transfer\n");
	abort_requested = true;
	// Unblock a read or write that is waiting on the PC.
	if (sock >= 0)
		net_shutdown(sock, 2);

	for (i = 0; i < 150 && busy; ++i)
		usleep(20 * 1000);
	if (busy)
		gprintf("devnet: transfer did not stop\n");
}
