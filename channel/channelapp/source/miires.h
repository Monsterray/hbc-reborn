#ifndef _MIIRES_H_
#define _MIIRES_H_

#include <stddef.h>
#include <gctypes.h>

// Copies the Wii Menu's Mii face resource to sd:/mii/RFL_Res.dat (miires.c).
// Returns 0 or a negative errno or IOS error; msg says what happened, in a
// line fit for a toast. Takes a second or two; one at a time.
s32 miires_save(char *msg, size_t len);

#endif
