// USB mass storage for two drives at once: "usb" and "usb2".
//
// Adapted from libogc 3.1's usbstorage.c (Sven Peter, tueidj, rodries,
// Tantric; zlib licence, notice kept below), which serves only one drive:
// its command buffer, transfer mode, timeout and "mounted" flag are global,
// so a second handle would race the first on the buffer, and unplugging
// either drive unmounts both. Here every piece of that state belongs to a
// drive. libogc's low-level USB_* calls are shared and safe to call from
// several threads; libogc's own usbstorage module is not used at all.
//
// The two drives are the first two bulk-only mass storage devices that
// mount, in IOS's device order; a drive keeps its slot while it stays
// plugged in. Only 512-byte sectors are offered (libogc 3's FAT expects
// them).
//
// ---- Original notice ------------------------------------------------------
// usbstorage.c -- Bulk-only USB mass storage support
// Copyright (C) 2008 Sven Peter (svpe) <svpe@gmx.net>
// Copyright (C) 2009-2010 tueidj, rodries, Tantric
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source
//    distribution.
// ----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <ogc/usb.h>
#include <ogc/usbstorage.h>

#include "usbmsd.h"

#define CBW_SIZE 31
#define CBW_SIGNATURE 0x43425355
#define CBW_IN (1 << 7)
#define CBW_OUT 0
#define CSW_SIZE 13
#define CSW_SIGNATURE 0x53425355
#define TAG_START 0x0badc0de

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2a
#define SCSI_SENSE_REPLY_SIZE 18
#define SCSI_SENSE_NOT_READY 0x02
#define SCSI_SENSE_MEDIUM_ERROR 0x03
#define SCSI_SENSE_HARDWARE_ERROR 0x04

#define CLASS_MASS_STORAGE 0x08
#define BULK_ONLY 0x50
#define GET_MAX_LUN 0xfe
#define MSD_RESET 0xff
#define ENDPOINT_BULK 0x02

#define CYCLE_RETRIES 3
#define TIMEOUT_S 2
#define INVALID_LUN -2
#define MAX_TRANSFER_V0 4096
#define MAX_TRANSFER_V5 (16 * 1024)
#define DEVLIST_MAX 8

// IOS's USB moves data only to and from 32-byte aligned MEM2 buffers; each
// drive has one for the CBW/CSW and one to bounce other buffers through.
#define CBW_BUF 64
#define DRIVE_MEM (CBW_BUF + MAX_TRANSFER_V5)

typedef struct {
	s32 fd;              // USB_OpenDevice handle, -1 when closed
	s32 device_id;
	u16 vid, pid;
	u8 ep_in, ep_out;
	u8 configuration, interface, alt_interface;
	u8 max_lun, lun;
	u32 tag;
	u32 sector_size;
	bool usb2;           // USBV5 with high-speed endpoints: bigger transfers
	bool suspended;
	volatile bool mounted;
	u32 timeout_s;
	u64 last_used;
	mutex_t lock;
	syswd_t alarm;
	volatile s32 retval;
	u8 *cbw;             // CBW_BUF bytes, MEM2
	u8 *buffer;          // MAX_TRANSFER_V5 bytes, MEM2
	// Why the last look for a drive found none, for HBCS "usb_drives".
	const char *why;
	u16 why_vid, why_pid;
	s32 why_code;
	u32 sector_seen;
} drive;

static drive drives[USBMSD_DRIVES];
static lwpq_t waitq;
static bool inited;

#define PROCESSING 0x7fffffff   // dev->retval while a transfer runs

// ---- Transfers with a timeout (libogc's way: an async call and an alarm) --

static s32 msg_done(s32 retval, void *arg) {
	drive *d = arg;

	d->retval = retval;
	SYS_CancelAlarm(d->alarm);
	LWP_ThreadBroadcast(waitq);
	return 0;
}

static void timed_out(syswd_t alarm, void *arg) {
	drive *d = arg;

	(void) alarm;
	d->retval = USBSTORAGE_ETIMEDOUT;
	LWP_ThreadBroadcast(waitq);
}

