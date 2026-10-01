#ifndef _ZMEM_H_
#define _ZMEM_H_

#include <gctypes.h>
#include <zlib.h>

// Reserves the MEM1 zlib arena; call early, while the heap is still in MEM1.
void zmem_init(void);
// "MEM1", "MEM2" or "none": where the arena landed, for the status reply.
const char *zmem_where(void);
// Points z's allocator at the arena if no other stream holds it; returns
// whether it did. Call before deflateInit/inflateInit, and zmem_release()
// after deflateEnd/inflateEnd when it returned true.
bool zmem_use(z_stream *z);
void zmem_release(void);
// The most arena bytes one stream has used, and the largest allocation
// that did not fit and went to the heap (0 if none).
u32 zmem_peak(void);
u32 zmem_heap_peak(void);

#endif
