#include <gctypes.h>
#define ATTRIBUTE_ALIGN(n) __attribute__((aligned(n)))

/* The libwiithreads calls keyboard.c uses: test.c runs them on a host thread. */
typedef u32 lwp_t;
#define LWP_THREAD_NULL ((lwp_t) -1)
s32 LWP_CreateThread (lwp_t *thethread, void *(*entry) (void *), void *arg, void *stackbase, u32 stack_size, u8 prio);
s32 LWP_JoinThread (lwp_t thethread, void **value_ptr);