static s32 wait_done(drive *d, u32 secs) {
	struct timespec ts = { secs, 0 };
	s32 retval;

	SYS_SetAlarm(d->alarm, &ts, timed_out, d);
	while ((retval = d->retval) == PROCESSING)
		LWP_ThreadSleep(waitq);
	return retval;
}

static s32 bulk(drive *d, u8 ep, u16 len, void *data, u32 secs) {
	s32 retval;

	d->retval = PROCESSING;
	retval = USB_WriteBlkMsgAsync(d->fd, ep, len, data, msg_done, d);
	if (retval < 0)
		return retval;
	retval = wait_done(d, secs);
	if (retval < 0)
		USB_ClearHalt(d->fd, ep);
	return retval;
}

static s32 control(drive *d, u8 type, u8 request, u16 value, u16 index, u16 len, void *data) {
	s32 retval;

	d->retval = PROCESSING;
	retval = USB_WriteCtrlMsgAsync(d->fd, type, request, value, index, len, data, msg_done, d);
	if (retval < 0)
		return retval;
	return wait_done(d, d->timeout_s);
}

// ---- Bulk-only transport ----------------------------------------------------

static s32 send_cbw(drive *d, u8 lun, u32 len, u8 flags, const u8 *cb, u8 cb_len) {
	s32 retval;

	if (!cb_len || cb_len > 16)
		return IPC_EINVAL;
	memset(d->cbw, 0, CBW_SIZE);
	__stwbrx(d->cbw, 0, CBW_SIGNATURE);
	__stwbrx(d->cbw, 4, ++d->tag);
	__stwbrx(d->cbw, 8, len);
	d->cbw[12] = flags;
	d->cbw[13] = lun;
	d->cbw[14] = cb_len > 6 ? 10 : 6;
	memcpy(d->cbw + 15, cb, cb_len);
	if (d->suspended) {
		USB_ResumeDevice(d->fd);
		d->suspended = false;
	}
	retval = bulk(d, d->ep_out, CBW_SIZE, d->cbw, d->timeout_s);
	if (retval == CBW_SIZE)
		return USBSTORAGE_OK;
	return retval > 0 ? USBSTORAGE_ESHORTWRITE : retval;
}

static s32 read_csw(drive *d, u8 *status, u32 *residue, u32 secs) {
	s32 retval;

	memset(d->cbw, 0, CSW_SIZE);
	retval = bulk(d, d->ep_in, CSW_SIZE, d->cbw, secs);
	if (retval > 0 && retval != CSW_SIZE)
		return USBSTORAGE_ESHORTREAD;
	if (retval < 0)
		return retval;
	if (__lwbrx(d->cbw, 0) != CSW_SIGNATURE)
		return USBSTORAGE_ESIGNATURE;
	if (residue)
		*residue = __lwbrx(d->cbw, 8);
	if (status)
		*status = d->cbw[12];
	return __lwbrx(d->cbw, 4) == d->tag ? USBSTORAGE_OK : USBSTORAGE_ETAG;
}

static s32 msd_reset(drive *d) {
	u32 t = d->timeout_s;
	s32 retval;

	d->timeout_s = 1;
	retval = control(d, USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_CLASS |
					 USB_CTRLTYPE_REC_INTERFACE, MSD_RESET, 0, d->interface, 0, NULL);
	d->timeout_s = t;
	usleep(60 * 1000);
	USB_ClearHalt(d->fd, d->ep_in);
	usleep(10000);
	USB_ClearHalt(d->fd, d->ep_out);
	usleep(10000);
	return retval;
}

