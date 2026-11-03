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

#define DISCOVER_PORT 4299

/* A query is exactly HBCU and 12 zero bytes; byte 4 is a reply's version.
 * Nothing from port 4299 is a query: that is another console answering (or
 * this one, looped back), and answering it would set two Wiis replying to
 * each other. from_port is in host order. Kept here for tests/test_discover.py. */
static inline bool discover_is_query(const u8 *q, s32 n, u16 from_port) {
	return n == 16 && q[0] == 'H' && q[1] == 'B' && q[2] == 'C' && q[3] == 'U' &&
		   q[4] == 0 && from_port != DISCOVER_PORT;
}

#endif
