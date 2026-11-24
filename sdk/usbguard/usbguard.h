/* usbguard: survive a stuck USB drive, for libogc programs.
 *
 * A USB drive that a reset left mid-transfer can stay powered and NAK every
 * packet for ever. The Wii's EHCI controller cannot switch port power, so
 * IOS's USB driver waits on that transfer for good, and while it waits every
 * other IOS request waits behind it: files (FS, SD), ES, the network,
 * Bluetooth (so no Wii Remotes) and IOS_ReloadIOS. Only two things end it:
 * pulling the drive, or disabling its port in the EHCI controller, which
 * needs AHBPROT (HW_AHBPROT reads ffffffff).
 *
 * usbguard runs the calls that can meet that drive on a worker thread it
 * owns, and keeps your thread free to tell the user what to do:
 *
 *     usbguard_opts o = USBGUARD_OPTS_INIT;
 *     o.on_wait = usbguard_default_ui;          // or your own
 *     s32 rc = usbguard_mount("usb", &__io_usbstorage, &o);
 *     if (rc == USBGUARD_STUCK) ...             // gave up; see "Results"
 *     if (rc == USBGUARD_FREED) ...             // freed, but nothing mounted
 *
 * What happens:
 *   - The call runs on a worker. While it runs, on_wait(ms, phase, ctx) is
 *     called on your thread about every poll_ms (default 50 ms). The phase is
 *     USBGUARD_PHASE_WAITING, then USBGUARD_PHASE_STUCK from warn_ms
 *     (default 5000 ms). Return false from on_wait to give up: the call then
 *     returns USBGUARD_STUCK and the worker stays blocked (see "The worker").
 *   - When the call comes back after STUCK with an error, the drive was
 *     pulled or usbguard_free() cut it off. If usbguard_free() did it, the
 *     result is USBGUARD_FREED. After a pull the library waits for the
 *     drive to be plugged back in (USBGUARD_PHASE_REPLUG) and runs the call
 *     once more; if that fails or gets stuck too, USBGUARD_FREED. A stuck
 *     call that answers successfully as the drive is pulled returns its
 *     result, after the same wait for the replug (with AHBPROT).
 *   - d2x cIOS rc3 does not block: it gives up on the drive after about
 *     7 s with -101 (USBGUARD_D2X_GAVE_UP). A -101 that came in under about
 *     6.5 s is returned as it is. With AHBPROT, a slower one while a drive
 *     is connected is taken as stuck: the library waits (phase STUCK) for the
 *     pull, then the replug, then retries once. Without AHBPROT a slow -101
 *     returns USBGUARD_STUCK.
 *   - Which USB 2.0 device is the drive cannot be told from the EHCI
 *     registers. A call notes every port with a device when it starts; a pull
 *     is one of those ports losing its device. The replug is every one of
 *     them back, or, if the drive was put into another port, a device on a
 *     port that was empty at the start for each one that is still missing. So
 *     a second device that stays on another port does not hide the pull. A
 *     hub, or a drive behind one, is one device: the pull is seen when the
 *     hub goes. usbguard_free() cuts off every port, so a hub goes with
 *     everything on it, and any other USB 2.0 device (a LAN adapter, a
 *     second drive, a keyboard) with it.
 *
 * Rules for on_wait (the waiting path makes no IOS call, and neither may you):
 *   - No IOS call of any kind: no file access (SD or NAND; stdout redirected
 *     to a file counts), no network, no WPAD, no IOS_ReloadIOS, no sound
 *     that streams from a file. Each would wait behind the stuck request.
 *   - Use only assets that are already loaded: fonts, textures, sounds in
 *     memory. Drawing with GX, writing to the XFB, printing to a text
 *     console and reading the GameCube controller (PAD_*, over SI) are fine.
 *   - Wii Remotes do not work while USB is stuck (Bluetooth is a USB
 *     device in IOS). Read the GameCube controller instead, or say that the
 *     drive has to be unplugged.
 *   - Offer "continue without USB" only when usbguard_can_free() is true;
 *     it calls usbguard_free(). usbguard never cuts off a drive on its own.
 *
 * Results. Every value that is not one of the USBGUARD_* codes below is the
 * call's own result. USBGUARD_STUCK means "gave up while stuck" and comes
 * back in three ways, so do not read it as "a request is still in IOS":
 *   - on_wait returned false (the worker is still blocked: usbguard_blocked()
 *     is true until it comes back);
 *   - no worker was free (the call did not run);
 *   - after the worker returned. The slow -101 case without AHBPROT, or with
 *     AHBPROT when on_wait gave up or replug_ms ran out while waiting for the
 *     pull, returns STUCK with the worker already gone: usbguard_blocked() is
 *     false, but the drive is still in and probably still not answering.
 *
 * The worker: a call you gave up on keeps its worker thread and stack until
 * the request comes back (a pull or usbguard_free). They are never reused or
 * freed while blocked. USBGUARD_WORKERS calls can be blocked at once; a call
 * with no free worker returns USBGUARD_STUCK without running. A call made
 * while another is blocked starts in phase STUCK.
 *
 * Threads: make the guarded calls from one thread at a time. The worker slots
 * are protected, but the free and pull records (usbguard_free, usbguard_pulled,
 * usbguard_freed_ports) and usbguard_default_ui keep plain globals.
 *
 * The decision logic builds on a PC with -DUSBGUARD_NO_OGC, as the other SDKs'
 * *_NO_OGC switches: threads and time come from the host, the EHCI registers
 * from a test program. See tests/host/usbguard_host, which also supplies stand-in
 * ogc/disc_io.h and fat.h headers.
 *
 * Public domain: see the end of usbguard.c.
 */

