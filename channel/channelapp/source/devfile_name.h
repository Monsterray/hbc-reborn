#ifndef _DEVFILE_NAME_H_
#define _DEVFILE_NAME_H_

#include <string.h>

#define DEVFILE_PATH_MAX 256

// Whether path starts with "/apps/", in any case: the card's names do not
// care about case, so neither does this.
static inline int devfile_in_apps(const char *path) {
	static const char apps[] = "/apps/";
	int i;

	for (i = 0; apps[i]; ++i) {
		char c = path[i];

		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c != apps[i])
			return 0;
	}
	return 1;
}

// Keep the app's folder (and any subfolders) for files under a device's
// apps directory. Other transfers show the file with its parent folder;
// a file at a device's root shows its name alone.
static inline const char *devfile_display_name(const char *path) {
	const char *root = strchr(path, ':');
	const char *base, *parent;

	if (root && devfile_in_apps(root + 1) && root[7])
		return root + 7;
	base = strrchr(path, '/');
	if (!base)
		return path;
	if (!base[1])   // a folder path names no file
		return base + 1;
	// Walk back to the folder's start; a file at the root has none.
	for (parent = base; parent > path && parent[-1] != '/' && parent[-1] != ':';
			--parent)
		;
	return parent < base && (parent == path || parent[-1] == '/') ? parent : base + 1;
}

#endif
