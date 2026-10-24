/* Host stand-in for libogc's gctypes.h: sdk/hbc_agent/way_out.c and the crash
 * block layout in sdk/hbc_agent.h, built on a PC by tests/test_way_out.py. */
#ifndef HOST_GCTYPES_H
#define HOST_GCTYPES_H
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef volatile uint32_t vu32;
#endif
