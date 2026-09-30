// The in-app agent: HBC's developer protocol (docs/devnet.md) inside a
// running app, plus a crash recorder. See ../hbc_agent.h for the API.
//
// One thread, below the app's, accepts connections on the Wiiload port and
// answers HBCV, HBCS, HBCF (the same file requests as HBC, from
// channel/channelapp/source/devfile.c), HBCN and HBCX (exit to HBC). A
// Wiiload upload (HAXX) cannot run here; the agent answers it by exiting to
// HBC, so the client's next attempt reaches HBC itself. `hbc.py run` does
// that sequence for you. It also keeps the app's recent output for the
// overlay's Log page (overlay.c, which is linked only by apps that call
// hbc_agent_home()).

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

#include "ogc_flavor.h"
#if AGENT_TUXEDO
#include <tuxedo/ppc/exception.h>
#else
#include <stddef.h>
#include <ogc/context.h>
#endif

#include "../../channel/channelapp/config.h"
#include "devfile.h"
#include "devstream.h"
#include "tcp.h"
#include "agent_int.h"

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
#if AGENT_TUXEDO
static PPCExcptPanicFn prev_panic;
#endif

volatile int hbc_agent_log_muted;

// The agent thread's own cost, for the status reply: its wake-ups while
// idle and the time they took (checking for a connection), and the time
// spent answering requests.
static u32 idle_wakes;
static u64 idle_ticks, request_ticks;
// In overlay.c, when the app links it: whether the remote handles were found.
int agent_wpad_handles(void) __attribute__((weak));
int agent_remote_diag(char *buf, int size) __attribute__((weak));
void agent_overlay_cost(u32 *frames, u32 *avg_us, u32 *max_us, u32 *bytes,
						const char **buffers) __attribute__((weak));
volatile bool agent_crash_stay;
volatile bool agent_listen_enabled = true;

// ---- The app's recent output, for the overlay's Log page ----------------

#define LOG_SIZE 8192

static char log_ring[LOG_SIZE], log_line[LOG_SIZE + 1];
static u32 log_head, log_len;
static const devoptab_t *log_prev_out, *log_prev_err;

static ssize_t log_write(const devoptab_t *prev, struct _reent *r, void *fd, const char *ptr,
						 size_t len) {
	u32 level, i;

	_CPU_ISR_Disable(level);
	for (i = 0; i < len; ++i) {
		log_ring[log_head] = ptr[i];
		log_head = (log_head + 1) % LOG_SIZE;
	}
	log_len = log_len + len > LOG_SIZE ? LOG_SIZE : log_len + len;
	_CPU_ISR_Restore(level);
	if (prev && prev->write_r)
		prev->write_r(r, fd, ptr, len);
	return len;
}

static ssize_t log_write_out(struct _reent *r, void *fd, const char *ptr, size_t len) {
	return log_write(log_prev_out, r, fd, ptr, len);
}

static ssize_t log_write_err(struct _reent *r, void *fd, const char *ptr, size_t len) {
	return log_write(log_prev_err, r, fd, ptr, len);
}

static devoptab_t log_dotab_out = { .name = "hbcagent", .write_r = log_write_out };
static devoptab_t log_dotab_err = { .name = "hbcagent", .write_r = log_write_err };

const char *agent_log_text(void) {
	u32 level, start, i;

	_CPU_ISR_Disable(level);
	start = (log_head + LOG_SIZE - log_len) % LOG_SIZE;
	for (i = 0; i < log_len; ++i)
		log_line[i] = log_ring[(start + i) % LOG_SIZE];
	log_line[log_len] = 0;
	_CPU_ISR_Restore(level);
	return log_line;
}

// ---- App slots beside Exit ------------------------------------------------

static struct {
	char label[16];
	void (*press)(void *user);
	void *user;
	char title[40];
	const hbc_agent_item *items;
	int count;
} slots[2];

void hbc_agent_set_slot(int slot, const char *label, void (*press)(void *user), void *user) {
	if (slot < 0 || slot > 1)
		return;
	snprintf(slots[slot].label, sizeof(slots[slot].label), "%s", label ? label : "");
	slots[slot].press = label ? press : NULL;
	slots[slot].user = user;
}

