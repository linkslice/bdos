/*
 * audio.c - the sound mixer.
 *
 * Anything that makes sound opens a stream at its own rate (mono or
 * stereo, 16-bit) and writes samples into it; the mixer resamples every
 * stream to 48 kHz stereo and adds them up for whatever plays them - the
 * HD Audio driver, or nothing at all, in which case the samples are used
 * up at the same pace so writers still keep time.
 *
 * Streams are touched from DOS boxes running without the big lock, so the
 * mixer has a spinlock of its own.
 */
#include "kernel.h"
#include "spinlock.h"
#include "audio.h"

#define MAX_STREAMS	8
#define RING		32768		/* frames per stream (stereo pairs) */

struct stream {
	int used;
	uint32_t rate;
	int16_t *ring;			/* stereo frames */
	uint32_t head, len;
	uint32_t frac;			/* 16.16 position between ring[head] and ring[head+1] */
	int volume;			/* 0..256 */
	uint64_t underruns;
};

static struct stream streams[MAX_STREAMS];
static struct spinlock lock;
static const char *device = "none";
static int master = 256;

int audio_open(uint32_t rate)
{
	if (rate < 1000 || rate > 192000)
		return -1;
	int16_t *ring = kmalloc(RING * 4);
	if (!ring)
		return -1;
	uint64_t f = spin_lock_irqsave(&lock);
	for (int i = 0; i < MAX_STREAMS; i++)
		if (!streams[i].used) {
			struct stream *s = &streams[i];
			memset(s, 0, sizeof *s);
			s->used = 1;
			s->rate = rate;
			s->ring = ring;
			s->volume = 256;
			spin_unlock_irqrestore(&lock, f);
			return i;
		}
	spin_unlock_irqrestore(&lock, f);
	kfree(ring);
	return -1;
}

void audio_close(int id)
{
	if (id < 0 || id >= MAX_STREAMS)
		return;
	uint64_t f = spin_lock_irqsave(&lock);
	int16_t *ring = streams[id].used ? streams[id].ring : NULL;
	streams[id].used = 0;
	streams[id].ring = NULL;
	spin_unlock_irqrestore(&lock, f);
	kfree(ring);
}

void audio_set_rate(int id, uint32_t rate)
{
	if (id >= 0 && id < MAX_STREAMS && rate >= 1000 && rate <= 192000)
		streams[id].rate = rate;
}

void audio_set_volume(int id, int vol)
{
	if (id >= 0 && id < MAX_STREAMS)
		streams[id].volume = MAX(0, MIN(vol, 256));
}

void audio_set_master(int vol)
{
	master = MAX(0, MIN(vol, 256));
}

/* frames (channels interleaved) in; returns how many fit */
uint32_t audio_write(int id, const int16_t *data, uint32_t frames, int channels)
{
	if (id < 0 || id >= MAX_STREAMS)
		return 0;
	uint64_t f = spin_lock_irqsave(&lock);
	struct stream *s = &streams[id];
	if (!s->used) {
		spin_unlock_irqrestore(&lock, f);
		return 0;
	}
	uint32_t room = RING - s->len, n = MIN(frames, room);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t at = (s->head + s->len + i) % RING;
		int16_t l = data[i * (uint32_t)channels], r = channels > 1 ? data[i * (uint32_t)channels + 1] : l;
		s->ring[at * 2] = l;
		s->ring[at * 2 + 1] = r;
	}
	s->len += n;
	spin_unlock_irqrestore(&lock, f);
	return n;
}

uint32_t audio_queued(int id)
{
	if (id < 0 || id >= MAX_STREAMS || !streams[id].used)
		return 0;
	return streams[id].len;
}

/* the frames a stream can still take */
uint32_t audio_room(int id)
{
	if (id < 0 || id >= MAX_STREAMS || !streams[id].used)
		return 0;
	return RING - streams[id].len;
}

/* mix: frames of 48 kHz stereo out (out may be NULL: just use them up) */
void audio_mix(int16_t *out, uint32_t frames)
{
	static int32_t acc[2048 * 2];
	while (frames) {
		uint32_t n = MIN(frames, 2048u);
		memset(acc, 0, n * 2 * sizeof acc[0]);
		uint64_t f = spin_lock_irqsave(&lock);
		for (int k = 0; k < MAX_STREAMS; k++) {
			struct stream *s = &streams[k];
			if (!s->used)
				continue;
			uint32_t step = (uint32_t)(((uint64_t)s->rate << 16) / AUDIO_RATE);
			for (uint32_t i = 0; i < n; i++) {
				if (s->len < 2) {
					s->underruns++;	/* ran dry: silence till more comes */
					break;
				}
				uint32_t a = s->head, b = (s->head + 1) % RING;
				int32_t fr = (int32_t)(s->frac >> 1);	/* 15 bits */
				int32_t l = s->ring[a * 2] + (((s->ring[b * 2] - s->ring[a * 2]) * fr) >> 15);
				int32_t r = s->ring[a * 2 + 1] + (((s->ring[b * 2 + 1] - s->ring[a * 2 + 1]) * fr) >> 15);
				acc[i * 2] += l * s->volume >> 8;
				acc[i * 2 + 1] += r * s->volume >> 8;
				s->frac += step;
				uint32_t adv = s->frac >> 16;
				s->frac &= 0xFFFF;
				adv = MIN(adv, s->len - 1);
				s->head = (s->head + adv) % RING;
				s->len -= adv;
			}
		}
		spin_unlock_irqrestore(&lock, f);
		if (out) {
			for (uint32_t i = 0; i < n * 2; i++) {
				int32_t v = acc[i] * master >> 8;
				out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			}
			out += n * 2;
		}
		frames -= n;
	}
}

