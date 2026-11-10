#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef GEKKO
#include <ogc/mutex.h>
#include <ogc/machine/processor.h>
#endif

#include "apprun.h"

#ifdef _WIN32
#define MKDIR(path) mkdir(path)
#define strcasecmp _stricmp
#else
#include <strings.h>
#define MKDIR(path) mkdir(path, 0777)
#endif

// One table at a time: the scan thread holds it from app_run_begin to
// app_run_end, and app_run_status_one and app_run_record (the menu, the
// loader) wait for that rather than load over it.
#ifdef GEKKO
static mutex_t run_lock = LWP_MUTEX_NULL;

static void lock(void) {
	u32 level;

	_CPU_ISR_Disable(level);
	if (run_lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&run_lock, false);
	_CPU_ISR_Restore(level);
	LWP_MutexLock(run_lock);
}

static void unlock(void) {
	LWP_MutexUnlock(run_lock);
}
#else
static void lock(void) {}
static void unlock(void) {}
#endif

static const char dir_name[] = "hbc";
static const char file_name[] = "apps_seen.txt";
static const char missed_word[] = "missed";
static const char *tracked[] = { "boot.dol", "boot.elf", "meta.xml", "icon.png" };
#define TRACKED_COUNT (sizeof(tracked) / sizeof(tracked[0]))
#define MAX_RECORDS 4096
#define LINE_MAX_LEN 1024
#define META_INDEX 2	// "meta.xml" in tracked[]
#define META_MAX 65536

typedef struct {
	unsigned present;
	uint64_t size;
	int64_t modified;
	// meta.xml only: a hash of its text without <sort_id> (meta_hash). Set
	// when the file was small enough to read. A line written by an older HBC
	// has none: that file is then judged by size and time.
	unsigned has_hash;
	uint32_t hash;
} file_stamp;

typedef struct {
	file_stamp files[TRACKED_COUNT];
} app_stamp;

typedef struct {
	char *name;
	app_stamp stamp;
	bool seen;
	// Not seen by the last scan that ended whole. A second such scan in a
	// row prunes the record; one miss may be a directory read error.
	bool missed;
} record;

static record *records;
static size_t record_count, record_cap;
static char table_root[PATH_MAX];
// table_readable: the file was read to its end, or there was none. After
// any other failure (EIO, a card pulled while reading) the table is only
// part of the file: never write it back, nor prune from it.
// table_missing: the file read as "not found" (ENOENT), the table is empty.
static bool table_dirty, table_open, table_readable, table_missing;

static void table_free(void) {
	size_t i;

	for (i = 0; i < record_count; ++i)
		free(records[i].name);
	free(records);
	records = NULL;
	record_count = record_cap = 0;
	table_dirty = false;
	table_open = false;
	table_readable = false;
	table_missing = false;
}

static record *table_find(const char *name) {
	size_t i;

	// FAT ignores case: "MyApp" and "myapp" are one folder.
	for (i = 0; i < record_count; ++i)
		if (!strcasecmp(records[i].name, name))
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
	r->missed = false;
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

static bool is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// FNV-1a over meta.xml with every <sort_id>...</sort_id> removed (hbc.py order
// and the Custom order rewrite it) and without the white space around tags, so
// a reorder leaves the hash alone and any other edit changes it.
static bool meta_hash(const char *path, uint32_t *out) {
	FILE *f = fopen(path, "rb");
	char *b, *at, *end;
	size_t n, i, j;
	uint32_t h = 2166136261u;

	if (!f)
		return false;
	b = malloc(META_MAX + 1);
	if (!b) {
		fclose(f);
		return false;
	}
	n = fread(b, 1, META_MAX + 1, f);
	if (ferror(f) || n > META_MAX) {
		fclose(f);
		free(b);
		return false;
	}
	fclose(f);
	b[n] = 0;
	while ((at = strstr(b, "<sort_id>")) && (end = strstr(at, "</sort_id>"))) {
		end += strlen("</sort_id>");
		memmove(at, end, n - (size_t) (end - b));
		n -= (size_t) (end - at);
		b[n] = 0;
	}
	for (i = 0; i < n; i = j) {
		if (is_space(b[i])) {
			for (j = i; j < n && is_space(b[j]); ++j)
				;
			if (!i || b[i - 1] == '>' || j == n || b[j] == '<')
				continue;
			for (; i < j; ++i)
				h = (h ^ (unsigned char) b[i]) * 16777619u;
			continue;
		}
		h = (h ^ (unsigned char) b[i]) * 16777619u;
		j = i + 1;
	}
	free(b);
	*out = h;
	return true;
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
		if (i == META_INDEX)
			stamp->files[i].has_hash = meta_hash(path, &stamp->files[i].hash);
	}
	// A launch needs an executable; don't create history for an
	// incomplete installation or a folder that vanished.
	return stamp->files[0].present || stamp->files[1].present;
}

