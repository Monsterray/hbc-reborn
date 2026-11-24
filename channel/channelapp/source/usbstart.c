#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>

#include "../config.h"
#include "bootlog.h"
#include "devnet.h"
#include "usbguard.h"
#include "usbstart.h"

static bool usb_stuck, usb_reloaded, usb_unplugged, usb_replugged;

// The requests WPAD_Init makes first, in its order: libogc's USB_Initialize
// opens /dev/usb/ven (USB 2.0) and asks its version, then the Bluetooth
// device is opened. A request to the USB 1.1 side alone (/dev/usb/oh1) can
// answer while the USB 2.0 side is stuck, and WPAD_Init would then wait for
// the drive. They run on usbguard's worker: one that never answers keeps it,
// and stays logged in.
static s32 usb_probe(void *arg) {
	static const char ven[] ATTRIBUTE_ALIGN(32) = "/dev/usb/ven";
	static const char bt[] ATTRIBUTE_ALIGN(32) = "/dev/usb/oh1/57e/305";
	static u32 version[8] ATTRIBUTE_ALIGN(32);
	s32 ven_fd, bt_fd;

	(void) arg;
	bootlog_thread("usbguard");
	ven_fd = BOOTLOG("probe /dev/usb/ven", IOS_Open(ven, 0));
	if (ven_fd >= 0)
		BOOTLOG("probe ven version", IOS_Ioctl(ven_fd, 0, NULL, 0, version, sizeof(version)));
	bt_fd = BOOTLOG("probe /dev/usb/oh1/57e/305", IOS_Open(bt, 0));
	if (ven_fd >= 0)
		IOS_Close(ven_fd);
	if (bt_fd >= 0)
		IOS_Close(bt_fd);
	return 0;
}

// No answer within warn_ms (the phase turns STUCK): give up. The worker stays
// in the request and HBC goes on without USB.
static bool usb_check_wait(u32 ms, usbguard_phase phase, void *ctx) {
	(void) ms;
	(void) ctx;
	return phase == USBGUARD_PHASE_WAITING;
}

bool usbstart_check(u32 ms) {
	usbguard_opts o = USBGUARD_OPTS_INIT;

	o.warn_ms = ms;
	o.poll_ms = 5;
	o.on_wait = usb_check_wait;
	o.no_retry = true;
	usb_stuck = usbguard_call(usb_probe, NULL, &o) == USBGUARD_STUCK;
#ifdef HBC_TEST_USB_STUCK
	// Test builds only: as if USB had not answered (Dolphin always does).
	usb_stuck = true;
#endif
	return !usb_stuck;
}

// The EHCI state at each step, for HBCS "ehci": the controller's command,
// status and async list head. (IOS's queue heads, in its MEM2, read back as
// ffffffff on the PPC, so they are not dumped.) Read only with AHBPROT.
#define EHCI_BASE 0xcd040000
#define PORTS_LOGGED 4

static char ehci_dump[512];
static int ehci_len;
static char port_names[PORTS_LOGGED][40];

static void ehci_state(const char *when) {
	u32 op = EHCI_BASE + (read32(EHCI_BASE) & 0xff);
	int start = ehci_len;

	if (start >= (int) sizeof(ehci_dump) - 80)
		return;
	ehci_len += snprintf(ehci_dump + start, sizeof(ehci_dump) - start,
		"%s%s: cmd %08x sts %08x async %08x", start ? "; " : "", when,
		(unsigned) read32(op), (unsigned) read32(op + 4), (unsigned) read32(op + 0x18));
	// Only this step: the dump holds the earlier ones too.
	hlog("Start-up: EHCI %s\n", ehci_dump + start + (start ? 2 : 0));
}

int usbstart_ehci_json(char *buf, int size) {
	int n;

	if (!ehci_len)
		return 0;
	n = snprintf(buf, size, ",\"ehci\":\"%s\"", ehci_dump);
	return n > 0 && n < size ? n : 0;
}

// usbguard_free disables every EHCI port with a drive (PE cleared), so the
// transfer the drive NAKs for ever fails as when it is pulled. Handing the
// port to the companion controller does not free it, and a port reset is not
// needed. The port stays disabled: IOS does not see the drive again until it
// is replugged.
bool usbstart_unplug(u32 ms) {
	usbguard_port_change ch[PORTS_LOGGED];
	u32 start;
	int i, n;

	if (!usbguard_can_free()) {
		hlog("Start-up: no AHBPROT: no USB unplug\n");
		return false;
	}
	ehci_state("stuck");
	if (!usbguard_free()) {	// nothing to unplug: main_pre falls back to IOS80
		hlog("Start-up: no EHCI port has a drive: no USB unplug\n");
		return false;
	}
	n = usbguard_freed_ports(ch, PORTS_LOGGED);
	for (i = 0; i < n; ++i) {
		snprintf(port_names[i], sizeof(port_names[i]), "ehci port %u %04x disable",
				 (unsigned) ch[i].port, (unsigned) (ch[i].before & 0xffff));
		bootlog_note(port_names[i], (s32) ch[i].after);
		hlog("Start-up: EHCI port %u disable: %08x -> %08x\n", (unsigned) ch[i].port,
			 (unsigned) ch[i].before, (unsigned) ch[i].after);
	}
	start = devnet_boot_ms();
	while (usbguard_blocked() && devnet_boot_ms() - start < ms)
		usleep(5 * 1000);
	usb_unplugged = !usbguard_blocked();
	hlog("Start-up: after the port disable the stuck USB request %s in %u ms\n",
		 usb_unplugged ? "answered" : "did not answer", (unsigned) (devnet_boot_ms() - start));
	ehci_state("after disable");
	return usb_unplugged;
}

bool usbstart_stuck(void) {
	return usb_stuck;
}

bool usbstart_unplugged(void) {
	return usb_unplugged;
}

void usbstart_ios_reloaded(void) {
	usb_reloaded = true;
}

bool usbstart_remotes_off(void) {
	return usb_stuck && !usb_reloaded && !usb_unplugged;
}

// usbguard_pulled: a connect change or no device on a port usbguard_free
// disabled. A port IOS re-enables does not count: IOS may re-enable the stuck
// drive itself. With no port disabled (no AHBPROT, the IOS80 fallback) it
// stays true.
bool usbstart_replug_needed(void) {
	if (!usb_stuck || usb_replugged)
		return false;
	if (!usbguard_pulled())
		return true;
	usb_replugged = true;
	hlog("USB: the stuck drive was unplugged\n");
	return false;
}
