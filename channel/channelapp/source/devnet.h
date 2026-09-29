#ifndef _DEVNET_H_
#define _DEVNET_H_

#include <gctypes.h>

// Handles a developer request whose 16-byte header is hdr. Returns false
// when the header is not a developer request.
bool devnet_handle(s32 s, const u8 *hdr, u32 client_ip);

#endif
