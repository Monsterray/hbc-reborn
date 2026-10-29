#ifndef _DEVFILE_NAME_H_
#define _DEVFILE_NAME_H_

#include <string.h>

#define DEVFILE_PATH_MAX 256

// Keep the app's folder (and any subfolders) for files under a device's
// apps directory. Other transfers still show just the file name.
static inline const char *devfile_display_name(const char *path) {
	const char *root = strchr(path, ':');
	const char *base;

	if (root && !strncmp(root + 1, "/apps/", 6) && root[7])
		return root + 7;
	base = strrchr(path, '/');
	return base ? base + 1 : path;
}

#endif
