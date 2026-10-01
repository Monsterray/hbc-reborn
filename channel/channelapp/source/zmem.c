// A MEM1 arena for zlib state. zlib's window and hash tables are hot,
// random-access data: on a Wii (tests/membench) inflate ran 25% faster and
// level 1 deflate 14% faster with their state in MEM1 than in MEM2. By the
// time a transfer starts, the menu has filled MEM1 and the heap hands out
// MEM2, so the arena is reserved at startup. One stream owns it at a time;
// others fall back to the heap.

#include <malloc.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>

#include "zmem.h"

// Level 1 deflate needs about 262 KiB (window, prev and head tables, and the
// pending buffer); inflate about 40 KiB.
#define ZMEM_SIZE (320 * 1024)

static u8 *arena;
static u32 used;
static bool owned;
static u32 peak, heap_peak;

static voidpf zmem_alloc(voidpf opaque, uInt items, uInt size) {
	u32 n = ((u32) items * size + 31) & ~31u;
	(void) opaque;

	if (used + n <= ZMEM_SIZE) {
		used += n;
		if (used > peak)
			peak = used;
		return arena + used - n;
	}
	if (n > heap_peak)
		heap_peak = n;
	return malloc((u32) items * size);
}

static void zmem_free(voidpf opaque, voidpf p) {
	(void) opaque;

	if ((u8 *) p < arena || (u8 *) p >= arena + ZMEM_SIZE)
		free(p);
}

void zmem_init(void) {
	if (!arena)
		arena = memalign(32, ZMEM_SIZE);
}

const char *zmem_where(void) {
	if (!arena)
		return "none";
	return ((u32) arena >> 28) == 8 ? "MEM1" : "MEM2";
}

bool zmem_use(z_stream *z) {
	bool taken = false;
	u32 level;

	z->zalloc = Z_NULL;
	z->zfree = Z_NULL;
	z->opaque = Z_NULL;

	_CPU_ISR_Disable(level);
	if (arena && !owned) {
		owned = true;
		taken = true;
	}
	_CPU_ISR_Restore(level);

	if (taken) {
		used = 0;
		z->zalloc = zmem_alloc;
		z->zfree = zmem_free;
	}
	return taken;
}

void zmem_release(void) {
	owned = false;
}

u32 zmem_peak(void) {
	return peak;
}

u32 zmem_heap_peak(void) {
	return heap_peak;
}
