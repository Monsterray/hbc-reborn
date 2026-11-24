#ifndef _DEVFILE_RM_H_
#define _DEVFILE_RM_H_

// Pure helpers of the tree delete (HBCF op 'E', docs/devnet.md), kept apart
// from devfile.c so tests/host/test_devfile_rm.py can compile them on the PC.

#include <stdbool.h>
#include <string.h>

// HBCF header byte 5 for op 'E'.
#define DEVNET_FLAG_GLOB 0x02      // the last path component is a * ? pattern
#define DEVNET_FLAG_RECURSIVE 0x04 // directories go with their contents

static inline int devfile_lower(int c) {
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static inline bool devfile_has_wildcard(const char *s) {
	return s[strcspn(s, "*?")] != 0;
}

// Whether name matches pattern: * is any run of characters (even none), ?
// is one byte, the rest compare without regard to case, as FAT does.
// Iterative, so a hostile pattern cannot use the stack.
static inline bool devfile_glob_match(const char *pat, const char *name) {
	const char *star = NULL, *retry = NULL;

	while (*name) {
		if (*pat == '*') {
			star = pat++;
			retry = name;
		} else if (*pat == '?' || (*pat && devfile_lower((unsigned char) *pat) ==
								   devfile_lower((unsigned char) *name))) {
			++pat;
			++name;
		} else if (star) {
			pat = star + 1;
			name = ++retry;
		} else {
			return false;
		}
	}
	while (*pat == '*')
		++pat;
	return !*pat;
}

// A device root ("sd:/") or the apps folder ("sd:/apps"), however spelled:
// repeated or "." components and any letter case reach the same folder on
// FAT. Such a path is never deleted, whatever the client asked.
static inline bool devfile_protected(const char *path) {
	const char *colon = strchr(path, ':');
	const char *p;
	char first[8];
	size_t n;
	int parts = 0;

	if (!colon)
		return false;
	p = colon + 1;
	first[0] = 0;
	while (*p) {
		const char *end;

		while (*p == '/')
			++p;
		end = p + strcspn(p, "/");
		n = end - p;
		if (n && !(n == 1 && *p == '.')) {
			if (++parts == 1) {
				if (n >= sizeof(first))
					return false;   // longer than "apps"
				memcpy(first, p, n);
				first[n] = 0;
			}
		}
		p = end;
	}
	if (parts == 0)
		return true;
	if (parts > 1)
		return false;
	for (n = 0; first[n]; ++n)
		first[n] = devfile_lower((unsigned char) first[n]);
	return !strcmp(first, "apps");
}

// A recursive pattern delete in a device root or the apps folder: sd:/b*
// would take bootmii/, sd:/apps/* every app. Folders there go by name only.
static inline bool devfile_glob_refused(const char *parent, bool recursive) {
	return recursive && devfile_protected(parent);
}

#endif
