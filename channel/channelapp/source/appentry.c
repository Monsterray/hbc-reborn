#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/dir.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/cond.h>
#include <sdcard/gcsd.h>
#include <sdcard/wiisd_io.h>
#include <ogc/usbstorage.h>
#include "usbmsd.h"
#include <fat.h>

#include "../config.h"
#include "tex.h"
#include "panic.h"

#include "appentry.h"
#include "bootlog.h"
#include "apprun.h"

typedef struct {
	const char *name;
	const DISC_INTERFACE *device;
	bool last_status;
} device_t;

static app_entry *entries_all[MAX_ENTRIES];
static u32 entry_count_all = 0;

app_entry *entries[MAX_ENTRIES];
u32 entry_count = 0;
u32 app_entry_scan_ms = 0;

const char *app_path = "/apps";
const char *app_fn_boot_elf = "boot.elf";
const char *app_fn_boot_dol = "boot.dol";
const char *app_fn_theme = "theme.zip";
const char *app_fn_meta = "meta.xml";
const char *app_fn_icon = "icon.png";

// The order is what settings.xml stores as the preferred device, so a new
// device goes at the end. Both USB drives come from usbmsd.c: libogc's own
// driver serves only one.
static device_t devices[DEVICE_COUNT] = {
	{ "sd", &__io_wiisd, false },
	{ "usb", &usbmsd_disc[0], false },
	{ "carda", &__io_gcsda, false },
	{ "cardb", &__io_gcsdb, false },
	{ "usb2", &usbmsd_disc[1], false }
};

static int device_active = -1;
static int device_prefered = -1;

// The devices' calls, timed for the start-up log (bootlog.h): "usb inserted".
enum { DEV_STARTUP, DEV_INSERTED, DEV_MOUNT, DEV_SHUTDOWN, DEV_CALLS };
static char dev_call_names[DEVICE_COUNT][DEV_CALLS][16];

static const char *dev_call(int i, int what) {
	static const char *const what_names[DEV_CALLS] = { "startup", "inserted", "mount", "shutdown" };

	if (!dev_call_names[i][what][0])
		snprintf(dev_call_names[i][what], sizeof(dev_call_names[i][what]), "%s %s",
				 devices[i].name, what_names[what]);
	return dev_call_names[i][what];
}

static bool dev_startup(int i) {
	return BOOTLOG(dev_call(i, DEV_STARTUP), devices[i].device->startup());
}

static bool dev_inserted(int i) {
	return BOOTLOG(dev_call(i, DEV_INSERTED), devices[i].device->isInserted());
}

static void dev_shutdown(int i) {
	BOOTLOG_VOID(dev_call(i, DEV_SHUTDOWN), devices[i].device->shutdown());
}
static app_filter current_filter = APP_FILTER_ALL;
static app_sort current_sort = APP_SORT_NAME;
static bool cmp_descending = false;
static bool cmp_release_date = false;
static bool cmp_sort_id = false;

static int cmp_app_entry (const void *p1, const void *p2) {
	const app_entry *a1;
	const app_entry *a2;

	if (cmp_descending) {
		a1 = *((const app_entry **) p2);
		a2 = *((const app_entry **) p1);
	} else {
		a1 = *((const app_entry **) p1);
		a2 = *((const app_entry **) p2);
	}

	if (!a1->meta && !a2->meta)
		return strcasecmp (a1->dirname, a2->dirname);

	if (!a1->meta && a2->meta)
		return 1;

	if (a1->meta && !a2->meta)
		return -1;

	if (cmp_release_date) {
		if (!a1->meta->release_date && !a2->meta->release_date)
			return 0;

		if (!a1->meta->release_date && a2->meta->release_date)
			return -1;

		if (a1->meta->release_date && !a2->meta->release_date)
			return 1;

		if (a1->meta->release_date > a2->meta->release_date)
			return 1;

		return -1;
	}

	if (cmp_sort_id && (a1->meta->has_sort_id || a2->meta->has_sort_id)) {
		if (!a2->meta->has_sort_id)
			return -1;
		if (!a1->meta->has_sort_id)
			return 1;
		if (a1->meta->sort_id != a2->meta->sort_id)
			return a1->meta->sort_id < a2->meta->sort_id ? -1 : 1;
		// The same id: by name, as below.
	}

	if (!a1->meta->name && !a2->meta->name)
		return strcasecmp (a1->dirname, a2->dirname);

	if (!a1->meta->name && a2->meta->name)
		return 1;

	if (a1->meta->name && !a2->meta->name)
		return -1;

	return strcasecmp (a1->meta->name, a2->meta->name);
}

