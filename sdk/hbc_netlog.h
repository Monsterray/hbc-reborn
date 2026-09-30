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
 * connection closes at exit(); call hbc_netlog_close() before leaving any
 * other way. hbc_netlog_init() gives up within about 5 s when the network
 * or the PC is unavailable, and waits for a network start-up another thread
 * began (such as hbc_agent.h's). The header needs nothing beyond libogc.
 *
 * Define HBC_NETLOG_LAYOUT_ONLY to get only the shared block layout.
 *
 * This file is in the public domain; copy it into your project.
 */

#ifndef HBC_NETLOG_H
#define HBC_NETLOG_H

#include <gctypes.h>

/* The block sits just past the reload stub's return-title words, where the
 * next app HBC starts finds it. IOS clears low memory when it boots a title,
 * so HBC also keeps a copy in MEM2 to restore after an app returns to the
 * installed channel; an app that uses that memory only costs the copy. */
#define HBC_NETLOG_ADDR 0x80002f20
#define HBC_NETLOG_KEEP_ADDR 0x91800000
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iosupport.h>
#include <network.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>

/* How long hbc_netlog_init waits for the network and for the PC, and for a
 * network start-up another thread began (hbc_agent.h starts one). */
#define HBC_NETLOG_INIT_MS 3000
#define HBC_NETLOG_BUSY_MS 10000
#define HBC_NETLOG_CONNECT_MS 2000
/* IOS poll event for "writable" (libogc does not export it). */
#define HBC_NETLOG_POLLOUT 0x0008

static s32 hbc_netlog_socket = -1;
static mutex_t hbc_netlog_lock = LWP_MUTEX_NULL;
static const devoptab_t *hbc_netlog_prev_out;
static const devoptab_t *hbc_netlog_prev_err;

/* Called with the lock held. */
static inline void hbc_netlog_send(const char *ptr, size_t len) {
	while (hbc_netlog_socket >= 0 && len > 0) {
		s32 res = net_write(hbc_netlog_socket, ptr, len > 1024 ? 1024 : len);
		if (res == -EAGAIN) {
			struct pollsd sd = { hbc_netlog_socket, HBC_NETLOG_POLLOUT, 0 };
			net_poll(&sd, 1, 100);
			continue;
		}
		if (res <= 0) {
			net_close(hbc_netlog_socket);
			hbc_netlog_socket = -1;
			return;
		}
		ptr += res;
		len -= res;
	}
}

/* Set by the agent's "Log to PC" switch (hbc_agent.h); absent without it. */
extern volatile int hbc_agent_log_muted __attribute__((weak));

static inline ssize_t hbc_netlog_write(const devoptab_t *prev, struct _reent *r,
									   void *fd, const char *ptr, size_t len) {
	if (prev && prev->write_r)
		prev->write_r(r, fd, ptr, len);
	if (&hbc_agent_log_muted && hbc_agent_log_muted)
		return len;
	LWP_MutexLock(hbc_netlog_lock);
	hbc_netlog_send(ptr, len);
	LWP_MutexUnlock(hbc_netlog_lock);
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

/* Flush and close the log so the PC sees its end. Safe to call twice. */
static inline void hbc_netlog_close(void) {
	if (hbc_netlog_socket < 0)
		return;
	fflush(stdout);
	fflush(stderr);
	LWP_MutexLock(hbc_netlog_lock);
	devoptab_list[STD_OUT] = hbc_netlog_prev_out;
	devoptab_list[STD_ERR] = hbc_netlog_prev_err;
	if (hbc_netlog_socket >= 0) {
		net_shutdown(hbc_netlog_socket, 2);
		net_close(hbc_netlog_socket);
		hbc_netlog_socket = -1;
	}
	LWP_MutexUnlock(hbc_netlog_lock);
}

/* Connect without blocking past HBC_NETLOG_CONNECT_MS, even when a firewall
 * silently drops the connection attempt. */
static inline s32 hbc_netlog_connect(s32 s, struct sockaddr_in *sa) {
	s64 start = gettime();
	s32 flags = net_fcntl(s, F_GETFL, 0);
	s32 res;

	if (flags >= 0)
		net_fcntl(s, F_SETFL, flags | 4);

	for (;;) {
		res = net_connect(s, (struct sockaddr *) sa, sizeof(*sa));
		if (res == 0 || res == -EISCONN) {
			res = 0;
			break;
		}
		if (res != -EINPROGRESS && res != -EALREADY)
			break;
		if (ticks_to_millisecs(diff_ticks(start, gettime())) > HBC_NETLOG_CONNECT_MS) {
			res = -ETIMEDOUT;
			break;
		}
		{
			struct pollsd sd = { s, HBC_NETLOG_POLLOUT, 0 };
			net_poll(&sd, 1, 100);
		}
	}

	/* Blocking sends: IOS can misreport a non-blocking send to a full buffer. */
	if (flags >= 0)
		net_fcntl(s, F_SETFL, flags & ~4);
	return res;
}

/* Returns 0 when connected, or a negative error code. */
static inline s32 hbc_netlog_init(void) {
	hbc_netlog_block block;
	struct sockaddr_in sa;
	s64 start;
	s32 res;

	if (hbc_netlog_socket >= 0)
		return 0;

	DCInvalidateRange((void *) HBC_NETLOG_ADDR, sizeof(block));
	memcpy(&block, (const void *) HBC_NETLOG_ADDR, sizeof(block));
	if (block.magic != HBC_NETLOG_MAGIC || block.version != HBC_NETLOG_VERSION ||
			block.check != hbc_netlog_check(&block) || !block.port)
		return -ENOENT;

	/* libogc's net_init() never returns if it runs while another thread's
	 * start-up is in progress, so wait for that one instead. */
	start = gettime();
	while ((res = net_get_status()) == -EBUSY &&
		   ticks_to_millisecs(diff_ticks(start, gettime())) < HBC_NETLOG_BUSY_MS)
		usleep(20 * 1000);
	if (res == -EBUSY)
		return -ETIMEDOUT;
	if (res < 0) {
		start = gettime();
		do {
			res = net_init();
		} while (res == -EAGAIN &&
				 ticks_to_millisecs(diff_ticks(start, gettime())) < HBC_NETLOG_INIT_MS);
		if (res < 0)
			return res;
	}

	if (hbc_netlog_lock == LWP_MUTEX_NULL && LWP_MutexInit(&hbc_netlog_lock, false) < 0)
		return -ENOMEM;

	res = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (res < 0)
		return res;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_len = sizeof(sa);
	sa.sin_port = block.port;
	sa.sin_addr.s_addr = block.ip;
	if (hbc_netlog_connect(res, &sa) < 0) {
		net_close(res);
		return -ETIMEDOUT;
	}
	hbc_netlog_socket = res;

	hbc_netlog_prev_out = devoptab_list[STD_OUT];
	hbc_netlog_prev_err = devoptab_list[STD_ERR];
	devoptab_list[STD_OUT] = &hbc_netlog_dotab_out;
	devoptab_list[STD_ERR] = &hbc_netlog_dotab_err;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	/* The reload stub drops the socket without a FIN, so close it first. */
	atexit(hbc_netlog_close);
	return 0;
}

#endif /* HBC_NETLOG_LAYOUT_ONLY */

#endif /* HBC_NETLOG_H */