static s32 cycle(drive *d, u8 lun, u8 *buffer, u32 len, const u8 *cb, u8 cb_len, bool write,
				 u8 *status_out) {
	u16 max = d->usb2 ? MAX_TRANSFER_V5 : MAX_TRANSFER_V0;
	u8 ep = write ? d->ep_out : d->ep_in, status = 0;
	s32 retval = USBSTORAGE_OK, retries = CYCLE_RETRIES + 1;

	LWP_MutexLock(d->lock);
	do {
		u8 *p = buffer;
		u32 left = len;

		retries--;
		if (retval == USBSTORAGE_ETIMEDOUT)
			break;
		retval = send_cbw(d, lun, len, write ? CBW_OUT : CBW_IN, cb, cb_len);
		while (left > 0 && retval >= 0) {
			u32 n = left > max ? max : left;

			// Straight from the caller's buffer when IOS can use it (32-byte
			// aligned, in MEM2), else through the drive's own.
			if (((u32) p & 0x1f) || !((u32) p & 0x10000000)) {
				if (write)
					memcpy(d->buffer, p, n);
				retval = bulk(d, ep, n, d->buffer, d->timeout_s);
				if (!write && retval > 0)
					memcpy(p, d->buffer, retval);
			} else {
				retval = bulk(d, ep, n, p, d->timeout_s);
			}
			if (retval == (s32) n) {
				left -= n;
				p += n;
			} else if (retval != USBSTORAGE_ETIMEDOUT) {
				retval = USBSTORAGE_EDATARESIDUE;
			}
		}
		if (retval >= 0)
			retval = read_csw(d, &status, NULL, d->timeout_s);
		if (retval < 0 && msd_reset(d) == USBSTORAGE_ETIMEDOUT)
			retval = USBSTORAGE_ETIMEDOUT;
	} while (retval < 0 && retries > 0);
	LWP_MutexUnlock(d->lock);
	if (status_out)
		*status_out = status;
	return retval;
}

// ---- SCSI ---------------------------------------------------------------------

static s32 clear_errors(drive *d, u8 lun) {
	u8 cmd[6] = { SCSI_TEST_UNIT_READY }, sense[SCSI_SENSE_REPLY_SIZE], status = 0;
	s32 retval = cycle(d, lun, NULL, 0, cmd, 1, false, &status);

	if (retval < 0 || !status)
		return retval;
	cmd[0] = SCSI_REQUEST_SENSE;
	cmd[1] = lun << 5;
	cmd[4] = SCSI_SENSE_REPLY_SIZE;
	memset(sense, 0, sizeof(sense));
	retval = cycle(d, lun, sense, sizeof(sense), cmd, 6, false, NULL);
	if (retval >= 0)
		switch (sense[2] & 0xf) {
		case SCSI_SENSE_NOT_READY:
			return USBSTORAGE_EINIT;
		case SCSI_SENSE_MEDIUM_ERROR:
		case SCSI_SENSE_HARDWARE_ERROR:
			return USBSTORAGE_ESENSE;
		}
	return retval;
}

static s32 read_capacity(drive *d, u8 lun, u32 *sector_size, u32 *n_sectors) {
	u8 cmd[10] = { SCSI_READ_CAPACITY, lun << 5 }, reply[8];
	s32 retval = cycle(d, lun, reply, sizeof(reply), cmd, sizeof(cmd), false, NULL);

	if (retval < 0)
		return retval;
	if (n_sectors)
		memcpy(n_sectors, reply, 4);
	if (sector_size)
		memcpy(sector_size, reply + 4, 4);
	return USBSTORAGE_OK;
}

static s32 mount_lun(drive *d, u8 lun) {
	u8 cmd[6] = { SCSI_INQUIRY, lun << 5, 0, 0, 36, 0 }, reply[36];
	u32 n_sectors = 0, size = 0;
	s32 retval;
	int i;

	if (lun >= d->max_lun)
		return IPC_EINVAL;
	usleep(50);
	if (clear_errors(d, lun) < 0) {
		LWP_MutexLock(d->lock);
		msd_reset(d);
		LWP_MutexUnlock(d->lock);
		clear_errors(d, lun);
	}
	for (i = 0; i < 2; ++i)
		if (cycle(d, lun, reply, sizeof(reply), cmd, sizeof(cmd), false, NULL) >= 0)
			break;
	retval = read_capacity(d, lun, &size, &n_sectors);
	d->sector_seen = size;
	if (retval >= 0 && (size != 512 || !n_sectors))
		return INVALID_LUN;
	if (retval >= 0)
		d->sector_size = size;
	return retval;
}

