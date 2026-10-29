#ifndef _AVATAR_H_
#define _AVATAR_H_

#include <gctypes.h>

#include "gfx.h"

// The user's Mii, standing beside the app list (avatar.c).

// Starts loading it in the background: the face models from
// sd:/mii/RFL_Res.dat (HOME > HBC > Save Mii faces) and the Mii from the
// Wii's Mii Channel database. Without either, HBC shows no avatar.
// mii_id: the creation ID (hex) of the Mii chosen in the picker, from
// settings.xml; "" or NULL, or a Mii no longer there: the first favourite.
void avatar_init(const char *mii_id);
void avatar_deinit(void);

// Loads it again (after Save Mii faces, or a new SD card).
void avatar_reload(void);

// Call before gfx_frame_start: poses the Mii for this frame. Returns the
// queue entry (a GFXE_DRAW: the Mii draws in place) to push after the background,
// or NULL while there is no avatar. pointer: the Wii Remote points at the
// screen, at (x, y); down and held: this frame's buttons (PADS_*).
const gfx_queue_entry *avatar_frame(bool pointer, s32 x, s32 y, u32 down, u32 held);

// Whether A was pressed on it this frame (or the PC asked for the picker:
// avatar_ask_picker); asking clears it.
bool avatar_clicked(void);
void avatar_ask_picker(void);
// Plays the Mii Channel body's animation of this name (for looking at
// them: HBCD, `hbc.py anim`): a one-shot once, a looping one until the next
// call; a name it does not have stops it. Nothing without that body.
void avatar_play(const char *name);

// Shows the Mii with this creation ID (hex), chosen in the picker.
void avatar_choose(const char *mii_id);
// The creation ID (hex) of the Mii shown, or NULL while there is none.
const char *avatar_mii_id(void);

// For HBCS: "loading", "ready" or "none"; the Mii's name and slot; the
// CPU time avatar_frame takes and the time between frames, averaged.
typedef struct {
	const char *state;
	char name[32];
	int slot;
	u32 frame_us, interval_us, frames;
	u32 bad;              // frames whose posed body had a wild vertex (HBC_DIAG_GPU)
	char bad_at[96];      // the first: animation, frame, vertex, value
} avatar_info;

void avatar_get_info(avatar_info *info);

// The Mii playground (miiplay.c): the avatar's Mii on the whole screen,
// standing on a floor, to be picked up with A+B and dropped, and made to
// play its animations, poses and expressions. Begin returns false while
// there is no Mii to play with; the avatar is back beside the app list
// after end. floor: where its feet stand (view units), the lowest they go.
bool avatar_pg_begin(int floor);
void avatar_pg_end(void);
// Bigger (1) or smaller (-1), a tenth at a time, from 0.4 to 1.4 times.
void avatar_pg_size(int step);
// MII_EXPR_*, held until the next call; -1: its own (blinks and smiles).
void avatar_pg_expression(int expr);
enum {
	AV_POSE_STAND,       // its own: facing the front, looking at the pointer
	AV_POSE_LEFT,
	AV_POSE_RIGHT,
	AV_POSE_AROUND,
	AV_POSE_UP,          // looks up
	AV_POSE_DOWN,
	AV_POSE_TILT,        // its head on one side
	AV_POSE_FREEZE,      // holds the frame it is at; again: plays on
	AV_POSE_ICON,        // the head alone, as its icon: faces and the head's effects
	AV_POSE_COUNT
};
void avatar_pg_pose(int p);
// The body's animations (avatar_play plays one): the name of the i-th, or
// NULL past the last (and without the Mii Channel's body).
const char *avatar_anim_name(int i);
// For HBCS, inside miiplay.c's "playground": ,"x":..,"y":..,"held":..
// (the Mii's feet, in view units), "falling", "landed" (the animation it
// last landed with), "expression", "pose", "size" (tenths) and "floor".
int avatar_pg_json(char *buf, int size);
// For HBCS: ,"anim":"<the animation shown>","anims":[<the body's>]
int avatar_anims_json(char *buf, int size);

#endif