void hbc_agent_set_slot_menu(int slot, const char *label, const char *title,
							 const hbc_agent_item *items, int count) {
	if (slot < 0 || slot > 1)
		return;
	hbc_agent_set_slot(slot, label, NULL, NULL);
	snprintf(slots[slot].title, sizeof(slots[slot].title), "%s", title ? title : "");
	slots[slot].items = label ? items : NULL;
	slots[slot].count = label && items ? (count > 12 ? 12 : count) : 0;
}

const hbc_agent_item *agent_slot_menu(int slot, const char **title, int *count) {
	*title = slots[slot].title;
	*count = slots[slot].count;
	return slots[slot].items;
}

const char *agent_slot_label(int slot) {
	if (slot == 1 && !slots[1].press && !slots[1].count)
		return "Shot";
	return slots[slot].label;
}

void agent_slot_press(int slot) {
	if (slots[slot].press)
		slots[slot].press(slots[slot].user);
}

// ---- Keys from the PC (HBCK), for driving the overlay without hands ------

#define KEYS 64

static char keys[KEYS];
static u32 key_head, key_count;

int agent_key_pop(void) {
	u32 level;
	int k = 0;

	_CPU_ISR_Disable(level);
	if (key_count) {
		k = keys[key_head];
		key_head = (key_head + 1) % KEYS;
		key_count--;
	}
	_CPU_ISR_Restore(level);
	return k;
}

bool hbc_agent_home_pending(void) {
	u32 level;
	bool home = false;

	// The overlay is closed, so only HOME means anything: drop whatever
	// comes before it, or a stray key would block every HOME behind it.
	_CPU_ISR_Disable(level);
	while (key_count && !home) {
		home = keys[key_head] == 'h';
		key_head = (key_head + 1) % KEYS;
		key_count--;
	}
	_CPU_ISR_Restore(level);
	return home;
}

static void push_keys(const u8 *k, u32 n) {
	u32 level, i;

	_CPU_ISR_Disable(level);
	for (i = 0; i < n && key_count < KEYS; ++i)
		if (strchr("udlrabh12w", k[i])) {
			keys[(key_head + key_count) % KEYS] = k[i];
			key_count++;
		}
	_CPU_ISR_Restore(level);
}

// ---- The picture on the TV (HBCP) ----------------------------------------

static u16 screen_w, screen_h;

void agent_set_screen_size(u16 w, u16 h) {
	screen_w = w;
	screen_h = h;
}

// Reply: u32 width, u32 height, then the YUYV framebuffer VI is showing.
static void send_screen(s32 s) {
	const u8 *fb = VIDEO_GetCurrentFramebuffer();
	u32 size;
	u8 dims[8];

	if (!screen_w) {
		GXRModeObj *m = VIDEO_GetPreferredMode(NULL);

		screen_w = m->fbWidth;
		screen_h = m->xfbHeight;
	}
	size = screen_w * screen_h * 2;

	if (!fb) {
		devfile_reply(s, -ENODEV, NULL, 0);
		return;
	}
	fb = agent_uncached((void *) fb);
	dims[0] = dims[1] = dims[4] = dims[5] = 0;
	dims[2] = screen_w >> 8;
	dims[3] = screen_w;
	dims[6] = screen_h >> 8;
	dims[7] = screen_h;
	{
		u8 hdr[8] = { 0, 0, 0, 0, (8 + size) >> 24, (8 + size) >> 16, (8 + size) >> 8, 8 + size };

		if (devfile_send_all(s, hdr, 8) && devfile_send_all(s, dims, 8))
			devfile_send_all(s, fb, size);
	}
}

static u16 get_u16(const u8 *p);

bool hbc_agent_handle(s32 s, const u8 *hdr) {
	if (!memcmp(hdr, "HBCK", 4)) {
		u8 k[KEYS];
		u32 n = get_u16(hdr + 4);

		if (n > KEYS || (n && !tcp_read(s, k, n, NULL, NULL))) {
			devfile_reply(s, -EINVAL, NULL, 0);
		} else {
			push_keys(k, n);
			devfile_reply(s, 0, NULL, 0);
		}
		return true;
	}
	if (!memcmp(hdr, "HBCP", 4)) {
		send_screen(s);
		return true;
	}
	return false;
}

