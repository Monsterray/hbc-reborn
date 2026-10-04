// File requests of the developer protocol (HBCF, docs/devnet.md), shared by
// HBC's loader thread and the in-app agent (sdk/hbc_agent). Framed transfers
// run in devstream.c; this file validates paths and runs the other ops.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

// Create each parent directory of path.
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
			if (!ok)
				break;
			left -= block;
		}
	}
	fclose(f);
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

	// One entry per line: "d <name>" or "f <size> <name>". A listing that
	// reaches the buffer limit ends with "! truncated".
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (n >= DEVFILE_LIST_MAX - DEVFILE_PATH_MAX - 32) {
			n += snprintf(buf + n, DEVFILE_LIST_MAX - n, "! truncated\n");
			break;
		}
		// A name too long for the path buffer cannot be stat'ed: skip it.
		if (snprintf(full, sizeof(full), "%s/%s", path, de->d_name) >= (int) sizeof(full) ||
				stat(full, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			n += snprintf(buf + n, DEVFILE_LIST_MAX - n, "d %s\n", de->d_name);
		else
			n += snprintf(buf + n, DEVFILE_LIST_MAX - n, "f %lld %s\n",
					(long long) st.st_size, de->d_name);
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
	case 'D':
		if (stat(path, &st))
			err = -ENOENT;
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
