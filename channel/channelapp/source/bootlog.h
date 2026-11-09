#ifndef _BOOTLOG_H_
#define _BOOTLOG_H_

#include <gctypes.h>

// Timed breadcrumbs around the blocking IOS calls of HBC's start-up (USB,
// ISFS, ES, WPAD, the network, the drive scan). A call logged in with no
// way out names the one IOS never answered; the calls that started after it
// and are still in show what waits behind it.
//
// They are kept in memory and reach the PC three ways:
//  - `hbc.py status`, "calls" (in and out in ms since the start of main,
//    the thread, the return code), once HBC answers on the network;
//  - the TV, drawn by bootscreen.h when the menu is late;
//  - DEV > Log and `hbc.py log`: one line for each call that took 50 ms or
//    more, as it comes back.
//
// Times are in ms on devnet_boot_mark's clock (devnet_early_init).

// Logs the call in; returns its slot for bootlog_out (-1 when none is free).
// The name must stay valid: it is kept, not copied.
int bootlog_in(const char *call);
// Logs the call out with its return code.
void bootlog_out(int slot, s32 rc);
// Logs a call that has already finished (a register write, say).
void bootlog_note(const char *call, s32 rc);

// rc = BOOTLOG("name", expr): expr timed, its value as the return code.
#define BOOTLOG(call, expr) ({ \
	int _bl_slot = bootlog_in(call); \
	s32 _bl_rc = (s32) (expr); \
	bootlog_out(_bl_slot, _bl_rc); \
	_bl_rc; })
// The same for a call with no value.
#define BOOTLOG_VOID(call, stmt) do { \
	int _bl_slot = bootlog_in(call); \
	stmt; \
	bootlog_out(_bl_slot, 0); } while (0)

// Names the calling thread in the log ("main", "net", "scan", ...).
void bootlog_thread(const char *name);

// Names the main thread and opens IOS's NAND, ES, SD and network nodes
// without waiting, each logged as a "probe" call: one IOS never answers
// stays in. USB gets no probe here (see usbstart.h).
void bootlog_start(void);

// The menu is about to draw its first frame. Calls logged from now on may
// reuse the slots of finished ones. Logs the "first_frame" startup mark.
void bootlog_menu_up(void);

// One call, as bootlog_next copies it.
typedef struct {
	const char *call;
	char thread[12];	// the name given to bootlog_thread, else the thread id in hex
	u32 in_ms, out_ms;
	s32 rc;
	u32 seq;
	bool done;		// false: IOS has not answered; out_ms and rc mean nothing
} bootlog_row;

// The call that began right after the one numbered `after` (0: the first),
// copied under the lock; false when there is none. Walking the log this way
// shows each row whole while slots are reused.
bool bootlog_next(u32 after, bootlog_row *row);

// ,"calls":{...} for HBCS.
int bootlog_json(char *buf, int size);

#endif