static bool same_stamp(const app_stamp *a, const app_stamp *b) {
	unsigned i;

	for (i = 0; i < TRACKED_COUNT; ++i) {
		const file_stamp *x = &a->files[i], *y = &b->files[i];
		if (x->present != y->present)
			return false;
		if (i == META_INDEX && x->has_hash && y->has_hash) {
			if (x->hash != y->hash)
				return false;
			continue;
		}
		if (x->size != y->size || x->modified != y->modified)
			return false;
	}
	return true;
}

// "<name>\tboot.dol=SIZE@TIME boot.elf=- meta.xml=h:HEX8 icon.png=SIZE@TIME[ missed]"
// (meta.xml=SIZE@TIME is the older form; see apprun.h)
static bool parse_line(char *line, char **name, app_stamp *stamp, bool *missed) {
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
		} else if (i == META_INDEX && p[0] == 'h' && p[1] == ':') {
			char *e;
			unsigned long v;

			p += 2;
			if (!isxdigit((unsigned char) *p))
				return false;
			v = strtoul(p, &e, 16);
			if (e - p != 8)
				return false;
			p = e;
			s->present = s->has_hash = 1;
			s->hash = (uint32_t) v;
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
	*missed = !strcmp(p, missed_word);
	return *p == 0 || *missed;
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
	if (!f) {
		table_readable = errno == ENOENT;
		table_missing = table_readable;
		return;
	}
	while (record_count < MAX_RECORDS && fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		char *name;
		app_stamp stamp;
		bool missed;
		record *added;

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
		if (line[0] == '#' || !parse_line(line, &name, &stamp, &missed) || table_find(name))
			continue;
		if ((added = table_add(name, &stamp)))
			added->missed = missed;
	}
	table_readable = !ferror(f) && (feof(f) || record_count == MAX_RECORDS);
	fclose(f);
}

