/* Stand-in for libogc's <ogc/disc_io.h> on a PC: only the two members that
 * sdk/usbguard calls. */
#ifndef USBGUARD_HOST_DISC_IO_H
#define USBGUARD_HOST_DISC_IO_H

#include <stdbool.h>

typedef struct {
	bool (*startup)(void);
	bool (*isInserted)(void);
} DISC_INTERFACE;

#endif
