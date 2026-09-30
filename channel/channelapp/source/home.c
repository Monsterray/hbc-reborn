// HBC's HOME button, built on the in-app agent's overlay (sdk/hbc_agent.h).
//
// This file doubles as the worked example for apps that want the overlay:
// everything HBC does to use it is here, and each step says why. In short:
//
//   1. hbc_agent_init() once at start-up, with a config that says what this
//      app is and how it wants the overlay to behave.
//   2. Optionally, a menu of its own on one of the bar's app slots.
//   3. hbc_agent_home() (here hbc_agent_home_fb()) when HOME is pressed.
//   4. Act on whatever the user chose once the overlay has closed.
//
// The old HOME menu's buttons and information (Back, About, Launch BootMii,
// Exit to System Menu, Shutdown, the Wii's IP, HBC's and IOS's versions)
// live in the "HBC" slot's menu; B takes the place of Back.

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <ogcsys.h>
#include <network.h>

#include "../config.h"
#include "../../../sdk/hbc_agent.h"
#include "controls.h"
#include "gfx.h"
#include "loader.h"
#include "panic.h"
#include "xml.h"

#include "home.h"

// ---- The "HBC" slot's menu -------------------------------------------------
//
// An app slot can be one button (hbc_agent_set_slot) or a menu of its own
// (hbc_agent_set_slot_menu). The overlay reads this array, and the strings
// its info rows point at, every frame while it is open, so both live for
// the whole session and the strings are updated in place before each
// opening (home_show).

static char version_text[32];
static char ios_text[32];
static char network_text[40];

// What the user picked, for main() to carry out once the overlay is closed.
static volatile home_action picked;

static void pick(void *user) {
	picked = (home_action) (u32) user;
}

// Info rows first, then buttons, which the overlay pairs two to a row.
// HBC_AGENT_ITEM_CLOSE closes the overlay before calling pick(): every one
// of these hands control to HBC's own screens or shutdown path, which must
// not run while the overlay still owns the screen.
static hbc_agent_item hbc_items[] = {
	{ "Version", version_text, NULL, NULL, 0 },
	{ "IOS", ios_text, NULL, NULL, 0 },
	{ "Network", network_text, NULL, NULL, 0 },
	{ "About", NULL, pick, (void *) HOME_ABOUT, HBC_AGENT_ITEM_CLOSE },
	{ "Launch BootMii", NULL, pick, (void *) HOME_BOOTMII, HBC_AGENT_ITEM_CLOSE },
	{ "Exit to System Menu", NULL, pick, (void *) HOME_SYSTEM_MENU, HBC_AGENT_ITEM_CLOSE },
	{ "Shutdown", NULL, pick, (void *) HOME_SHUTDOWN, HBC_AGENT_ITEM_CLOSE },
};
#define BOOTMII_ITEM 4

// ---- Exit and DEV hooks --------------------------------------------------

// Exit's choices would otherwise leave straight from inside the overlay.
// HBC has a shutdown path of its own that saves settings and stops its
// threads first, so it takes the choice and returns true ("handled").
static bool exit_choice(int choice, void *user) {
	(void) user;

	switch (choice) {
	case HBC_AGENT_EXIT_SYSTEM_MENU:
		picked = HOME_SYSTEM_MENU;
		return true;
	case HBC_AGENT_EXIT_RESTART:
		picked = HOME_RESTART;
		return true;
	case HBC_AGENT_EXIT_POWER_OFF:
		picked = HOME_SHUTDOWN;
		return true;
	}
	return false;
}

// DEV > Save: HBC's settings are its only state worth saving by hand.
static bool save(void *user) {
	(void) user;
	return settings_save();
}

static void signal_loader(void *user) {
	(void) user;
	loader_signal_threads();
}

// ---- BootMii --------------------------------------------------------------

// BootMii installed as IOS254 has a TMD whose title starts "BM"; offering
// it otherwise would reboot into nothing.
static bool bootmii_is_installed(u64 title_id) {
	u32 tmd_view_size;
	u8 *tmdbuf;
	bool ret;

	if (ES_GetTMDViewSize(title_id, &tmd_view_size) < 0)
		return false;
	if (tmd_view_size < 90 || tmd_view_size > 1024)
		return false;

	tmdbuf = pmemalign(32, 1024);
	if (ES_GetTMDView(title_id, (tmd_view *) tmdbuf, tmd_view_size) < 0) {
		free(tmdbuf);
		return false;
	}
	ret = tmdbuf[50] == 'B' && tmdbuf[51] == 'M';
	free(tmdbuf);
	return ret;
}

