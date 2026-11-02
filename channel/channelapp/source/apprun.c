#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "apprun.h"

static const char marker[] = ".hbc-ran";
static const char *tracked[] = { "boot.dol", "boot.elf", "icon.png", "meta.xml" };
#define TRACKED_COUNT (sizeof(tracked) / sizeof(tracked[0]))

typedef struct {
	unsigned present;
	uint64_t size;
	int64_t modified, created;
} file_stamp;

typedef struct {
	file_stamp files[TRACKED_COUNT];
} app_stamp;

static bool sibling_path(char *out, const char *boot_path, const char *name) {
	const char *slash = strrchr(boot_path, '/');
	size_t prefix;

	if (!slash || (strcmp(slash + 1, "boot.dol") && strcmp(slash + 1, "boot.elf")))
		return false;
	prefix = slash - boot_path + 1;
	if (prefix + strlen(name) + 1 > PATH_MAX)
		return false;
	memcpy(out, boot_path, prefix);
	strcpy(out + prefix, name);
	return true;
}

static bool snapshot(const char *boot_path, app_stamp *stamp) {
	char path[PATH_MAX];
	struct stat st;
	unsigned i;

	memset(stamp, 0, sizeof(*stamp));
	for (i = 0; i < TRACKED_COUNT; ++i) {
		if (!sibling_path(path, boot_path, tracked[i]))
			return false;
		if (stat(path, &st)) {
			if (errno == ENOENT)
				continue;
			return false;
		}
		if (!S_ISREG(st.st_mode))
			return false;
		stamp->files[i].present = 1;
		stamp->files[i].size = st.st_size;
		stamp->files[i].modified = st.st_mtime;
		stamp->files[i].created = st.st_ctime;
	}
	// A successful launch must have an executable; don't create history
	// for an incomplete installation or a directory that vanished.
	return stamp->files[0].present || stamp->files[1].present;
}

static bool same_stamp(const app_stamp *a, const app_stamp *b) {
	unsigned i;
	for (i = 0; i < TRACKED_COUNT; ++i) {
		const file_stamp *x = &a->files[i], *y = &b->files[i];
		if (x->present != y->present || x->size != y->size ||
				x->modified != y->modified || x->created != y->created)
			return false;
	}
	return true;
}

// 0: missing/invalid, 1: legacy, 2: file stamps.
static int read_stamp(const char *boot_path, app_stamp *stamp) {
	char path[PATH_MAX], header[32];
	char backup[PATH_MAX + 5];
	FILE *f;
	unsigned i;
	int version = 0;

	memset(stamp, 0, sizeof(*stamp));
	if (!sibling_path(path, boot_path, marker))
		return 0;
	f = fopen(path, "rb");
	if (!f && errno == ENOENT) {
		// A power cut between the two renames must not erase run history.
		snprintf(backup, sizeof(backup), "%s.bak", path);
		f = fopen(backup, "rb");
	}
	if (!f)
		return 0;
	if (!fgets(header, sizeof(header), f))
		goto done;
	if (!strcmp(header, "HBC ran\n")) {
		version = fgetc(f) == EOF && !ferror(f) ? 1 : 0;
		goto done;
	}
	if (strcmp(header, "HBC ran 2\n"))
		goto done;
	for (i = 0; i < TRACKED_COUNT; ++i) {
		file_stamp *s = &stamp->files[i];
		if (fscanf(f, "%u %" SCNu64 " %" SCNd64 " %" SCNd64 "\n",
				&s->present, &s->size, &s->modified, &s->created) != 4 || s->present > 1)
			goto done;
	}
	if (fgetc(f) == EOF && !ferror(f))
		version = 2;
 done:
	fclose(f);
	return version;
}

static bool write_stamp(const char *boot_path, const app_stamp *stamp) {
	char path[PATH_MAX], part[PATH_MAX + 5], backup[PATH_MAX + 5];
	struct stat st;
	FILE *f;
	bool ok, moved_old = false;
	unsigned i;

	if (!sibling_path(path, boot_path, marker))
		return false;
	snprintf(part, sizeof(part), "%s.part", path);
	snprintf(backup, sizeof(backup), "%s.bak", path);
	if (!(f = fopen(part, "wb")))
		return false;
	ok = fputs("HBC ran 2\n", f) >= 0;
	for (i = 0; ok && i < TRACKED_COUNT; ++i) {
		const file_stamp *s = &stamp->files[i];
		ok = fprintf(f, "%u %" PRIu64 " %" PRId64 " %" PRId64 "\n",
				s->present, s->size, s->modified, s->created) > 0;
	}
	if (fclose(f))
		ok = false;
	// FAT rename need not replace an existing destination. Keep the old
	// marker until the new one commits; restore it if the commit fails.
	if (ok) {
		if (!stat(path, &st)) {
			ok = !remove(backup) || errno == ENOENT;
			if (ok)
				ok = moved_old = rename(path, backup) == 0;
		} else if (errno != ENOENT) {
			ok = false;
		}
	}
	if (ok && rename(part, path)) {
		ok = false;
		if (moved_old)
			rename(backup, path);
	}
	if (ok)
		remove(backup);
	if (!ok)
		remove(part);
	return ok;
}

app_run_state app_run_status(const char *boot_path) {
	app_stamp saved, current;
	int version = read_stamp(boot_path, &saved);

	if (!version)
		return APP_RUN_NEW;
	if (version == 1) {
		// Old markers contain no previous stamps. Establish today's baseline
		// once, rather than showing every previously cleared app as updated.
		if (snapshot(boot_path, &current))
			write_stamp(boot_path, &current);
		return APP_RUN_UNCHANGED;
	}
	if (!snapshot(boot_path, &current) || !same_stamp(&saved, &current))
		return APP_RUN_UPDATED;
	return APP_RUN_UNCHANGED;
}

bool app_run_record(const char *boot_path) {
	app_stamp saved, current;
	char path[PATH_MAX];
	struct stat st;
	int version;

	if (!snapshot(boot_path, &current))
		return false;
	version = read_stamp(boot_path, &saved);
	if (version == 2 && same_stamp(&saved, &current) &&
			sibling_path(path, boot_path, marker) && !stat(path, &st))
		return true;
	return write_stamp(boot_path, &current);
}
