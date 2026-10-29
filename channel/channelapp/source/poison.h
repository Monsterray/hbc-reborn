/* Memory poisoning for bug hunts: `make DEBUG_POISON=1` wraps malloc,
 * memalign and free so every fresh and every freed block is filled with
 * 0x7FC00000 (a float NaN, an invalid pointer), and fills the main
 * thread's stack at start. A read of memory that isn't what the code
 * thinks it is then shows the same way in any layout. Off by default:
 * every call here is a no-op. */

#ifndef _POISON_H_
#define _POISON_H_

#include <stddef.h>

#ifdef DEBUG_POISON
void poison_fill(void *p, size_t n);
#else
#define poison_fill(p, n) ((void) 0)
#endif

#endif
