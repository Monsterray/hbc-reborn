// Developer network requests on the Wiiload port: status, SD/USB files, the
// app log target and crash reports. The loader thread calls devnet_handle()
// for each accepted local connection; see docs/devnet.md for the wire format.
// File requests themselves run in devfile.c, shared with sdk/hbc_agent.

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
#include <zlib.h>

#include "../config.h"
#include "appentry.h"
#include "devfile.h"
#include "devnet.h"
#include "loader.h"
#include "tcp.h"
#include "zmem.h"

#define HBC_NETLOG_LAYOUT_ONLY
#include "../../../sdk/hbc_netlog.h"
#include "../../../sdk/hbc_agent.h"

static const char *device_names[DEVICE_COUNT] = { "sd", "usb", "carda", "cardb" };
static u32 log_ip;
static u16 log_port;

// The crash an agent-enabled app recorded before returning here, if any,
// and the MEM2 copy of the log target, both taken at startup.
static hbc_crash_block crash;
static bool have_crash;
static hbc_netlog_block kept;
// The next Wiiload upload's name, from its sender (HBCA), and when it came.
#define UPLOAD_NAME_TTL_S 120
static char upload_name[64];
static u32 upload_flags;
static u64 upload_name_at;
// What became of the last upload (HBCS "upload"), counted from 1.
static struct {
	u32 seq;
	const char *result, *error;
	char text[96];
	u64 at;
} upload;

// The last app's output, kept by its agent as it stopped (HBCL).
static hbc_lastlog_block lastlog;
static bool have_lastlog;

// App folders changed by file requests, for the menu to reload.
#define CHANGES 8
static char changes[CHANGES][64];
static u32 change_count;

static u32 init_ms;
// Startup steps (devnet_boot_mark), for HBCS "startup".
#define BOOT_MARKS 20
static struct {
	const char *name;
	u32 ms;
} boot_marks[BOOT_MARKS];
static u32 boot_count;
static u64 boot_start;

#define MS(t) ((u32) ticks_to_millisecs (t))

static u16 get_u16(const u8 *p) {
	return (p[0] << 8) | p[1];
}

static const char *exception_name(u32 exid) {
	static const char *names[] = {
		NULL, "reset", "machine check", "DSI", "ISI", "interrupt", "alignment",
		"program", "floating point", "decrementer", NULL, NULL, "system call",
		"trace", NULL, "performance monitor", NULL, NULL, NULL, "breakpoint"
	};

	if (exid < sizeof(names) / sizeof(names[0]) && names[exid])
		return names[exid];
	return "unknown";
}

// Printable ASCII, with no JSON quoting needed: anything else becomes '?'.
static void json_text(char *out, const char *in, size_t max) {
	size_t i;

	for (i = 0; i + 1 < max && in[i]; ++i)
		out[i] = in[i] >= 0x20 && in[i] < 0x7f && in[i] != '"' && in[i] != '\\' ? in[i] : '?';
	out[i] = 0;
}

static const char *crash_kind(u32 kind) {
	static const char *names[] = { "exception", "fatal", "hang", "assert", "abort", "stack" };
	return kind < 6 ? names[kind] : "exception";
}

static const char *lastlog_why(u32 why) {
	static const char *names[] = { "exit", "exception", "fatal", "hang", "abort", "stack",
								   "power" };
	return why < 7 ? names[why] : "unknown";
}

// ,"crash":{...} or ,"crash":null
static s32 crash_json(char *buf, size_t size) {
	char app[sizeof(crash.app) + 1];
	s32 n;
	u32 i;

	if (!have_crash)
		return snprintf(buf, size, ",\"crash\":null");

	// Keep the name printable and free of JSON quoting.
	for (i = 0; i < sizeof(crash.app) && crash.app[i]; ++i)
		app[i] = crash.app[i] >= 0x20 && crash.app[i] < 0x7f &&
				crash.app[i] != '"' && crash.app[i] != '\\' ? crash.app[i] : '?';
	app[i] = 0;

	{
		char reason[HBC_CRASH_REASON + 1];

		json_text(reason, crash.reason, sizeof(reason));
		n = snprintf(buf, size, ",\"crash\":{\"kind\":\"%s\",\"code\":%u,\"reason\":\"%s\",",
				crash_kind(crash.kind), (unsigned) crash.code, reason);
	}
	n += snprintf(buf + n, size - n,
			"\"app\":\"%s\",\"exception\":%u,\"name\":\"%s\","
			"\"pc\":\"%08x\",\"lr\":\"%08x\",\"msr\":\"%08x\",\"cr\":\"%08x\","
			"\"ctr\":\"%08x\",\"dar\":\"%08x\",\"dsisr\":\"%08x\",\"sp\":\"%08x\","
			"\"uptime_ms\":%u,\"frames\":[",
			app, crash.exception, exception_name(crash.exception), crash.pc,
			crash.lr, crash.msr, crash.cr, crash.ctr, crash.dar, crash.dsisr,
			crash.sp, crash.uptime_ms);
	for (i = 0; i < HBC_CRASH_FRAMES && crash.frames[i]; ++i)
		n += snprintf(buf + n, size - n, "%s\"%08x\"", i ? "," : "", crash.frames[i]);
	n += snprintf(buf + n, size - n, "]}");
	return n;
}

