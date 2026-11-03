#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "apprun.h"

#ifdef _WIN32
#define MKDIR(path) mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0777)
#endif

static const char dir_name[] = "hbc";
static const char file_name[] = "apps_seen.txt";
static const char *tracked[] = { "boot.dol", "boot.elf", "meta.xml", "icon.png" };
#define TRACKED_COUNT (sizeof(tracked) / sizeof(tracked[0]))
#define MAX_RECORDS 4096
#define LINE_MAX_LEN 1024

typedef struct {
	unsigned present;
	uint64_t size;
	int64_t modified;
} file_stamp;

typedef struct {
	file_stamp files[TRACKED_COUNT];
} app_stamp;

typedef struct {
	char *name;
	app_stamp stamp;
	bool seen;
} record;

static record *records;
static size_t record_count, record_cap;
static char table_root[PATH_MAX];
static bool table_dirty, table_open;

static void table_free(void) {
	size_t i;

	for (i = 0; i < record_count; ++i)
		free(records[i].name);
	free(records);
	records = NULL;
	record_count = record_cap = 0;
	table_dirty = false;
	table_open = false;
}

static record *table_find(const char *name) {
	size_t i;

	for (i = 0; i < record_count; ++i)
		if (!strcmp(records[i].name, name))
			return &records[i];
	return NULL;
}

static record *table_add(const char *name, const app_stamp *stamp) {
	record *r;

	if (record_count == record_cap) {
		size_t cap = record_cap ? record_cap * 2 : 32;
		record *grown = realloc(records, cap * sizeof(*records));

		if (!grown)
			return NULL;
		records = grown;
		record_cap = cap;
	}
	r = &records[record_count];
	r->name = strdup(name);
	if (!r->name)
		return NULL;
	r->stamp = *stamp;
	r->seen = false;
	record_count++;
	return r;
}

static bool file_path(char *out, size_t size, const char *root, const char *suffix) {
	int n = snprintf(out, size, "%s/%s/%s%s", root, dir_name, file_name, suffix);

	return n > 0 && (size_t) n < size;
}

static bool app_file_path(char *out, size_t size, const char *root, const char *name,
		const char *file) {
	int n = snprintf(out, size, "%s/apps/%s/%s", root, name, file);

	return n > 0 && (size_t) n < size;
}

static bool snapshot(const char *root, const char *name, app_stamp *stamp) {
	char path[PATH_MAX];
	struct stat st;
	unsigned i;

	memset(stamp, 0, sizeof(*stamp));
	for (i = 0; i < TRACKED_COUNT; ++i) {
		if (!app_file_path(path, sizeof(path), root, name, tracked[i]))
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
	}
	// A launch needs an executable; don't create history for an
	// incomplete installation or a folder that vanished.
	return stamp->files[0].present || stamp->files[1].present;
}

static bool same_stamp(const app_stamp *a, const app_stamp *b) {
	unsigned i;

	for (i = 0; i < TRACKED_COUNT; ++i) {
		const file_stamp *x = &a->files[i], *y = &b->files[i];
		if (x->present != y->present || x->size != y->size || x->modified != y->modified)
			return false;
	}
	return true;
}

// "<name>\tboot.dol=SIZE@TIME boot.elf=- meta.xml=SIZE@TIME icon.png=SIZE@TIME"
static bool parse_line(char *line, char **name, app_stamp *stamp) {
	char *tab = strchr(line, '\t'), *p;
	unsigned i;

	if (!tab || tab == line)
		return false;
	*tab = 0;
	*name = line;
	p = tab + 1;
	memset(stamp, 0, sizeof(*stamp));
	for (i = 0; i < TRACKED_COUNT; ++i) {
		size_t len = strlen(tracked[i]);
		file_stamp *s = &stamp->files[i];
		int used = 0;

		while (*p == ' ')
			p++;
		if (strncmp(p, tracked[i], len) || p[len] != '=')
			return false;
		p += len + 1;
		if (*p == '-') {
			p++;
		} else {
			if (sscanf(p, "%" SCNu64 "@%" SCNd64 "%n", &s->size, &s->modified, &used) != 2)
				return false;
			p += used;
			s->present = 1;
		}
		if (*p != ' ' && *p != 0)
			return false;
	}
	while (*p == ' ')
		p++;
	return *p == 0;
}

