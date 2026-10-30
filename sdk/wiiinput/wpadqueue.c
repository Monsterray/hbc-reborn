/* SPDX-FileCopyrightText: 2026 RiftWii contributors */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Wii Remotes' command queues, taken from libogc's kernel heap into
 * the normal one (linked in place of libogc's wiiuse_init_cmd_queue with
 * -Wl,--wrap=wiiuse_init_cmd_queue). From RiftWii's
 * wii/controllers/wpadqueue.cpp (github.com/KakarottoCake/Riftwii).
 *
 * libogc 3.1 gives each of its six Bluetooth slots (four Wii Remotes, the
 * Balance Board, one more) a queue of 256 commands, 18 KB, from its kernel
 * heap: 64 KB in all, shared with its threads. Only the first three fit;
 * slots 3-5 keep an empty queue, and the first command to a device there
 * (its LEDs, as it connects) reads address 0. So a fourth Wii Remote, or a
 * Balance Board that is still on, crashes the program as it connects.
 *
 * HBC links with -Wl,--wrap=wiiuse_init_cmd_queue (channel/channelapp/Makefile).
 */

#include <malloc.h>
#include <ogc/lwp_queue.h>
#include <wiiuse/wiiuse.h>
#include "wiiinput.h"

#define SLOTS 6       /* WPAD_MAX_DEVICES in libogc 3.1 */
#define COMMANDS 256  /* libogc's own queue length */

/* Once per slot, kept across WPAD_Shutdown and WPAD_Init. */
static u8 *buffers[SLOTS];

int __real_wiiuse_init_cmd_queue(struct wiimote_t *wm);

int __wrap_wiiuse_init_cmd_queue(struct wiimote_t *wm)
{
    int slot = wm->unid;
    if (slot < 0 || slot >= SLOTS)
        return __real_wiiuse_init_cmd_queue(wm);
    if (!buffers[slot]) {
        buffers[slot] = memalign(32, COMMANDS * sizeof(struct cmd_blk_t));
        if (!buffers[slot])
            return __real_wiiuse_init_cmd_queue(wm);
    }
    __lwp_queue_initialize((lwp_queue *)&wm->cmdq, buffers[slot], COMMANDS, sizeof(struct cmd_blk_t));
    return 0;
}

