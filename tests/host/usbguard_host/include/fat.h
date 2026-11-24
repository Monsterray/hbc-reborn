/* Stand-in for libfat's <fat.h> on a PC. The test program defines it. */
#ifndef USBGUARD_HOST_FAT_H
#define USBGUARD_HOST_FAT_H

#include <ogc/disc_io.h>

bool fatMountSimple(const char *name, const DISC_INTERFACE *iface);

#endif
