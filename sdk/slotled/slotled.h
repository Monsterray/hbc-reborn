/* slotled: the Wii's blue disc-slot light, for libogc programs.
 *
 * The light can be driven three ways, and this library does all three:
 *   - directly: the PowerPC owns the light's pin (Hollywood GPIO 5), so it
 *     can switch it on and off itself. slotled turns that into 256 levels
 *     of brightness (pulse-width modulation from a timer interrupt, about
 *     250 times a second, with no thread and no CPU time between
 *     changes) and into effects: blink, pulse, heartbeat, Morse code,
 *     a short flash for activity, and any sequence of steps you give it;
 *   - through IOS (/dev/stm/immediate): the "slot illumination" level
 *     (off, dim, bright) and IOS's own flash patterns, the ones the Wii
 *     uses when new mail arrives;
 *   - the saved setting: the Wii Menu's "Slot illumination" in SYSCONF
 *     (IPL.IDL), read and written.
 *
 * Usage:
 *     slotled_init();                    // -1: the light's pin is not reachable
 *     slotled_pulse(0, 255, 2000, 0);    // breathe, every 2 s, until told
 *     ...
 *     slotled_flash(255, 50);            // a blink over it, for activity
 *     ...
 *     slotled_shutdown();                // before exit: back as it was
 *
 * Every direct call returns at once; the effect runs from the timer. A
 * new effect replaces the one running. Levels are perceived brightness:
 * 128 looks about half as bright as 255 (the duty cycle is level squared).
 *
 * The IOS calls and the direct calls drive the same pin. While a direct
 * effect runs it wins; IOS's patterns are for when you want the Wii's
 * own look, and do not need slotled_init().
 *
 * The step engine (slotled_effect_level), the effect builders and the
 * SYSCONF patcher are plain C and build on a PC with -DSLOTLED_NO_OGC,
 * for tools and tests.
 *
 * zlib licence: see the end of slotled.c.
 */

#ifndef SLOTLED_H
#define SLOTLED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLOTLED_VERSION "1.0.0"

#define SLOTLED_MAX 255             /* full brightness */
#define SLOTLED_PERIOD_US 4000      /* one modulation period: 250 Hz */
#define SLOTLED_MAX_STEPS 512       /* steps the builders can make */

/* Slot illumination, for the IOS level and the saved setting. */
enum {
	SLOTLED_ILLUM_OFF = 0,
	SLOTLED_ILLUM_DIM = 1,
	SLOTLED_ILLUM_BRIGHT = 2
};

#define SLOTLED_IOS_PATTERNS 30     /* IOS's built-in flash patterns: 0..29 */

enum {
	SLOTLED_OK = 0,
	SLOTLED_E_NO_PIN = -1,      /* the PowerPC cannot reach the light's pin */
	SLOTLED_E_ARG = -2,         /* a level, pattern or count out of range */
	SLOTLED_E_NOT_INIT = -3,    /* slotled_init() was not called or failed */
	SLOTLED_E_IOS = -4,         /* IOS refused (the IOS error is not kept) */
	SLOTLED_E_FORMAT = -5,      /* SYSCONF is not as the Wii makes it */
	SLOTLED_E_TOO_LONG = -6     /* more steps than SLOTLED_MAX_STEPS */
};

/* One step: go to `level` and stay for `ms`. With `fade`, the level moves
 * there evenly over the `ms` instead of jumping. */
typedef struct {
	uint8_t level;
	uint8_t fade;
	uint16_t ms;
} slotled_step;

/* Steps played in order, `repeat` times (0: until replaced). When it ends
 * the light holds the last step's level. */
typedef struct {
	const slotled_step *steps;
	uint32_t count;
	uint32_t repeat;
} slotled_effect;

/* ---- Directly --------------------------------------------------------- */

/* Finds the light's pin and sets up the timer. Turns nothing on. */
int slotled_init(void);

/* Stops the effect and leaves the light as slotled_init() found it. */
void slotled_shutdown(void);

