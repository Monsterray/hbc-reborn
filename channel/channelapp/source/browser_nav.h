#ifndef HBC_BROWSER_NAV_H
#define HBC_BROWSER_NAV_H

#include <stdbool.h>
#include <stdint.h>

enum { BN_UP = 1, BN_RIGHT = 2, BN_DOWN = 4, BN_LEFT = 8 };

typedef struct {
	unsigned held;
	uint64_t next_ms;
} browser_repeat;

// One step now, then after 400 ms, then every 120 ms. Do not catch up with
// a burst of steps after a page animation or a slow frame.
static inline unsigned browser_repeat_step(browser_repeat *r, unsigned down,
		unsigned held, uint64_t now_ms) {
	unsigned key = down | held;
	if (!key || (key & (key - 1))) {
		r->held = 0;
		return 0;
	}
	if (key != r->held || down) {
		r->held = key;
		r->next_ms = now_ms + 400;
		return key;
	}
	if (now_ms >= r->next_ms) {
		r->next_ms = now_ms + 120;
		return key;
	}
	return 0;
}

// Horizontal navigation with edge paging. The result is an absolute app
// index; the caller changes pages when it falls outside the current page.
static inline int browser_edge_step(int selected, int count, int columns,
		int rows, unsigned key) {
	int size = columns * rows;
	int top = selected / size * size;
	int local = selected - top;
	int row = local / columns;
	int target = selected;
	if (key == BN_LEFT) {
		if (local % columns)
			--target;
		else if (top > 0)
			target = top - size + row * columns + columns - 1;
	} else if (key == BN_RIGHT) {
		if (local % columns < columns - 1 && selected + 1 < count)
			++target;
		else if (top + size < count) {
			target = top + size + row * columns;
			if (target >= count)
				target = count - 1;
		}
	}
	return target;
}

#endif
