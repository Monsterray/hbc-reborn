/* See poison.h. Linked with -Wl,--wrap=malloc,--wrap=memalign,--wrap=free
 * when DEBUG_POISON is set; the wrappers then take those three calls from
 * every object and library in the link. calloc, realloc and newlib's
 * _malloc_r are not wrapped: what they hand out is not poisoned. */

#ifdef DEBUG_POISON

#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>

#include "poison.h"

#define POISON 0x7FC00000u
#ifndef POISON_MODE
#define POISON_MODE 3   /* 1: fresh blocks, 2: freed blocks */
#endif

void *__real_malloc(size_t n);
void *__real_memalign(size_t a, size_t n);
void __real_free(void *p);

unsigned poison_mallocs, poison_frees;

void poison_fill(void *p, size_t n) {
	uint32_t *w = p;
	size_t i;

	for (i = 0; i < n / 4; i++)
		w[i] = POISON;
}

void *__wrap_malloc(size_t n) {
	void *p = __real_malloc(n);

	if (p && (POISON_MODE & 1)) {
		poison_fill(p, malloc_usable_size(p));
		poison_mallocs++;
	}
	return p;
}

void *__wrap_memalign(size_t a, size_t n) {
	void *p = __real_memalign(a, n);

	if (p && (POISON_MODE & 1)) {
		poison_fill(p, malloc_usable_size(p));
		poison_mallocs++;
	}
	return p;
}

void __wrap_free(void *p) {
	if (p && (POISON_MODE & 2)) {
		poison_fill(p, malloc_usable_size(p));
		poison_frees++;
	}
	__real_free(p);
}

/* The main thread's stack, below the frame this runs in. Constructors run
 * on that stack before main. */
static void __attribute__((constructor)) poison_stack(void) {
	extern char __stack_end[];
	char *lo = __stack_end, *hi = (char *) __builtin_frame_address(0) - 512;

	if (hi > lo)
		poison_fill(lo, (size_t) (hi - lo) & ~(size_t) 3);
}

#endif
