#ifndef _EDGE_BUMP_H_
#define _EDGE_BUMP_H_

#include <math.h>

#define EDGE_BUMP_MS 320
#define EDGE_BUMP_PX 7.0f
#define EDGE_BUMP_PERIOD_MS 110.0f

// Pixels to shift a widget the D-pad could not move focus away from, ms
// after the press: a damped shake that starts toward the pushed edge and
// settles back to zero.
static inline float edge_bump_offset(unsigned ms) {
	const float tau = 6.28318530718f;
	float left;

	if (ms >= EDGE_BUMP_MS)
		return 0;
	left = 1.0f - (float) ms / EDGE_BUMP_MS;
	return EDGE_BUMP_PX * left * left * sinf(tau * ms / EDGE_BUMP_PERIOD_MS);
}

#endif
