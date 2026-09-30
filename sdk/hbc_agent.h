/*
 * hbc_agent.h - keep the Homebrew Channel's developer tools working inside
 * a running app.
 *
 * Link libhbcagent.a (build it with `make -C sdk/hbc_agent`) and call
 * hbc_agent_init() once at startup. A low-priority thread then answers
 * `tools/hbc.py` on TCP port 4299 while the app runs:
 *
 *   hbc.py status              the app's name, uptime and memory
 *   hbc.py ls/get/put/rm/sync  files on any device the app has mounted
 *   hbc.py run new.dol         asks the app to exit to HBC, then sends new.dol
 *   hbc.py exit                asks the app to exit to HBC
 *   hbc.py crash               after a crash: exception, registers, backtrace
 *   hbc.py key / screen        drive the HOME overlay, grab the TV picture
 *
 * hbc_agent_home() (below) adds a HOME overlay in HBC's style: a status
 * strip with DEV, Exit, Shot and WiiMote menus over the game's last frame.
 * HBC's own HOME menu is built on it: channel/channelapp/source/home.c is a
 * commented example of every step.
 *
 * In the app:
 *
 *     #include <fat.h>
 *     #include "hbc_netlog.h"   // optional: stdout to the PC
 *     #include "hbc_agent.h"
 *
 *     int main(int argc, char **argv) {
 *         // ... video setup ...
 *         fatInitDefault();           // before the agent, for file access
 *         hbc_netlog_init();          // optional
 *         hbc_agent_init(NULL);
 *         while (!hbc_agent_exit_requested()) {
 *             // ... one frame ...
 *         }
 *         // ... save and clean up ...
 *         return 0;                   // back to HBC
 *     }
 *
 * The agent costs one thread and about 20 KiB while idle. A file transfer
 * borrows up to about 560 KiB (less without zlib) and frees it at the end.
 * The thread runs below the app's main thread, so it never takes frame time
 * from a thread that waits for the next frame; an app that never blocks
 * starves it, and the tools time out.
 *
 * Networking: the agent starts the network in its thread if the app has not.
 * libogc's net_init() hangs if it runs while another thread's start-up is in
 * progress, so an app that calls net_init() itself must call it before
 * hbc_agent_init(), or first call hbc_agent_net_wait(). hbc_netlog.h already
 * waits for a start-up in progress.
 *
 * Crashes: the agent records the exception, registers and a backtrace in a
 * small low-memory block, shows libogc's crash screen for a few seconds, and
 * returns to HBC, which reports it (`hbc.py crash`).
 *
 * Define HBC_AGENT_LAYOUT_ONLY to get only the crash block layout.
 */

#ifndef HBC_AGENT_H
#define HBC_AGENT_H

#include <gctypes.h>

/* The crash block sits in MEM2 next to hbc_netlog.h's kept copy: IOS clears
 * low memory when the reload stub boots the installed channel, and HBC reads
 * this before its own allocations could reach it. */
#define HBC_CRASH_ADDR 0x91800020
#define HBC_CRASH_MAGIC 0x48424343 /* 'HBCC' */
#define HBC_CRASH_VERSION 1
#define HBC_CRASH_FRAMES 12

typedef struct {
	u32 magic;
	u32 version;
	u32 exception;   /* PPC_EXCPT_*, e.g. 3 for DSI */
	u32 pc, msr, lr, cr, ctr;
	u32 dar, dsisr;  /* data address and cause, for DSI and alignment */
	u32 sp;
	u32 uptime_ms;   /* since hbc_agent_init() */
	u32 frames[HBC_CRASH_FRAMES]; /* return addresses from the stack */
	char app[20];
	u32 check;       /* hbc_crash_check() */
} hbc_crash_block;

static inline u32 hbc_crash_check(const hbc_crash_block *b) {
	const u32 *w = (const u32 *) b;
	u32 i, x = 0x5a17c0de;

	for (i = 0; i < (sizeof(*b) - sizeof(b->check)) / 4; ++i)
		x = ((x << 5) | (x >> 27)) ^ w[i];
	return x;
}

#ifndef HBC_AGENT_LAYOUT_ONLY

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/* Shown by `hbc.py status`; default "app". */
	const char *name;
	const char *version;
	/* The agent thread's priority, 0-127 (higher runs first). Keep it below
	   the app's threads; default 40 (libogc's main thread runs at 64). */
	s32 priority;
	/* Seconds libogc's crash screen stays up before returning to HBC;
	   default (0) 3. Negative keeps libogc's own setting, which without
	   __exception_setreload() leaves the screen up for good. */
	s32 crash_reload_s;
	/* Set when the app calls hbc_agent_exit_requested() in its main loop:
	   an exit request then waits up to exit_grace_ms (default 5000) for the
	   app to exit by itself before the agent calls exit(0). Without it, the
	   agent calls on_exit and exit(0) at once. Leave any field 0 for its
	   default. */
	bool app_polls_exit;
	u32 exit_grace_ms;
	/* Runs in the agent's thread before it calls exit(0); save state here. */
	void (*on_exit)(void *user);
	void *user;
	/* Leave the crash handler out. */
	bool no_crash_handler;
	/* The overlay's DEV > Save and Restart app; each is greyed out when
	   NULL. on_save returns whether it saved. on_restart runs in the app's
	   thread once the overlay has closed. */
	bool (*on_save)(void *user);
	void (*on_restart)(void *user);

	/* For a host with its own network server, such as HBC: no listener
	   thread and no network start-up. DEV then leaves out the hbc.py
	   connection and Log to PC switches; hand HBCK and HBCP requests to
	   hbc_agent_handle() to keep `hbc.py key` and `screen` working. */
	bool no_network;
	/* Also read GameCube controllers in the overlay (START is HOME). Set it
	   only if the app called PAD_Init(). */
	bool gc_pads;
	/* Exit's choices, HBC_AGENT_EXIT_*. hide_exit_choices is a mask of
	   (1 << choice) to leave out. on_exit_choice, if set, runs in the app's
	   thread once the overlay has closed; returning true means the app took
	   care of it (for example through its own shutdown), false lets the
	   agent do it. */
	unsigned hide_exit_choices;
	bool (*on_exit_choice)(int choice, void *user);
	/* Runs once per overlay frame in the app's thread, for work the app's
	   own loop would otherwise do each frame (HBC keeps its network
	   server accepting). Keep it short. */
	void (*on_frame)(void *user);
} hbc_agent_config;

