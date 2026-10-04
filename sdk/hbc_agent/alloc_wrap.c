// Failed allocations, for apps linked with
//   -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign
// Each call the app's code makes then goes through here: one compare on the
// way back. A failure is counted for HBCS "safety" (alloc_failures), and the
// first few are printed with the size and the caller, so they end up in the
// kept log. Without those flags nothing refers to this file and the linker
// leaves it out. newlib's own allocations (fopen's buffers, for one) call
// _malloc_r directly and are not seen.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include "agent_int.h"

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);
void *__real_memalign(size_t align, size_t size);

agent_alloc_stats agent_alloc;

static void failed(const char *what, size_t size, void *from) {
	static volatile int printing;

	agent_alloc.count++;
	agent_alloc.last_size = size;
	agent_alloc.last_from = (u32) from;
	// printf can allocate; a failure inside it is counted, not printed.
	if (agent_alloc.count <= 4 && !printing) {
		printing = 1;
		printf("agent: %s(%u) failed, called from %08x\n", what, (unsigned) size,
			   (unsigned) from);
		printing = 0;
	}
}

void *__wrap_malloc(size_t size) {
	void *p = __real_malloc(size);

	if (!p && size)
		failed("malloc", size, __builtin_return_address(0));
	return p;
}

void *__wrap_calloc(size_t n, size_t size) {
	void *p = __real_calloc(n, size);

	if (!p && n && size)
		failed("calloc", n * size, __builtin_return_address(0));
	return p;
}

void *__wrap_realloc(void *old, size_t size) {
	void *p = __real_realloc(old, size);

	if (!p && size)
		failed("realloc", size, __builtin_return_address(0));
	return p;
}

void *__wrap_memalign(size_t align, size_t size) {
	void *p = __real_memalign(align, size);

	if (!p && size)
		failed("memalign", size, __builtin_return_address(0));
	return p;
}
