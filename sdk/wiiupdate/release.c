// GitHub's /releases reply (wiiupdate.h): the newest release with a WAD.
// libwiiupdate's version comparison, and a small JSON walk in place of its
// string search, so that a release's own keys are told from its assets'.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "wiiupdate.h"

int wiiupdate_compare_versions(const char *a, const char *b) {
	while (*a && !isdigit((unsigned char) *a))
		a++;
	while (*b && !isdigit((unsigned char) *b))
		b++;
	while (*a || *b) {
		unsigned long x = strtoul(a, (char **) &a, 10);
		unsigned long y = strtoul(b, (char **) &b, 10);

		if (x != y)
			return x < y ? -1 : 1;
		// Only a "." and a digit carry on; "-beta" and the like end it.
		a = (*a == '.' && isdigit((unsigned char) a[1])) ? a + 1 : "";
		b = (*b == '.' && isdigit((unsigned char) b[1])) ? b + 1 : "";
	}
	return 0;
}

// ---- JSON ----
//
// Just enough to walk GitHub's reply: values are skipped whole, strings
// with their escapes, and an object's or array's members are visited in
// order. Every function takes the end of the text and stops there.

static const char *ws(const char *p, const char *end) {
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

// Past a string starting at its quote; NULL if it does not end.
static const char *skip_string(const char *p, const char *end) {
	for (p++; p < end; p++) {
		if (*p == '\\')
			p++;
		else if (*p == '"')
			return p + 1;
	}
	return NULL;
}

// Past any value.
static const char *skip_value(const char *p, const char *end) {
	int depth = 0;

	p = ws(p, end);
	if (p >= end)
		return NULL;
	if (*p == '"')
		return skip_string(p, end);
	if (*p != '{' && *p != '[') {
		while (p < end && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char) *p))
			p++;
		return p;
	}
	for (; p < end; p++) {
		if (*p == '"') {
			p = skip_string(p, end);
			if (!p)
				return NULL;
			p--;
		} else if (*p == '{' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			if (--depth == 0)
				return p + 1;
		}
	}
	return NULL;
}

// The value of member key in the object at obj, or NULL.
static const char *member(const char *obj, const char *end, const char *key) {
	const char *p = ws(obj, end);
	size_t n = strlen(key);

	if (p >= end || *p != '{')
		return NULL;
	for (p = ws(p + 1, end); p < end && *p == '"'; p = ws(p + 1, end)) {
		const char *name = p + 1, *after = skip_string(p, end);

		if (!after)
			return NULL;
		p = ws(after, end);
		if (p >= end || *p != ':')
			return NULL;
		p = ws(p + 1, end);
		if ((size_t) (after - 1 - name) == n && !memcmp(name, key, n))
			return p;
		p = skip_value(p, end);
		if (!p)
			return NULL;
		p = ws(p, end);
		if (p >= end || *p != ',')
			return NULL;
	}
	return NULL;
}

// The array element after p (the '[' or a previous element's end), or NULL.
static const char *next_element(const char *p, const char *end, bool first) {
	p = ws(p, end);
	if (p >= end)
		return NULL;
	if (first) {
		if (*p != '[')
			return NULL;
	} else if (*p != ',') {
		return NULL;
	}
	p = ws(p + 1, end);
	return p < end && *p != ']' ? p : NULL;
}

static void put_utf8(char **o, char *oend, u32 c) {
	char t[3];
	int n, i;

	if (c < 0x80) {
		t[0] = c;
		n = 1;
	} else if (c < 0x800) {
		t[0] = 0xc0 | c >> 6;
		t[1] = 0x80 | (c & 0x3f);
		n = 2;
	} else {
		t[0] = 0xe0 | c >> 12;
		t[1] = 0x80 | ((c >> 6) & 0x3f);
		t[2] = 0x80 | (c & 0x3f);
		n = 3;
	}
	if (*o + n >= oend)
		return;
	for (i = 0; i < n; i++)
		*(*o)++ = t[i];
}

// The string at p, unescaped and cut to fit (a UTF-16 surrogate pair becomes
// one '?', as HBC's fonts have none of those characters). Returns false when p is
// not a string.
static bool get_string(const char *p, const char *end, char *out, size_t len) {
	char *o = out, *oend = out + len;

	if (!len || p >= end || *p != '"')
		return false;
	for (p++; p < end && *p != '"'; p++) {
		u32 c = (u8) *p;

		if (c == '\\' && p + 1 < end) {
			switch (*++p) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			// GitHub's text has CR LF line ends; a CR would draw as a box.
			case 'r': continue;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u':
				if (p + 4 < end) {
					char hex[5] = { p[1], p[2], p[3], p[4], 0 };

					c = strtoul(hex, NULL, 16);
					p += 4;
					if (c >= 0xdc00 && c < 0xe000)
						continue;
					if (c >= 0xd800 && c < 0xdc00)
						c = '?';
				}
				break;
			default: c = (u8) *p; break;
			}
			put_utf8(&o, oend, c);
		} else if (o + 1 < oend) {
			*o++ = c;
		}
	}
	*o = 0;
	return p < end;
}

static bool is_true(const char *v, const char *end) {
	return v && end - v >= 4 && !strncmp(v, "true", 4);
}

static bool ends_with(const char *s, const char *suffix) {
	size_t a = strlen(s), b = strlen(suffix);

	return a >= b && !strcasecmp(s + a - b, suffix);
}

static bool has_digit(const char *s) {
	for (; *s; s++)
		if (isdigit((unsigned char) *s))
			return true;
	return false;
}

// The asset of release rel whose name ends in suffix.
static bool find_wad(const char *rel, const char *end, const char *suffix, char *url, size_t len) {
	const char *assets = member(rel, end, "assets"), *a;
	bool first = true;

	if (!assets)
		return false;
	for (a = assets; (a = next_element(a, end, first)); first = false) {
		char name[128];

		if (get_string(member(a, end, "name"), end, name, sizeof(name)) && ends_with(name, suffix) &&
				get_string(member(a, end, "browser_download_url"), end, url, len))
			return true;
		a = skip_value(a, end);
		if (!a)
			break;
	}
	return false;
}

int wiiupdate_newest_release(const char *json, const char *wad_suffix, wiiupdate_release *out) {
	const char *end = json + strlen(json), *rel, *best = NULL;
	bool first = true;

	memset(out, 0, sizeof(*out));
	for (rel = json; (rel = next_element(rel, end, first)); first = false) {
		char tag[sizeof(out->tag)], url[sizeof(out->wad_url)];

		if (get_string(member(rel, end, "tag_name"), end, tag, sizeof(tag)) && has_digit(tag) &&
				!is_true(member(rel, end, "draft"), end) &&
				!is_true(member(rel, end, "prerelease"), end) &&
				(!out->tag[0] || wiiupdate_compare_versions(tag, out->tag) > 0) &&
				find_wad(rel, end, wad_suffix, url, sizeof(url))) {
			strcpy(out->tag, tag);
			strcpy(out->wad_url, url);
			best = rel;
		}
		rel = skip_value(rel, end);
		if (!rel)
			break;
	}
	if (!best)
		return -1;
	if (!get_string(member(best, end, "body"), end, out->notes, sizeof(out->notes)))
		out->notes[0] = 0;
	return 0;
}