static s32 rw(drive *d, u32 sector, u16 n, u8 *buffer, bool write) {
	u8 cmd[10] = { write ? SCSI_WRITE_10 : SCSI_READ_10, d->lun << 5, sector >> 24,
				   sector >> 16, sector >> 8, sector, 0, n >> 8, n, 0 };
	u8 status = 0;
	s32 retval;

	// Asleep after a minute idle: wake it with the drive's own long timeout.
	if (ticks_to_secs(gettime() - d->last_used) > 60) {
		d->timeout_s = 10;
		mount_lun(d, d->lun);
	}
	retval = cycle(d, d->lun, buffer, n * d->sector_size, cmd, sizeof(cmd), write, &status);
	if (retval > 0 && status)
		retval = USBSTORAGE_ESTATUS;
	d->last_used = gettime();
	d->timeout_s = TIMEOUT_S;
	return retval;
}

// ---- Opening and closing a drive -----------------------------------------------

static s32 removed(s32 retval, void *arg) {
	drive *d = arg;

	(void) retval;
	d->mounted = false;
	return 0;
}

static void close_drive(drive *d) {
	d->mounted = false;
	if (d->fd != -1)
		USB_CloseDevice(&d->fd);
	d->fd = -1;
	d->vid = d->pid = 0;
	d->device_id = 0;
}

static s32 open_drive(drive *d, s32 device_id, u16 vid, u16 pid) {
	static u8 max_lun[32] ATTRIBUTE_ALIGN(32);
	usb_devdesc udd;
	usb_configurationdesc *ucd;
	usb_interfacedesc *uid = NULL;
	u32 c, i, e;
	u8 conf = 0xff;
	s32 retval;
	bool found = false;

	d->fd = -1;
	d->tag = TAG_START;
	d->timeout_s = TIMEOUT_S;
	d->suspended = false;
	if (USB_OpenDevice(device_id, vid, pid, &d->fd) < 0) {
		d->fd = -1;
		return -1;
	}
	if (USB_GetDescriptors(d->fd, &udd) < 0) {
		close_drive(d);
		return -1;
	}
	for (c = 0; c < udd.bNumConfigurations && !found; ++c) {
		ucd = &udd.configurations[c];
		for (i = 0; i < ucd->bNumInterfaces && !found; ++i) {
			uid = &ucd->interfaces[i];
			if (uid->bInterfaceClass != CLASS_MASS_STORAGE ||
					uid->bInterfaceProtocol != BULK_ONLY || uid->bNumEndpoints < 2)
				continue;
			d->ep_in = d->ep_out = 0;
			for (e = 0; e < uid->bNumEndpoints; ++e) {
				usb_endpointdesc *ued = &uid->endpoints[e];

				if (ued->bmAttributes != ENDPOINT_BULK)
					continue;
				if (ued->bEndpointAddress & USB_ENDPOINT_IN) {
					d->ep_in = ued->bEndpointAddress;
				} else {
					d->ep_out = ued->bEndpointAddress;
					d->usb2 = ued->wMaxPacketSize > 64 && (d->fd >= 0x20 || d->fd < -1);
				}
			}
			if (d->ep_in && d->ep_out) {
				d->configuration = ucd->bConfigurationValue;
				d->interface = uid->bInterfaceNumber;
				d->alt_interface = uid->bAlternateSetting;
				found = true;
			}
		}
	}
	USB_FreeDescriptors(&udd);
	if (!found) {
		close_drive(d);
		return USBSTORAGE_ENOINTERFACE;
	}
	USB_GetConfiguration(d->fd, &conf);   // some drives fail this; ignored
	if (conf != d->configuration)
		USB_SetConfiguration(d->fd, d->configuration);
	if (d->alt_interface)
		USB_SetAlternativeInterface(d->fd, d->interface, d->alt_interface);
	if (!d->usb2) {
		LWP_MutexLock(d->lock);
		msd_reset(d);
		LWP_MutexUnlock(d->lock);
	}
	LWP_MutexLock(d->lock);
	retval = control(d, USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS |
					 USB_CTRLTYPE_REC_INTERFACE, GET_MAX_LUN, 0, d->interface, 1, max_lun);
	LWP_MutexUnlock(d->lock);
	if (retval == USBSTORAGE_ETIMEDOUT) {
		close_drive(d);
		return retval;
	}
	d->max_lun = retval < 0 ? 1 : max_lun[0] + 1;
	d->device_id = device_id;
	d->vid = vid;
	d->pid = pid;
	USB_DeviceRemovalNotifyAsync(d->fd, removed, d);
	return USBSTORAGE_OK;
}

