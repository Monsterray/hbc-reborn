// The in-app agent: HBC's developer protocol (docs/devnet.md) inside a
// running app, plus a crash recorder. See ../hbc_agent.h for the API.
//
// One thread, below the app's, accepts connections on the Wiiload port and
// answers HBCV, HBCS, HBCF (the same file requests as HBC, from
// channel/channelapp/source/devfile.c), HBCN and HBCX (exit to HBC). A
// Wiiload upload (HAXX) cannot run here; the agent answers it by exiting to
// HBC, so the client's next attempt reaches HBC itself. `hbc.py run` does
// that sequence for you.

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iosupport.h>

#include <ogcsys.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <network.h>
#include <tuxedo/ppc/exception.h>

#include "../../channel/channelapp/config.h"
#include "devfile.h"
#include "devstream.h"
#include "tcp.h"

#define HBC_NETLOG_LAYOUT_ONLY
#include "../hbc_netlog.h"
#include "../hbc_agent.h"

#define AGENT_STACK (12 * 1024)
#define AGENT_PROTO 3
#define AGENT_DEFAULT_PRIO 40
// Transfers run a little above the agent's idle priority, still below the
// app's main thread.
#define AGENT_TRANSFER_BOOST 8
#define AGENT_ACCEPT_POLL_MS 200
#define AGENT_RETRY_MS 1000

_Static_assert(HBC_CRASH_ADDR >= HBC_NETLOG_KEEP_ADDR + sizeof(hbc_netlog_block),
			   "the crash block must not overlap the kept log target");

// libogc: seconds its crash screen shows before returning to the loader.
extern void __exception_setreload(int t);

static const char *device_names[] = { "sd", "usb", "carda", "cardb" };
#define DEVICES (sizeof(device_names) / sizeof(device_names[0]))

static hbc_agent_config cfg;
static lwp_t thread = LWP_THREAD_NULL;
static u8 *stack;
static u64 start_ticks;
static volatile bool exit_requested;
static PPCExcptPanicFn prev_panic;

static u16 get_u16(const u8 *p) {
	return (p[0] << 8) | p[1];
}

static u32 uptime_ms(void) {
	return ticks_to_millisecs(diff_ticks(start_ticks, gettime()));
}

// Copy src into dst (size bytes) with anything JSON would need to quote
// replaced by '?'.
static void json_safe(char *dst, const char *src, size_t size) {
	size_t i;

	for (i = 0; i + 1 < size && src && src[i]; ++i)
		dst[i] = src[i] >= 0x20 && src[i] < 0x7f && src[i] != '"' && src[i] != '\\'
				? src[i] : '?';
	dst[i] = 0;
}

static u32 stack_used(void) {
	u32 i;

	for (i = 0; i < AGENT_STACK && !stack[i]; ++i)
		;
	return AGENT_STACK - i;
}

static s32 status_json(char *buf, size_t size) {
	hbc_netlog_block *block = (hbc_netlog_block *) HBC_NETLOG_ADDR;
	const devfile_transfer *last = &devfile_last;
	struct mallinfo heap = mallinfo();
	char name[48], version[32], dev[8];
	u32 ip = net_gethostip();
	bool first = true;
	s32 n;
	u32 i;

	json_safe(name, cfg.name, sizeof(name));
	json_safe(version, cfg.version, sizeof(version));

	n = snprintf(buf, size,
			"{\"agent\":true,\"version\":\"%s\",\"proto\":%d,\"app\":\"%s\","
			"\"app_version\":\"%s\",\"uptime_ms\":%u,\"ios\":%d,\"ios_revision\":%d,"
			"\"ahbprot\":%s,\"mem1_free\":%u,\"mem2_free\":%u,\"heap_free\":%u,"
			"\"agent_stack_used\":%u,\"agent_stack_size\":%u,"
			"\"ip\":\"%u.%u.%u.%u\",\"exit_requested\":%s,\"inserted\":[",
			CHANNEL_VERSION_STR, AGENT_PROTO, name, version, uptime_ms(),
			IOS_GetVersion(), IOS_GetRevision(),
			read32(0x0d800064) == 0xffffffff ? "true" : "false",
			SYS_GetArena1Size(), SYS_GetArena2Size(), heap.fordblks,
			stack_used(), AGENT_STACK,
			ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff,
			exit_requested ? "true" : "false");

	// The devices the app has mounted; the first is the "device".
	dev[0] = 0;
	for (i = 0; i < DEVICES; ++i) {
		char probe[8];

		snprintf(probe, sizeof(probe), "%s:", device_names[i]);
		if (FindDevice(probe) < 0)
			continue;
		n += snprintf(buf + n, size - n, "%s\"%s\"", first ? "" : ",", device_names[i]);
		if (first)
			strcpy(dev, device_names[i]);
		first = false;
	}
	if (dev[0])
		n += snprintf(buf + n, size - n, "],\"device\":\"%s\"", dev);
	else
		n += snprintf(buf + n, size - n, "],\"device\":null");

	DCInvalidateRange(block, sizeof(*block));
	if (block->magic == HBC_NETLOG_MAGIC && block->check == hbc_netlog_check(block) &&
			block->port)
		n += snprintf(buf + n, size - n, ",\"log\":\"%u.%u.%u.%u:%u\"",
				block->ip >> 24, (block->ip >> 16) & 0xff, (block->ip >> 8) & 0xff,
				block->ip & 0xff, block->port);
	else
		n += snprintf(buf + n, size - n, ",\"log\":null");

	if (last->op)
		n += snprintf(buf + n, size - n, ",\"last\":{\"op\":\"%c\",\"bytes\":%u,"
				"\"wire\":%u,\"ms\":%u,\"net_ms\":%u,\"disk_ms\":%u,\"cpu_ms\":%u}",
				last->op, last->bytes, last->st.wire,
				(u32) ticks_to_millisecs(last->total),
				(u32) ticks_to_millisecs(last->st.net),
				(u32) ticks_to_millisecs(last->st.disk),
				(u32) ticks_to_millisecs(last->st.cpu));
	n += snprintf(buf + n, size - n, ",\"tcp_last_failure\":\"%s\"}", tcp_last_failure());
	return n;
}

