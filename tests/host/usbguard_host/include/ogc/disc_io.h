/* Stand-in for libogc2's <ogc/disc_io.h> on a PC: only the two members that
 * sdk/usbguard calls (libogc2 passes the interface to each callback). */
#ifndef USBGUARD_HOST_DISC_IO_H
#define USBGUARD_HOST_DISC_IO_H

#include <stdbool.h>

typedef struct DISC_INTERFACE_STRUCT DISC_INTERFACE;
struct DISC_INTERFACE_STRUCT {
	bool (*startup)(DISC_INTERFACE *);
	bool (*isInserted)(DISC_INTERFACE *);
};

#endif