static void app_entry_filter(app_filter filter) {
	u32 i;

	for (i = 0; i < MAX_ENTRIES; ++i)
		entries[i] = NULL;

	entry_count = 0;

	for (i = 0; i < MAX_ENTRIES; ++i) {
		if (!entries_all[i])
			continue;

		switch (filter) {
		case APP_FILTER_ICONSONLY:
			if (!entries_all[i]->icon)
				continue;
			break;

		case APP_FILTER_DATEONLY:
			if (!entries_all[i]->meta) //|| !entries_all[i]->meta->release_date)
				continue;
			break;

		default:
			break;
		}

		entries[entry_count] = entries_all[i];
		entry_count++;
	}

	current_filter = filter;

	if (entry_count)
		qsort(entries, entry_count, sizeof(app_entry *), cmp_app_entry);
}

app_sort app_entry_get_sort(void) {
	return current_sort;
}

void app_entry_set_sort(app_sort sort) {
	switch (sort) {
	case APP_SORT_DATE:
		cmp_descending = true;
		cmp_release_date = true;
		cmp_sort_id = false;
		current_filter = APP_FILTER_DATEONLY;
		current_sort = APP_SORT_DATE;
		break;

	case APP_SORT_CUSTOM:
		cmp_descending = false;
		cmp_release_date = false;
		cmp_sort_id = true;
		current_filter = APP_FILTER_ALL;
		current_sort = APP_SORT_CUSTOM;
		break;

	default:
		cmp_descending = false;
		cmp_release_date = false;
		cmp_sort_id = false;
		current_filter = APP_FILTER_ALL;
		current_sort = APP_SORT_NAME;
		break;
	}

	if (settings.sort_order != current_sort)
		settings.sort_order = current_sort;
}

// in_scan: called from the scan thread's device scan, which has
// apps_seen.txt open (apprun.c); otherwise (app_entry_add, from the menu)
// it reads the file for the one app, after any scan in progress.
static app_entry *app_entry_load_single (const char *dirname, bool in_scan) {
	app_entry *entry;
	app_entry_type type;
	char tmp[PATH_MAX + 32];
	struct stat st;

	type = AET_BOOT_ELF;
	sprintf(tmp, "%s/%s/%s", app_path, dirname, app_fn_boot_elf);

	if (stat(tmp, &st)) {
		type = AET_BOOT_DOL;
		sprintf(tmp, "%s/%s/%s", app_path, dirname, app_fn_boot_dol);

		if (stat(tmp, &st)) {
			type = AET_THEME;
			sprintf(tmp, "%s/%s/%s", app_path, dirname, app_fn_theme);

			if (stat(tmp, &st))
				return NULL;

			if (st.st_size > MAX_THEME_ZIP_SIZE)
				return NULL;
		}
	}

	if (!st.st_size || st.st_size > LD_MAX_SIZE)
		return NULL;

	entry = (app_entry *) pmalloc(sizeof(app_entry));

	entry->type = type;
	entry->size = st.st_size;
	entry->run_state = APP_RUN_UNCHANGED;
	// Options > App badges: only then read the device's apps_seen.txt.
	if (type != AET_THEME && device_active >= 0) {
		if (in_scan) {
			// Always mark the app seen while the scan runs: with badges
			// turned off midway, the apps after that point would be missed
			// and pruned. The badge shows only while the setting is on.
			app_run_state state = app_run_status(dirname);

			if (settings.new_badges)
				entry->run_state = state;
		} else if (settings.new_badges) {
			char root[16];

			snprintf(root, sizeof(root), "%s:", devices[device_active].name);
			entry->run_state = app_run_status_one(root, dirname);
		}
	}
	entry->dirname = pstrdup(dirname);
	entry->icon = NULL;
	entry->meta = NULL;

	sprintf(tmp, "%s/%s/%s", app_path, dirname, app_fn_meta);
	if (stat(tmp, &st) == 0)
		entry->meta = meta_parse(tmp);

	sprintf(tmp, "%s/%s/%s", app_path, dirname, app_fn_icon);
	if (stat(tmp, &st) == 0)
		entry->icon = tex_from_png_file(tmp, &st, APP_ENTRY_ICON_WIDTH,
										APP_ENTRY_ICON_HEIGHT);

	return entry;
}

