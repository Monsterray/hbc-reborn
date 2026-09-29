/*
 * hbc_netlog.h - stream an app's stdout and stderr to a PC over the network.
 *
 * Run `python3 tools/hbc.py log` (or `tools/hbc.py run app.dol`) on the PC.
 * It registers itself with the Homebrew Channel, which leaves the PC's address
 * in a small low-memory block for the next app it launches. In the app:
 *
 *     #include "hbc_netlog.h"
 *
 *     int main(void) {
 *         // ... video and console setup, if any ...
 *         hbc_netlog_init();   // returns 0 when connected
 *         printf("hello from the Wii\n");
 *     }
 *
 * Output still reaches any console installed before hbc_netlog_init(). The
 * header has no dependencies beyond libogc and needs no extra library.
 *
 * Define HBC_NETLOG_LAYOUT_ONLY to get only the shared block layout.
 *
 * This file is in the public domain; copy it into your project.
 */

#ifndef HBC_NETLOG_H
#define HBC_NETLOG_H

#include <gctypes.h>

/* The block sits just past the reload stub's return-title words. */
#define HBC_NETLOG_ADDR 0x80002f20
#define HBC_NETLOG_MAGIC 0x4842434e /* 'HBCN' */
#define HBC_NETLOG_VERSION 1
#define HBC_NETLOG_DEFAULT_PORT 4405

typedef struct {
	u32 magic;
	u32 version;
	u32 ip;       /* PC address, network (big-endian) byte order */
	u16 port;
	u16 flags;    /* reserved, 0 */
	u32 check;    /* magic ^ version ^ ip ^ port */
	u32 reserved[3];
} hbc_netlog_block;

static inline u32 hbc_netlog_check(const hbc_netlog_block *b) {
	return b->magic ^ b->version ^ b->ip ^ b->port;
}

#ifndef HBC_NETLOG_LAYOUT_ONLY

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <network.h>
#include <ogc/cache.h>

static s32 hbc_netlog_socket = -1;
static const devoptab_t *hbc_netlog_prev_out;
static const devoptab_t *hbc_netlog_prev_err;

static inline void hbc_netlog_send(const char *ptr, size_t len) {
	while (hbc_netlog_socket >= 0 && len > 0) {
		s32 res = net_write(hbc_netlog_socket, ptr, len > 1024 ? 1024 : len);
		if (res == -EAGAIN)
			continue;
		if (res <= 0) {
			net_close(hbc_netlog_socket);
			hbc_netlog_socket = -1;
			return;
		}
		ptr += res;
		len -= res;
	}
}

static inline ssize_t hbc_netlog_write(const devoptab_t *prev, struct _reent *r,
									   void *fd, const char *ptr, size_t len) {
	if (prev && prev->write_r)
		prev->write_r(r, fd, ptr, len);
	hbc_netlog_send(ptr, len);
	return len;
}

static ssize_t hbc_netlog_write_out(struct _reent *r, void *fd, const char *ptr,
									size_t len) {
	return hbc_netlog_write(hbc_netlog_prev_out, r, fd, ptr, len);
}

static ssize_t hbc_netlog_write_err(struct _reent *r, void *fd, const char *ptr,
									size_t len) {
	return hbc_netlog_write(hbc_netlog_prev_err, r, fd, ptr, len);
}

static devoptab_t hbc_netlog_dotab_out = { .name = "hbclog",
										   .write_r = hbc_netlog_write_out };
static devoptab_t hbc_netlog_dotab_err = { .name = "hbclog",
										   .write_r = hbc_netlog_write_err };

/* Returns 0 when connected, or a negative error code. */
static inline s32 hbc_netlog_init(void) {
	hbc_netlog_block block;
	struct sockaddr_in sa;
	s32 res;

	if (hbc_netlog_socket >= 0)
		return 0;

	DCInvalidateRange((void *) HBC_NETLOG_ADDR, sizeof(block));
	memcpy(&block, (const void *) HBC_NETLOG_ADDR, sizeof(block));
	if (block.magic != HBC_NETLOG_MAGIC || block.version != HBC_NETLOG_VERSION ||
			block.check != hbc_netlog_check(&block))
		return -ENOENT;

	do {
		res = net_init();
	} while (res == -EAGAIN);
	if (res < 0)
		return res;

	res = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (res < 0)
		return res;
	hbc_netlog_socket = res;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_len = sizeof(sa);
	sa.sin_port = block.port;
	sa.sin_addr.s_addr = block.ip;
	res = net_connect(hbc_netlog_socket, (struct sockaddr *) &sa, sizeof(sa));
	if (res < 0) {
		net_close(hbc_netlog_socket);
		hbc_netlog_socket = -1;
		return res;
	}

	hbc_netlog_prev_out = devoptab_list[STD_OUT];
	hbc_netlog_prev_err = devoptab_list[STD_ERR];
	devoptab_list[STD_OUT] = &hbc_netlog_dotab_out;
	devoptab_list[STD_ERR] = &hbc_netlog_dotab_err;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	return 0;
}

#endif /* HBC_NETLOG_LAYOUT_ONLY */

#endif /* HBC_NETLOG_H */
