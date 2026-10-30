// Updates of the installed channel from this project's GitHub releases,
// through sdk/wiiupdate (libwiiupdate, rewritten for HBC).
//
// Once the network is up, main.c starts a check: a thread asks GitHub for
// the latest releases and picks the highest-numbered one with a WAD for
// this channel's title ID (hbc-1.13.0-OHBC.wad, or -LULZ.wad for a build
// over the original channel). If it is newer than this build, main.c calls
// update_execute(), which asks, downloads the WAD into memory, checks it
// and installs it through ES, as RiftWii's channel installer does, with
// HBC's own dialogs: the question, then a progress bar for each step.
// Then HBC starts again, as the new version.
//
// Only the installed channel checks: a DOL started some other way is not
// what the WAD would replace.

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <ogcsys.h>

#include "../config.h"

#ifdef ENABLE_UPDATES

#include "view.h"
#include "dialogs.h"
#include "i18n.h"
#include "essig.h"
#include "title.h"
#include "wiiupdate.h"

#include "update.h"

#define UPDATE_RELEASES "https://api.github.com/repos/" UPDATE_OWNER "/" UPDATE_REPO "/releases?per_page=10"
#define UPDATE_MAX_JSON (512 * 1024)
#define UPDATE_MAX_WAD (16 * 1024 * 1024)
#define UPDATE_MSG_SIZE 4096

// ---- One worker thread at a time, its stack on the heap ----

static lwp_t worker = LWP_THREAD_NULL;
static u8 *worker_stack;
static volatile bool working;
static volatile int cancel;

static bool worker_start(void *(*func)(void *)) {
	worker_stack = memalign(32, UPDATE_THREAD_STACKSIZE);
	if (!worker_stack)
		return false;
	working = true;
	if (LWP_CreateThread(&worker, func, NULL, worker_stack, UPDATE_THREAD_STACKSIZE,
						 UPDATE_THREAD_PRIO) < 0) {
		working = false;
		free(worker_stack);
		worker_stack = NULL;
		return false;
	}
	return true;
}

// Once working has gone false.
static void worker_join(void) {
	LWP_JoinThread(worker, NULL);
	worker = LWP_THREAD_NULL;
	free(worker_stack);
	worker_stack = NULL;
}

// ---- The check ----

typedef enum {
	UPDATE_IDLE = 0,
	UPDATE_CHECKING,
	UPDATE_NONE,
	UPDATE_FOUND
} update_state;

static update_state us = UPDATE_IDLE;
static wiiupdate_release *found;
// The worker's answer; update_busy() moves it to us once the thread is done.
static volatile bool found_newer;

typedef struct {
	u8 *data;
	u32 len, max;
} buffer;

static int to_buffer(void *ctx, const u8 *data, size_t len) {
	buffer *b = ctx;

	// One byte stays free, for the JSON's terminating 0.
	if (!b->data || len >= b->max - b->len)
		return -1;
	memcpy(b->data + b->len, data, len);
	b->len += len;
	return 0;
}

// The WAD's name ends in the title's four characters: "-OHBC.wad".
static void wad_suffix(char *out) {
	u32 t = (u32) MY_TITLEID;

	sprintf(out, "-%c%c%c%c.wad", t >> 24, (t >> 16) & 0xff, (t >> 8) & 0xff, t & 0xff);
}

static void *check_func(void *arg) {
	buffer b = { malloc(UPDATE_MAX_JSON), 0, UPDATE_MAX_JSON };
	char suffix[16];
	bool newer = false;

	wad_suffix(suffix);
	if (b.data && wiiupdate_https_get(UPDATE_RELEASES, "application/vnd.github+json",
									  to_buffer, NULL, &b, &cancel) == 0) {
		b.data[b.len] = 0;
		if (wiiupdate_newest_release((const char *) b.data, suffix, found) == 0) {
			hlog("Update check: newest release %s\n", found->tag);
			newer = wiiupdate_compare_versions(found->tag, CHANNEL_VERSION_STR) > 0;
		} else {
			hlog("Update check: no release with a %s\n", suffix);
		}
	} else {
		hlog("Update check: GitHub did not answer\n");
	}
	free(b.data);
	found_newer = newer;
	working = false;
	return NULL;
}

bool update_signal(void) {
	if (us != UPDATE_IDLE || working || title_get_id() != MY_TITLEID)
		return false;
	found = malloc(sizeof(*found));
	if (!found)
		return false;
	cancel = 0;
	found_newer = false;
	us = UPDATE_CHECKING;
	if (!worker_start(check_func)) {
		us = UPDATE_NONE;
		free(found);
		found = NULL;
		return false;
	}
	return true;
}

bool update_busy(bool *update_available) {
	if (us != UPDATE_CHECKING)
		return false;
	if (working)
		return true;
	worker_join();
	us = found_newer ? UPDATE_FOUND : UPDATE_NONE;
	*update_available = us == UPDATE_FOUND;
	if (!*update_available) {
		free(found);
		found = NULL;
	}
	return false;
}

