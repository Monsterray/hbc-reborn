#ifndef _BOOTSCREEN_H_
#define _BOOTSCREEN_H_

// The start-up call log (bootlog.h) on the TV, for a start that hangs: when
// the menu is not up 5 s after the start, the calls are drawn on the
// framebuffer and redrawn twice a second, calls with no answer on dark red.
// A black screen then shows what HBC waits on (and the bench capture records
// it).

// Starts the thread that draws the list. After gfx_init_video, so there is a
// framebuffer to draw on.
void bootscreen_start(void);

// The menu is about to draw its first frame: the list stops, and a draw in
// progress ends first.
void bootscreen_stop(void);

#endif
