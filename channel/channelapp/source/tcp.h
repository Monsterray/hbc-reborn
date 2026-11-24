#ifndef _TCP_H_
#define _TCP_H_

#include <gctypes.h>
#include <ogc/mutex.h>

s32 tcp_socket (void);
s32 tcp_connect (char *host, u16 port);
s32 tcp_listen (u16 port, s32 backlog);

bool tcp_read (s32 s, u8 *buffer, u32 length, const mutex_t *mutex, u32 *progress);
bool tcp_read_timeout (s32 s, u8 *buffer, u32 length, const mutex_t *mutex,
					   u32 *progress, s32 timeout_ms);
bool tcp_write (s32 s, const u8 *buffer, u32 length, const mutex_t *mutex,
				u32 *progress);
void tcp_close (s32 s);
const char *tcp_last_failure (void);

// The calling thread's longest call into IOS's network, and the accepts it
// made, as HBCS fields (tcp_longest_json); tcp_accept is net_accept, timed.
struct sockaddr;
void tcp_measure_this_thread (void);
int tcp_longest_json (char *buf, int size);
s32 tcp_accept (s32 s, struct sockaddr *sa, u32 *len);

#endif