void app_entries_free(void) {
	u32 i;

	for (i = 0; i < MAX_ENTRIES; ++i) {
		entries[i] = NULL;

		if (!entries_all[i])
			continue;

		free (entries_all[i]->dirname);
		tex_free (entries_all[i]->icon);
		meta_free (entries_all[i]->meta);

		free(entries_all[i]);
		entries_all[i] = NULL;
	}

	entry_count_all = 0;
	entry_count = 0;
}

static void app_entry_load_all (void) {
	app_entry *entry;
	DIR *d;
	struct dirent *de;
	bool badge_scan = false;

	app_entries_free ();

	d = opendir(app_path);
	if (!d)
		return;

	if (settings.new_badges && device_active >= 0) {
		char root[16];

		snprintf(root, sizeof(root), "%s:", devices[device_active].name);
		app_run_begin(root);
		badge_scan = true;
	}
	// errno tells a readdir error from the end of the folder.
	for (;;) {
		errno = 0;
		if (!(de = readdir(d)))
			break;
		// ignore dotfiles / dotdirs
		if (de->d_name[0] == '\0' || de->d_name[0] == '.')
			continue;

		// All apps have their own dir
		if (de->d_type != DT_DIR)
			continue;

		entry = app_entry_load_single(de->d_name, badge_scan);

		if (!entry)
			continue;

		entries_all[entry_count_all] = entry;

		entry_count_all++;

		if (entry_count_all >= MAX_ENTRIES)
			break;
	}

	// Forget the badge records of apps that are gone, but only after a scan
	// that saw every folder (no readdir error) and a card that is still there.
	if (badge_scan) {
		app_run_end(!de && !errno && dev_inserted(device_active));
	}

	closedir(d);

	app_entry_filter(current_filter);
}

bool app_entry_remove(app_entry *app) {
	u32 i;
	bool res = false;

	for (i = 0; i < MAX_ENTRIES; ++i)
		if (entries_all[i] == app) {
			free(entries_all[i]->dirname);
			tex_free(entries_all[i]->icon);
			meta_free(entries_all[i]->meta);

			free(entries_all[i]);
			entries_all[i] = NULL;
			entry_count_all--;

			res = true;

			break;
		}

	if (res)
		app_entry_filter(current_filter);

	return res;
}

app_entry *app_entry_add(const char *dirname) {
	app_entry *entry = NULL;
	u32 i;
	bool filter = false;

	gprintf("adding entry '%s'\n", dirname);

	for (i = 0; i < MAX_ENTRIES; ++i) {
		if (entries_all[i]) {
			if (!strcasecmp(entries_all[i]->dirname, dirname)) {
				gprintf("removing old entry '%s'\n", entries_all[i]->dirname);

				free(entries_all[i]->dirname);
				tex_free(entries_all[i]->icon);
				meta_free(entries_all[i]->meta);

				free(entries_all[i]);
				entries_all[i] = NULL;
				entry_count_all--;

				filter = true;

				break;
			}
		}
	}

	if (entry_count_all >= MAX_ENTRIES)
		goto exit;

	entry = app_entry_load_single(dirname, false);

	if (!entry)
		goto exit;

	for (i = 0; i < MAX_ENTRIES; ++i)
		if (!entries_all[i]) {
			entries_all[i] = entry;
			entry_count_all++;

			filter = true;

			break;
		}

exit:
	if (filter)
		app_entry_filter(current_filter);

	return entry;
}

// Devices mounted for file requests alone (devnet's hbc.py access), beside
// the active one the app list comes from. Only those that cost nothing to
// look for: SD and the USB drives, not the SDGecko slots.
static u32 le32(const u8 *p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (u32) p[3] << 24;
}

// C12A7328-F81F-11D2-BA4B-00A0C93EC93B as stored (its first three fields
// little-endian).
static const u8 efi_system[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
								   0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };

static bool extra_mounted[DEVICE_COUNT];
static bool mount_failed[DEVICE_COUNT];   // inserted, but fatMountSimple failed
static char mount_why[DEVICE_COUNT][48];  // what its first sector holds instead

