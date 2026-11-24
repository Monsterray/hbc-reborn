#ifndef HOST_LWP_WATCHDOG_H
#define HOST_LWP_WATCHDOG_H
#include "../gctypes.h"
u64 gettime(void);
#define ticks_to_microsecs(t) ((u64) (t))
#endif
