/* Host stand-ins for the libogc headers channel/channelapp/source/keyboard.c
 * uses, so its USB handling can be tested on a PC against a fake USB. */
#ifndef HOST_GCTYPES_H
#define HOST_GCTYPES_H
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
#endif