// ---- Setup --------------------------------------------------------------

void home_init(void) {
	// The config is copied, so a local is fine. Every field left 0 keeps
	// the agent's default.
	hbc_agent_config cfg = { 0 };

	cfg.name = "The Homebrew Channel";
	cfg.version = CHANNEL_VERSION_STR;

	// HBC answers port 4299 itself (loader.c, devnet.c), so the agent must
	// not start a second server or bring up the network on its own. It
	// still serves `hbc.py key` and `screen`: devnet.c passes HBCK and HBCP
	// requests to hbc_agent_handle().
	cfg.no_network = true;
	// HBC is the loader apps return to; a crash here has nowhere to report
	// to, so keep libogc's own crash screen.
	cfg.no_crash_handler = true;
	// HBC reads GameCube controllers too (controls.c calls PAD_Init), and
	// START on one opens this menu, so the overlay must read them as well.
	cfg.gc_pads = true;
	// "Exit to The Homebrew Channel" from inside HBC would only restart it.
	cfg.hide_exit_choices = 1 << HBC_AGENT_EXIT_HBC;
	cfg.on_exit_choice = exit_choice;
	cfg.on_save = save;
	// The overlay runs its own frame loop, so HBC's main loop stops while
	// it is open; its network thread, though, accepts a connection only
	// when told to each frame. Keep telling it, so hbc.py and file
	// transfers still work with the HOME menu up.
	cfg.on_frame = signal_loader;
	hbc_agent_init(&cfg);

	if (!bootmii_is_installed(TITLEID_BOOTMII))
		hbc_items[BOOTMII_ITEM].flags |= HBC_AGENT_ITEM_DISABLED;

	// Slot 0 is the button left of Exit. The right one (slot 1) keeps the
	// agent's default, Shot, which saves the frame to sd:/screenshots.
	hbc_agent_set_slot_menu(0, "HBC", "The Homebrew Channel", hbc_items,
							sizeof(hbc_items) / sizeof(hbc_items[0]));
}

bool home_requested(u32 buttons_down) {
	// hbc_agent_home_pending() is true once after `hbc.py key h`, so the
	// menu can be opened (and tested) from the PC.
	return (buttons_down & PADS_HOME) || hbc_agent_home_pending();
}

// ---- Showing it -------------------------------------------------------------

// Low MEM1, where HBC loads the apps it launches, is unused while the menu
// runs: an app is copied there only after HBC's main loop has ended. HBC's
// heap is in MEM2 by the time the menu is up, and the video interface only
// scans out MEM1, so the overlay borrows two framebuffers from here rather
// than allocating them.
#define HOME_FB_ADDR 0x80a00000

home_action home_show(void) {
	GXRModeObj *vmode = gfx_video_mode();
	u32 size = VIDEO_GetFrameBufferSize(vmode);
	u8 *fb0 = (u8 *) HOME_FB_ADDR;
	u8 *fb1 = fb0 + ((size + 31) & ~31);
	u32 ip = net_gethostip();

	// Refresh the info rows; the overlay copies them each frame.
	snprintf(version_text, sizeof(version_text), "%s", CHANNEL_VERSION_STR);
	snprintf(ios_text, sizeof(ios_text), "IOS%d v%d.%d", IOS_GetVersion(),
			 IOS_GetRevisionMajor(), IOS_GetRevisionMinor());
	if (loader_tcp_initialized() && ip)
		snprintf(network_text, sizeof(network_text), "%u.%u.%u.%u", ip >> 24,
				 (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
	else
		snprintf(network_text, sizeof(network_text), "Not initialized");

	// HBC buzzes the remote when the pointer enters a button and stops it a
	// few frames later from its own loop, which the overlay pauses: stop it
	// now, or it would buzz until the overlay closed.
	controls_rumble(0);

	// Runs its own frame loop until the user closes it (B at the top, or
	// HOME), calling pick() or exit_choice() on the way out.
	picked = HOME_NONE;
	hbc_agent_home_fb(vmode, fb0, fb1);
	return picked;
}
