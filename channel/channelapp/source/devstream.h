#ifndef _DEVSTREAM_H_
#define _DEVSTREAM_H_

#include <gctypes.h>

typedef struct {
	u64 net, disk, cpu;   // ticks spent on Wi-Fi, SD and zlib/CRC
	u32 wire;             // bytes that crossed the network
} devstream_stats;

// Receives a framed upload of size raw bytes into part, then renames it to
// path. Returns 0 or a negative errno.
s32 devstream_put(s32 s, const char *path, const char *part, u32 size,
				  devstream_stats *stats);

// Sends path as a reply header followed by frames and a terminator frame.
void devstream_get(s32 s, const char *path, bool compress, devstream_stats *stats);

#endif