static void table_load(const char *root) {
	char path[PATH_MAX], backup[PATH_MAX + 8], line[LINE_MAX_LEN];
	FILE *f;

	table_free();
	table_open = true;
	snprintf(table_root, sizeof(table_root), "%s", root);
	if (!file_path(path, sizeof(path), root, ""))
		return;
	f = fopen(path, "rb");
	if (!f && errno == ENOENT) {
		// A power cut between the two renames must not erase run history.
		if (file_path(backup, sizeof(backup), root, ".bak"))
			f = fopen(backup, "rb");
	}
	if (!f)
		return;
	while (record_count < MAX_RECORDS && fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		char *name;
		app_stamp stamp;

		if (len && line[len - 1] == '\n') {
			line[--len] = 0;
			if (len && line[len - 1] == '\r')
				line[--len] = 0;
		} else if (!feof(f)) {
			// Too long to be ours: skip to the end of the line.
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
		if (line[0] == '#' || !parse_line(line, &name, &stamp) || table_find(name))
			continue;
		table_add(name, &stamp);
	}
	fclose(f);
}

static bool table_write(void) {
	char path[PATH_MAX], part[PATH_MAX + 8], backup[PATH_MAX + 8], dir[PATH_MAX];
	struct stat st;
	FILE *f;
	bool ok, moved_old = false;
	size_t r;
	unsigned i;

	if (!file_path(path, sizeof(path), table_root, "") ||
			!file_path(part, sizeof(part), table_root, ".part") ||
			!file_path(backup, sizeof(backup), table_root, ".bak") ||
			snprintf(dir, sizeof(dir), "%s/%s", table_root, dir_name) >= (int) sizeof(dir))
		return false;
	if (MKDIR(dir) && errno != EEXIST)
		return false;
	if (!(f = fopen(part, "wb")))
		return false;
	ok = fputs("# HBC app badges: one app folder per line, then the size@modified time of\n"
			"# its files at the last run from HBC. Delete this file to start over.\n", f) >= 0;
	for (r = 0; ok && r < record_count; ++r) {
		const record *rec = &records[r];

		ok = fprintf(f, "%s\t", rec->name) > 0;
		for (i = 0; ok && i < TRACKED_COUNT; ++i) {
			const file_stamp *s = &rec->stamp.files[i];

			if (s->present)
				ok = fprintf(f, "%s%s=%" PRIu64 "@%" PRId64, i ? " " : "", tracked[i],
						s->size, s->modified) > 0;
			else
				ok = fprintf(f, "%s%s=-", i ? " " : "", tracked[i]) > 0;
		}
		ok = ok && fputc('\n', f) != EOF;
	}
	if (fclose(f))
		ok = false;
	// FAT rename need not replace an existing destination. Keep the old
	// file until the new one commits; restore it if the commit fails.
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

static app_run_state judge(const char *name) {
	record *r = table_find(name);
	app_stamp current;

	if (!r)
		return APP_RUN_NEW;
	r->seen = true;
	if (!snapshot(table_root, name, &current) || !same_stamp(&r->stamp, &current))
		return APP_RUN_UPDATED;
	return APP_RUN_UNCHANGED;
}

void app_run_begin(const char *root) {
	table_load(root);
}

app_run_state app_run_status(const char *name) {
	return table_open ? judge(name) : APP_RUN_NEW;
}

void app_run_end(bool complete) {
	size_t i, kept = 0;

	if (table_open && complete) {
		for (i = 0; i < record_count; ++i) {
			if (records[i].seen)
				records[kept++] = records[i];
			else
				free(records[i].name);
		}
		if (kept != record_count) {
			record_count = kept;
			table_dirty = true;
		}
		if (table_dirty)
			table_write();
	}
	table_free();
}

app_run_state app_run_status_one(const char *root, const char *name) {
	app_run_state state;

	table_load(root);
	state = judge(name);
	table_free();
	return state;
}

// boot_path is <root>/apps/<name>/boot.dol|boot.elf; split it into root and name.
static bool split_boot_path(const char *boot_path, char *root, size_t root_size,
		char *name, size_t name_size) {
	const char *file = strrchr(boot_path, '/'), *name_start, *apps_start;
	size_t len;

	if (!file || (strcmp(file + 1, "boot.dol") && strcmp(file + 1, "boot.elf")))
		return false;
	for (name_start = file; name_start > boot_path && name_start[-1] != '/'; --name_start)
		;
	if (name_start == boot_path)
		return false;
	for (apps_start = name_start - 1; apps_start > boot_path && apps_start[-1] != '/'; --apps_start)
		;
	// name_start - 1 is the slash after "apps"; the root ends before the one ahead of it.
	if (name_start - 1 - apps_start != 4 || strncmp(apps_start, "apps", 4))
		return false;
	len = apps_start - 1 - boot_path;
	if (apps_start == boot_path || len >= root_size)
		return false;
	memcpy(root, boot_path, len);
	root[len] = 0;
	len = file - name_start;
	if (!len || len >= name_size || memchr(name_start, '\t', len))
		return false;
	memcpy(name, name_start, len);
	name[len] = 0;
	return true;
}

bool app_run_record(const char *boot_path) {
	char root[PATH_MAX], name[NAME_MAX + 1];
	app_stamp current;
	record *r;
	bool ok = true;

	if (!split_boot_path(boot_path, root, sizeof(root), name, sizeof(name)) ||
			!snapshot(root, name, &current))
		return false;
	table_load(root);
	r = table_find(name);
	if (!r) {
		ok = (r = table_add(name, &current)) != NULL;
		table_dirty = ok;
	} else if (!same_stamp(&r->stamp, &current)) {
		r->stamp = current;
		table_dirty = true;
	}
	if (ok && table_dirty)
		ok = table_write();
	table_free();
	return ok;
}