/* Steady at a level, 0..255. */
int slotled_set(int level);

/* The level shown now. */
int slotled_get(void);

/* On and off at `level`: `on_ms`, then `off_ms`, `count` times (0: until
 * replaced). */
int slotled_blink(int level, uint32_t on_ms, uint32_t off_ms, uint32_t count);

/* Breathing: from `lo` up to `hi` and back down, once every `period_ms`. */
int slotled_pulse(int lo, int hi, uint32_t period_ms, uint32_t count);

/* Two quick beats, then rest, once every `period_ms` (at least 600). */
int slotled_heartbeat(int level, uint32_t period_ms, uint32_t count);

/* `text` in Morse code at `level`, `unit_ms` a dot (WPM = 1200 / unit_ms).
 * Letters, digits and .,?'!/()&:;=+-_"$@ ; other characters are a word gap.
 * Returns the characters sent, or an error. */
int slotled_morse(const char *text, int level, uint32_t unit_ms, uint32_t count);

/* Plays your steps. They are read while it plays: keep them until
 * slotled_busy() is false or another effect replaces them. */
int slotled_play(const slotled_effect *effect);

/* `level` for `ms` over whatever is running, which carries on beneath it
 * (its time keeps going). Call it again to extend it: for disc or network
 * activity, call it on each transfer. */
int slotled_flash(int level, uint32_t ms);

/* Whether a finite effect, or a flash, is still running. */
int slotled_busy(void);

/* Waits until slotled_busy() is false. */
void slotled_wait(void);

/* ---- Through IOS ------------------------------------------------------- */

/* The slot illumination level, off/dim/bright: how bright IOS's own flash
 * patterns are. Not saved: the Wii Menu sets the saved one again at boot. */
int slotled_ios_level(int illum);

/* One of IOS's built-in patterns, 0..29. A pattern runs only if its
 * priority (0..255) is higher than the one running. */
int slotled_ios_flash(int pattern, int priority);

/* The raw ioctl (STM 0x6001), for patterns and flags not yet understood:
 * `in` is the 32-byte input buffer, `out` gets the 32-byte output (may be
 * NULL). Returns IOS's result. */
int slotled_ios_flash_raw(const uint32_t in[8], uint32_t out[8]);

/* ---- The saved setting ------------------------------------------------- */

/* The Wii Menu's Slot illumination, 0..2, or an error. */
int slotled_saved_level(void);

/* Saves it in SYSCONF (and sets the IOS level to match). The Wii Menu
 * uses it from the next boot. Writes the NAND: only on the user's request. */
int slotled_save_level(int illum);

/* ---- Pure parts (also on a PC) ---------------------------------------- */

/* The level an effect shows `ms` into it; `*done` (may be NULL) is set
 * once a finite effect has ended. `start` is the level it fades from at
 * first. */
int slotled_effect_level(const slotled_effect *effect, uint64_t ms, int start, int *done);

/* The on-time, in microseconds, for a level in one SLOTLED_PERIOD_US. */
uint32_t slotled_duty_us(int level);

/* Builders: fill `steps` (room for `max`) and return how many, or an
 * error. The direct calls above use these. */
int slotled_build_blink(slotled_step *steps, size_t max, int level, uint32_t on_ms, uint32_t off_ms);
int slotled_build_pulse(slotled_step *steps, size_t max, int lo, int hi, uint32_t period_ms);
int slotled_build_heartbeat(slotled_step *steps, size_t max, int level, uint32_t period_ms);
int slotled_build_morse(slotled_step *steps, size_t max, const char *text, int level,
                        uint32_t unit_ms, int *chars);

/* Finds IPL.IDL in a SYSCONF image and reads or sets its Slot illumination
 * byte, in place. Returns the old value (0..2), or SLOTLED_E_FORMAT. */
int slotled_sysconf_illum(uint8_t *sysconf, size_t size, int set);

#ifdef __cplusplus
}
#endif

#endif