// What is on a drive FAT could not mount, from its first sector: a boot
// sector's file system name, else the MBR's partition types.
static void probe_fs(int i) {
	static u8 sector[512] ATTRIBUTE_ALIGN(32);
	char *out = mount_why[i];
	int p, n = 0;

	out[0] = 0;
	if (!devices[i].device->readSectors(0, 1, sector)) {
		snprintf(out, sizeof(mount_why[i]), "first sector unreadable");
		return;
	}
	if (!memcmp(sector + 3, "NTFS    ", 8) || !memcmp(sector + 3, "EXFAT   ", 8)) {
		snprintf(out, sizeof(mount_why[i]), "%.5s volume, no partition table", sector + 3);
		return;
	}
	if (sector[510] != 0x55 || sector[511] != 0xaa) {
		snprintf(out, sizeof(mount_why[i]), "no boot sector or partition table");
		return;
	}
	// GPT: each partition's file system, by its boot sector's name.
	if (sector[0x1be + 4] == 0xee && devices[i].device->readSectors(1, 1, sector) &&
			!memcmp(sector, "EFI PART", 8)) {
		static u8 entries[512] ATTRIBUTE_ALIGN(32);
		u32 lba = le32(sector + 72), size = le32(sector + 84), e;

		n = snprintf(out, sizeof(mount_why[i]), "GPT:");
		if (size >= 128 && size <= 512 && !(512 % size) &&
				devices[i].device->readSectors(lba, 1, entries))
			for (e = 0; e < 512 / size; ++e) {
				const u8 *q = entries + e * size;
				u32 first = le32(q + 32);
				const char *fs;

				if (!first)
					continue;
				if (!memcmp(q, efi_system, 16))
					fs = "EFI";
				else if (!devices[i].device->readSectors(first, 1, sector))
					fs = "?";
				else
					fs = !memcmp(sector + 3, "NTFS", 4) ? "NTFS" :
						 !memcmp(sector + 3, "EXFAT", 5) ? "exFAT" :
						 !memcmp(sector + 0x52, "FAT32", 5) ? "FAT32" :
						 !memcmp(sector + 0x36, "FAT", 3) ? "FAT" : "other";
				n += snprintf(out + n, sizeof(mount_why[i]) - n, " %u %s", e + 1, fs);
			}
		return;
	}
	n = snprintf(out, sizeof(mount_why[i]), "MBR types");
	for (p = 0; p < 4; ++p) {
		u8 type = sector[0x1be + p * 16 + 4];

		if (type)
			n += snprintf(out + n, sizeof(mount_why[i]) - n, " %02x%s", type,
						  type == 0x07 ? " (NTFS/exFAT)" : type == 0xee ? " (GPT)" :
						  type == 0x83 ? " (Linux)" : "");
	}
}

const char *app_entry_mount_why(int device) {
	return device >= 0 && device < DEVICE_COUNT && mount_failed[device] ? mount_why[device] :
		NULL;
}
// extra_mounted, mount_failed and mount_why: the scan thread and the threads
// that make file requests (network, avatar, Mii resources) both change them.
// Made by app_entry_init, before any of those threads starts.
static mutex_t extra_lock = LWP_MUTEX_NULL;

// libogc's FAT finds partitions only in an MBR. A GPT disk (its MBR holds just
// the 0xee protective entry) gets each of its partitions tried in turn, at
// its first sector. 512-byte sectors (usbmsd.c offers no others).
static bool mount_gpt(int i) {
	static u8 sector[512] ATTRIBUTE_ALIGN(32);
	const DISC_INTERFACE *d = devices[i].device;
	u32 entries_lba, count, size, e, lba = 0;

	if (!d->readSectors(1, 1, sector) || memcmp(sector, "EFI PART", 8))
		return false;
	entries_lba = le32(sector + 72);   // the high word is 0 below 2 TiB
	count = le32(sector + 80);
	size = le32(sector + 84);
	if (size < 128 || size > 512 || 512 % size || count > 128)
		return false;
	for (e = 0; e < count; ++e) {
		const u8 *p;
		u32 first;

		if (lba != entries_lba + e * size / 512) {
			lba = entries_lba + e * size / 512;
			if (!d->readSectors(lba, 1, sector))
				return false;
		}
		p = sector + e * size % 512;
		first = le32(p + 32);
		if (!le32(p) && !le32(p + 4) && !le32(p + 8) && !le32(p + 12))
			continue;   // an unused entry
		if (!memcmp(p, efi_system, 16))
			continue;   // the EFI System Partition: boot files, not the user's
		if (first && fatMount(devices[i].name, d, first, 4, 64)) {
			hlog("Mounted %s: GPT partition %u at sector %u\n", devices[i].name, e + 1, first);
			return true;
		}
		if (lba != entries_lba + e * size / 512 || !d->readSectors(lba, 1, sector))
			return false;   // fatMount used the buffer's sector? reread it
	}
	return false;
}

