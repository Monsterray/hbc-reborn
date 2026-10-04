// The Wii's hardware and settings, for DEV > Info's pages (ov_ui.c) and
// `hbc.py hw` (HBCH). Read-only, and only when asked: nothing here runs
// while the app does. Each read is a few IOS calls; the slow ones (the
// SD card's free space on a big card, USB and network queries) take up to
// about a second together, so callers run this in a thread of their own.
//
// Sources: SYSCONF through libogc's CONF_*, setting.txt (model and serial,
// XOR-scrambled), ES (console ID, boot2, the Wii Menu's TMD), ISFS stats,
// the IOS USB and network devices, and /shared2/sys/net/02/config.dat for
// the connection in use. Nothing is written.

#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/iosupport.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/cache.h>
#include <ogc/card.h>
#include <ogc/conf.h>
#include <ogc/es.h>
#include <ogc/ipc.h>
#include <ogc/isfs.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <ogc/usb.h>
#include <network.h>

#include "agent_int.h"
#include "ov_ui.h"

static void row(ov_page *pg, const char *label, const char *fmt, ...)
		__attribute__((format(printf, 3, 4)));

static void row(ov_page *pg, const char *label, const char *fmt, ...) {
	ov_row *r;
	va_list ap;

	if (pg->count >= OV_INFO_ROWS)
		return;
	r = &pg->row[pg->count++];
	snprintf(r->label, sizeof(r->label), "%s", label);
	va_start(ap, fmt);
	vsnprintf(r->value, sizeof(r->value), fmt, ap);
	va_end(ap);
}

static bool ahbprot(void) {
	return read32(0x0d800064) == 0xffffffff;
}

// A whole NAND file into a 32-byte aligned buffer; its length, or < 0.
static s32 nand_read(const char *path, void *buf, u32 size) {
	s32 fd = ISFS_Open(path, ISFS_OPEN_READ), n;

	if (fd < 0)
		return fd;
	n = ISFS_Read(fd, buf, size);
	ISFS_Close(fd);
	return n;
}

// ---- System -----------------------------------------------------------------

// setting.txt: "KEY=VALUE\r\n" lines, each byte XORed with a rotating key.
static bool setting(const char *text, const char *key, char *out, size_t size) {
	size_t n = strlen(key);
	const char *p = text;

	while ((p = strstr(p, key))) {
		if ((p == text || p[-1] == '\n') && p[n] == '=') {
			size_t i = 0;

			p += n + 1;
			while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < size) {
				out[i] = p[i];
				++i;
			}
			out[i] = 0;
			return true;
		}
		p += n;
	}
	return false;
}