const hbc_agent_config *agent_cfg(void) {
	return &cfg;
}

u32 agent_uptime_ms(void) {
	return ticks_to_millisecs(diff_ticks(start_ticks, gettime()));
}

static u16 get_u16(const u8 *p) {
	return (p[0] << 8) | p[1];
}

static u32 uptime_ms(void) {
	return agent_uptime_ms();
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

// The stack was zeroed before the thread started; the lowest byte still zero
// marks how deep it has reached. libogc2 and libogc 1.x write 0xDEADBABE into
// the lowest word when the thread starts, so the scan starts above it.
static u32 stack_used(void) {
	u32 i = 0;

	if (*(u32 *) stack == 0xdeadbabe)
		i = 4;
	for (; i < AGENT_STACK && !stack[i]; ++i)
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
			"\"ahbprot\":%s,\"mem1_free\":%u,\"mem2_free\":%u,\"heap_free\":%u,\"heap_arena\":%u,"
			"\"agent_stack_used\":%u,\"agent_stack_size\":%u,"
			"\"ip\":\"%u.%u.%u.%u\",\"exit_requested\":%s,\"inserted\":[",
			CHANNEL_VERSION_STR, AGENT_PROTO, name, version, uptime_ms(),
			IOS_GetVersion(), IOS_GetRevision(),
			read32(0x0d800064) == 0xffffffff ? "true" : "false",
			SYS_GetArena1Size(), SYS_GetArena2Size(), heap.fordblks, heap.arena,
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
	if (agent_wpad_handles)
		n += snprintf(buf + n, size - n, ",\"wpad_handles\":%s",
				agent_wpad_handles() ? "true" : "false");
	if (agent_remote_diag) {
		n += snprintf(buf + n, size - n, ",\"remotes\":");
		n += agent_remote_diag(buf + n, size - n);
	}
	// What the HOME overlay cost the last time it was open.
	if (agent_overlay_cost) {
		u32 frames, avg, max, bytes;
		const char *buffers;

		agent_overlay_cost(&frames, &avg, &max, &bytes, &buffers);
		if (frames)
			n += snprintf(buf + n, size - n, ",\"overlay\":{\"frames\":%u,\"avg_us\":%u,"
					"\"max_us\":%u,\"bytes\":%u,\"buffers\":\"%s\"}",
					frames, avg, max, bytes, buffers);
	}
	n += snprintf(buf + n, size - n, ",\"agent_idle_wakes\":%u,\"agent_idle_us\":%u,"
			"\"agent_request_ms\":%u", idle_wakes, (u32) ticks_to_microsecs(idle_ticks),
			(u32) ticks_to_millisecs(request_ticks));
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
	char json[2048];

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
	} else if (hbc_agent_handle(s, hdr)) {
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
		// The overlay's "hbc.py connection" switch.
		if (!agent_listen_enabled) {
			if (ls >= 0) {
				net_close(ls);
				ls = -1;
			}
			usleep(250 * 1000);
			continue;
		}
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

		u64 woke = gettime();
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_len = sizeof(sa);
		len_sa = sizeof(sa);
		s = net_accept(ls, (struct sockaddr *) &sa, &len_sa);
		if (s == -EAGAIN) {
			idle_wakes++;
			idle_ticks += diff_ticks(woke, gettime());
			continue;
		}
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
		request_ticks += diff_ticks(woke, gettime());
	}
	return NULL;
}

// Return addresses up the stack chain: each frame's back chain at 0(sp), and
// the saved LR of the frame it called at 4(back chain).
static bool ram_word(u32 a) {
	return !(a & 3) && ((a >= 0x80000000 && a < 0x81800000) ||
						(a >= 0x90000000 && a < 0x94000000));
}