static bool mount_device(int i) {
	return BOOTLOG(dev_call(i, DEV_MOUNT),
				   fatMountSimple(devices[i].name, devices[i].device) || mount_gpt(i));
}

// A drive that file requests mounted, or found unmountable, and that has
// since been unplugged: forget it, so status stops listing it. Before, it
// stayed "inserted" until the next file request to that drive.
static void drop_removed_extras(bool *status) {
	int i;

	LWP_MutexLock(extra_lock);
	for (i = 0; i < DEVICE_COUNT; ++i) {
		if (i == device_active)
			continue;
		if (extra_mounted[i] && !dev_inserted(i)) {
			hlog("Device lost: %s (mounted for file requests)\n", devices[i].name);
			fatUnmount(devices[i].name);
			dev_shutdown(i);
			extra_mounted[i] = false;
			status[i] = false;
		}
		if (mount_failed[i] && !dev_inserted(i)) {
			mount_failed[i] = false;
			mount_why[i][0] = 0;
		}
	}
	LWP_MutexUnlock(extra_lock);
}

static bool _mount(int index) {
	bool was_extra;

	// Already mounted for file requests: it becomes the active one as is.
	LWP_MutexLock(extra_lock);
	was_extra = extra_mounted[index];
	extra_mounted[index] = false;
	LWP_MutexUnlock(extra_lock);
	if (was_extra) {
		devices[index].last_status = true;
		return true;
	}
	devices[index].last_status = mount_device(index);
	if (devices[index].last_status) {
		LWP_MutexLock(extra_lock);
		mount_failed[index] = false;   // an "unmountable" note from before is stale
		LWP_MutexUnlock(extra_lock);
	}

	if (!devices[index].last_status) {
		dev_shutdown(index);

		return false;
	}

	return true;
}

typedef enum {
	AE_CMD_IDLE = 0,
	AE_CMD_EXIT,
	AE_CMD_SCAN,
	AE_CMD_POLLSTATUS
} ae_cmd;

typedef struct {
	bool running;
	ae_cmd cmd;
	ae_action action;
	bool status[DEVICE_COUNT];
	bool umount;
	int mount;
	bool loading;
	mutex_t cmutex;
	cond_t cond;
} ae_args;

static lwp_t ae_thread;
static u8 ae_stack[APPENTRY_THREAD_STACKSIZE] ATTRIBUTE_ALIGN (32);
static ae_args ta_ae;