// ---- The disc interfaces -----------------------------------------------------

static bool startup(void) {
	u32 level, i;
	u8 *mem;

	if (USB_Initialize() < 0)
		return false;
	_CPU_ISR_Disable(level);
	if (!inited) {
		// The drives' buffers from the top of MEM2, as libogc's driver does.
		mem = (u8 *) (((u32) SYS_GetArena2Hi() - USBMSD_DRIVES * DRIVE_MEM) & ~31);
		if ((u32) mem < (u32) SYS_GetArena2Lo()) {
			_CPU_ISR_Restore(level);
			return false;
		}
		SYS_SetArena2Hi(mem);
		LWP_InitQueue(&waitq);
		for (i = 0; i < USBMSD_DRIVES; ++i) {
			drives[i].fd = -1;
			drives[i].cbw = mem + i * DRIVE_MEM;
			drives[i].buffer = drives[i].cbw + CBW_BUF;
			LWP_MutexInit(&drives[i].lock, false);
			SYS_CreateAlarm(&drives[i].alarm);
		}
		inited = true;
	}
	_CPU_ISR_Restore(level);
	return true;
}

// The other slot has this device.
static bool claimed(int slot, s32 device_id) {
	int i;

	for (i = 0; i < USBMSD_DRIVES; ++i)
		if (i != slot && drives[i].fd != -1 && drives[i].device_id == device_id)
			return true;
	return false;
}

static void note(drive *d, const char *why, u16 vid, u16 pid, s32 code) {
	d->why = why;
	d->why_vid = vid;
	d->why_pid = pid;
	d->why_code = code;
}

static bool is_inserted(int slot) {
	static usb_device_entry list[DEVLIST_MAX] ATTRIBUTE_ALIGN(32);
	drive *d = &drives[slot];
	u8 count = 0, i, lun;
	s32 res;

	if (!inited)
		return false;
	if (d->mounted)
		return true;
	if ((res = USB_GetDeviceList(list, DEVLIST_MAX, CLASS_MASS_STORAGE, &count)) < 0) {
		note(d, "no device list", 0, 0, res);
		close_drive(d);
		return false;
	}
	note(d, count ? "every drive taken or unusable" : "no mass storage device", 0, 0, count);
	usleep(100);
	// Ours before: still listed means a brief removal notice (it comes back
	// mounted); gone means unplugged, and the caller unmounts on this false.
	if (d->fd != -1) {
		for (i = 0; i < count; ++i)
			if (list[i].device_id == d->device_id && list[i].vid == d->vid &&
					list[i].pid == d->pid) {
				d->mounted = true;
				usleep(50);
				return true;
			}
		close_drive(d);
		return false;
	}
	for (i = 0; i < count; ++i) {
		if (!list[i].vid || !list[i].pid || claimed(slot, list[i].device_id))
			continue;
		if (list[i].vid == 0x0b95 && list[i].pid == 0x7720)   // a USB LAN adapter
			continue;
		if ((res = open_drive(d, list[i].device_id, list[i].vid, list[i].pid)) < 0) {
			note(d, "could not open it", list[i].vid, list[i].pid, res);
			continue;
		}
		for (lun = 0; lun < d->max_lun; ++lun) {
			s32 retval = mount_lun(d, lun);

			if (retval == INVALID_LUN) {
				note(d, "no 512-byte-sector LUN", list[i].vid, list[i].pid, d->sector_seen);
				continue;
			}
			if (retval < 0)
				note(d, "LUN did not answer", list[i].vid, list[i].pid, retval);
			if (retval < 0) {
				LWP_MutexLock(d->lock);
				msd_reset(d);
				LWP_MutexUnlock(d->lock);
				continue;
			}
			d->lun = lun;
			d->mounted = true;
			note(d, NULL, 0, 0, 0);
			d->last_used = gettime() - secs_to_ticks(100);
			usleep(10000);
			break;
		}
		if (d->mounted)
			break;
		close_drive(d);
	}
	return d->mounted;
}