#ifndef USBGUARD_H
#define USBGUARD_H

#include <stdbool.h>
#include <stdint.h>
#include <ogc/disc_io.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USBGUARD_VERSION "1.0.0"

/* Results; see the comment at the top. */
#define USBGUARD_STUCK (-0x7547)        /* gave up on a request that is not answering */
#define USBGUARD_FREED (-0x7546)        /* the stuck request ended, but the call did not succeed */
#define USBGUARD_NO_WORKER (-0x7545)    /* no stack for a worker (out of memory) */
#define USBGUARD_NO_DRIVE (-0x7544)     /* usbguard_mount: startup or isInserted said no */
#define USBGUARD_NOT_MOUNTED (-0x7543)  /* usbguard_mount: fatMount failed */

/* d2x cIOS rc3's result when it gives up on a drive that does not answer. */
#define USBGUARD_D2X_GAVE_UP (-101)

/* The text to show while a drive is stuck. */
#define USBGUARD_MSG_STUCK "The USB drive is not responding. Unplug it and plug it back in."

#define USBGUARD_WORKERS 4               /* calls that can be blocked at once */
#define USBGUARD_STACK_SIZE (32 * 1024)  /* each worker's stack (fatMount needs some) */

typedef enum {
	USBGUARD_PHASE_WAITING,	/* the call runs; nothing wrong yet */
	USBGUARD_PHASE_STUCK,	/* warn_ms passed (or the drive gave up): ask for a replug */
	USBGUARD_PHASE_REPLUG,	/* the drive came out: waiting for it to be plugged in */
} usbguard_phase;

typedef enum {
	USBGUARD_OK,		/* no connected port is disabled */
	USBGUARD_CUT_OFF,	/* a drive is connected but its port is disabled (CCS=1, PE=0) */
	USBGUARD_NO_ACCESS,	/* no AHBPROT: the EHCI registers cannot be read */
} usbguard_state;

/* Called on the caller's thread while the call runs. ms counts from the
 * start of the call's current attempt. Return true to keep waiting, false
 * to give up (WAITING or STUCK: the result is USBGUARD_STUCK; REPLUG: no
 * retry, USBGUARD_FREED). See the rules above. */
typedef bool (*usbguard_wait_fn)(uint32_t ms, usbguard_phase phase, void *ctx);
typedef int32_t (*usbguard_fn)(void *arg);

