// A WAD installed through ES (wiiupdate.h), from memory. The steps are
// RiftWii's channel installer's (github.com/KakarottoCake/Riftwii,
// channel/installer/install.cpp) and tests/wadinstall's: the ticket, the
// TMD, each content in 32 KiB pieces, then the finish, cancelling on any
// failure; a failed first install takes its ticket away again, so the
// title does not look installed with no TMD; and the stored TMD is read
// back to check the version that landed.
//
// A WAD is a 0x20-byte header, then the certificate chain, a CRL (none in
// any WAD this installs), the ticket, the TMD and the contents, each part
// starting on a 64-byte boundary. Each content in the data part is the
// encrypted one ES stores, padded to 16 bytes, and also starts on a
// 64-byte boundary, in the TMD's order.

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/es.h>

#include "wiiupdate.h"

#define CHUNK (32 * 1024)
#define MAX_CONTENT (64 * 1024 * 1024)

typedef struct {
	u32 certs, certs_n, tik, tik_n, tmd, tmd_n, data, data_n;
} layout;

static u32 be32(const u8 *p) {
	return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

static u32 align64(u32 n) {
	return (n + 63) & ~63u;
}

static int fail(char *err, size_t len, const char *fmt, s32 r) {
	if (err && len)
		snprintf(err, len, fmt, r);
	return -1;
}

// Every part, and every content in the data part, within the WAD.
static int parse(const u8 *wad, u32 size, layout *l, wiiupdate_wad *out) {
	const signed_blob *stmd;
	const tmd *t;
	u32 hdr, crl_n, at, i;

	if (size < 0x20)
		return -1;
	hdr = be32(wad);
	l->certs_n = be32(wad + 8);
	crl_n = be32(wad + 12);
	l->tik_n = be32(wad + 16);
	l->tmd_n = be32(wad + 20);
	l->data_n = be32(wad + 24);
	if (hdr != 0x20 || memcmp(wad + 4, "Is", 2) || crl_n || !l->certs_n || l->certs_n > 0x10000 ||
			l->tik_n != 0x2a4 || l->tmd_n > 0x10000 || l->data_n > size)
		return -1;
	l->certs = align64(hdr);
	l->tik = l->certs + align64(l->certs_n);
	l->tmd = l->tik + align64(l->tik_n);
	l->data = l->tmd + align64(l->tmd_n);
	if (l->data > size || l->data_n > size - l->data)
		return -1;

	stmd = (const signed_blob *) (wad + l->tmd);
	if (!IS_VALID_SIGNATURE(stmd) || l->tmd_n < SIGNATURE_SIZE(stmd) + sizeof(tmd))
		return -1;
	t = SIGNATURE_PAYLOAD(stmd);
	if (!t->num_contents || l->tmd_n < SIGNED_TMD_SIZE(stmd))
		return -1;
	for (i = 0, at = 0; i < t->num_contents; ++i) {
		u64 n = (t->contents[i].size + 15) & ~15ull;

		if (n > MAX_CONTENT || at + n > l->data_n)
			return -1;
		at += align64((u32) n);
	}

	if (out) {
		out->title_id = t->title_id;
		out->version = t->title_version;
		out->contents = t->num_contents;
		out->size = size;
	}
	return 0;
}

int wiiupdate_wad_parse(const u8 *wad, u32 size, wiiupdate_wad *out) {
	layout l;

	return parse(wad, size, &l, out);
}

s32 wiiupdate_title_version(u64 title_id) {
	static u8 view[0x400] ATTRIBUTE_ALIGN(32);
	u32 tickets = 0, size = 0;

	if (ES_GetNumTicketViews(title_id, &tickets) < 0 || !tickets)
		return -1;
	if (ES_GetTMDViewSize(title_id, &size) < 0 || size < sizeof(tmd_view) || size > sizeof(view))
		return -1;
	if (ES_GetTMDView(title_id, (tmd_view *) view, size) < 0)
		return -1;
	return ((const tmd_view *) view)->title_version;
}

static s32 delete_tickets(u64 title_id) {
	tikview *views;
	u32 count = 0, i;
	s32 r = 0;

	if (ES_GetNumTicketViews(title_id, &count) < 0 || !count)
		return 0;
	views = memalign(32, sizeof(tikview) * count);
	if (!views)
		return -1;
	r = ES_GetTicketViews(title_id, views, count);
	for (i = 0; r >= 0 && i < count; ++i)
		r = ES_DeleteTicket(&views[i]);
	free(views);
	return r;
}

// A part copied to its own 32-byte aligned buffer, as ES's ioctlv wants.
static void *part(const u8 *wad, u32 at, u32 size) {
	void *p = memalign(32, align64(size));

	if (p)
		memcpy(p, wad + at, size);
	return p;
}

int wiiupdate_wad_install(const u8 *wad, u32 size,
						  void (*progress)(void *ctx, u32 done, u32 total), void *ctx,
						  char *err, size_t err_len) {
	signed_blob *certs = NULL, *tik = NULL, *stmd = NULL;
	u8 *chunk = NULL;
	const tmd *t;
	layout l;
	wiiupdate_wad w;
	u32 i, at, done = 0;
	s32 r;
	bool had_title = true, ticket_added = false, started = false;
	int result = -1;

	if (parse(wad, size, &l, &w) < 0)
		return fail(err, err_len, "the WAD is damaged", 0);
	certs = part(wad, l.certs, l.certs_n);
	tik = part(wad, l.tik, l.tik_n);
	stmd = part(wad, l.tmd, l.tmd_n);
	chunk = memalign(32, CHUNK);
	if (!certs || !tik || !stmd || !chunk) {
		fail(err, err_len, "out of memory", 0);
		goto out;
	}
	t = SIGNATURE_PAYLOAD(stmd);
	had_title = wiiupdate_title_version(w.title_id) >= 0;

	r = ES_AddTicket(tik, l.tik_n, certs, l.certs_n, NULL, 0);
	if (r < 0) {
		fail(err, err_len, "ES_AddTicket: %d", r);
		goto out;
	}
	ticket_added = true;

	r = ES_AddTitleStart(stmd, l.tmd_n, certs, l.certs_n, NULL, 0);
	if (r < 0) {
		fail(err, err_len, "ES_AddTitleStart: %d", r);
		goto out;
	}
	started = true;

	for (i = 0, at = l.data; i < t->num_contents; ++i) {
		const tmd_content *c = &t->contents[i];
		u32 len = (u32) ((c->size + 15) & ~15ull), off;
		s32 cfd = ES_AddContentStart(w.title_id, c->cid);

		if (cfd < 0) {
			fail(err, err_len, "ES_AddContentStart: %d", cfd);
			goto out;
		}
		for (off = 0; off < len; off += CHUNK) {
			u32 n = len - off < CHUNK ? len - off : CHUNK;

			memcpy(chunk, wad + at + off, n);
			r = ES_AddContentData(cfd, chunk, n);
			if (r < 0) {
				fail(err, err_len, "ES_AddContentData: %d", r);
				goto out;
			}
			done += n;
			if (progress)
				progress(ctx, done, l.data_n);
		}
		r = ES_AddContentFinish(cfd);
		if (r < 0) {
			fail(err, err_len, "ES_AddContentFinish: %d", r);
			goto out;
		}
		at += align64(len);
	}

	r = ES_AddTitleFinish();
	started = false;
	if (r < 0) {
		fail(err, err_len, "ES_AddTitleFinish: %d", r);
		goto out;
	}

	r = wiiupdate_title_version(w.title_id);
	if (r != w.version) {
		fail(err, err_len, "installed, but the title reads back as version %d", r);
		goto out;
	}
	result = 0;

out:
	if (started)
		ES_AddTitleCancel();
	if (result < 0 && ticket_added && !had_title)
		delete_tickets(w.title_id);
	free(chunk);
	free(stmd);
	free(tik);
	free(certs);
	return result;
}