static bool read_sectors(int slot, sec_t sector, sec_t n, void *buffer) {
	drive *d = &drives[slot];

	while (d->mounted && n) {
		u16 part = n > 0xffff ? 0xffff : n;

		if (rw(d, sector, part, buffer, false) < 0)
			return false;
		sector += part;
		n -= part;
		buffer = (u8 *) buffer + part * 512;
	}
	return d->mounted;
}

static bool write_sectors(int slot, sec_t sector, sec_t n, const void *buffer) {
	drive *d = &drives[slot];

	while (d->mounted && n) {
		u16 part = n > 0xffff ? 0xffff : n;

		if (rw(d, sector, part, (u8 *) buffer, true) < 0)
			return false;
		sector += part;
		n -= part;
		buffer = (const u8 *) buffer + part * 512;
	}
	return d->mounted;
}

static bool clear_status(void) {
	return true;
}

static bool shutdown(int slot) {
	if (inited)
		close_drive(&drives[slot]);
	return true;
}

// libogc 3's disc interfaces take no argument: one set of entry points a slot.
#define SLOT(n) \
	static bool inserted##n(void) { return is_inserted(n); } \
	static bool read##n(sec_t s, sec_t c, void *b) { return read_sectors(n, s, c, b); } \
	static bool write##n(sec_t s, sec_t c, const void *b) { return write_sectors(n, s, c, b); } \
	static bool shutdown##n(void) { return shutdown(n); }
SLOT(0)
SLOT(1)

DISC_INTERFACE usbmsd_disc[USBMSD_DRIVES] = {
	{ DEVICE_TYPE_WII_USB, FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_WII_USB,
	  startup, inserted0, read0, write0, clear_status, shutdown0 },
	{ DEVICE_TYPE_WII_USB, FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_WII_USB,
	  startup, inserted1, read1, write1, clear_status, shutdown1 },
};

int usbmsd_json(char *buf, int size) {
	int n = snprintf(buf, size, ",\"usb_drives\":["), i;

	for (i = 0; i < USBMSD_DRIVES; ++i) {
		drive *d = &drives[i];

		if (d->mounted)
			n += snprintf(buf + n, size - n, "%s{\"vid\":\"%04x\",\"pid\":\"%04x\","
						  "\"lun\":%u,\"usb2\":%s}", i ? "," : "", d->vid, d->pid, d->lun,
						  d->usb2 ? "true" : "false");
		else
			n += snprintf(buf + n, size - n, "%s{\"why\":\"%s\",\"vid\":\"%04x\","
						  "\"pid\":\"%04x\",\"code\":%d}", i ? "," : "",
						  d->why ? d->why : "not looked for", d->why_vid, d->why_pid,
						  (int) d->why_code);
	}
	return n + snprintf(buf + n, size - n, "]");
}

bool usbmsd_info(int slot, u16 *vid, u16 *pid, u32 *sectors) {
	drive *d = &drives[slot];
	u32 size;

	if (slot < 0 || slot >= USBMSD_DRIVES || !d->mounted)
		return false;
	*vid = d->vid;
	*pid = d->pid;
	if (sectors && read_capacity(d, d->lun, &size, sectors) < 0)
		*sectors = 0;
	return true;
}
