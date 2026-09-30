#ifndef _HOME_H_
#define _HOME_H_

#include <gctypes.h>

// What the HOME overlay's user chose, for the main loop to carry out.
typedef enum {
	HOME_NONE,          // closed with B or HOME: back to the app list
	HOME_ABOUT,
	HOME_BOOTMII,
	HOME_SYSTEM_MENU,
	HOME_RESTART,
	HOME_SHUTDOWN
} home_action;

// Sets up the agent's overlay for HBC; call once, after the video is up.
void home_init(void);
// Whether this frame's buttons (or `hbc.py key h`) ask for the HOME menu.
bool home_requested(u32 buttons_down);
// Shows the HOME overlay until the user closes it.
home_action home_show(void);

#endif