// ---- Download and install ----

static buffer wad;
static volatile u32 step_done, step_total;
static char install_error[128];

static void on_length(void *ctx, s64 len) {
	if (len > 0 && len <= UPDATE_MAX_WAD)
		step_total = (u32) len;
}

static int to_wad(void *ctx, const u8 *data, size_t len) {
	if (to_buffer(ctx, data, len) < 0)
		return -1;
	step_done = wad.len;
	return 0;
}

static volatile bool download_ok;

static void *download_func(void *arg) {
	download_ok = wiiupdate_https_get(found->wad_url, "application/octet-stream", to_wad,
									  on_length, &wad, &cancel) == 0;
	working = false;
	return NULL;
}

static void on_install(void *ctx, u32 done, u32 total) {
	step_done = done;
	step_total = total;
}

static volatile bool install_ok;

static void *install_func(void *arg) {
	install_ok = wiiupdate_wad_install(wad.data, wad.len, on_install, NULL, install_error,
									   sizeof(install_error)) == 0;
	working = false;
	return NULL;
}

// Runs func on the worker while a progress bar shows its steps; the bar
// appears once step_total is known. A download's caption says its size.
static void run_with_progress(view *sub_view, void *(*func)(void *), bool download) {
	static char caption[64];
	view *v = NULL;

	step_done = step_total = 0;
	if (!worker_start(func))
		return;
	while (working) {
		if (!v && step_total) {
			if (download)
				snprintf(caption, sizeof(caption), _("Downloading Update (%u kB)"),
						 step_total / 1024);
			else
				snprintf(caption, sizeof(caption), "%s", _("Installing Update"));
			v = dialog_progress(sub_view, caption, step_total);
			dialog_fade(v, true);
		}
		if (v)
			dialog_set_progress(v, step_done);
		view_plot(v ? v : sub_view, DIALOG_MASK_COLOR, NULL, NULL, NULL);
	}
	worker_join();
	if (v)
		dialog_fade(v, false);
}

static bool update_failed(view *sub_view, const char *text) {
	show_message_timed(sub_view, DLGMT_ERROR, DLGB_OK, text, 0, NET_POPUP_TIMEOUT_S);
	return false;
}

static bool install(view *sub_view) {
	static char text[UPDATE_MSG_SIZE];
	wiiupdate_wad w;
	s32 installed;
	int sites;

	wad.data = malloc(UPDATE_MAX_WAD);
	wad.len = 0;
	wad.max = UPDATE_MAX_WAD;
	if (!wad.data)
		return update_failed(sub_view, _("Download failed"));
	download_ok = false;
	run_with_progress(sub_view, download_func, true);
	hlog("Update: downloaded %u bytes of %s\n", wad.len, found->wad_url);
	if (!download_ok)
		return update_failed(sub_view, _("Download failed"));

	// The WAD must be this channel, newer than what is installed.
	installed = wiiupdate_title_version(MY_TITLEID);
	if (wiiupdate_wad_parse(wad.data, wad.len, &w) < 0 || w.title_id != MY_TITLEID ||
			(installed >= 0 && w.version <= installed)) {
		hlog("Update: the WAD is not one to install (title %016llx, version %u, installed %d)\n",
			 w.title_id, w.version, installed);
		return update_failed(sub_view, _("The update is not for this channel."));
	}

	sites = es_sigcheck_lift();
	hlog("Update: installing version %u over %d (%d signature check sites lifted)\n",
		 w.version, installed, sites);
	install_ok = false;
	install_error[0] = 0;
	run_with_progress(sub_view, install_func, false);
	es_sigcheck_restore();
	if (!install_ok) {
		hlog("Update: install failed: %s\n", install_error);
		snprintf(text, sizeof(text), _("The update could not be installed (%s). "
									   "The Homebrew Channel is unchanged."), install_error);
		return update_failed(sub_view, text);
	}
	hlog("Update: installed %s\n", found->tag);
	snprintf(text, sizeof(text), _("The Homebrew Channel %s is installed. "
								   "It will now start again."), found->tag);
	show_message_timed(sub_view, DLGMT_INFO, DLGB_OK, text, 0, NET_POPUP_TIMEOUT_S);
	return true;
}

bool update_execute(view *sub_view) {
	static char text[UPDATE_MSG_SIZE];
	bool ok = false;

	if (us != UPDATE_FOUND || !found)
		return false;
	us = UPDATE_NONE;

	snprintf(text, sizeof(text), _("An update to the Homebrew Channel "
				"(version %s, replacing the installed version %s) "
				"is available for installation, do you want "
				"to update now?\n\n"
				"Release notes:\n\n%s"),
				found->tag, CHANNEL_VERSION_STR, found->notes);

	if (show_message_timed(sub_view, DLGMT_CONFIRM, DLGB_YESNO, text, 0,
						   NET_POPUP_TIMEOUT_S) == 0)
		ok = install(sub_view);

	free(wad.data);
	wad.data = NULL;
	free(found);
	found = NULL;
	return ok;
}

#endif
