#ifndef _TRANSFER_ARC_H_
#define _TRANSFER_ARC_H_

#include <math.h>
#include <stdint.h>

#define TRANSFER_ARC_SIZE 24
#define TRANSFER_ARC_TURN 256

static inline unsigned transfer_arc_amount(uint32_t done, uint32_t total) {
	if (!total)
		return 0;
	if (done >= total)
		return TRANSFER_ARC_TURN;
	return (uint64_t) done * TRANSFER_ARC_TURN / total;
}

// A tintable arc on a faint track, clockwise from twelve o'clock. The output
// is GX's tiled RGBA8 layout; unlike font masks it keeps square geometry
// in both aspect ratios. Four samples
// per pixel soften the circle and the moving edge on a television.
static inline void transfer_arc_render(unsigned char *pixels, unsigned filled,
									  unsigned start) {
	unsigned x, y, sx, sy;
	const float tau = 6.28318530718f;

	if (filled > TRANSFER_ARC_TURN)
		filled = TRANSFER_ARC_TURN;
	start %= TRANSFER_ARC_TURN;
	for (y = 0; y < TRANSFER_ARC_SIZE; ++y)
		for (x = 0; x < TRANSFER_ARC_SIZE; ++x) {
			unsigned alpha = 0;
			for (sy = 0; sy < 2; ++sy)
				for (sx = 0; sx < 2; ++sx) {
					float dx = x + 0.25f + 0.5f * sx - TRANSFER_ARC_SIZE / 2;
					float dy = y + 0.25f + 0.5f * sy - TRANSFER_ARC_SIZE / 2;
					float radius = sqrtf(dx * dx + dy * dy);
					float angle;
					unsigned turn;

					if (radius < 7.5f || radius > 10.0f)
						continue;
					angle = atan2f(dx, -dy);
					if (angle < 0)
						angle += tau;
					turn = (unsigned) (angle * TRANSFER_ARC_TURN / tau);
					turn = (turn + TRANSFER_ARC_TURN - start) % TRANSFER_ARC_TURN;
					alpha += turn < filled ? 255 : 48;
				}
			unsigned offset = ((y / 4) * (TRANSFER_ARC_SIZE / 4) + x / 4) * 64 +
							  ((y % 4) * 4 + x % 4) * 2;
			pixels[offset] = (alpha + 2) / 4;  // A, R plane
			pixels[offset + 1] = 255;
			pixels[offset + 32] = 255;        // G, B plane
			pixels[offset + 33] = 255;
		}
}

#endif
