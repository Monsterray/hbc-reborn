/* wiispk: clean Wii Remote speaker audio for libogc programs.
 *
 * libogc's WPAD_SendStreamData sounds broken on real remotes, for three
 * reasons this library works around:
 *   - libogc never puts a remote's Bluetooth link in sniff mode, so the link
 *     carries about 100 reports a second, fewer than the 150 speaker reports
 *     a second the speaker needs;
 *   - it queues speaker data as commands that wait for an acknowledgement
 *     the remote never sends, which stalls the remote's command queue;
 *   - a lost report desynchronises the speaker's ADPCM decoder for the rest
 *     of the sound.
 * wiispk sets up the speaker the way commercial Wii software does, streams
 * raw reports from its own timer, and encodes each report as it is sent,
 * skipping (not queueing) a block the Bluetooth controller has no room for,
 * so encoder and decoder never drift apart.
 *
 * Usage:
 *     WPAD_Init();                       // as usual
 *     wiispk_init();
 *     ...
 *     s16 *pcm = wiispk_load_wav("sd:/beep.wav", 10, &n);   // 6 kHz mono
 *     wiispk_play(0, pcm, n, WIISPK_ADPCM);
 *     while (running) {
 *         WPAD_ScanPads();
 *         wiispk_update();               // once a frame
 *         ...
 *     }
 *     wiispk_shutdown();                 // before exit
 *
 * The samples passed to wiispk_play are read while the sound plays; keep
 * them until wiispk_busy() says it is done. Sounds are mono signed 16-bit,
 * at WIISPK_ADPCM_RATE (6000 Hz) for WIISPK_ADPCM, the format to use, or
 * WIISPK_PCM_RATE (3000 Hz) for WIISPK_PCM, which is there to compare with.
 *
 * It needs wiiuse's per-remote handles, which libogc does not export; it
 * finds them the way described in wiispk.c and does nothing (every call
 * returns an error) if it cannot.
 *
 * zlib licence: see the end of wiispk.c.
 */

#ifndef WIISPK_H
#define WIISPK_H

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIISPK_ADPCM 0x00       /* 4-bit Yamaha ADPCM, 6 kHz: the clean one */
#define WIISPK_PCM 0x40         /* signed 8-bit PCM, 3 kHz */
#define WIISPK_ADPCM_RATE 6000
#define WIISPK_PCM_RATE 3000
#define WIISPK_VOLUME_DEFAULT 0x40

typedef struct {
	u32 sent;           /* reports sent */
	u32 skipped;        /* blocks skipped because the controller was full */
	u32 wrapped;        /* timer ticks where lwbt's buffer count had wrapped */
	u32 gap_max_us;     /* longest time between two timer ticks */
	u16 free_min, free_max; /* the controller's free ACL buffers seen */
	bool sniff;         /* the link was put in sniff mode */
} wiispk_stats;

/* Finds the remote handles; 0, or -1 if this libogc's layout is unknown. */
int wiispk_init(void);
/* Advances each remote's speaker start-up; call once a frame. */
void wiispk_update(void);
/* Plays a sound on a remote (0 to 3), starting its speaker if needed.
 * Replaces whatever that remote was playing. 0, or -1. */
int wiispk_play(int chan, const s16 *samples, u32 count, int format);
/* Stops the sound; the speaker stays on for the next one. */
void wiispk_stop(int chan);
/* Whether a sound is starting or playing. */
bool wiispk_busy(int chan);
/* The speaker's volume byte, 0 to 127 (0x40 is the Wii's usual level);
 * applied from the next sound. */
void wiispk_set_volume(int chan, int volume);
/* Turns a remote's speaker off. */
void wiispk_off(int chan);
/* Turns every speaker off and stops the timer. */
void wiispk_shutdown(void);
void wiispk_get_stats(int chan, wiispk_stats *out);
/* wiiuse's handle for a connected remote, or NULL: useful for LEDs and IR
 * settings libogc's WPAD calls do not reach. */
struct wiimote_t;
struct wiimote_t *wiispk_wiimote(int chan);

/* Resamples mono signed 16-bit audio from in_rate to out_rate with a
 * windowed-sinc low-pass. Returns a malloc'd buffer (free it), or NULL. */
s16 *wiispk_resample(const s16 *in, u32 count, int in_rate, int out_rate, u32 *out_count);
/* Reads a PCM WAV file (8, 16, 24 or 32-bit integer, or 32-bit float; any
 * rate; mono or more channels, which are mixed), keeping at most max_s
 * seconds, as 6 kHz mono scaled so its peak is about three quarters of full
 * scale. Returns a malloc'd buffer (free it) and its sample count, or NULL
 * with errno set. Slow for a long file: call it from a thread. */
s16 *wiispk_load_wav(const char *path, int max_s, u32 *out_count);

#ifdef __cplusplus
}
#endif

#endif
