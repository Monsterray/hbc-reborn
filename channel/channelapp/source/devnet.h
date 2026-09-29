#ifndef _DEVNET_H_
#define _DEVNET_H_

#include <stddef.h>
#include <gctypes.h>

// Developer protocol version reported by HBCS; 2 adds framed transfers.
#define DEVNET_PROTO 2
// HBCF header byte 5 for op 'g': the client accepts zlib frames.
#define DEVNET_FLAG_COMPRESS 0x01
// Transfers run above the UI thread (64) and the loader's usual 48.
#define DEVNET_THREAD_PRIO 80

// Handles a developer request whose 16-byte header is hdr. Returns false
// when the header is not a developer request.
bool devnet_handle(s32 s, const u8 *hdr, u32 client_ip);

// Blocking send of all of data; see devnet.c for why.
bool devnet_send_all(s32 s, const void *data, u32 len);
// Reply header (s32 status, u32 length) plus payload.
bool devnet_reply(s32 s, s32 status, const void *data, u32 len);

// Restores the app log target that survived the last app launch.
void devnet_init(void);
// Records how long HBC took to reach its menu, for the status reply.
void devnet_set_init_ms(u32 ms);
// Stops a running file transfer and waits (up to 3 s) for it to unwind,
// before an app launch unmounts the card.
void devnet_abort(void);
bool devnet_aborted(void);
// Takes the next app folder name a file request changed under
// "<device>:/apps/", so the menu can reload that entry.
bool devnet_take_app_change(char *dirname, size_t size);

#endif
