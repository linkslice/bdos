/* audio.h - the sound mixer (audio.c) and what plays it */
#ifndef BDOS_AUDIO_H
#define BDOS_AUDIO_H
#include <stdint.h>

#define AUDIO_RATE 48000		/* what the mixer makes: 16-bit stereo */

void audio_init(void);
int audio_open(uint32_t rate);		/* a stream; -1 if none free */
void audio_close(int id);
void audio_set_rate(int id, uint32_t rate);
void audio_set_volume(int id, int vol);	/* 0..256 */
void audio_set_master(int vol);
uint32_t audio_write(int id, const int16_t *data, uint32_t frames, int channels);	/* frames taken */
uint32_t audio_queued(int id);
uint32_t audio_room(int id);
void audio_mix(int16_t *out, uint32_t frames);	/* for the driver */
const char *audio_device(void);
int audio_streams(void);
#endif