// What the HOME overlay (home.c, via sdk/hbc_agent) cost the last time.
int agent_overlay_cost_json(char *buf, int size);
// Each Wii Remote's command queue, as the overlay sees it.
int agent_remote_diag(char *buf, int size);

static s32 status_json(char *buf, size_t size) {
	bool mounted[DEVICE_COUNT] = { false };
	int active = app_entry_get_status(mounted);
	// newlib's heap spans MEM1 and MEM2, so mallinfo's "used" figure counts
	// the gap between them; only the free figure is meaningful.
	struct mallinfo heap = mallinfo();
	u32 ip = net_gethostip();
	int i, n;

	n = snprintf(buf, size,
			"{\"version\":\"%s\",\"proto\":%d,\"ios\":%d,\"ios_revision\":%d,"
			"\"ahbprot\":%s,\"mem1_free\":%u,\"mem2_free\":%u,"
			"\"heap_free\":%u,\"tcp_stack_used\":%u,"
			"\"tcp_stack_size\":%u,\"init_ms\":%u,\"scan_ms\":%u,"
			"\"ip\":\"%u.%u.%u.%u\",\"apps\":%u,\"device\":",
			CHANNEL_VERSION_STR, DEVNET_PROTO, IOS_GetVersion(), IOS_GetRevision(),
			read32(0x0d800064) == 0xffffffff ? "true" : "false",
			SYS_GetArena1Size(), SYS_GetArena2Size(),
			heap.fordblks, loader_tcp_stack_used(),
			LD_THREAD_STACKSIZE, init_ms, app_entry_scan_ms,
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

	if (devfile_last.op)
		n += snprintf(buf + n, size - n, ",\"last\":{\"op\":\"%c\",\"bytes\":%u,"
				"\"wire\":%u,\"ms\":%u,\"net_ms\":%u,\"disk_ms\":%u,\"cpu_ms\":%u}",
				devfile_last.op, devfile_last.bytes, devfile_last.st.wire, MS(devfile_last.total),
				MS(devfile_last.st.net), MS(devfile_last.st.disk), MS(devfile_last.st.cpu));
	n += snprintf(buf + n, size - n, ",\"zlib_mem\":\"%s\",\"zlib_peak\":%u,\"zlib_heap\":%u,"
			"\"tcp_last_failure\":\"%s\"", zmem_where(), zmem_peak(), zmem_heap_peak(),
			tcp_last_failure());
	n += snprintf(buf + n, size - n, ",\"startup\":{");
	for (i = 0; i < (int) boot_count; ++i)
		n += snprintf(buf + n, size - n, "%s\"%s\":%u", i ? "," : "",
				boot_marks[i].name, boot_marks[i].ms);
	n += snprintf(buf + n, size - n, "}");
	n += crash_json(buf + n, size - n);
	if (upload.seq) {
		char text[sizeof(upload.text)];

		json_text(text, upload.text, sizeof(text));
		n += snprintf(buf + n, size - n, ",\"upload\":{\"seq\":%u,\"result\":\"%s\","
				"\"error\":%s%s%s,\"text\":\"%s\",\"ago_ms\":%u}", (unsigned) upload.seq,
				upload.result, upload.error ? "\"" : "", upload.error ? upload.error : "null",
				upload.error ? "\"" : "", text, MS(diff_ticks(upload.at, gettime())));
	} else {
		n += snprintf(buf + n, size - n, ",\"upload\":null");
	}
	if (have_lastlog)
		n += snprintf(buf + n, size - n, ",\"lastlog\":{\"why\":\"%s\",\"bytes\":%u}",
				lastlog_why(lastlog.why), (unsigned) lastlog.len);
	else
		n += snprintf(buf + n, size - n, ",\"lastlog\":null");
	n += agent_overlay_cost_json(buf + n, size - n);
	n += snprintf(buf + n, size - n, ",\"remotes\":");
	n += agent_remote_diag(buf + n, size - n);
	n += snprintf(buf + n, size - n, "}");

	return n;
}

// Note the app folder a change under "<active device>:/apps/<name>" touched.
static void note_app_change(const char *path) {
	bool mounted[DEVICE_COUNT];
	int active = app_entry_get_status(mounted);
	const char *p, *name;
	char dirname[64];
	size_t len;
	u32 level, i;

	if (active < 0 || active >= DEVICE_COUNT)
		return;
	len = strlen(device_names[active]);
	if (strncasecmp(path, device_names[active], len) ||
			strncasecmp(path + len, ":/apps/", 7))
		return;

	name = path + len + 7;
	p = strchr(name, '/');
	len = p ? (size_t) (p - name) : strlen(name);
	if (!len || len >= sizeof(dirname))
		return;
	memcpy(dirname, name, len);
	dirname[len] = 0;

	_CPU_ISR_Disable(level);
	for (i = 0; i < change_count; ++i)
		if (!strcasecmp(changes[i], dirname))
			break;
	if (i == change_count && change_count < CHANGES)
		strcpy(changes[change_count++], dirname);
	_CPU_ISR_Restore(level);
}

bool devnet_take_app_change(char *dirname, size_t size) {
	bool found = false;
	u32 level;

	_CPU_ISR_Disable(level);
	if (change_count) {
		strncpy(dirname, changes[0], size - 1);
		dirname[size - 1] = 0;
		memmove(changes[0], changes[1], --change_count * sizeof(changes[0]));
		found = true;
	}
	_CPU_ISR_Restore(level);
	return found;
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

static bool valid_block(const hbc_netlog_block *block) {
	return block->magic == HBC_NETLOG_MAGIC && block->version == HBC_NETLOG_VERSION &&
			block->check == hbc_netlog_check(block) && block->port;
}

// The low-memory block is for the next app; the MEM2 copy outlives the title
// launch that brings the installed channel back after that app.
static void set_log_target(u32 ip, u16 port) {
	log_ip = ip;
	log_port = port;
	write_block((hbc_netlog_block *) HBC_NETLOG_ADDR, ip, port);
	write_block((hbc_netlog_block *) HBC_NETLOG_KEEP_ADDR, ip, port);
}

bool devnet_handle(s32 s, const u8 *hdr, u32 client_ip) {
	char json[2048];

	// `hbc.py key` and `screen`: HOME-overlay presses and the TV picture,
	// answered by the agent that draws HBC's HOME menu (home.c).
	if (hbc_agent_handle(s, hdr))
		return true;

	if (!memcmp(hdr, "HBCS", 4)) {
		s32 n = status_json(json, sizeof(json));
		devfile_reply(s, 0, json, n);
		return true;
	}

	if (!memcmp(hdr, "HBCF", 4)) {
		// The UI thread outranks the loader; run transfers above it. The
		// thread mostly waits on IOS, so the menu keeps drawing meanwhile.
		devfile_handle(s, hdr, DEVNET_THREAD_PRIO, LD_THREAD_PRIO, note_app_change);
		return true;
	}

	if (!memcmp(hdr, "HBCC", 4)) {
		have_crash = false;
		devfile_reply(s, 0, NULL, 0);
		return true;
	}

	if (!memcmp(hdr, "HBCA", 4)) {
		// The name of the Wiiload upload that follows, for the Message
		// Board's play log: hbc.py sends the app's folder or file name.
		// Flags (protocol 6): DEVNET_UPLOAD_YES installs a ZIP unasked.
		char name[sizeof(upload_name)];
		u16 len = get_u16(hdr + 4), flags = get_u16(hdr + 6);
		u32 level;

		if ((!len && !flags) || len >= sizeof(name) ||
				(len && !tcp_read(s, (u8 *) name, len, NULL, NULL))) {
			devfile_reply(s, -EINVAL, NULL, 0);
			return true;
		}
		name[len] = 0;
		_CPU_ISR_Disable(level);
		memcpy(upload_name, name, len + 1);
		upload_flags = flags;
		upload_name_at = gettime();
		_CPU_ISR_Restore(level);
		devfile_reply(s, 0, NULL, 0);
		return true;
	}

	if (!memcmp(hdr, "HBCL", 4)) {
		// The kept log: a header line, then the text. The agent answers the
		// same request with a running app's log so far ("live").
		static char reply[64 + HBC_LASTLOG_SIZE];
		char app[sizeof(lastlog.app) + 1];
		int n;

		if (!have_lastlog) {
			devfile_reply(s, -ENOENT, NULL, 0);
			return true;
		}
		json_text(app, lastlog.app, sizeof(app));
		n = snprintf(reply, 64, "HBCL 1 %s %u %s\n", lastlog_why(lastlog.why),
				(unsigned) lastlog.uptime_ms, app);
		memcpy(reply + n, lastlog.text, lastlog.len);
		devfile_reply(s, 0, reply, n + lastlog.len);
		return true;
	}

	if (!memcmp(hdr, "HBCN", 4)) {
		u16 port = get_u16(hdr + 4);

		set_log_target(client_ip, port);
		devfile_reply(s, 0, NULL, 0);
		return true;
	}

	return false;
}

void devnet_early_init(void) {
	hbc_crash_block *cb = (hbc_crash_block *) HBC_CRASH_ADDR;
	hbc_netlog_block *keep = (hbc_netlog_block *) HBC_NETLOG_KEEP_ADDR;

	boot_start = gettime();
	// A crash an agent recorded before returning here: keep it for the
	// status reply, and clear it so it is reported once.
	DCInvalidateRange(cb, sizeof(*cb));
	if (cb->magic == HBC_CRASH_MAGIC && cb->version == HBC_CRASH_VERSION &&
			cb->check == hbc_crash_check(cb)) {
		memcpy(&crash, cb, sizeof(crash));
		have_crash = true;
	} else if (cb->magic == HBC_CRASH_MAGIC && cb->version == 1 &&
			((u32 *) cb)[HBC_CRASH_V1_WORDS] == hbc_check_words(cb, HBC_CRASH_V1_WORDS)) {
		// An app built with an older agent: an exception, with no reason.
		memset(&crash, 0, sizeof(crash));
		memcpy(&crash, cb, 4 * HBC_CRASH_V1_WORDS);
		crash.kind = HBC_CRASH_EXCEPTION;
		have_crash = true;
	}
	memset(cb, 0, sizeof(*cb));
	DCFlushRange(cb, sizeof(*cb));

	{
		hbc_lastlog_block *lb = (hbc_lastlog_block *) HBC_LASTLOG_ADDR;

		DCInvalidateRange(lb, sizeof(*lb));
		if (lb->magic == HBC_LASTLOG_MAGIC && lb->version == HBC_LASTLOG_VERSION &&
				lb->len <= HBC_LASTLOG_SIZE && lb->check == hbc_lastlog_check(lb)) {
			memcpy(&lastlog, lb, sizeof(lastlog));
			have_lastlog = true;
		}
		// Once only: an app without the agent must not inherit this one.
		memset(lb, 0, 64);
		DCFlushRange(lb, 64);
	}

	DCInvalidateRange(keep, sizeof(*keep));
	memcpy(&kept, keep, sizeof(kept));
}

void devnet_init(void) {
	hbc_netlog_block *block = (hbc_netlog_block *) HBC_NETLOG_ADDR;

	// A target registered before the last app launch: still in low memory
	// when a loader started this HBC directly, only in the MEM2 copy after
	// a title launch (which clears low memory).
	DCInvalidateRange(block, sizeof(*block));
	if (valid_block(block))
		set_log_target(block->ip, block->port);
	else if (valid_block(&kept))
		set_log_target(kept.ip, kept.port);
}

bool devnet_take_upload_name(char *out, size_t size) {
	bool have;
	u32 level;

	_CPU_ISR_Disable(level);
	have = upload_name[0] && upload_name_at &&
			diff_sec(upload_name_at, gettime()) < UPLOAD_NAME_TTL_S;
	if (have)
		strlcpy(out, upload_name, size);
	upload_name[0] = 0;
	_CPU_ISR_Restore(level);
	return have;
}

u32 devnet_upload_flags(void) {
	u32 flags, level;

	_CPU_ISR_Disable(level);
	flags = upload_name_at && diff_sec(upload_name_at, gettime()) < UPLOAD_NAME_TTL_S ?
			upload_flags : 0;
	_CPU_ISR_Restore(level);
	return flags;
}

void devnet_upload_result(const char *result, const char *error, const char *text) {
	u32 level;

	_CPU_ISR_Disable(level);
	upload.result = result;
	upload.error = error;
	strlcpy(upload.text, text ? text : "", sizeof(upload.text));
	upload.at = gettime();
	upload.seq++;
	// The sender's name and flags were for this upload alone; a launched
	// app's name goes on to the play log (devnet_take_upload_name).
	if (strcmp(result, "launched"))
		upload_name[0] = 0;
	upload_flags = 0;
	_CPU_ISR_Restore(level);
}

const char *devnet_lastlog_app(void) {
	static char app[sizeof(lastlog.app) + 1];

	if (!have_lastlog || !lastlog.app[0])
		return NULL;
	json_text(app, lastlog.app, sizeof(app));
	return app;
}

void devnet_set_init_ms(u32 ms) {
	init_ms = ms;
}

void devnet_boot_mark(const char *name) {
	u32 ms = ticks_to_millisecs(diff_ticks(boot_start, gettime()));
	u32 level;

	_CPU_ISR_Disable(level);
	if (boot_count < BOOT_MARKS) {
		boot_marks[boot_count].name = name;
		boot_marks[boot_count].ms = ms;
		boot_count++;
	}
	_CPU_ISR_Restore(level);
}
