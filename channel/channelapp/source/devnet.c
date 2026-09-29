// Developer network requests on the Wiiload port: status, SD/USB files and
// the app log target. The loader thread calls devnet_handle() for each
// accepted local connection; see docs/devnet.md for the wire format.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>
#include <network.h>

#include "../config.h"
#include "appentry.h"
#include "devnet.h"
#include "devstream.h"
#include "tcp.h"

#define HBC_NETLOG_LAYOUT_ONLY
#include "../../../sdk/hbc_netlog.h"

#define DEVNET_PATH_MAX 256
#define DEVNET_PUT_MAX (512 * 1024 * 1024)
#define DEVNET_LIST_MAX (256 * 1024)
#define DEVNET_CHUNK (32 * 1024)

static const char *device_names[DEVICE_COUNT] = { "sd", "usb", "carda", "cardb" };
static u8 chunk[DEVNET_CHUNK] ATTRIBUTE_ALIGN(32);
static u32 log_ip;
static u16 log_port;

// Timing of the last file transfer, reported in the status reply.
static struct {
	char op;
	u32 bytes;
	u64 total;
	devstream_stats st;
} last;

#define MS(t) ((u32) ticks_to_millisecs (t))

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
bool devnet_send_all(s32 s, const void *data, u32 len) {
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
bool devnet_reply(s32 s, s32 status, const void *data, u32 len) {
	u8 hdr[8];

	put_u32(hdr, status);
	put_u32(hdr + 4, len);
	if (!devnet_send_all(s, hdr, sizeof(hdr)))
		return false;
	return !len || devnet_send_all(s, data, len);
}

static s32 status_json(char *buf, size_t size) {
	bool mounted[DEVICE_COUNT] = { false };
	int active = app_entry_get_status(mounted);
	u32 ip = net_gethostip();
	int i, n;

	n = snprintf(buf, size,
			"{\"version\":\"%s\",\"proto\":%d,\"ios\":%d,\"ios_revision\":%d,"
			"\"ahbprot\":%s,\"mem1_free\":%u,\"mem2_free\":%u,"
			"\"ip\":\"%u.%u.%u.%u\",\"apps\":%u,\"device\":",
			CHANNEL_VERSION_STR, DEVNET_PROTO, IOS_GetVersion(), IOS_GetRevision(),
			read32(0x0d800064) == 0xffffffff ? "true" : "false",
			SYS_GetArena1Size(), SYS_GetArena2Size(),
			ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff,
			entry_count);
	if (active >= 0 && active < DEVICE_COUNT)
		n += snprintf(buf + n, size - n, "\"%s\"", device_names[active]);
	else
		n += snprintf(buf + n, size - n, "null");

	// Only the active device is mounted; the others are last seen inserted.
	n += snprintf(buf + n, size - n, ",\"inserted\":[");
	for (i = 0; i < DEVICE_COUNT; ++i)
		if (mounted[i] || i == active)
			n += snprintf(buf + n, size - n, "%s\"%s\"",
					buf[n - 1] == '[' ? "" : ",", device_names[i]);

	if (log_port)
		n += snprintf(buf + n, size - n, "],\"log\":\"%u.%u.%u.%u:%u\"",
				log_ip >> 24, (log_ip >> 16) & 0xff, (log_ip >> 8) & 0xff,
				log_ip & 0xff, log_port);
	else
		n += snprintf(buf + n, size - n, "],\"log\":null");

	if (last.op)
		n += snprintf(buf + n, size - n, ",\"last\":{\"op\":\"%c\",\"bytes\":%u,"
				"\"wire\":%u,\"ms\":%u,\"net_ms\":%u,\"disk_ms\":%u,\"cpu_ms\":%u}",
				last.op, last.bytes, last.st.wire, MS(last.total),
				MS(last.st.net), MS(last.st.disk), MS(last.st.cpu));
	n += snprintf(buf + n, size - n, "}");

	return n;
}

// Accept only "<device>:/<path>" on a known device, without "..", "//",
// backslashes, or control characters.
static bool valid_path(const char *path) {
	const char *p = strchr(path, ':');
	size_t len;
	int i;

	if (!p || p[1] != '/')
		return false;

	len = p - path;
	for (i = 0; i < DEVICE_COUNT; ++i)
		if (strlen(device_names[i]) == len && !strncmp(path, device_names[i], len))
			break;
	if (i == DEVICE_COUNT)
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
	FILE *f;
	u32 left = size;
	s32 err = 0;

	f = fopen(part, "wb");
	if (!f)
		err = -errno;

	// Read the whole upload even after an error, so the reply stays in sync.
	while (left) {
		u32 block = left > DEVNET_CHUNK ? DEVNET_CHUNK : left;
		u64 t0 = gettime();

		if (!tcp_read(s, chunk, block, NULL, NULL)) {
			err = -EIO;
			break;
		}
		u64 t1 = gettime();
		if (f && !err && fwrite(chunk, 1, block, f) != block)
			err = errno ? -errno : -ENOSPC;
		last.st.net += diff_ticks(t0, t1);
		last.st.disk += diff_ticks(t1, gettime());
		last.st.wire += block;
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
	struct stat st;
	u8 hdr[8];
	FILE *f;
	u32 left;

	if (stat(path, &st) || !S_ISREG(st.st_mode)) {
		devnet_reply(s, -ENOENT, NULL, 0);
		return;
	}

	f = fopen(path, "rb");
	if (!f) {
		devnet_reply(s, -errno, NULL, 0);
		return;
	}

	left = st.st_size;
	put_u32(hdr, 0);
	put_u32(hdr + 4, left);
	if (devnet_send_all(s, hdr, sizeof(hdr))) {
		while (left) {
			u32 block = left > DEVNET_CHUNK ? DEVNET_CHUNK : left;
			u64 t0 = gettime();

			// A short read means the file changed; pad so the length holds.
			size_t got = fread(chunk, 1, block, f);
			if (got < block)
				memset(chunk + got, 0, block - got);
			u64 t1 = gettime();
			bool ok = devnet_send_all(s, chunk, block);
			last.st.disk += diff_ticks(t0, t1);
			last.st.net += diff_ticks(t1, gettime());
			last.st.wire += block;
			if (!ok)
				break;
			left -= block;
		}
	}
	fclose(f);
}

static void file_list(s32 s, const char *path) {
	char full[DEVNET_PATH_MAX * 2];
	struct dirent *de;
	struct stat st;
	char *buf;
	size_t n = 0;
	DIR *d;

	d = opendir(path);
	if (!d) {
		devnet_reply(s, -errno, NULL, 0);
		return;
	}

	buf = malloc(DEVNET_LIST_MAX);
	if (!buf) {
		closedir(d);
		devnet_reply(s, -ENOMEM, NULL, 0);
		return;
	}

	// One entry per line: "d <name>" or "f <size> <name>".
	while ((de = readdir(d)) && n < DEVNET_LIST_MAX - DEVNET_PATH_MAX - 16) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
		if (stat(full, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			n += snprintf(buf + n, DEVNET_LIST_MAX - n, "d %s\n", de->d_name);
		else
			n += snprintf(buf + n, DEVNET_LIST_MAX - n, "f %lld %s\n",
					(long long) st.st_size, de->d_name);
	}
	closedir(d);

	devnet_reply(s, 0, buf, n);
	free(buf);
}

static void file_request(s32 s, const u8 *hdr) {
	char path[DEVNET_PATH_MAX];
	char part[DEVNET_PATH_MAX + 8];
	u8 op = hdr[4];
	u16 path_len = get_u16(hdr + 6);
	u32 size = get_u32(hdr + 8);
	struct stat st;
	s32 err;

	if (!path_len || path_len >= sizeof(path) ||
			!tcp_read(s, (u8 *) path, path_len, NULL, NULL)) {
		devnet_reply(s, -EINVAL, NULL, 0);
		return;
	}
	path[path_len] = 0;

	// Strip one trailing slash so "sd:/apps/" lists like "sd:/apps".
	if (path_len > 4 && path[path_len - 1] == '/' && path[path_len - 2] != ':')
		path[path_len - 1] = 0;

	if (!valid_path(path)) {
		devnet_reply(s, -EINVAL, NULL, 0);
		return;
	}

	gprintf("devnet: %c %s (%u)\n", op, path, size);

	switch (op) {
	case 'P':
	case 'p':
		// An oversized upload cannot be drained sensibly; the reply closes it.
		if (size > DEVNET_PUT_MAX) {
			devnet_reply(s, -EFBIG, NULL, 0);
			break;
		}
		make_parents(path);
		snprintf(part, sizeof(part), "%s.part", path);
		err = op == 'p' ? devstream_put(s, path, part, size, &last.st)
						: file_put_raw(s, path, part, size);
		devnet_reply(s, err, NULL, 0);
		break;
	case 'G':
		file_get_raw(s, path);
		break;
	case 'g':
		devstream_get(s, path, hdr[5] & DEVNET_FLAG_COMPRESS, &last.st);
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
		devnet_reply(s, err, NULL, 0);
		break;
	case 'M':
		make_parents(path);
		err = mkdir(path, 0777) && errno != EEXIST ? -errno : 0;
		devnet_reply(s, err, NULL, 0);
		break;
	default:
		devnet_reply(s, -ENOSYS, NULL, 0);
		break;
	}
}

static void set_log_target(u32 ip, u16 port) {
	hbc_netlog_block *block = (hbc_netlog_block *) HBC_NETLOG_ADDR;

	log_ip = ip;
	log_port = port;

	memset(block, 0, sizeof(*block));
	if (port) {
		block->magic = HBC_NETLOG_MAGIC;
		block->version = HBC_NETLOG_VERSION;
		block->ip = ip;
		block->port = port;
		block->check = hbc_netlog_check(block);
	}
	DCFlushRange(block, sizeof(*block));
}

bool devnet_handle(s32 s, const u8 *hdr, u32 client_ip) {
	char json[640];

	if (!memcmp(hdr, "HBCS", 4)) {
		s32 n = status_json(json, sizeof(json));
		devnet_reply(s, 0, json, n);
		return true;
	}

	if (!memcmp(hdr, "HBCF", 4)) {
		// The UI thread outranks the loader; run transfers above it. The
		// thread mostly waits on IOS, so the menu keeps drawing meanwhile.
		u64 start = gettime();

		memset(&last, 0, sizeof(last));
		last.op = hdr[4];
		last.bytes = get_u32(hdr + 8);
		LWP_SetThreadPriority(LWP_GetSelf(), DEVNET_THREAD_PRIO);
		file_request(s, hdr);
		LWP_SetThreadPriority(LWP_GetSelf(), LD_THREAD_PRIO);
		last.total = diff_ticks(start, gettime());
		return true;
	}

	if (!memcmp(hdr, "HBCN", 4)) {
		u16 port = get_u16(hdr + 4);

		set_log_target(client_ip, port);
		devnet_reply(s, 0, NULL, 0);
		return true;
	}

	return false;
}
