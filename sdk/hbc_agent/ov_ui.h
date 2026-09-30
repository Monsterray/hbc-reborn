/* The overlay's layout, animation and input handling, as portable C.
 * overlay.c feeds it live data (ov_ext) and controller presses each frame
 * and carries out the actions it asks for; tests/overlay_preview feeds it
 * made-up data and renders frames on the PC. */

#ifndef OV_UI_H
#define OV_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "ov_draw.h"

#define OV_REMOTES 4

typedef struct {
	bool connected;
	int battery;          /* 0 to 4 bars */
	int battery_pct;
	char ext[24];         /* "Nunchuk", "Classic Controller", "" */
	bool motionplus;
	bool rumble;          /* rumble allowed for this remote */
	bool finding;
	int volume;           /* speaker, 0 to 10 */
} ov_remote;

/* An app slot's own menu (hbc_agent_set_slot_menu). */
#define OV_MENU_ITEMS 12
enum { OV_ITEM_INFO = 1, OV_ITEM_DISABLED = 2, OV_ITEM_CLOSE = 4 };

typedef struct {
	char label[32];
	char value[40];       /* info rows */
	int flags;            /* OV_ITEM_* */
} ov_menu_item;

typedef struct {
	char title[40];
	int count;            /* 0: a plain button that runs its callback */
	ov_menu_item item[OV_MENU_ITEMS];
} ov_menu;

typedef struct {
	char app[40];         /* "agent_app 1.0" */
	char clock[8];        /* "14:32" */
	char date[32];        /* "14:32, Tue 29 Sep" */
	char playing[16];
	char network[32];
	char sd[24];
	unsigned mem_free_kb[2], mem_total_kb[2];   /* MEM1, MEM2 */
	ov_remote remote[OV_REMOTES];

	bool log_pc, crash_stay, hbcpy;
	bool has_save, has_restart;
	bool sensor_above;
	int ir_sens;          /* 1 to 5 */
	char auto_off[12];    /* "5 min", "Never" */
	bool rumble_all;
	bool searching;       /* Connect remote in progress */
	bool can_leds;        /* remote handles found: LEDs, IR and sensor bar work */

	char slot[2][16];     /* app slots beside Exit; "" is blank */
	ov_menu menu[2];      /* their menus, if the app gave them one */

	/* What applies to this app: rows of DEV's Actions tab, and Exit's
	   choices (bit 0 The Homebrew Channel, 1 System Menu, 2 Restart Wii,
	   3 Power off). */
	bool show_net, show_log_pc, show_crash;
	unsigned exit_mask;

	const char *log_text; /* Log page: the app's recent output */
	char test[6][40];     /* Test page lines for the selected remote */
	int cal_progress;     /* 0 to 100 */
	int cal_wait_s;       /* seconds before it starts measuring */
	char cal_result[48];
	char toast[64];
} ov_ext;

/* Controller input, as presses this frame. */
enum {
	OV_UP = 1, OV_DOWN = 2, OV_LEFT = 4, OV_RIGHT = 8,
	OV_A = 16, OV_B = 32, OV_HOME = 64, OV_ANY = 128,
	OV_TEST_EXIT = 256,   /* + and - together */
	OV_1 = 512, OV_2 = 1024
};

/* What the overlay asks overlay.c to do; arg is a remote or a value. */
enum {
	OVA_HBC = 1, OVA_SYSMENU, OVA_RESTART_WII, OVA_POWEROFF,
	OVA_SLOT,             /* arg: 0 left, 1 right; runs after the overlay closes */
	OVA_SHOT,
	OVA_RESTART_APP,      /* runs after the overlay closes */
	OVA_PAUSE, OVA_SAVE,
	OVA_LOG_PC, OVA_CRASH_STAY, OVA_HBCPY,
	OVA_FIND, OVA_RUMBLE, OVA_DISCONNECT,
	OVA_TEST_START, OVA_TEST_STOP, OVA_CAL_START,
	OVA_CONNECT, OVA_DISCONNECT_ALL, OVA_SENSOR_ABOVE, OVA_IR_SENS,
	OVA_AUTO_OFF,         /* arg: -1 or +1 */
	OVA_RUMBLE_ALL,
	OVA_SLOT_ITEM,        /* arg: slot * 16 + item; after closing if OV_ITEM_CLOSE */
	OVA_VOLUME,           /* arg: remote | 16 for louder */
	OVA_RESET_REMOTES,
	OVA_SOUND_TEST        /* arg: remote | 16 for PCM (else ADPCM) */
};

typedef void (*ov_act_fn)(int action, int arg, void *user);

#define OV_MAX_ITEMS 96

typedef struct {
	int kind, id, flags;
	int x, y, w, h;
	int arg;
	const ov_font *font;
	char text[48];
} ov_item;

typedef struct {
	int w, h;
	int open_t, dev_t, wm_t, exit_t;   /* animation, 0 to 256 */
	int slot_t[2];
	int menu;                           /* OV_MENU_* */
	bool closing, paused;
	int dev_tab, dev_page, wm_page, wm_sel;
	int focus, bar_focus;
	int after;                          /* action to run once closed */
	int after_arg;
	unsigned frame;
	/* The Wii Remotes' pointers (ov_point), and the item under the one in
	   use; while it points at the screen, A presses that item. */
	int px[OV_REMOTES], py[OV_REMOTES];
	float pa[OV_REMOTES];  /* each remote's twist, degrees */
	unsigned pointing;    /* bit per remote on screen */
	int pointer;          /* the remote in use, or -1 */
	int hover;            /* item under it, or 0 */
	bool aiming;          /* the pointer, not the D-pad, was used last */
	int aim_x, aim_y;     /* where it was when last checked */
	uint32_t drawn;       /* signature of the last frame drawn */
	int n;
	ov_item items[OV_MAX_ITEMS];
} ov_ui;

void ov_init(ov_ui *ui, int w, int h);
/* Before each ov_step: where each remote points (valid: a bit per remote)
 * and which remote is in use (the last to press a button), or -1. */
void ov_point(ov_ui *ui, const int x[OV_REMOTES], const int y[OV_REMOTES],
			  const float angle[OV_REMOTES], unsigned valid, int active);
/* One frame: handle presses, advance animations, lay out. Returns false
 * once the overlay has fully closed; ui->after then names an action
 * (OVA_SLOT or OVA_RESTART_APP) to run in the app, or 0. */
bool ov_step(ov_ui *ui, const ov_ext *e, unsigned pressed, ov_act_fn act, void *user);
/* Whether the frame ov_step just laid out looks different from the last
 * one drawn (which it then records); false means the screen can stay. */
bool ov_changed(ov_ui *ui, const ov_ext *e);
/* Draws the laid-out frame over whatever the canvas holds. */
void ov_draw(const ov_ui *ui, const ov_ext *e, ov_canvas *c);

#endif
