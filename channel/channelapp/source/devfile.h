#ifndef _DEVFILE_H_
#define _DEVFILE_H_

#include <gctypes.h>

#include "devstream.h"

// HBCF header byte 5 for op 'g': the client accepts zlib frames.
#define DEVNET_FLAG_COMPRESS 0x01

// The most recent file transfer, for the status reply.
typedef struct {
	char op;
	u32 bytes;
	u64 total;
	devstream_stats st;
} devfile_transfer;

extern devfile_transfer devfile_last;

// Called after a put or delete of path succeeds.
typedef void (*devfile_change_fn)(const char *path);

// Set by a host that mounts devices on demand (HBC: a drive other than the
// one its app list comes from); called with each valid path before use.
extern void (*devfile_mount_hook)(const char *path);

// Runs the HBCF request whose 16-byte header is hdr on socket s. The calling
// thread runs at prio (and the transfer worker with it) until the request
// ends, then returns to restore_prio.
void devfile_handle(s32 s, const u8 *hdr, s32 prio, s32 restore_prio,
					devfile_change_fn changed);

// Blocking send of all of data; see devfile.c for why.
bool devfile_send_all(s32 s, const void *data, u32 len);
// Reply header (s32 status, u32 length) plus payload.
bool devfile_reply(s32 s, s32 status, const void *data, u32 len);

// Stops a running file request and waits (up to 3 s) for it to unwind.
void devfile_abort(void);
bool devfile_aborted(void);
// Priority of the thread running the current request, for its worker.
s32 devfile_prio(void);

#endif
