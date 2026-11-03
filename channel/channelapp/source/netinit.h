#ifndef HBC_NETINIT_H
#define HBC_NETINIT_H

#include <stdbool.h>
#include <gctypes.h>

#define HBC_NET_INIT_TIMEOUT_MS 30000u

// One budget across all attempts. Cancellation does not need the TCP mutex.
s32 netinit_wait(u64 started, const volatile bool *stop, const volatile bool *restart);

#endif
