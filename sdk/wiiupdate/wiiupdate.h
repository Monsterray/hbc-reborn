// Updates from GitHub releases, installed as a WAD: a rewrite of libwiiupdate
// (github.com/quatric/libwiiupdate) for HBC's channel, which updates itself
// as a title rather than as files on the SD card. See README.md.
//
//  - https.c: an HTTPS GET over libogc's sockets, TLS 1.2 through the
//    mbedTLS in mbedtls/ (configured by hbc_mbedtls_config.h), the server checked
//    against the roots in certs.h
//  - release.c: GitHub's /releases reply: the newest release and its WAD
//  - wad.c: a WAD installed through ES, after RiftWii's channel installer
//
// Nothing here brings up the network or draws: the caller does both.

#ifndef _WIIUPDATE_H_
#define _WIIUPDATE_H_

#include <stddef.h>
#include <gctypes.h>

// ---- HTTPS (https.c) ----

// Gets the body as it arrives; returning < 0 stops the transfer.
typedef int (*wiiupdate_sink)(void *ctx, const u8 *data, size_t len);

// GET url (https:// only), following up to five redirects. on_length (may
// be NULL) gets the body's size, -1 if unknown, before the first sink call.
// *cancel set from another thread stops it within a quarter second.
// Returns 0 once the whole body of a 200 reply has gone to the sink.
int wiiupdate_https_get(const char *url, const char *accept, wiiupdate_sink sink,
						void (*on_length)(void *ctx, s64 len), void *ctx,
						volatile int *cancel);

// ---- Releases (release.c) ----

typedef struct {
	char tag[48];		// e.g. "v1.13.0"
	char wad_url[512];	// the release asset ending in the suffix asked for
	char notes[2048];	// the release's text, unescaped, cut to fit
} wiiupdate_release;

// Compares two versions numerically ("v1.10" > "1.9.2"); text before the
// first digit is skipped. Returns < 0, 0 or > 0.
int wiiupdate_compare_versions(const char *a, const char *b);

// From the JSON of GET /repos/OWNER/REPO/releases: the highest-numbered
// release that is not a draft or prerelease and has an asset whose name
// ends in wad_suffix (e.g. "-OHBC.wad"). Rolling tags with no digits
// ("latest") are skipped. Returns 0 when one was found.
int wiiupdate_newest_release(const char *json, const char *wad_suffix,
							 wiiupdate_release *out);

// ---- WADs (wad.c) ----

typedef struct {
	u64 title_id;
	u16 version;		// the TMD's title version
	u16 contents;
	u32 size;			// of the WAD, all its parts checked to lie inside it
} wiiupdate_wad;

// Checks a WAD held in memory: its header, every part inside the buffer,
// and every content inside the data part. Returns 0 if it is one to install.
int wiiupdate_wad_parse(const u8 *wad, u32 size, wiiupdate_wad *out);

// The installed version of a title, or -1 when it is not installed.
s32 wiiupdate_title_version(u64 title_id);

// Installs a WAD held in memory through ES: its ticket, its TMD, then each
// content in 32 KiB pieces, as RiftWii's channel installer and
// tests/wadinstall do. Any failure before the last step cancels the
// install, so the title already on the console stays as it was; a failed
// first install also takes its new ticket away again. The stored TMD is
// read back at the end and must carry the WAD's version. progress (may be
// NULL) gets bytes written so far and the total. ES must accept the WAD's
// signatures (see channel/channelapp/source/essig.c). Returns 0 on success;
// err gets a short reason otherwise.
int wiiupdate_wad_install(const u8 *wad, u32 size,
						  void (*progress)(void *ctx, u32 done, u32 total), void *ctx,
						  char *err, size_t err_len);

#endif
