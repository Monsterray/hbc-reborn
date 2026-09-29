#ifndef _DEVNET_H_
#define _DEVNET_H_

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

#endif