typedef struct {
	uint32_t warn_ms;	/* 0: 5000. When the phase turns STUCK. */
	uint32_t poll_ms;	/* 0: 50. How often on_wait is called. */
	uint32_t replug_ms;	/* 0: 10000. Without AHBPROT the replug cannot be seen:
				   how long REPLUG lasts before the retry. With no
				   on_wait it also ends the wait for a pull (STUCK)
				   and for a replug (FREED) with AHBPROT. */
	usbguard_wait_fn on_wait;	/* NULL: silent; the waits for a pull or a replug
				   end after replug_ms. A request that is still
				   in IOS is waited for until it answers. */
	void *ctx;		/* passed to on_wait */
	bool no_retry;		/* true: no replug wait and retry after a pull */
	uint8_t priority;	/* the worker's LWP priority; 0: 64 */
} usbguard_opts;

#define USBGUARD_OPTS_INIT { 0, 0, 0, 0, 0, false, 0 }

/* Runs fn(arg) on a worker, guarded as above. opts may be NULL (defaults). */
int32_t usbguard_call(usbguard_fn fn, void *arg, const usbguard_opts *opts);

/* iface->startup(), iface->isInserted() and fatMountSimple(name, iface) on
 * the worker. 0: mounted. Else USBGUARD_NO_DRIVE, USBGUARD_NOT_MOUNTED,
 * USBGUARD_STUCK or USBGUARD_FREED. A startup that fails after warn_ms or
 * more is taken as d2x's -101 (see above). */
int32_t usbguard_mount(const char *name, const DISC_INTERFACE *iface, const usbguard_opts *opts);

#ifndef USBGUARD_NO_OGC
/* IOS_ReloadIOS(ios) on the worker. A reload waits behind a stuck request
 * too, for as long as the request does (minutes). If you give up, IOS is
 * half shut down: leave by a pull or usbguard_free() only. Returns
 * IOS_ReloadIOS's result (a reload that finished passes even when
 * usbguard_free cut the wait short), USBGUARD_STUCK or USBGUARD_FREED. No
 * retry. */
int32_t usbguard_reload_ios(int ios, const usbguard_opts *opts);

/* A ready-made on_wait for apps with a text console (printf). It prints
 * nothing while the phase is WAITING. When STUCK it prints
 * USBGUARD_MSG_STUCK, that Wii Remotes do not work now and which
 * GameCube controller buttons do; it reads the controller directly over SI
 * (PAD_ScanPads; it calls PAD_Init once). A: continue without USB
 * (usbguard_free), offered only when usbguard_can_free(). In REPLUG,
 * B: go on without the drive. ctx is unused. It starts afresh for each
 * attempt of each call. */
bool usbguard_default_ui(uint32_t ms, usbguard_phase phase, void *ctx);
#endif

/* Reads every EHCI port: CUT_OFF when one has a drive connected (CCS=1)
 * but disabled (PE=0). A port can read so for a moment while IOS
 * enumerates a new drive. NO_ACCESS without AHBPROT. */
usbguard_state usbguard_check(void);

/* true: AHBPROT is open, so usbguard_free and the port checks work. */
bool usbguard_can_free(void);

/* Disables (clears PE on) every EHCI port with a connected drive. The
 * write-1-to-clear bits are masked, except the connect change, which is
 * cleared on purpose so that a later one means the drive came out
 * (usbguard_pulled). The stuck transfer then fails as if the drive had been
 * pulled. The drive is not seen again until it is replugged. Needs AHBPROT.
 * Only ever runs when you call it. true: at least one connected port is
 * now disabled. */
bool usbguard_free(void);

/* true once a port usbguard_free disabled shows a device taken out (a
 * connect change, or no device). One port is enough: usbguard_free cuts
 * every port with a device, and the user is asked to unplug the drive. False
 * if usbguard_free never ran. */
bool usbguard_pulled(void);

/* true while a call's worker has been in its request past warn_ms, or was
 * given up on, and has not come back. */
bool usbguard_blocked(void);

/* 1: a USB 2.0 device is connected to an EHCI port (any device, not only
 * the drive). 0: none. -1: no AHBPROT. */
int usbguard_drive_present(void);

/* What the latest usbguard_free did, for logs: each port it wrote, PORTSC
 * before and after. Every usbguard_free that has AHBPROT starts the list
 * again, so read it before freeing a second time. Returns how many it
 * filled (at most max). */
typedef struct {
	uint8_t port;
	uint32_t before, after;
} usbguard_port_change;
int usbguard_freed_ports(usbguard_port_change *out, int max);

#ifdef __cplusplus
}
#endif

#endif
