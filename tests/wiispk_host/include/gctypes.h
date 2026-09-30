/* Host stand-ins for the libogc headers sdk/wiispk/wiispk.c uses, so its
 * encoder, resampler and WAV reader can be tested on a PC. */
#ifndef HOST_GCTYPES_H
#define HOST_GCTYPES_H
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define ATTRIBUTE_ALIGN(n)
#endif
