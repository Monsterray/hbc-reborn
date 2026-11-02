#ifndef _TRANSFER_ARC_H_
#define _TRANSFER_ARC_H_

#include <math.h>
#include <stdint.h>

// The transfer line's progress circle (Options: Transfer progress), drawn
// into a texture of its own: a tintable arc on a faint track, filling
// clockwise from twelve o'clock. A texture of RGBA pixels keeps it round in
// 4:3 and 16:9, which a font's mask, stretched sideways, would not.
#define TRANSFER_ARC_SIZE 24
#define TRANSFER_ARC_TURN 256   // a whole turn, in steps
#define TRANSFER_ARC_SAMPLES 4  // per pixel, to soften the circle on a television
#define TRANSFER_ARC_OUT 0xffff // a sample off the ring

// How far a transfer has come, in steps of a turn; 0 while its size is unknown.
static inline unsigned transfer_arc_amount(uint32_t done, uint32_t total) {
	if (!total)
		return 0;
	if (done >= total)
		return TRANSFER_ARC_TURN;
	return (uint64_t) done * TRANSFER_ARC_TURN / total;
}

// Where each sample lies around the ring, in steps from twelve o'clock. The
// angles never change, so they are found once, and a new picture is a pass
// over this table with no square roots or arctangents.
static uint16_t transfer_arc_where[TRANSFER_ARC_SIZE * TRANSFER_ARC_SIZE * TRANSFER_ARC_SAMPLES];
static int transfer_arc_ready;

static inline void transfer_arc_prepare(void) {
	const float tau = 6.28318530718f;
	unsigned x, y, s;

	if (transfer_arc_ready)
		return;
	for (y = 0; y < TRANSFER_ARC_SIZE; ++y)
		for (x = 0; x < TRANSFER_ARC_SIZE; ++x)
			for (s = 0; s < TRANSFER_ARC_SAMPLES; ++s) {
				float dx = x + 0.25f + 0.5f * (s & 1) - TRANSFER_ARC_SIZE / 2;
				float dy = y + 0.25f + 0.5f * (s >> 1) - TRANSFER_ARC_SIZE / 2;
				float radius = sqrtf(dx * dx + dy * dy);
				float angle;
				uint16_t *at = &transfer_arc_where[(y * TRANSFER_ARC_SIZE + x) *
												   TRANSFER_ARC_SAMPLES + s];

				if (radius < 7.5f || radius > 10.0f) {
					*at = TRANSFER_ARC_OUT;
					continue;
				}
				angle = atan2f(dx, -dy);
				if (angle < 0)
					angle += tau;
				*at = (unsigned) (angle * TRANSFER_ARC_TURN / tau) % TRANSFER_ARC_TURN;
			}
	transfer_arc_ready = 1;
}

// The circle in pixels, in GX's tiled RGBA8 layout (TRANSFER_ARC_SIZE
// squared, 4 bytes each): the first filled steps from step start are solid,
// the rest of the ring faint. start turns the whole arc, for a transfer of
// unknown size to spin.
static inline void transfer_arc_render(unsigned char *pixels, unsigned filled,
									  unsigned start) {
	unsigned x, y, s;

	transfer_arc_prepare();
	if (filled > TRANSFER_ARC_TURN)
		filled = TRANSFER_ARC_TURN;
	start %= TRANSFER_ARC_TURN;
	for (y = 0; y < TRANSFER_ARC_SIZE; ++y)
		for (x = 0; x < TRANSFER_ARC_SIZE; ++x) {
			const uint16_t *at = &transfer_arc_where[(y * TRANSFER_ARC_SIZE + x) *
													 TRANSFER_ARC_SAMPLES];
			unsigned alpha = 0;
			unsigned offset = ((y / 4) * (TRANSFER_ARC_SIZE / 4) + x / 4) * 64 +
							  ((y % 4) * 4 + x % 4) * 2;

			for (s = 0; s < TRANSFER_ARC_SAMPLES; ++s) {
				unsigned turn;

				if (at[s] == TRANSFER_ARC_OUT)
					continue;
				turn = (at[s] + TRANSFER_ARC_TURN - start) % TRANSFER_ARC_TURN;
				alpha += turn < filled ? 255 : 48;
			}
			pixels[offset] = (alpha + 2) / 4;   // A, R plane
			pixels[offset + 1] = 255;
			pixels[offset + 32] = 255;          // G, B plane
			pixels[offset + 33] = 255;
		}
}

#endif
