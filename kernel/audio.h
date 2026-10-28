/* audio.h - the sound mixer (audio.c) and what plays it */
#ifndef BDOS_AUDIO_H
#define BDOS_AUDIO_H
#include <stdint.h>
#include <stddef.h>

#define AUDIO_RATE 48000		/* what the mixer makes: 16-bit stereo */

void audio_init(void);
int audio_open(uint32_t rate);		/* a stream in the caller's session; -1 if none free */
int audio_open_named(uint32_t rate, const char *name);
void audio_close(int id);
void audio_set_rate(int id, uint32_t rate);
void audio_set_volume(int id, int vol);	/* 0..256 */
void audio_set_master(int vol);		/* the caller's session's volume, 0..256 */
int audio_get_master(void);
void audio_set_mute(int on);
int audio_get_mute(void);
uint32_t audio_write(int id, const int16_t *data, uint32_t frames, int channels);	/* frames taken */
uint32_t audio_queued(int id);
uint32_t audio_room(int id);
void audio_mix(int16_t *out, uint32_t frames);	/* the console's, for the driver */
uint32_t audio_mix_session(int sess, int16_t *out, uint32_t frames);

/* sessions: the console is 0; each remote session makes its own, and its
 * tasks (and theirs) sound in it. A sink gets 10 ms of 48 kHz stereo at a
 * time, while anything's playing (NULL: the session is heard nowhere) */
typedef void (*audio_sink_fn)(void *ctx, const int16_t *pcm, uint32_t frames);
int audio_session_new(const char *name, audio_sink_fn sink, void *ctx);
void audio_session_end(int id);
int audio_session_of_task(void);
int audio_session_info(char *name, size_t n, int *volume, int *muted, int *audible);
int audio_stream_info(int i, char *name, size_t n, int *volume, uint32_t *rate);
int audio_session_has_mixer_out(void);
int audio_play_file(const char *path, int quiet);	/* PLAY: a .WAV, to the end */
const char *audio_device(void);
int audio_streams(void);
#endif