static const char *menu_name(u16 v) {
	static const struct { u16 v; const char *name; } known[] = {
		{ 512, "4.3J" }, { 513, "4.3U" }, { 514, "4.3E" }, { 518, "4.3K" },
		{ 480, "4.2J" }, { 481, "4.2U" }, { 482, "4.2E" }, { 486, "4.2K" },
		{ 448, "4.1J" }, { 449, "4.1U" }, { 450, "4.1E" }, { 454, "4.1K" },
		{ 416, "4.0J" }, { 417, "4.0U" }, { 418, "4.0E" },
		{ 384, "3.4J" }, { 385, "3.4U" }, { 386, "3.4E" }, { 390, "3.5K" },
		{ 608, "4.3J (vWii)" }, { 609, "4.3U (vWii)" }, { 610, "4.3E (vWii)" },
		{ 4609, "4.3U (Wii mini)" }, { 4610, "4.3E (Wii mini)" },
	};
	unsigned i;

	for (i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
		if (known[i].v == v)
			return known[i].name;
	return NULL;
}

static void system_page(ov_page *pg) {
	static const char *langs[] = { "Japanese", "English", "German", "French", "Spanish",
								   "Italian", "Dutch", "Chinese (simplified)",
								   "Chinese (traditional)", "Korean" };
	static const char *regions[] = { "Japan", "USA", "Europe", "?", "Korea", "China" };
	static u8 buf[256] __attribute__((aligned(32)));
	char model[24] = "", code[8] = "", serno[16] = "";
	u32 id = 0, boot2 = 0, size = 0, pvr = mfpvr();
	s32 lang = CONF_GetLanguage(), region = CONF_GetRegion();
	bool wiiu = ahbprot() && (read32(0x0d8005a0) >> 16) == 0xcafe;

	if (nand_read("/title/00000001/00000002/data/setting.txt", buf, 256) == 256) {
		u32 key = 0x73b5dbfa, i;

		for (i = 0; i < 256; ++i) {
			buf[i] ^= key & 0xff;
			key = (key << 1) | (key >> 31);
		}
		buf[255] = 0;
		setting((char *) buf, "MODEL", model, sizeof(model));
		setting((char *) buf, "CODE", code, sizeof(code));
		setting((char *) buf, "SERNO", serno, sizeof(serno));
	}
	if (wiiu)
		row(pg, "Console", "Wii U, in Wii mode%s%s", model[0] ? " " : "", model);
	else
		row(pg, "Console", "Wii %s", model[0] ? model : "(model unknown)");
	row(pg, "Region", "%s", region >= 0 && region < 6 ? regions[region] : "unknown");
	if (code[0] || serno[0])
		row(pg, "Serial", "%s%s", code, serno);
	if (ES_GetDeviceID(&id) >= 0)
		row(pg, "Console ID", "%u (%08x)", (unsigned) id, (unsigned) id);
	if (ES_GetStoredTMDSize(0x0000000100000002ULL, &size) >= 0 && size >= 0x1e0 && size < 0x4000) {
		u8 *tmd = memalign(32, (size + 31) & ~31);

		if (tmd && ES_GetStoredTMD(0x0000000100000002ULL, (signed_blob *) tmd, size) >= 0) {
			u16 v = tmd[0x1dc] << 8 | tmd[0x1dd];
			const char *name = menu_name(v);

			row(pg, "Wii Menu", "%s%sv%u%s", name ? name : "", name ? " (" : "", v,
				name ? ")" : "");
		}
		free(tmd);
	}
	row(pg, "IOS", "IOS%d v%d, AHBPROT %s", IOS_GetVersion(), IOS_GetRevision(),
		ahbprot() ? "open" : "closed");
	if (ES_GetBoot2Version(&boot2) >= 0)
		row(pg, "boot2", "v%u", (unsigned) boot2);
	row(pg, "CPU", "%s, %u MHz (PVR %08x)",
		(pvr >> 16) == 0x7001 ? "Espresso" : "Broadway",
		(unsigned) (TB_CORE_CLOCK / 1000000), (unsigned) pvr);
	row(pg, "Hollywood", "revision %08x", (unsigned) *(u32 *) 0x80003138);
	row(pg, "Language", "%s", lang >= 0 && lang < 10 ? langs[lang] : "unknown");
}

// ---- Video and input ------------------------------------------------------

static void video_page(ov_page *pg, const GXRModeObj *rm) {
	static const char *fmts[] = { "NTSC", "PAL", "MPAL", "debug", "debug PAL", "PAL 60" };
	static const char *sounds[] = { "Mono", "Stereo", "Surround" };
	conf_pads pads;
	s32 video = CONF_GetVideo(), sound = CONF_GetSoundMode();

	if (rm) {
		u32 fmt = rm->viTVMode >> 2, mode = rm->viTVMode & 3;

		row(pg, "Mode", "%ux%u, %s, %u Hz (%s)", rm->fbWidth, rm->xfbHeight,
			mode == VI_PROGRESSIVE ? "progressive" : mode == VI_NON_INTERLACE ? "240p" :
			"interlaced", fmt == VI_PAL ? 50 : 60, fmt < 6 ? fmts[fmt] : "?");
	}
	row(pg, "Cable", "%s", VIDEO_HaveComponentCable() ? "Component" : "Composite (or none)");
	row(pg, "TV type", "%s", video == CONF_VIDEO_PAL ? "PAL" : video == CONF_VIDEO_MPAL ? "MPAL" :
		"NTSC");
	row(pg, "Screen", "%s", CONF_GetAspectRatio() == CONF_ASPECT_16_9 ? "16:9" : "4:3");
	row(pg, "480p", "%s", CONF_GetProgressiveScan() > 0 ? "On" : "Off");
	if (video == CONF_VIDEO_PAL)
		row(pg, "PAL 60", "%s", CONF_GetEuRGB60() > 0 ? "On" : "Off");
	row(pg, "Sound", "%s", sound >= 0 && sound < 3 ? sounds[sound] : "unknown");
	row(pg, "Sensor bar", "%s, sensitivity %d of 5",
		CONF_GetSensorBarPosition() == CONF_SENSORBAR_TOP ? "Above the TV" : "Below the TV",
		(int) CONF_GetIRSensitivity() + 1);
	if (CONF_GetPadDevices(&pads) >= 0)
		row(pg, "Paired remotes", "%u", pads.num_registered);
}

// ---- Storage --------------------------------------------------------------

static void volume_row(ov_page *pg, const char *label, const char *dev) {
	struct statvfs st;
	double total, avail;

	if (FindDevice(dev) < 0) {
		row(pg, label, "Not mounted by the app");
		return;
	}
	if (statvfs(dev, &st) || !st.f_frsize) {
		row(pg, label, "Mounted, size unknown");
		return;
	}
	total = (double) st.f_blocks * st.f_frsize / 1e9;
	avail = (double) st.f_bavail * st.f_frsize / 1e9;
	row(pg, label, "%s%.1f GB, %.1f GB free",
		dev[0] == 's' ? total <= 2.2 ? "SD, " : total <= 34 ? "SDHC, " : "SDXC, " : "",
		total, avail);
}

static void storage_page(ov_page *pg) {
	static u32 stats[8] __attribute__((aligned(32)));
	int slot;

	volume_row(pg, "SD card", "sd:/");
	volume_row(pg, "USB drive", "usb:/");
	if (FindDevice("usb2:/") >= 0)
		volume_row(pg, "USB drive 2", "usb2:/");
	// NAND: clusters of 16 KiB; the Wii Menu counts blocks of 128 KiB.
	if (ISFS_Initialize() >= 0 && ISFS_GetStats(stats) >= 0) {
		u32 cluster = stats[0] ? stats[0] : 0x4000, free_c = stats[1], used_c = stats[2];

		row(pg, "NAND", "%.1f MB free of %.1f MB", (double) free_c * cluster / 1048576,
			(double) (free_c + used_c) * cluster / 1048576);
		row(pg, "NAND blocks", "%u free (the Wii Menu's 128 KiB)",
			(unsigned) ((u64) free_c * cluster / 131072));
		row(pg, "NAND files", "%u free of %u", (unsigned) stats[5],
			(unsigned) (stats[5] + stats[6]));
	}
	for (slot = 0; slot < 2; ++slot) {
		s32 mbits = 0, sector = 0, res = CARD_ProbeEx(slot, &mbits, &sector);

		if (res >= 0 && mbits > 0)
			row(pg, slot ? "Memory card B" : "Memory card A", "%d blocks (%d Mbit)",
				(int) (mbits * 128 / 8 - 5), (int) mbits);
		else
			row(pg, slot ? "Memory card B" : "Memory card A", "None");
	}
}

// ---- USB ------------------------------------------------------------------

static const char *usb_name(u16 vid, u16 pid) {
	static const struct { u16 vid, pid; const char *name; } known[] = {
		{ 0x057e, 0x0305, "Wii's own Bluetooth" },
		{ 0x057e, 0x0337, "GameCube controller adapter" },
		{ 0x057e, 0x0306, "Wii Remote" },
		{ 0x0b05, 0x1786, "Wi-Fi adapter" },
		{ 0x0d8c, 0x000c, "Audio adapter" },
		{ 0x046d, 0x0a03, "Logitech microphone" },
		{ 0x1430, 0x0150, "Guitar Hero dongle" },
		{ 0x12ba, 0x0100, "Rock Band guitar" },
		{ 0x1bad, 0x0004, "Rock Band guitar" },
		{ 0x057e, 0x0308, "Wii Speak" },
	};
	unsigned i;

	for (i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
		if (known[i].vid == vid && known[i].pid == pid)
			return known[i].name;
	return NULL;
}

// libogc's own device lists, which libogc keeps up to date from IOS's
// device-change replies; they open nothing.
static int usb_list_libogc(ov_page *pg) {
	static usb_device_entry all[32], hid[32];
	u8 n = 0, nh = 0, i, j;

	if (USB_GetDeviceList(all, 32, 0, &n) < 0)
		return -1;
	if (USB_GetDeviceList(hid, 32, USB_CLASS_HID, &nh) < 0)
		nh = 0;
	for (i = 0; i < n; ++i) {
		const char *name = usb_name(all[i].vid, all[i].pid);
		bool is_hid = false;
		char label[12];

		for (j = 0; j < nh; ++j)
			is_hid |= hid[j].device_id == all[i].device_id;
		snprintf(label, sizeof(label), "%04x:%04x", all[i].vid, all[i].pid);
		row(pg, label, "%s%s%s", is_hid ? "HID" : "Device", name ? ", " : "", name ? name : "");
	}
	return n;
}

// An app that never started libogc's USB gets it started here, and left
// running: IOS's USB devices keep one device-change request each, so asking
// IOS directly and closing again could take the app's or hang on the close.
// An app that starts USB itself later finds it already up.
static void usb_page(ov_page *pg) {
	static bool started;
	int n;

	if (!started) {
		started = true;
		if (USB_Initialize() >= 0)
			usleep(300 * 1000);   // the first device-change reply fills the lists
	}
	n = usb_list_libogc(pg);
	if (n < 0)
		row(pg, "USB", "Not available on IOS%d", IOS_GetVersion());
	else if (!n)
		row(pg, "USB", "Nothing plugged in");
}

// ---- Network --------------------------------------------------------------

// IOS's network device: an interface option (GetInterfaceOpt, ioctlv 0x1c).
static s32 if_opt(s32 fd, u32 opt, void *buf, u32 size) {
	static u32 in[2] __attribute__((aligned(32)));
	static u32 len __attribute__((aligned(32)));
	static ioctlv v[3] __attribute__((aligned(32)));
	s32 res;

	in[0] = 0xfffe;
	in[1] = opt;
	len = size;
	v[0].data = in;
	v[0].len = sizeof(in);
	v[1].data = buf;
	v[1].len = size;
	v[2].data = &len;
	v[2].len = sizeof(len);
	// Cleared first: Dolphin reads the routing table's starting offset from
	// the buffer, and IOS fills only what it has.
	memset(buf, 0, size);
	DCFlushRange(in, sizeof(in));
	DCFlushRange(&len, sizeof(len));
	DCFlushRange(buf, size);
	res = IOS_Ioctlv(fd, 0x1c, 1, 2, v);
	DCInvalidateRange(buf, size);
	DCInvalidateRange(&len, sizeof(len));
	return res < 0 ? res : (s32) len;
}

// A handle of our own on IOS's network device, opened once and never
// closed: Dolphin has one device object behind every handle, so closing ours
// (even at exit, where libogc still shuts its own down) stops the app's.
// IOS drops it when it reloads for the next program.
static s32 top_fd = -1;

static s32 top_open(void) {
	if (top_fd < 0)
		top_fd = IOS_Open("/dev/net/ip/top", IPC_OPEN_NONE);
	return top_fd;
}

static void ip_text(char *out, size_t size, u32 a) {
	snprintf(out, size, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
}

static void network_page(ov_page *pg) {
	u8 *cfg;
	static u32 buf[48] __attribute__((aligned(32)));
	static const char *crypt[] = { "open", "WEP64", "WEP128", "?", "WPA TKIP", "WPA2 AES",
								   "WPA AES" };
	u8 mac[6] __attribute__((aligned(32)));
	char a[16], b[16];
	s32 fd, n, i;

	if (net_get_status() == 0) {
		ip_text(a, sizeof(a), net_gethostip());
		fd = top_open();
		if (fd >= 0 && if_opt(fd, 0x4003, buf, 12) >= 12) {
			ip_text(b, sizeof(b), buf[1]);
			row(pg, "IP", "%s / %s", a, b);
		} else {
			row(pg, "IP", "%s", a);
		}
		if (fd >= 0) {
			n = if_opt(fd, 0x4006, buf, sizeof(buf));
			for (i = 0; i + 24 <= n; i += 24)
				if (!buf[i / 4] && buf[i / 4 + 2]) {   // the default route
					ip_text(a, sizeof(a), buf[i / 4 + 2]);
					row(pg, "Gateway", "%s", a);
					break;
				}
			n = if_opt(fd, 0xb003, buf, 8);
			if (n >= 4 && buf[0] && ~buf[0]) {
				bool two = n >= 8 && buf[1] && ~buf[1];

				ip_text(a, sizeof(a), buf[0]);
				ip_text(b, sizeof(b), buf[1]);
				row(pg, "DNS", two ? "%s, %s" : "%s", a, b);
			}
		}
	} else {
		row(pg, "IP", "Not connected");
	}
	if (net_get_mac_address(mac) >= 0)
		row(pg, "MAC", "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
			mac[5]);
	// The Wii Menu's connection settings: three of 0x91c bytes after 8.
	// Flags: 1 wired, 2 DNS by DHCP, 4 IP by DHCP, 16 proxy, 128 the one in
	// use; the SSID at 0x7c4, its length at 0x7e5, the encryption at 0x7e9.
	// 7 KiB, only while it is read.
	cfg = memalign(32, 0x1b60);
	if (cfg && nand_read("/shared2/sys/net/02/config.dat", cfg, 0x1b5c) >= 8 + 3 * 0x91c)
		for (i = 0; i < 3; ++i) {
			const u8 *c = cfg + 8 + i * 0x91c;
			char ssid[33];
			u32 len = c[0x7e5] < 32 ? c[0x7e5] : 32;

			if (!(c[0] & 0x80))
				continue;   // not the one selected
			memcpy(ssid, c + 0x7c4, len);
			ssid[len] = 0;
			if (c[0] & 0x01) {
				row(pg, "Connection", "%d of 3, wired LAN adapter", i + 1);
			} else {
				row(pg, "Connection", "%d of 3, Wi-Fi", i + 1);
				row(pg, "Wi-Fi network", "%s", ssid);
				row(pg, "Security", "%s", c[0x7e9] < 7 ? crypt[c[0x7e9]] : "?");
			}
			row(pg, "Settings", "IP by %s, DNS by %s%s", c[0] & 0x04 ? "DHCP" : "hand",
				c[0] & 0x02 ? "DHCP" : "hand", c[0] & 0x10 ? ", proxy" : "");
			break;
		}
	free(cfg);
}

// ---- All of it ------------------------------------------------------------

// The overlay's worker and HBCH share the static IOS buffers above.
static volatile bool busy;

bool agent_info_gather(void *out, const struct _gx_rmodeobj *rmode, u32 mask) {
	ov_page *pages = out;
	u32 level;

	_CPU_ISR_Disable(level);
	if (busy) {
		_CPU_ISR_Restore(level);
		return false;
	}
	busy = true;
	_CPU_ISR_Restore(level);
	memset(pages, 0, OV_INFO_PAGES * sizeof(*pages));
	// The NAND reads need libogc's ISFS, which an app may not have started.
	ISFS_Initialize();
	if (mask & (1 << OV_INFO_SYSTEM))
		system_page(&pages[OV_INFO_SYSTEM]);
	if (mask & (1 << OV_INFO_VIDEO))
		video_page(&pages[OV_INFO_VIDEO], rmode ? (const GXRModeObj *) rmode :
				   VIDEO_GetPreferredMode(NULL));
	if (mask & (1 << OV_INFO_STORAGE))
		storage_page(&pages[OV_INFO_STORAGE]);
	if (mask & (1 << OV_INFO_USB))
		usb_page(&pages[OV_INFO_USB]);
	if (mask & (1 << OV_INFO_NETWORK))
		network_page(&pages[OV_INFO_NETWORK]);
	busy = false;
	return true;
}

// HBCH: the same as JSON, {"System":{"Console":"...",...},...}; mask is
// the pages (bit per OV_INFO_*), 0 for all.
s32 agent_info_json(char *buf, size_t size, u32 mask) {
	static const char *names[OV_INFO_PAGES] = { "System", "Video", "Storage", "USB", "Network" };
	ov_page *pages = malloc(OV_INFO_PAGES * sizeof(*pages));
	s32 n = 0;
	int p, i;

	if (!pages)
		return -ENOMEM;
	if (!mask)
		mask = (1 << OV_INFO_PAGES) - 1;
	if (!agent_info_gather(pages, NULL, mask)) {
		free(pages);
		return -EBUSY;
	}
	n += snprintf(buf + n, size - n, "{");
	for (p = 0; p < OV_INFO_PAGES; ++p) {
		if (!(mask & (1 << p)))
			continue;
		n += snprintf(buf + n, size - n, "%s\"%s\":{", buf[n - 1] == '{' ? "" : ",", names[p]);
		for (i = 0; i < pages[p].count; ++i) {
			char v[sizeof(pages[p].row[i].value)];
			const char *s = pages[p].row[i].value;
			int j = 0;

			// JSON: quotes and backslashes become apostrophes and slashes.
			for (; *s && j + 1 < (int) sizeof(v); ++s)
				v[j++] = *s == '"' ? '\'' : *s == '\\' ? '/' : (u8) *s < 0x20 ? ' ' : *s;
			v[j] = 0;
			n += snprintf(buf + n, size - n, "%s\"%s\":\"%s\"", i ? "," : "",
						  pages[p].row[i].label, v);
		}
		n += snprintf(buf + n, size - n, "}");
	}
	n += snprintf(buf + n, size - n, "}");
	free(pages);
	return n < (s32) size ? n : (s32) size - 1;
}
