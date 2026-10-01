// Network block-size experiment: is the Wii's TCP throughput bound by the
// size of each IOS socket call? libogc's net_send/net_recv copy every block
// through its 64 KiB network heap, which caps a block near 16 KiB. This app
// also calls IOS's /dev/net/ip/top directly with its own aligned buffer, at
// up to 128 KiB a call, and times both both ways.
//
// usage (argv from Wiiload): netblock.dol PC-IP PORT
// tests/netblock_run.py listens on PC-IP:PORT and prints the results.

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <network.h>
#include <ogc/ipc.h>
#include <ogc/lwp_watchdog.h>

#define TOTAL (4 * 1024 * 1024)
#define BUF_MAX (128 * 1024)

#define IOCTLV_SO_RECVFROM 12
#define IOCTLV_SO_SENDTO 13

extern void __exception_setreload(int t);

static s32 ip_top = -1;
static u8 *buf;

struct sendto_params {
	u32 socket;
	u32 flags;
	u32 has_destaddr;
	u8 destaddr[28];
};

static s32 ios_send(s32 s, const void *data, u32 len) {
	static struct sendto_params p ATTRIBUTE_ALIGN(32);
	static ioctlv v[2] ATTRIBUTE_ALIGN(32);

	memset(&p, 0, sizeof(p));
	p.socket = s;
	v[0].data = (void *) data;
	v[0].len = len;
	v[1].data = &p;
	v[1].len = sizeof(p);
	DCFlushRange((void *) data, len);
	DCFlushRange(&p, sizeof(p));
	return IOS_Ioctlv(ip_top, IOCTLV_SO_SENDTO, 2, 0, v);
}

static s32 ios_recv(s32 s, void *data, u32 len) {
	static u32 params[8] ATTRIBUTE_ALIGN(32);
	static ioctlv v[3] ATTRIBUTE_ALIGN(32);
	s32 r;

	params[0] = s;
	params[1] = 0;
	v[0].data = params;
	v[0].len = 8;
	v[1].data = data;
	v[1].len = len;
	v[2].data = NULL;
	v[2].len = 0;
	DCFlushRange(params, 32);
	DCInvalidateRange(data, len);
	r = IOS_Ioctlv(ip_top, IOCTLV_SO_RECVFROM, 1, 2, v);
	if (r > 0)
		DCInvalidateRange(data, r);
	return r;
}

static int send_all(s32 s, const void *p, u32 n, u32 bs, int direct) {
	const u8 *c = p;

	while (n) {
		u32 k = n < bs ? n : bs;
		s32 r = direct ? ios_send(s, c, k) : net_send(s, c, k, 0);
		if (r <= 0)
			return r ? r : -1;
		c += r;
		n -= r;
	}
	return 0;
}

static void line(s32 s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(s32 s, const char *fmt, ...) {
	char t[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(t, sizeof(t), fmt, ap);
	va_end(ap);
	printf("%s", t);
	send_all(s, t, strlen(t), sizeof(t), 0);
}

static int wait_byte(s32 s) {
	char c;
	return net_recv(s, &c, 1, 0) == 1 ? 0 : -1;
}

// One direction, one way of calling IOS, one block size: ms, or -1.
static s32 run_send(s32 s, u32 bs, int direct) {
	u64 t;

	line(s, "SEND %u\n", TOTAL);
	t = gettime();
	for (u32 done = 0; done < TOTAL; done += BUF_MAX)
		if (send_all(s, buf, BUF_MAX, bs, direct))
			return -1;
	if (wait_byte(s))
		return -1;
	return ticks_to_millisecs(diff_ticks(t, gettime()));
}

static s32 run_recv(s32 s, u32 bs, int direct) {
	u64 t;
	u32 got = 0;

	line(s, "RECV %u\n", TOTAL);
	t = gettime();
	while (got < TOTAL) {
		u32 k = TOTAL - got < bs ? TOTAL - got : bs;
		s32 r = direct ? ios_recv(s, buf, k) : net_recv(s, buf, k, 0);
		if (r <= 0)
			return -1;
		got += r;
	}
	return ticks_to_millisecs(diff_ticks(t, gettime()));
}

int main(int argc, char **argv) {
	static const struct {
		const char *how;
		u32 bs;
		int direct;
	} tests[] = {
		{ "libogc", 4 * 1024, 0 }, { "libogc", 16 * 1024, 0 },
		{ "ios", 16 * 1024, 1 }, { "ios", 32 * 1024, 1 },
		{ "ios", 64 * 1024, 1 }, { "ios", 128 * 1024, 1 },
	};
	struct sockaddr_in sa;
	void *xfb;
	GXRModeObj *rmode;
	s32 s, res;
	u32 i, round;

	__exception_setreload(5);
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	if (argc < 3) {
		printf("netblock: needs PC-IP PORT\n");
		sleep(5);
		return 1;
	}
	buf = memalign(32, BUF_MAX);
	for (i = 0; i < BUF_MAX; ++i)
		buf[i] = (u8) (i * 2654435761u >> 13);

	res = net_init();
	printf("net_init %d\n", (int) res);
	ip_top = IOS_Open("/dev/net/ip/top", 0);
	printf("ip_top %d\n", (int) ip_top);
	s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_len = sizeof(sa);
	sa.sin_port = htons(atoi(argv[2]));
	inet_aton(argv[1], &sa.sin_addr);
	if (res < 0 || ip_top < 0 || s < 0 || net_connect(s, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		printf("netblock: no connection\n");
		sleep(5);
		return 1;
	}

	// Two interleaved rounds: single runs vary with Wi-Fi conditions.
	for (round = 1; round <= 2; ++round)
		for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
			s32 up = run_send(s, tests[i].bs, tests[i].direct);
			s32 down = run_recv(s, tests[i].bs, tests[i].direct);
			line(s, "RESULT %u %s %u wii_send_ms %d wii_recv_ms %d\n", round, tests[i].how,
				 tests[i].bs / 1024, (int) up, (int) down);
		}
	line(s, "DONE\n");
	net_close(s);
	IOS_Close(ip_top);
	return 0;
}