const char *audio_device(void)
{
	return device;
}

int audio_streams(void)
{
	int n = 0;
	for (int i = 0; i < MAX_STREAMS; i++)
		n += streams[i].used;
	return n;
}

/* ---- with no sound card: use the samples up in real time ---- */

static int have_hw;

static void null_sink(void *arg)
{
	(void)arg;
	uint64_t last = uptime_us();
	uint64_t carry = 0;
	for (;;) {
		sleep_ms(20);
		if (have_hw)
			return;
		uint64_t now = uptime_us();
		carry += (now - last) * AUDIO_RATE;
		last = now;
		uint32_t frames = (uint32_t)(carry / 1000000);
		carry -= (uint64_t)frames * 1000000;
		audio_mix(NULL, frames);
	}
}

void audio_init(void)
{
	extern int hda_init(void);
	if (hda_init()) {
		have_hw = 1;
		device = "HD Audio";
	} else {
		task_spawn("audio", null_sink, NULL, NULL);
	}
}

void audio_set_device(const char *name)
{
	device = name;
}

/* ---- PLAY: a .WAV file ---- */

#include "vfs.h"

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int audio_play_file(const char *path, int quiet)
{
	int e = 0;
	struct vfile *f = vfs_open(path, O_READ, &e);
	if (!f)
		return e;
	uint8_t h[12];
	if (vfs_read(f, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
		vfs_close(f);
		return E_INVAL;
	}
	uint32_t rate = 0;
	int channels = 0, bits = 0, fmt = 0;
	uint32_t data_len = 0;
	for (;;) {
		uint8_t ch[8];
		if (vfs_read(f, ch, 8) != 8) {
			vfs_close(f);
			return E_INVAL;
		}
		uint32_t len = le32(ch + 4);
		if (!memcmp(ch, "fmt ", 4)) {
			uint8_t b[40];
			uint32_t take = MIN(len, (uint32_t)sizeof b);
			if (vfs_read(f, b, take) != (long)take) {
				vfs_close(f);
				return E_INVAL;
			}
			fmt = le16(b);
			channels = le16(b + 2);
			rate = le32(b + 4);
			bits = le16(b + 14);
			if (len > take)
				vfs_seek(f, vfs_tell(f) + (len - take) + (len & 1));
			else if (len & 1)
				vfs_seek(f, vfs_tell(f) + 1);
		} else if (!memcmp(ch, "data", 4)) {
			data_len = len;
			break;
		} else {
			vfs_seek(f, vfs_tell(f) + len + (len & 1));
		}
	}
	if (fmt != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2) {
		vfs_close(f);
		return E_NOTSUP;
	}
	int id = audio_open(rate);
	if (id < 0) {
		vfs_close(f);
		return E_NOMEM;
	}
	if (!quiet)
		kprintf("Playing %s: %u Hz, %d-bit, %s, %u.%u s on %s\n", vfs_basename(path), rate, bits,
		    channels == 2 ? "stereo" : "mono", data_len / (rate * (uint32_t)channels * (uint32_t)bits / 8),
		    data_len * 10 / (rate * (uint32_t)channels * (uint32_t)bits / 8) % 10, audio_device());
	uint8_t raw[4096];
	int16_t pcm[4096];
	uint32_t left = data_len, frame_bytes = (uint32_t)(channels * bits / 8);
	int stopped = 0;
	while (left >= frame_bytes && !stopped) {
		uint32_t want = MIN(left, (uint32_t)sizeof raw) / frame_bytes * frame_bytes;
		long got = vfs_read(f, raw, want);
		if (got <= 0)
			break;
		left -= (uint32_t)got;
		uint32_t frames = (uint32_t)got / frame_bytes;
		for (uint32_t i = 0; i < frames * (uint32_t)channels; i++)
			pcm[i] = bits == 8 ? (int16_t)((raw[i] - 128) << 8) : (int16_t)le16(raw + i * 2);
		uint32_t done = 0;
		while (done < frames) {
			done += audio_write(id, pcm + done * (uint32_t)channels, frames - done, channels);
			if (done < frames) {
				if (con_pollkey() == 3) {
					stopped = 1;
					break;
				}
				sleep_ms(10);
			}
		}
	}
	vfs_close(f);
	/* let the tail play out */
	while (audio_queued(id) > 1 && !stopped) {
		if (con_pollkey() == 3)
			break;
		sleep_ms(10);
	}
	audio_close(id);
	return 0;
}
