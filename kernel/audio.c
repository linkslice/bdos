/*
 * audio.c - the sound mixer, a mix for each session.
 *
 * Anything that makes sound opens a stream at its own rate (mono or
 * stereo, 16-bit) and writes samples into it; the mixer resamples every
 * stream to 48 kHz stereo and adds them up.
 *
 * Streams belong to the session of the task that opened them, the way T:
 * does: the machine's own console is session 0, and each remote session
 * (bterm, CPU \\HOST) is another. A session's mix goes where it's heard:
 * the console's to the HD Audio driver (or, with no sound card, nowhere,
 * used up at the same pace so writers still keep time); a remote one's to
 * its sink -- the network, if the far end can play it -- or nowhere. So a
 * game in your bterm window sounds on your machine, the console's on the
 * console, and nobody hears anyone else's.
 *
 * Streams are touched from DOS boxes running without the big lock, so the
 * mixer has a spinlock of its own.
 */
#include "kernel.h"
#include "spinlock.h"
#include "audio.h"

#define MAX_STREAMS	32
#define MAX_SESSIONS	16
#define RING		32768		/* frames per stream (stereo pairs) */

struct stream {
	int used;
	int sess;			/* the session it sounds in; -1: its session ended */
	uint32_t rate;
	int16_t *ring;			/* stereo frames */
	uint32_t head, len;
	uint32_t frac;			/* 16.16 position between ring[head] and ring[head+1] */
	int volume;			/* 0..256 */
	uint64_t underruns;
	int primed;			/* has had PRIME frames since it last ran dry */
	int waited;			/* mixes it sat out unprimed (a short tail plays anyway) */
	char name[16];
};

/*
 * A stream isn't heard till it has this much waiting (and again after it
 * runs dry): whoever writes it and whatever mixes it keep time separately,
 * and this much slack keeps their jitter from becoming gaps.
 */
#define PRIME	(AUDIO_RATE / 50)	/* 20 ms */

struct session {
	int used;
	int raw_stream;			/* AUDIO$'s, +1 */
	int volume, muted;
	audio_sink_fn sink;		/* where its mix goes (NULL: nowhere) */
	void *ctx;
	volatile int stop, stopped;
	char name[24];
};

static struct stream streams[MAX_STREAMS];
static struct session sessions[MAX_SESSIONS] = { [0] = { .used = 1, .volume = 256, .name = "console" } };
static struct spinlock lock;
static const char *device = "none";

static int task_session(void)
{
	struct task *t = task_current();
	int s = t ? t->asess : 0;
	return s >= 0 && s < MAX_SESSIONS && sessions[s].used ? s : 0;
}

int audio_open(uint32_t rate)
{
	return audio_open_named(rate, task_current() ? task_current()->name : "");
}