static void *ae_func (void *arg) {
	ae_args *ta = (ae_args *) arg;
	int i;
	char cwd[16];

	ta->running = true;
	bootlog_thread("scan");

	for (i = 0; i < DEVICE_COUNT; ++i)
		ta_ae.status[i] = false;

	LWP_MutexLock (ta->cmutex);

	while (true) {

		ta->cmd = AE_CMD_IDLE;
		LWP_CondWait(ta->cond, ta->cmutex);

		if (ta->cmd == AE_CMD_EXIT)
			break;

		switch (ta->cmd) {
		case AE_CMD_SCAN:
			drop_removed_extras(ta->status);

			if (device_active >= 0) {
				if (!ta->umount && dev_inserted(device_active))
					continue;

				gprintf("device lost: %s\n", devices[device_active].name);

				ta->umount = false;

				fatUnmount(devices[device_active].name);
				dev_shutdown(device_active);
				devices[device_active].last_status = false;
				ta->status[device_active] = false;   // else status keeps saying inserted
				device_active = -1;

				ta->action = AE_ACT_REMOVE;

				continue;
			}

			if (ta->mount >= 0) {
				if (_mount(ta->mount))
					device_active = ta->mount;
				ta->mount = -1;
			}

			if ((device_active < 0) && (device_prefered >= 0)) {
				if (_mount(device_prefered))
					device_active = device_prefered;
			}

			if (device_active < 0) {
				for (i = 0; i < DEVICE_COUNT; ++i)
					if (_mount(i)) {
						device_active = i;
						break;
					}
			}

			if (device_active >= 0) {
				gprintf("device mounted: %s\n", devices[device_active].name);

				ta->loading = true;

				strcpy(cwd, devices[device_active].name);
				strcat(cwd, ":/");

				gprintf("chdir to '%s'\n", cwd);
				if (chdir(cwd))
					gprintf("chdir failed: %d\n", errno);

				s64 scan_start = gettime();
				app_entry_load_all();
				app_entry_scan_ms =
					ticks_to_millisecs(diff_ticks(scan_start, gettime()));
				hlog("Apps: %s read in %u ms\n", cwd, (unsigned) app_entry_scan_ms);

				ta->loading = false;

				ta->action = AE_ACT_ADD;
			}

			continue;

		case AE_CMD_POLLSTATUS:
			for (i = 0; i < DEVICE_COUNT; ++i) {
				if (i == device_active) {
					ta->status[i] = devices[i].last_status =
						dev_inserted(i);

					if (!ta->umount && !devices[i].last_status)
						ta->umount = true;

					continue;
				}

				if (devices[i].last_status) {
					if (!dev_inserted(i)) {
						ta->status[i] = devices[i].last_status = false;
						dev_shutdown(i);
					}

					continue;
				}

				if (!dev_startup(i)) {
					ta->status[i] = devices[i].last_status = false;
					continue;
				}

				ta->status[i] = devices[i].last_status =
					dev_inserted(i);

				if (!devices[i].last_status)
					dev_shutdown(i);
			}

			continue;

		default:
			break;
		}
	}

	LWP_MutexUnlock (ta->cmutex);

	gprintf("app entry thread done\n");

	ta->running = false;

	return NULL;
}

void app_entry_init(void) {
	s32 res;

	memset(&entries_all, 0, sizeof(app_entry *) * MAX_ENTRIES);
	memset(&entries, 0, sizeof(app_entry *) * MAX_ENTRIES);

	gprintf("starting app entry thread\n");

	ta_ae.cmd = AE_CMD_IDLE;
	ta_ae.action = AE_ACT_NONE;
	ta_ae.umount = false;
	ta_ae.mount = -1;
	ta_ae.loading = false;

	res = LWP_MutexInit(&extra_lock, false);
	if (res) {
		gprintf("error creating extra_lock: %d\n", res);
		return;
	}

	res = LWP_MutexInit (&ta_ae.cmutex, false);

	if (res) {
		gprintf ("error creating cmutex: %d\n", res);
		return;
	}

	res = LWP_CondInit (&ta_ae.cond);

	if (res) {
		gprintf ("error creating cond: %d\n", res);
		return;
	}

	memset(&ae_stack, 0, APPENTRY_THREAD_STACKSIZE);
	res = LWP_CreateThread(&ae_thread, ae_func, &ta_ae, ae_stack,
							APPENTRY_THREAD_STACKSIZE, APPENTRY_THREAD_PRIO);

	if (res) {
		gprintf("error creating thread: %d\n", res);
	}
}

void app_entry_deinit (void) {
	u8 i;

	if (ta_ae.running) {
		gprintf ("stopping app entry thread\n");

		for (i = 0; i < 25; ++i) {
			if (LWP_MutexTryLock (ta_ae.cmutex) == 0) {
				break;
			}
			usleep (20 * 1000);
		}
		if (i < 25) {
			gprintf ("sending app entry thread the exit cmd\n");
			ta_ae.cmd = AE_CMD_EXIT;
			LWP_SetThreadPriority (ae_thread, LWP_PRIO_HIGHEST);
			LWP_CondBroadcast (ta_ae.cond);
			LWP_MutexUnlock (ta_ae.cmutex);
			LWP_JoinThread(ae_thread, NULL);
			LWP_CondDestroy (ta_ae.cond);
			LWP_MutexDestroy (ta_ae.cmutex);
			app_entries_free ();
		} else {
			gprintf("app entry thread didn't shutdown gracefully!\n");
		}

		if (device_active >= 0) {
			fatUnmount(devices[device_active].name);
			dev_shutdown(device_active);
		}
		LWP_MutexLock(extra_lock);
		for (i = 0; i < DEVICE_COUNT; ++i)
			if (extra_mounted[i]) {
				fatUnmount(devices[i].name);
				dev_shutdown(i);
				extra_mounted[i] = false;
			}
		LWP_MutexUnlock(extra_lock);

		USB_Deinitialize();
	}

}