static bool table_write(void) {
	char path[PATH_MAX], part[PATH_MAX + 8], backup[PATH_MAX + 8], dir[PATH_MAX];
	struct stat st;
	FILE *f;
	bool ok, moved_old = false;
	size_t r;
	unsigned i;

	if (!table_readable)
		return false;
	if (!file_path(path, sizeof(path), table_root, "") ||
			!file_path(part, sizeof(part), table_root, ".part") ||
			!file_path(backup, sizeof(backup), table_root, ".bak") ||
			snprintf(dir, sizeof(dir), "%s/%s", table_root, dir_name) >= (int) sizeof(dir))
		return false;
	// libfat turns a sector read error during the path lookup into ENOENT, so
	// "not found" at load can be a file that is there. Writing this empty
	// table over it would erase every app's history (and mark all "New").
	if (table_missing && (!stat(path, &st) || !stat(backup, &st)))
		return false;
	if (MKDIR(dir) && errno != EEXIST)
		return false;
	if (!(f = fopen(part, "wb")))
		return false;
	ok = fputs("# HBC app badges: one app folder per line, then the size@modified time of\n"
			"# its files at the last run from HBC (meta.xml: a hash of its text without\n"
			"# <sort_id>, h:HEX8; an older meta.xml=SIZE@TIME is judged by size and time\n"
			"# until the app's next run). Delete this file to start over.\n", f) >= 0;
	for (r = 0; ok && r < record_count; ++r) {
		const record *rec = &records[r];

		ok = fprintf(f, "%s\t", rec->name) > 0;
		for (i = 0; ok && i < TRACKED_COUNT; ++i) {
			const file_stamp *s = &rec->stamp.files[i];

			if (s->present && s->has_hash)
				ok = fprintf(f, "%s%s=h:%08" PRIx32, i ? " " : "", tracked[i], s->hash) > 0;
			else if (s->present)
				ok = fprintf(f, "%s%s=%" PRIu64 "@%" PRId64, i ? " " : "", tracked[i],
						s->size, s->modified) > 0;
			else
				ok = fprintf(f, "%s%s=-", i ? " " : "", tracked[i]) > 0;
		}
		if (ok && rec->missed)
			ok = fprintf(f, " %s", missed_word) > 0;
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
	// A folder that cannot be read now (a stat error, no boot file) is not
	// "updated": app_run_record could never clear that badge.
	if (!snapshot(table_root, name, &current))
		return APP_RUN_UNCHANGED;
	if (!same_stamp(&r->stamp, &current))
		return APP_RUN_UPDATED;
	return APP_RUN_UNCHANGED;
}

void app_run_begin(const char *root) {
	lock();
	table_load(root);
}

app_run_state app_run_status(const char *name) {
	return table_open ? judge(name) : APP_RUN_NEW;
}

void app_run_end(bool complete) {
	size_t i, kept = 0;

	if (table_open && complete && table_readable) {
		// libfat's readdir shows a read error as the end of the folder, which
		// no flag can tell from a whole scan. So a record goes only after two
		// whole scans in a row missed it (the flag is kept in the file).
		for (i = 0; i < record_count; ++i) {
			record *r = &records[i];

			if (!r->seen && r->missed) {
				free(r->name);
				table_dirty = true;
				continue;
			}
			if (r->seen == r->missed) {	// seen after a miss, or missed the first time
				r->missed = !r->seen;
				table_dirty = true;
			}
			records[kept++] = *r;
		}
		record_count = kept;
		if (table_dirty)
			table_write();
	}
	table_free();
	unlock();
}

app_run_state app_run_status_one(const char *root, const char *name) {
	app_run_state state;

	lock();
	table_load(root);
	state = judge(name);
	table_free();
	unlock();
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
	lock();
	table_load(root);
	r = table_find(name);
	if (!r) {
		ok = (r = table_add(name, &current)) != NULL;
		table_dirty = ok;
	} else if (!same_stamp(&r->stamp, &current) || r->missed ||
			(current.files[META_INDEX].has_hash && !r->stamp.files[META_INDEX].has_hash)) {
		r->stamp = current;
		r->missed = false;
		table_dirty = true;
	}
	if (ok && table_dirty)
		ok = table_write();
	table_free();
	unlock();
	return ok;
}

// Does a network upload (HBCA caption "<name>/boot.dol" or boot.elf) hold the
// very bytes of the installed app <root>/apps/<name>/<file>? Then the run is
// that app's: boot_path gets the path for app_run_record. Size first, then
// the content. Anything else (a loose file, another build) is not recorded.
bool app_run_match_upload(const char *root, const char *caption, const void *data,
		size_t len, char *boot_path, size_t boot_size) {
	char name[NAME_MAX + 1], path[PATH_MAX], buf[1024];
	const char *slash = caption ? strrchr(caption, '/') : NULL, *file;
	const unsigned char *d = data;
	struct stat st;
	FILE *f;
	size_t off = 0, n, nlen;
	bool same = true;

	if (!slash || slash == caption)
		return false;
	file = slash + 1;
	if (!strcasecmp(file, "boot.dol"))
		file = tracked[0];
	else if (!strcasecmp(file, "boot.elf"))
		file = tracked[1];
	else
		return false;
	nlen = (size_t) (slash - caption);
	if (nlen >= sizeof(name) || memchr(caption, '/', nlen) || memchr(caption, '\\', nlen) ||
			memchr(caption, ':', nlen) || memchr(caption, '\t', nlen) ||
			(nlen == 1 && caption[0] == '.') || (nlen == 2 && !strncmp(caption, "..", 2)))
		return false;
	memcpy(name, caption, nlen);
	name[nlen] = 0;
	if (!app_file_path(path, sizeof(path), root, name, file) || stat(path, &st) ||
			!S_ISREG(st.st_mode) || (uint64_t) st.st_size != len)
		return false;
	if (!(f = fopen(path, "rb")))
		return false;
	while (same && off < len) {
		n = fread(buf, 1, len - off < sizeof(buf) ? len - off : sizeof(buf), f);
		if (!n || memcmp(buf, d + off, n))
			same = false;
		off += n;
	}
	fclose(f);
	if (!same || snprintf(boot_path, boot_size, "%s", path) >= (int) boot_size)
		return false;
	return true;
}