int audio_open_named(uint32_t rate, const char *name)
{
	if (rate < 1000 || rate > 192000)
		return -1;
	int16_t *ring = kmalloc(RING * 4);
	if (!ring)
		return -1;
	int sess = task_session();
	uint64_t f = spin_lock_irqsave(&lock);
	for (int i = 0; i < MAX_STREAMS; i++)
		if (!streams[i].used) {
			struct stream *s = &streams[i];
			memset(s, 0, sizeof *s);
			s->used = 1;
			s->sess = sess;
			s->rate = rate;
			s->ring = ring;
			s->volume = 256;
			strncpy0(s->name, name ? name : "", sizeof s->name);
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

/* the volume of the session the caller is in (the console's is the machine's) */
void audio_set_master(int vol)
{
	sessions[task_session()].volume = MAX(0, MIN(vol, 256));
}

int audio_get_master(void)
{
	return sessions[task_session()].volume;
}

void audio_set_mute(int on)
{
	sessions[task_session()].muted = on != 0;
}

int audio_get_mute(void)
{
	return sessions[task_session()].muted;
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
	if (s->sess < 0) {		/* nobody to hear it: taken, and gone */
		spin_unlock_irqrestore(&lock, f);
		return frames;
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
	if (streams[id].sess < 0)
		return RING;
	return RING - streams[id].len;
}

/*
 * One session's mix: frames of 48 kHz stereo out (out may be NULL: just use
 * them up). Returns how many of them had anything playing in them.
 */
uint32_t audio_mix_session(int sess, int16_t *out, uint32_t frames)
{
	int32_t acc[256 * 2];
	uint32_t sounding = 0;
	struct session *se = &sessions[sess];
	while (frames) {
		uint32_t n = MIN(frames, 256u), heard = 0;
		memset(acc, 0, n * 2 * sizeof acc[0]);
		uint64_t f = spin_lock_irqsave(&lock);
		for (int k = 0; k < MAX_STREAMS; k++) {
			struct stream *s = &streams[k];
			if (!s->used || s->sess != sess)
				continue;
			if (!s->primed) {
				if (s->len < PRIME && (!s->len || ++s->waited < 4))
					continue;
				s->primed = 1;
				s->waited = 0;
			}
			uint32_t step = (uint32_t)(((uint64_t)s->rate << 16) / AUDIO_RATE);
			uint32_t i = 0;
			for (; i < n; i++) {
				if (s->len < 2) {
					s->underruns++;	/* ran dry: silence till more comes */
					s->primed = 0;
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
			heard = MAX(heard, i);
		}
		spin_unlock_irqrestore(&lock, f);
		if (out) {
			int vol = se->muted ? 0 : se->volume;
			for (uint32_t i = 0; i < n * 2; i++) {
				int32_t v = acc[i] * vol >> 8;
				out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			}
			out += n * 2;
		}
		sounding += heard;
		frames -= n;
	}
	return sounding;
}

/* the console's mix: for the sound card's driver */
void audio_mix(int16_t *out, uint32_t frames)
{
	audio_mix_session(0, out, frames);
}

/* ---- sessions ---- */

/*
 * A session's clock: every 10 ms, the mix since last time -- to its sink
 * when anything's playing, else just used up -- so programs in it keep time
 * whether or not anyone hears them.
 */
#define BLOCK	480		/* 10 ms */

static void session_pump(void *arg)
{
	int id = (int)(intptr_t)arg;
	struct session *se = &sessions[id];
	bkl_release_all();	/* the mixer has its own lock; the sink takes the big one to send */
	int16_t *pcm = kmalloc(BLOCK * 4);
	uint64_t last = uptime_us(), carry = 0;
	int quiet = 8;		/* nothing yet: nothing to send */
	while (!se->stop) {
		sleep_ms(5);
		uint64_t now = uptime_us();
		carry += (now - last) * AUDIO_RATE;
		last = now;
		if (carry > (uint64_t)AUDIO_RATE * 200000)	/* held up a long time: don't flood */
			carry = (uint64_t)AUDIO_RATE * 20000;
		while (carry >= (uint64_t)BLOCK * 1000000 && !se->stop) {
			carry -= (uint64_t)BLOCK * 1000000;
			uint32_t heard = audio_mix_session(id, pcm, BLOCK);
			/* a few blocks of silence after the sound stops, so its end
			 * isn't clipped; then nothing goes at all */
			quiet = heard ? 0 : quiet + 1;
			if (pcm && se->sink && quiet < 8)
				se->sink(se->ctx, pcm, BLOCK);
		}
	}
	kfree(pcm);
	se->stopped = 1;
}

int audio_session_new(const char *name, audio_sink_fn sink, void *ctx)
{
	uint64_t f = spin_lock_irqsave(&lock);
	int id = -1;
	for (int i = 1; i < MAX_SESSIONS; i++)
		if (!sessions[i].used) {
			id = i;
			struct session *se = &sessions[i];
			memset(se, 0, sizeof *se);
			se->used = 1;
			se->volume = 256;
			se->sink = sink;
			se->ctx = ctx;
			strncpy0(se->name, name ? name : "", sizeof se->name);
			break;
		}
	spin_unlock_irqrestore(&lock, f);
	if (id < 0)
		return -1;
	char tn[24];
	snprintf(tn, sizeof tn, "audio %d", id);
	if (!task_spawn(tn, session_pump, (void *)(intptr_t)id, NULL)) {
		sessions[id].used = 0;
		return -1;
	}
	return id;
}

/* the session's over: its pump stops, and anything still playing in it
 * goes nowhere (writes are taken and dropped, so no writer waits) */
void audio_session_end(int id)
{
	if (id <= 0 || id >= MAX_SESSIONS || !sessions[id].used)
		return;
	struct session *se = &sessions[id];
	se->stop = 1;
	while (!se->stopped)
		task_idle();
	if (se->raw_stream)
		audio_close(se->raw_stream - 1);
	se->raw_stream = 0;
	uint64_t f = spin_lock_irqsave(&lock);
	for (int i = 0; i < MAX_STREAMS; i++)
		if (streams[i].used && streams[i].sess == id) {
			streams[i].sess = -1;
			streams[i].len = 0;
		}
	se->used = 0;
	spin_unlock_irqrestore(&lock, f);
}

int audio_session_of_task(void)
{
	return task_session();
}

/* SOUND and the Sound applet: the caller's session, and what's in it */
int audio_session_info(char *name, size_t n, int *volume, int *muted, int *audible)
{
	int id = task_session();
	struct session *se = &sessions[id];
	if (name)
		strncpy0(name, se->name, n);
	if (volume)
		*volume = se->volume;
	if (muted)
		*muted = se->muted;
	if (audible)
		*audible = id == 0 ? 1 : se->sink != NULL;
	return id;
}

int audio_stream_info(int i, char *name, size_t n, int *volume, uint32_t *rate)
{
	if (i < 0 || i >= MAX_STREAMS)
		return -1;
	struct stream *s = &streams[i];
	int sess = task_session();
	if (!s->used || s->sess != sess)
		return 0;
	if (name)
		strncpy0(name, s->name, n);
	if (volume)
		*volume = s->volume;
	if (rate)
		*rate = s->rate;
	return 1;
}

const char *audio_device(void)
{
	return task_session() == 0 ? device : sessions[task_session()].sink ? "the network" : "none (muted)";
}

int audio_streams(void)
{
	int n = 0, sess = task_session();
	for (int i = 0; i < MAX_STREAMS; i++)
		n += streams[i].used && streams[i].sess == sess;
	return n;
}

/* the speaker beeps of a program: through the mixer if this session is heard
 * through one, else (the console with no sound card) the PC's own speaker */
int audio_session_has_mixer_out(void)
{
	int id = task_session();
	return id == 0 ? strcmp(device, "none") != 0 : 1;
}

/* ---- with no sound card: use the console's samples up in real time ---- */

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

/* ---- VOLUME$ and AUDIO$: the session's sound from the shell ---- */

#include "vfs.h"

/* TYPE VOLUME$: the session's volume (0-100), whether it's muted, where it's
 * heard. ECHO 50 > VOLUME$, ECHO MUTE > VOLUME$, ECHO UNMUTE > VOLUME$ */
static long volume_read(struct chardev *d, void *buf, size_t n)
{
	(void)d;
	char name[24], out[160];
	int vol, muted, audible;
	int id = audio_session_info(name, sizeof name, &vol, &muted, &audible);
	int len = snprintf(out, sizeof out, "volume %d\nmuted %d\nsession %d %s\nheard %s\n",
	    (vol * 100 + 128) / 256, muted, id, name, id == 0 ? device : audible ? "remote" : "nowhere");
	size_t c = MIN(n, (size_t)len);
	memcpy(buf, out, c);
	return (long)c;
}

static long volume_write(struct chardev *d, const void *buf, size_t n)
{
	(void)d;
	char line[32];
	size_t l = MIN(n, sizeof line - 1);
	memcpy(line, buf, l);
	line[l] = 0;
	const char *p = line;
	while (*p == ' ')
		p++;
	if (!strncasecmp(p, "unmute", 6))
		audio_set_mute(0);
	else if (!strncasecmp(p, "mute", 4))
		audio_set_mute(1);
	else {
		if (!strncasecmp(p, "volume", 6))
			p += 6;
		while (*p == ' ')
			p++;
		if (*p < '0' || *p > '9')
			return E_INVAL;
		long v = strtol(p, NULL, 10);
		audio_set_master((int)(MAX(0, MIN(v, 100)) * 256 / 100));
	}
	return (long)n;
}

/* COPY SOUND.RAW AUDIO$: 48 kHz 16-bit stereo, played in the session */
static long audio_dev_write(struct chardev *d, const void *buf, size_t n)
{
	(void)d;
	struct session *se = &sessions[task_session()];
	if (!se->raw_stream) {
		int id = audio_open_named(AUDIO_RATE, "AUDIO$");
		if (id < 0)
			return E_NOMEM;
		se->raw_stream = id + 1;
	}
	const int16_t *pcm = buf;
	uint32_t frames = (uint32_t)(n / 4), done = 0;
	while (done < frames) {
		done += audio_write(se->raw_stream - 1, pcm + done * 2, frames - done, 2);
		if (done < frames)
			sleep_ms(5);
	}
	return (long)n;
}

static long audio_dev_read(struct chardev *d, void *buf, size_t n)
{
	(void)d;
	(void)buf;
	(void)n;
	return 0;
}

static struct chardev volume_dev = { "VOLUME$", volume_read, volume_write, NULL, 1 };
static struct chardev audio_dev = { "AUDIO$", audio_dev_read, audio_dev_write, NULL, 0 };

void audio_init(void)
{
	dev_register(&volume_dev);
	dev_register(&audio_dev);
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
