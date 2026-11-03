#include <sys/errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <network.h>

#include "../config.h"
#include "discover.h"

#define DISCOVER_PORT 4299
#define DISCOVER_NAME_MAX 80

/* Bind INADDR_ANY. A socket bound to the console's own address does not
 * receive a subnet broadcast. TCP keeps its own listener on this port. */
s32 discover_open(void) {
	struct sockaddr_in sa;
	s32 fd, res;
	u32 reuse = 1;

	fd = net_socket(PF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return fd;

	net_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	res = net_fcntl(fd, F_GETFL, 0);
	if (res >= 0)
		res = net_fcntl(fd, F_SETFL, res | 4);
	if (res < 0) {
		net_close(fd);
		return res;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_len = sizeof(sa);
	sa.sin_port = htons(DISCOVER_PORT);
	sa.sin_addr.s_addr = 0;

	res = net_bind(fd, (struct sockaddr *) &sa, sizeof(sa));
	if (res < 0) {
		gprintf("discover bind failed: %d\n", res);
		net_close(fd);
		return res;
	}
	return fd;
}

/* The same characters HBCS puts in a name: printable ASCII, no quotes. */
static u8 copy_name(u8 *dst, const char *src) {
	u8 n = 0;

	if (!src)
		return 0;
	while (src[n] && n < DISCOVER_NAME_MAX) {
		unsigned char c = (unsigned char) src[n];

		dst[n] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? c : '?';
		n++;
	}
	return n;
}

s32 discover_poll(s32 fd, u32 netmask, const char *name, bool agent) {
	u8 query[16], reply[12 + DISCOVER_NAME_MAX];
	struct sockaddr_in from;
	socklen_t fromlen;
	s32 n;
	u8 i, namelen;
	u32 ip;

	if (!netmask)
		return 0;

	for (i = 0; i < 8; i++) {
		memset(&from, 0, sizeof(from));
		from.sin_family = AF_INET;
		from.sin_len = sizeof(from);
		fromlen = sizeof(from);
		n = net_recvfrom(fd, query, sizeof(query), 0,
						 (struct sockaddr *) &from, &fromlen);
		if (n == -EAGAIN)
			return 0;
		if (n < 0)
			return n;
		if (n < 4 || memcmp(query, "HBCU", 4) ||
				(from.sin_addr.s_addr & 0xffff0000) != netmask)
			continue;

		namelen = copy_name(reply + 12, name);
		memcpy(reply, "HBCU", 4);
		reply[4] = 1;
		reply[5] = agent ? 1 : 0;
		reply[6] = namelen;
		reply[7] = 0;
		ip = net_gethostip();
		reply[8] = ip >> 24;
		reply[9] = (ip >> 16) & 0xff;
		reply[10] = (ip >> 8) & 0xff;
		reply[11] = ip & 0xff;
		// Back to the address as IOS gave it: its sockaddr is 8 bytes, and
		// sizeof(from) (16, which Dolphin takes) is EINVAL on a real Wii.
		net_sendto(fd, reply, 12 + namelen, 0,
				   (struct sockaddr *) &from, fromlen);
	}
	return 0;
}
