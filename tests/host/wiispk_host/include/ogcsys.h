#ifndef HOST_OGCSYS_H
#define HOST_OGCSYS_H
#include <time.h>
#include "gctypes.h"
typedef u32 syswd_t;
typedef void (*alarmcallback)(syswd_t alarm, void *arg);
s32 SYS_CreateAlarm(syswd_t *alarm);
s32 SYS_SetPeriodicAlarm(syswd_t alarm, const struct timespec *start, const struct timespec *period,
						 alarmcallback cb, void *arg);
s32 SYS_CancelAlarm(syswd_t alarm);
#endif
