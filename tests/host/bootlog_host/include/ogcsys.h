// Just enough of libogc for bootlog.c on the host (tests/host/test_bootlog.py).
#ifndef _OGCSYS_H_
#define _OGCSYS_H_

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
typedef u32 lwp_t;

lwp_t LWP_GetSelf(void);

typedef s32 (*ipccallback)(s32 result, void *usrdata);
s32 IOS_OpenAsync(const char *filepath, u32 mode, ipccallback cb, void *usrdata);
s32 IOS_CloseAsync(s32 fd, ipccallback cb, void *usrdata);

#endif
