#ifndef _DEVNET_H_
#define _DEVNET_H_

#include <stddef.h>
#include <gctypes.h>

// Developer protocol version reported by HBCS; 2 adds framed transfers, 3
// crash reports (HBCC) and the in-app agent's exit request (HBCX), 4 the
// kind, code and reason of a crash (fatals and hangs) and the kept log (HBCL),
// 5 the name of the next Wiiload upload, for the play log (HBCA), 6 HBCA's
// flags and each upload's result (HBCS "upload").
#define DEVNET_PROTO 6
// Transfers run above the UI thread (64) and the loader's usual 48.
#define DEVNET_THREAD_PRIO 80

// Handles a developer request whose 16-byte header is hdr. Returns false
// when the header is not a developer request.
bool devnet_handle(s32 s, const u8 *hdr, u32 client_ip);

// Takes the crash report an agent-enabled app left and the kept log target
// from MEM2. Call first in main(), before allocations can reach them.
void devnet_early_init(void);
// Restores the app log target that survived the last app launch.
void devnet_init(void);
// Records how long HBC took to reach its menu, for the status reply.
void devnet_set_init_ms(u32 ms);
// The name the last app's agent gave itself, from its kept log; NULL if none.
const char *devnet_lastlog_app(void);
// The name a sender gave the next Wiiload upload (HBCA) in the last two
// minutes: copied to out and forgotten. False when there is none.
bool devnet_take_upload_name(char *out, size_t size);
// HBCA's flags for the next upload, while its name would be kept.
#define DEVNET_UPLOAD_YES 1 // install a ZIP without asking
u32 devnet_upload_flags(void);
// What became of an upload, for HBCS "upload": result is "launched",
// "installed", "theme", "declined" or "error"; error a short code for an
// error ("receive", "not_wii_app", ...) and text the message HBC showed.
// Forgets the sender's flags, and its name unless the app launched.
void devnet_upload_result(const char *result, const char *error, const char *text);
// Startup steps for HBCS "startup": name and ms since devnet_early_init()
// (the start of main), in the order they happen. Any thread may call it.
void devnet_boot_mark(const char *name);
// Takes the next app folder name a file request changed under
// "<device>:/apps/", so the menu can reload that entry.
bool devnet_take_app_change(char *dirname, size_t size);

#endif