// Runs inside the exception, with floating point off: integer code only.
static void agent_record(u32 exid, u32 pc, u32 msr, u32 lr, u32 cr, u32 ctr, u32 sp) {
	hbc_crash_block *b = (hbc_crash_block *) HBC_CRASH_ADDR;
	u32 dar = mfspr(19), dsisr = mfspr(18);
	u32 i;

	memset(b, 0, sizeof(*b));
	b->magic = HBC_CRASH_MAGIC;
	b->version = HBC_CRASH_VERSION;
	b->exception = exid;
	b->pc = pc;
	b->msr = msr;
	b->lr = lr;
	b->cr = cr;
	b->ctr = ctr;
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
}

#if AGENT_TUXEDO
static void agent_panic(unsigned exid, PPCContext *ctx) {
	agent_record(exid, ctx->pc, ctx->msr, ctx->lr, ctx->cr, ctx->ctr, ctx->gpr[1]);
	if (prev_panic)
		prev_panic(exid, ctx);
}

static void install_crash_hook(void) {
	prev_panic = PPCExcptCurPanicFn;
	PPCExcptCurPanicFn = agent_panic;
}
#else
// libogc2 and libogc 1.x: _exceptionhandlertable[] holds assembly entry
// points, not C functions (ogc_exc.S says what they receive). The agent's
// entry, agent_exc_entry, builds libogc's frame and calls agent_exc(), which
// records the crash and then shows libogc's own crash screen.
_Static_assert(offsetof(frame_context, SRR0) == AGENT_EXC_SRR0 - AGENT_EXC_NUMBER &&
			   offsetof(frame_context, GPR[1]) == AGENT_EXC_GPR(1) - AGENT_EXC_NUMBER &&
			   offsetof(frame_context, GQR[0]) == AGENT_EXC_GQR(0) - AGENT_EXC_NUMBER &&
			   offsetof(frame_context, CR) == AGENT_EXC_CR - AGENT_EXC_NUMBER &&
			   offsetof(frame_context, XER) == AGENT_EXC_XER - AGENT_EXC_NUMBER,
			   "ogc_exc.S's frame offsets must match this libogc's frame_context");

typedef void (*agent_exc_fn)(frame_context *);
extern agent_exc_fn _exceptionhandlertable[NUM_EXCEPTIONS];
extern void default_exceptionhandler(frame_context *);
extern void c_default_exceptionhandler(frame_context *);
extern void agent_exc_entry(frame_context *);
void agent_exc(frame_context *ctx);

// libogc's exception index to the vector number tuxedo's PPC_EXCPT_* use
// (vector / 0x100), which is what the crash block and hbc.py report.
static const u8 exc_vector[NUM_EXCEPTIONS] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 0x0c, 0x0d, 0x0f, 0x13, 0x14, 0x17
};

void agent_exc(frame_context *ctx) {
	u32 n = ctx->EXCPT_Number;

	agent_record(n < NUM_EXCEPTIONS ? exc_vector[n] : n, ctx->SRR0, ctx->SRR1, ctx->LR,
				 ctx->CR, ctx->CTR, ctx->GPR[1]);
	c_default_exceptionhandler(ctx);
}

// Take over only the exceptions that would reach libogc's crash screen; the
// FPU, interrupt and decrementer handlers, and any a debugger (libdb) or the
// app put in, stay.
static void install_crash_hook(void) {
	u32 level, i;

	_CPU_ISR_Disable(level);
	for (i = 0; i < NUM_EXCEPTIONS; ++i)
		if (_exceptionhandlertable[i] == default_exceptionhandler)
			_exceptionhandlertable[i] = agent_exc_entry;
	_CPU_ISR_Restore(level);
}
#endif

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
		install_crash_hook();
	}

	// Keep the app's output for the Log page, still passing it on.
	log_prev_out = devoptab_list[STD_OUT];
	log_prev_err = devoptab_list[STD_ERR];
	devoptab_list[STD_OUT] = &log_dotab_out;
	devoptab_list[STD_ERR] = &log_dotab_err;

	// A host with its own server wants only the overlay.
	if (cfg.no_network) {
		thread = 0;
		return 0;
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
