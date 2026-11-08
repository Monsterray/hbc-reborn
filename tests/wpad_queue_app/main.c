// Starts WPAD and waits, for tests/dolphin_wpad_queue.py to read the Wii
// Remotes' command queues from memory. Built with and without
// channel/channelapp/source/wpadqueue.c; see the Makefile.

#include <gccore.h>
#include <wiiuse/wpad.h>

int main(void) {
	VIDEO_Init();
	WPAD_Init();
	for (;;)
		VIDEO_WaitVSync();
}
