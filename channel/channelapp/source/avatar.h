#ifndef _AVATAR_H_
#define _AVATAR_H_

#include <gctypes.h>

#include "gfx.h"

// The user's Mii, standing beside the app list (avatar.c).

// Starts loading it in the background: the face models from
// sd:/mii/RFL_Res.dat (HOME > HBC > Save Mii faces) and the Mii from the
// Wii's Mii Channel database. Without either, HBC shows no avatar.
void avatar_init(void);
void avatar_deinit(void);

// Loads it again (after Save Mii faces, or a new SD card).
void avatar_reload(void);

// Call before gfx_frame_start: poses the Mii for this frame and draws it
// into its texture. Returns the queue entry to push after the background,
// or NULL while there is no avatar. pointer: the Wii Remote points at the
// screen, at (x, y).
// A pressed on it shows the next Mii.
const gfx_queue_entry *avatar_frame(bool pointer, s32 x, s32 y, bool a_down);

// For HBCS: "loading", "ready" or "none"; the Mii's name and slot; the
// CPU time avatar_frame takes and the time between frames, averaged.
typedef struct {
	const char *state;
	char name[32];
	int slot;
	u32 frame_us, interval_us, frames;
} avatar_info;

void avatar_get_info(avatar_info *info);

#endif