static void write_block(hbc_netlog_block *block, u32 ip, u16 port) {
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

// HBCN: the log target for the apps after this one, in the same blocks HBC
// uses: low memory and the MEM2 copy HBC restores after its title launch.
static void set_log_target(u32 ip, u16 port) {
	write_block((hbc_netlog_block *) HBC_NETLOG_ADDR, ip, port);
	write_block((hbc_netlog_block *) HBC_NETLOG_KEEP_ADDR, ip, port);
}

// Leaves the app for HBC, through the reload stub like any other exit().
static void __attribute__((noreturn)) agent_exit(void) {
	exit_requested = true;

	if (cfg.app_polls_exit) {
		u64 t = gettime();

		while (ticks_to_millisecs(diff_ticks(t, gettime())) < cfg.exit_grace_ms)
			usleep(50 * 1000);
	}

	// The app did not exit by itself (or does not poll): do it here, above
	// the app's threads so they cannot run into a half-shut-down system.
	LWP_SetThreadPriority(LWP_GetSelf(), LWP_PRIO_HIGHEST);
	if (cfg.on_exit)
		cfg.on_exit(cfg.user);
	exit(0);
}

static void handle(s32 s, const u8 *hdr, u32 client_ip) {
	char json[1024];

	if (!memcmp(hdr, "HBCV", 4)) {
		static const char version[] = CHANNEL_VERSION_STR " agent";

		tcp_write(s, (const u8 *) version, sizeof(version), NULL, NULL);
	} else if (!memcmp(hdr, "HBCS", 4)) {
		devfile_reply(s, 0, json, status_json(json, sizeof(json)));
	} else if (!memcmp(hdr, "HBCF", 4)) {
		devfile_handle(s, hdr, cfg.priority + AGENT_TRANSFER_BOOST, cfg.priority, NULL);
		devstream_release();
	} else if (!memcmp(hdr, "HBCN", 4)) {
		set_log_target(client_ip, get_u16(hdr + 4));
		devfile_reply(s, 0, NULL, 0);
	} else if (!memcmp(hdr, "HBCX", 4)) {
		devfile_reply(s, 0, NULL, 0);
		tcp_close(s);
		agent_exit();
	} else if (!memcmp(hdr, "HAXX", 4)) {
		// Uploads need HBC's loader. Close without reading, so the client
		// sees the upload fail, and go back to HBC for the retry.
		net_close(s);
		agent_exit();
	} else {
		devfile_reply(s, -ENOSYS, NULL, 0);
	}
	tcp_close(s);
}

// Starts the network unless it is up or starting; 0 once it is up.
static s32 net_start(void) {
	s32 res = net_get_status();

	if (res == -EBUSY || res < 0) {
		if (res != -EBUSY) {
			res = net_init_async(NULL, NULL);
			if (res < 0 && res != -EBUSY)
				return res;
		}
		while ((res = net_get_status()) == -EBUSY)
			usleep(50 * 1000);
	}
	return res < 0 ? res : 0;
}

static void *agent_thread(void *arg) {
	struct sockaddr_in sa;
	u32 len_sa, mask = 0;
	s32 ls = -1, s;
	u8 hdr[16];
	(void) arg;

	while (true) {
		if (ls < 0) {
			if (net_start() < 0) {
				usleep(AGENT_RETRY_MS * 1000);
				continue;
			}
			mask = net_gethostip() & 0xffff0000;
			ls = tcp_listen(LD_TCP_PORT, 3);
			if (ls < 0) {
				if (ls == -ENETRESET)
					net_deinit();
				usleep(AGENT_RETRY_MS * 1000);
				continue;
			}
		}

		// Wait for a connection without spinning. IOS may not report a
		// pending connection as readable, so try accept after each wait.
		{
			struct pollsd sd = { ls, 0x0003, 0 };  // POLLIN

			net_poll(&sd, 1, AGENT_ACCEPT_POLL_MS);
		}

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_len = sizeof(sa);
		len_sa = sizeof(sa);
		s = net_accept(ls, (struct sockaddr *) &sa, &len_sa);
		if (s == -EAGAIN)
			continue;
		if (s < 0) {
			net_close(ls);
			ls = -1;
			if (s == -ENETRESET)
				net_deinit();
			continue;
		}

		// Like HBC, answer only the Wii's own /16.
		if ((sa.sin_addr.s_addr & 0xffff0000) != mask ||
				!tcp_read_timeout(s, hdr, sizeof(hdr), NULL, NULL, LD_HEADER_TIMEOUT)) {
			net_close(s);
			continue;
		}
		handle(s, hdr, sa.sin_addr.s_addr);
	}
	return NULL;
}

// Return addresses up the stack chain: each frame's back chain at 0(sp), and
// the saved LR of the frame it called at 4(back chain).
static bool ram_word(u32 a) {
	return !(a & 3) && ((a >= 0x80000000 && a < 0x81800000) ||
						(a >= 0x90000000 && a < 0x94000000));
}

static void agent_panic(unsigned exid, PPCContext *ctx) {
	hbc_crash_block *b = (hbc_crash_block *) HBC_CRASH_ADDR;
	u32 dar = mfspr(19), dsisr = mfspr(18);
	u32 sp = ctx->gpr[1], i;

	memset(b, 0, sizeof(*b));
	b->magic = HBC_CRASH_MAGIC;
	b->version = HBC_CRASH_VERSION;
	b->exception = exid;
	b->pc = ctx->pc;
	b->msr = ctx->msr;
	b->lr = ctx->lr;
	b->cr = ctx->cr;
	b->ctr = ctx->ctr;
	b->dar = dar;
	b->dsisr = dsisr;
	b->sp = sp;
	b->uptime_ms = uptime_ms();
	for (i = 0; i < HBC_CRASH_FRAMES && ram_word(sp); ++i) {
		u32 next = *(u32 *) sp;

		if (!ram_word(next) || next <= sp)
			break;
		b->frames[i] = *(u32 *) (next + 4);
		sp = next;
	}
	strncpy(b->app, cfg.name, sizeof(b->app) - 1);
	b->check = hbc_crash_check(b);
	DCFlushRange(b, sizeof(*b));

	if (prev_panic)
		prev_panic(exid, ctx);
}

s32 hbc_agent_init(const hbc_agent_config *config) {
	s32 res;

	if (thread != LWP_THREAD_NULL)
		return -EALREADY;

	memset(&cfg, 0, sizeof(cfg));
	if (config)
		cfg = *config;
	if (!cfg.name)
		cfg.name = "app";
	if (!cfg.version)
		cfg.version = "";
	if (cfg.priority <= 0 || cfg.priority > LWP_PRIO_HIGHEST - AGENT_TRANSFER_BOOST)
		cfg.priority = AGENT_DEFAULT_PRIO;
	if (!cfg.crash_reload_s)
		cfg.crash_reload_s = 3;
	if (!cfg.exit_grace_ms)
		cfg.exit_grace_ms = 5000;
	start_ticks = gettime();

	if (!cfg.no_crash_handler) {
		if (cfg.crash_reload_s > 0)
			__exception_setreload(cfg.crash_reload_s);
		prev_panic = PPCExcptCurPanicFn;
		PPCExcptCurPanicFn = agent_panic;
	}

	// Start the network now, so hbc_agent_net_wait() and hbc_netlog_init()
	// see a start-up in progress rather than none.
	if (net_get_status() < 0 && net_get_status() != -EBUSY)
		net_init_async(NULL, NULL);

	stack = memalign(32, AGENT_STACK);
	if (!stack)
		return -ENOMEM;
	memset(stack, 0, AGENT_STACK);
	res = LWP_CreateThread(&thread, agent_thread, NULL, stack, AGENT_STACK, cfg.priority);
	if (res) {
		free(stack);
		stack = NULL;
		thread = LWP_THREAD_NULL;
		return res < 0 ? res : -EAGAIN;
	}
	return 0;
}

bool hbc_agent_exit_requested(void) {
	return exit_requested;
}

s32 hbc_agent_net_wait(u32 ms) {
	u64 t = gettime();
	s32 res;

	while ((res = net_get_status()) == -EBUSY) {
		if (ticks_to_millisecs(diff_ticks(t, gettime())) >= ms)
			return -ETIMEDOUT;
		usleep(20 * 1000);
	}
	return res < 0 ? res : 0;
}
