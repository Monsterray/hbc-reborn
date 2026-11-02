// The Wii Remotes' command queues, from the normal heap.
//
// libogc 3.1 gives each of its six Bluetooth slots (four Wii Remotes, the
// Balance Board, one more) a queue of 256 commands from its kernel heap, 64
// KB shared with its threads. Only the first three fit; slots 3 to 5 keep an
// empty queue, and the first command sent to a device there (its LEDs, as it
// connects) reads address 0. A fourth Wii Remote, or a Balance Board left on,
// crashes the program as it connects.
//
// The app is linked with -Wl,--wrap=wiiuse_init_cmd_queue (Makefile), so
// libogc's call from wiiuse_init lands here. tests/dolphin_wpad_queue.py
// shows the empty queues without this file and the full ones with it.

#include <malloc.h>
#include <gccore.h>
#include <wiiuse/wiiuse.h>

#define SLOTS 6		// WPAD_MAX_DEVICES in libogc 3.1
#define COMMANDS 256	// libogc's own queue length

int __real_wiiuse_init_cmd_queue (struct wiimote_t *wm);
int __wrap_wiiuse_init_cmd_queue (struct wiimote_t *wm);

// Once per slot, kept across WPAD_Shutdown and WPAD_Init.
static void *buffers[SLOTS];

int __wrap_wiiuse_init_cmd_queue (struct wiimote_t *wm) {
	int slot = wm->unid;

	if(slot < 0 || slot >= SLOTS)
		return __real_wiiuse_init_cmd_queue (wm);

	if(!buffers[slot]) {
		buffers[slot] = memalign (32, COMMANDS * sizeof(struct cmd_blk_t));
		if(!buffers[slot])
			return __real_wiiuse_init_cmd_queue (wm);
	}

	// wiiuse.h marks the queue const for apps; libogc's own code sets it.
	__lwp_queue_initialize ((lwp_queue *) &wm->cmdq, buffers[slot], COMMANDS, sizeof(struct cmd_blk_t));
	return 0;
}