void app_entry_scan(void) {
	if (!ta_ae.running)
		return;

	if (LWP_MutexTryLock(ta_ae.cmutex) != 0)
		return;

	ta_ae.cmd = AE_CMD_SCAN;
	LWP_CondBroadcast(ta_ae.cond);
	LWP_MutexUnlock(ta_ae.cmutex);
}

ae_action app_entry_action(void) {
	ae_action res;

	if (!ta_ae.running)
		return AE_ACT_NONE;

	if (LWP_MutexTryLock(ta_ae.cmutex) != 0)
		return AE_ACT_NONE;

	res = ta_ae.action;
	ta_ae.action = AE_ACT_NONE;
	LWP_MutexUnlock(ta_ae.cmutex);

	return res;
}

void app_entry_poll_status(bool reset) {
	if (!ta_ae.running)
		return;

	if (LWP_MutexTryLock(ta_ae.cmutex) != 0)
		return;

	ta_ae.cmd = AE_CMD_POLLSTATUS;
	LWP_CondBroadcast(ta_ae.cond);
	LWP_MutexUnlock(ta_ae.cmutex);
}

int app_entry_get_status(bool *status) {
	u8 i;

	if (status)
		for (i = 0; i < DEVICE_COUNT; ++i)
			status[i] = ta_ae.status[i] || extra_mounted[i];

	return device_active;
}

void app_entry_set_prefered(int device) {
	if (device < 0) {
		device_prefered = -1;
		return;
	}

	if (device > DEVICE_COUNT - 1) {
		device_prefered = -1;
		return;
	}

	device_prefered = device;

	if (settings.device != device_prefered)
		settings.device = device_prefered;
}

void app_entry_set_device(int device) {
	if (device < 0)
		return;

	if (device > DEVICE_COUNT - 1)
		return;

	ta_ae.umount = true;
	ta_ae.mount = device;
}

void app_entry_mount_for(const char *path) {
	const char *colon = strchr(path, ':');
	int i;

	if (!colon)
		return;
	for (i = 0; i < DEVICE_COUNT; ++i)
		if (strlen(devices[i].name) == (size_t) (colon - path) &&
				!strncmp(path, devices[i].name, colon - path))
			break;
	// The active device is the app list's; the SDGecko slots are probed
	// only from Options.
	if (i == DEVICE_COUNT || i == device_active || !strncmp(devices[i].name, "card", 4))
		return;
	LWP_MutexLock(extra_lock);
	if (extra_mounted[i] && !dev_inserted(i)) {
		fatUnmount(devices[i].name);
		dev_shutdown(i);
		extra_mounted[i] = false;
	}
	if (!extra_mounted[i] && i != device_active && dev_startup(i) &&
			dev_inserted(i)) {
		extra_mounted[i] = mount_device(i);
		if (extra_mounted[i])
			hlog("Mounted %s: for file requests\n", devices[i].name);
		else {
			mount_failed[i] = true;   // a drive, but no FAT on it
			probe_fs(i);
		}
	}
	LWP_MutexUnlock(extra_lock);
}

bool app_entry_get_path(char *buf) {
	if (device_active < 0)
		return false;

	strcpy(buf, devices[device_active].name);
	return true;
}

bool app_entry_get_filename(char *buf, app_entry *app) {
	if (device_active < 0)
		return false;

	sprintf(buf, "%s:%s/%s/", devices[device_active].name,
			app_path, app->dirname);

	switch(app->type) {
	case AET_BOOT_ELF:
		strcat(buf, app_fn_boot_elf);
		return true;
	case AET_BOOT_DOL:
		strcat(buf, app_fn_boot_dol);
		return true;
	case AET_THEME:
		strcat(buf, app_fn_theme);
		return true;
	}

	return false;
}

app_entry *app_entry_find(char *dirname) {
	u32 i;

	for (i = 0; i < entry_count; ++i)
		if (!strcasecmp(entries[i]->dirname, dirname))
			return entries[i];

	return NULL;
}

bool app_entry_is_loading(void) {
	if (!ta_ae.running)
		return false;

	return ta_ae.loading;
}