enum {
	HBC_AGENT_EXIT_HBC,         /* exit(0): back through the reload stub */
	HBC_AGENT_EXIT_SYSTEM_MENU,
	HBC_AGENT_EXIT_RESTART,     /* restart the Wii */
	HBC_AGENT_EXIT_POWER_OFF
};

/* Starts the agent. cfg may be NULL for the defaults. Returns 0, or a
   negative error when the thread cannot start. Call once. */
s32 hbc_agent_init(const hbc_agent_config *cfg);

/* True once `hbc.py exit` or `hbc.py run` asked the app to exit. */
bool hbc_agent_exit_requested(void);

/* Waits up to ms for the network (0 when it is up, else a negative error). */
s32 hbc_agent_net_wait(u32 ms);

/* The HOME overlay (link with -lwiiuse -lbte, which WPAD apps already use).
 * Call it when HOME is pressed, between frames; it pauses the app's loop by
 * running its own until the user closes it with HOME (or B at the top), and
 * returns 0, or a negative error when there is no memory for it (it borrows
 * three framebuffers, about 1.8 MB at 640x480, while open). It reads the
 * Wii Remotes itself, so the app needs WPAD_Init(). rmode may be NULL for
 * the preferred video mode. Exit's choices leave the app from inside. */
struct _gx_rmodeobj;
s32 hbc_agent_home(const struct _gx_rmodeobj *rmode);

/* The same, drawing into framebuffers the app lends it instead of
 * allocating two: each at least fbWidth * xfbHeight * 2 bytes, 32-byte
 * aligned, in MEM1 (where the video interface reads from), and not the one
 * on screen. fb1 may be NULL (one buffer: the menus may tear while they
 * slide). hbc_agent_home() uses its own allocation, falling back to drawing
 * over the app's own framebuffer when the heap only has MEM2 left. */
s32 hbc_agent_home_fb(const struct _gx_rmodeobj *rmode, void *fb0, void *fb1);

/* True once when `hbc.py key h` asked for the overlay: check it next to the
 * HOME button, `if ((down & WPAD_BUTTON_HOME) || hbc_agent_home_pending())`. */
bool hbc_agent_home_pending(void);

/* Show a short message at the top of the HOME overlay for a few seconds.
 * From on_save or a slot item, it replaces the overlay's own "Saved" or
 * "Couldn't save", so an app can say what happened. */
void hbc_agent_toast(const char *msg);

/* The two bar buttons beside Exit: slot 0 on its left (blank by default),
 * slot 1 on its right (Shot, a screenshot to sd:/screenshots, by default).
 * press runs in the app's thread after the overlay closes. A NULL label
 * restores the default. */
void hbc_agent_set_slot(int slot, const char *label, void (*press)(void *user), void *user);

/* A menu for a slot instead of a single action: pressing the button slides
 * it in from the button's side, like DEV and WiiMote. Items show in order;
 * info rows (value set) take a whole row, buttons pair up two to a row.
 * The overlay reads items[] and each value every frame, so keep them alive
 * and update the strings in place. Up to 12 items. */
enum {
	HBC_AGENT_ITEM_CLOSE = 1,     /* close the overlay first, then run press */
	HBC_AGENT_ITEM_DISABLED = 2   /* greyed out */
};

typedef struct {
	const char *label;
	const char *value;            /* non-NULL: an info row showing this text */
	void (*press)(void *user);    /* buttons; runs in the app's thread */
	void *user;
	unsigned flags;
} hbc_agent_item;

void hbc_agent_set_slot_menu(int slot, const char *label, const char *title,
							 const hbc_agent_item *items, int count);

/* For hosts with their own server (no_network): answers HBCK (`hbc.py key`)
 * and HBCP (`hbc.py screen`) on an accepted connection whose 16-byte header
 * is hdr, returning false for any other request. The caller closes s. */
bool hbc_agent_handle(s32 s, const u8 *hdr);

/* Calibration measured by WiiMote > More > Calibrate, for this session. */
typedef struct {
	bool valid, motionplus;
	int accel[3];      /* raw accelerometer at rest */
	int gyro[3];       /* raw MotionPlus rates at rest (its zero offsets) */
	int stick[2];      /* raw Nunchuk stick centre */
} hbc_agent_cal;

bool hbc_agent_calibration(int chan, hbc_agent_cal *out);

#ifdef __cplusplus
}
#endif

#endif /* HBC_AGENT_LAYOUT_ONLY */
#endif /* HBC_AGENT_H */
