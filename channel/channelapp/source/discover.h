#ifndef DISCOVER_H_
#define DISCOVER_H_

#include <gctypes.h>
#include <stdbool.h>

/* UDP discovery on the Wiiload port (docs/devnet.md, HBCU). A client sends
 * one 16-byte query to the subnet broadcast or to a known console. The menu
 * and the in-app agent answer with the name HBCS would show. */
s32 discover_open(void);
/* Drains waiting queries. 0 when the socket is still good, or a negative
 * errno when it must be closed. netmask is the Wii's /16, as for TCP. */
s32 discover_poll(s32 fd, u32 netmask, const char *name, bool agent);

#endif
