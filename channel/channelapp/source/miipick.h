#ifndef _MIIPICK_H_
#define _MIIPICK_H_

#include "view.h"

// The Mii picker (miipick.c): every Mii in the Mii Channel, a page at a
// time, over sub_view. current: the creation ID (hex) of the avatar's Mii,
// whose page it opens on, or NULL. Returns the slot of the Mii chosen, its
// creation ID in id_out (17 bytes), or -1 for Back.
int show_mii_picker(const view *sub_view, const char *current, char *id_out);

// For HBCS: ,"mii_picker":{...}.
int miipick_status_json(char *buf, int size);

#endif
