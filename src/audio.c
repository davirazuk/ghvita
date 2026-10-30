#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "decoder.h"
#include "plat.h"

#define RING_FRAMES 65536               // 1.4 s of 48 kHz stereo per stem
#define CHUNK_FRAMES 4096
#define MAX_MIX 2048
#define MAX_VOICES 24
#define HISTORY 32
#define WORKERS 2                       // decode threads (on the Vita, cores 2 and 1): a stem goes to stem % WORKERS
#define MAX_BEND 600                    // the whammy's furthest, in frames behind the read position

typedef struct {
    Decoder * dec;
    bool open, eof;
    int16_t * ring;
    volatile uint32_t wpos, rpos;       // frames written by the decoder / read by the mixer
    volatile uint32_t skip;             // frames the mixer had to go without: the decoder drops them to stay in step
    float vol_target, vol;
    float bend_target, bend;            // the whammy: frames behind the read position
} Stream;

struct Sound {
    int16_t * pcm;                      // stereo
    int frames;
};

typedef struct {
    Sound * sound;
    int pos;
    float vol_target, vol;
    bool loop, active;
} Voice;

static Stream streams[MAX_STEMS];
static int stream_count;
static Voice voices[MAX_VOICES];
static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
// a worker's lock is held while it decodes its stems; loading and seeking take them all
static pthread_mutex_t decode_lock[WORKERS] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;     // one load at a time: a song's, or a preview's

// a preview the decode thread is to open (opening and seeking the stems is many card reads: the song list mustn't wait)
static volatile bool preview_pending;
static struct {
    char dir[512];
    char stems[MAX_STEMS][64];
    int count;
    double from;
    float volume, fade_in;
} preview;

// the song as the mixer plays it
static bool song_active, song_playing;
static volatile bool previewing;        // what's loaded is a preview
static int64_t song_frame;              // the next frame the mixer makes, counted from the audio's start
static int64_t ring_start;              // the frame the rings start with (earlier ones are the lead-in's silence)
static float fade_vol = 1, fade_target = 1, fade_step;
static float music_vol = 1, effects_vol = 1;
static volatile int underruns;
static float song_speed = 1;            // below 1: the song plays slower and lower, like a tape (practice)
static float slow_pos;                  // where the next output frame falls in the slowed buffer
static float slowed[(MAX_MIX + 2) * 2]; // the song before it's stretched; its first two frames are the last block's

// which song frame each output buffer started with, to match the output's "started" reports
typedef struct {
    int64_t out_frame, song_frame;
    bool playing;
} Mixed;
static Mixed history[HISTORY];
static int64_t out_frames;

// the clock: the song frame heard at anchor_us
static volatile int64_t anchor_frame, anchor_us;
static volatile bool anchor_playing;
static bool anchor_from_output;         // the output reports when buffers start (not on a fixed-clock test)
static double last_time;

static volatile float mix_level;

// how loud the last buffer was, 0..1 (what the band moves to)
float audio_level(void)
{
    return mix_level;
}

static void mix(int16_t * out, int frames)
{
    static float acc[MAX_MIX * 2];
    int64_t level = 0;
    if (frames > MAX_MIX)
        frames = MAX_MIX;
    memset(acc, 0, frames * 2 * sizeof(float));
    pthread_mutex_lock(&mix_lock);
    Mixed * h = &history[(out_frames / frames) % HISTORY];
    h->out_frame = out_frames;
    h->song_frame = song_frame;
    h->playing = song_active && song_playing;
    if (song_active && song_playing) {
        // the frames of the song this buffer takes, and where they go
        bool slow = song_speed < 1;
        int count = slow ? (int)(slow_pos + frames * song_speed) : frames;
        float * dst = slow ? slowed + 4 : acc;
        if (slow)
            memset(dst, 0, count * 2 * sizeof(float));
        int offset = 0;
        if (song_frame < ring_start)
            offset = ring_start - song_frame < count ? (int)(ring_start - song_frame) : count;
        int n = count - offset;
        // the song's fade, per buffer
        float f0 = fade_vol;
        if (fade_vol != fade_target) {
            fade_vol += fade_step * frames;
            if ((fade_step > 0 && fade_vol > fade_target) || (fade_step < 0 && fade_vol < fade_target))
                fade_vol = fade_target;
        }
        for (int s = 0; s < stream_count && n > 0; s++) {
            Stream * st = &streams[s];
            if (!st->open)
                continue;
            uint32_t avail = st->wpos - st->rpos;
            int take = (int)avail < n ? (int)avail : n;
            if (take < n && !st->eof) {
                __sync_fetch_and_add(&st->skip, n - take);
                underruns++;
            }
            float v0 = st->vol * f0 * music_vol, v1 = st->vol_target * fade_vol * music_vol;
            float dv = take > 0 ? (v1 - v0) / take : 0;
            float v = v0;
            uint32_t r = st->rpos;
            if (st->bend > 0 || st->bend_target > 0) {
                // the whammy reads the stem through a delay that moves: the pitch goes with the bar and comes back, and
                // the stem never leaves the song
                float b = st->bend, db = take > 0 ? (st->bend_target - b) / take : 0;
                for (int i = 0; i < take; i++) {
                    float at = i - b;
                    int k = (int)floorf(at);
                    float f = at - k;
                    const int16_t * s0 = &st->ring[((r + k) % RING_FRAMES) * 2];
                    const int16_t * s1 = &st->ring[((r + k + 1) % RING_FRAMES) * 2];
                    dst[(offset + i) * 2] += (s0[0] + (s1[0] - s0[0]) * f) * v;
                    dst[(offset + i) * 2 + 1] += (s0[1] + (s1[1] - s0[1]) * f) * v;
                    v += dv;
                    b += db;
                }
                st->bend = st->bend_target;
            } else
                for (int i = 0; i < take; i++) {
                    const int16_t * smp = &st->ring[((r + i) % RING_FRAMES) * 2];
                    dst[(offset + i) * 2] += smp[0] * v;
                    dst[(offset + i) * 2 + 1] += smp[1] * v;
                    v += dv;
                }
            __sync_synchronize();
            st->rpos = r + take;
            st->vol = st->vol_target;
        }
        song_frame += count;
        if (slow) {
            float pos = slow_pos;
            for (int i = 0; i < frames; i++, pos += song_speed) {
                int k = (int)pos;
                float f = pos - k;
                acc[i * 2] = slowed[k * 2] + (slowed[k * 2 + 2] - slowed[k * 2]) * f;
                acc[i * 2 + 1] = slowed[k * 2 + 1] + (slowed[k * 2 + 3] - slowed[k * 2 + 1]) * f;
            }
            memmove(slowed, slowed + count * 2, 4 * sizeof(float));
            slow_pos = pos - count;
        }
    }
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice * vc = &voices[i];
        if (!vc->active)
            continue;
        float v0 = vc->vol * effects_vol, v1 = vc->vol_target * effects_vol, dv = (v1 - v0) / frames, v = v0;
        for (int j = 0; j < frames; j++) {
            if (vc->pos >= vc->sound->frames) {
                if (!vc->loop) {
                    vc->active = false;
                    break;
                }
                vc->pos = 0;
            }
            acc[j * 2] += vc->sound->pcm[vc->pos * 2] * v;
            acc[j * 2 + 1] += vc->sound->pcm[vc->pos * 2 + 1] * v;
            vc->pos++;
            v += dv;
        }
        vc->vol = vc->vol_target;
    }
    out_frames += frames;
    pthread_mutex_unlock(&mix_lock);
    for (int i = 0; i < frames * 2; i++) {
        float s = acc[i];
        out[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
        level += abs(out[i]);
    }
    mix_level = (float)level / (frames * 2) / 12000.0f;
}

static void started(int64_t frame, int64_t when_us)
{
    pthread_mutex_lock(&mix_lock);
    for (int i = 0; i < HISTORY; i++) {
        if (history[i].out_frame == frame) {
            if (history[i].playing && song_playing) {
                anchor_frame = history[i].song_frame;
                anchor_us = when_us;
                anchor_playing = true;
                anchor_from_output = true;
            }
            break;
        }
    }
    pthread_mutex_unlock(&mix_lock);
}

static void start_preview(void);

static void lock_decoders(void)
{
    for (int i = 0; i < WORKERS; i++)
        pthread_mutex_lock(&decode_lock[i]);
}

static void unlock_decoders(void)
{
    for (int i = WORKERS - 1; i >= 0; i--)
        pthread_mutex_unlock(&decode_lock[i]);
}

static int decode_thread(void * arg)
{
    int worker = (int)(intptr_t)arg;
    static int16_t tmp[WORKERS][CHUNK_FRAMES * 2];
    for (;;) {
        if (worker == 0 && preview_pending)
            start_preview();
        bool busy = false;
        pthread_mutex_lock(&decode_lock[worker]);
        for (int s = worker; s < stream_count; s += WORKERS) {
            Stream * st = &streams[s];
            // what the whammy has yet to read stays in the ring behind the mixer
            if (!st->open || st->eof || RING_FRAMES - (st->wpos - st->rpos) < CHUNK_FRAMES + MAX_BEND + 2)
                continue;
            int got = 0;
            while (got < CHUNK_FRAMES) {
                int n = dec_read(st->dec, tmp[worker] + got * 2, CHUNK_FRAMES - got);
                if (n <= 0) {
                    st->eof = true;
                    break;
                }
                got += n;
            }
            // drop what the mixer already went without
            int drop = 0;
            uint32_t skip = st->skip;
            if (skip) {
                drop = (int)skip < got ? (int)skip : got;
                __sync_fetch_and_sub(&st->skip, drop);
            }
            uint32_t w = st->wpos;
            for (int i = drop; i < got; i++) {
                int16_t * d = &st->ring[((w + i - drop) % RING_FRAMES) * 2];
                d[0] = tmp[worker][i * 2];
                d[1] = tmp[worker][i * 2 + 1];
            }
            __sync_synchronize();
            st->wpos = w + (got - drop);
            busy = true;
        }
        pthread_mutex_unlock(&decode_lock[worker]);
        if (!busy)
            plat_sleep_us(4000);
    }
    return 0;
}

void audio_init(void)
{
    for (int i = 0; i < MAX_STEMS; i++)
        streams[i].ring = malloc(RING_FRAMES * 2 * sizeof(int16_t));
    plat_thread("decode", decode_thread, (void *)0, 2);
    plat_thread("decode2", decode_thread, (void *)1, 1);
    plat_audio_start(mix, started);
}

static void song_unload(void)
{
    pthread_mutex_lock(&mix_lock);
    song_active = false;
    song_playing = false;
    anchor_playing = false;
    last_time = -1e9;
    pthread_mutex_unlock(&mix_lock);
    lock_decoders();
    for (int i = 0; i < stream_count; i++) {
        if (streams[i].open)
            dec_close(streams[i].dec);
        streams[i].dec = NULL;
        streams[i].open = false;
    }
    stream_count = 0;
    unlock_decoders();
}

static bool song_load(const char * dir, const char * const * stems, int count)
{
    song_unload();
    lock_decoders();
    int opened = 0;
    for (int i = 0; i < count && i < MAX_STEMS; i++) {
        Stream * st = &streams[i];
        char path[600];
        // pack.py's stems are named without the .ogg; a Clone Hero song's keep their own files
        snprintf(path, sizeof(path), "%s/%s%s", dir, stems[i], strchr(stems[i], '.') ? "" : ".ogg");
        st->dec = dec_open(path);
        st->open = st->dec != NULL;
        if (st->open)
            opened++;
        st->eof = !st->open;
        st->wpos = st->rpos = st->skip = 0;
        st->vol = st->vol_target = 1;
        st->bend = st->bend_target = 0;
    }
    stream_count = count < MAX_STEMS ? count : MAX_STEMS;
    unlock_decoders();
    return opened > 0;
}

static void song_play(double from, double lead_in)
{
    pthread_mutex_lock(&mix_lock);
    song_active = false;
    pthread_mutex_unlock(&mix_lock);
    int64_t start = (int64_t)(from * AUDIO_RATE);
    if (start < 0)
        start = 0;
    lock_decoders();
    for (int i = 0; i < stream_count; i++) {
        Stream * st = &streams[i];
        if (st->open) {
            dec_seek(st->dec, start);
            st->eof = false;
        }
        st->wpos = st->rpos = st->skip = 0;
    }
    unlock_decoders();
    pthread_mutex_lock(&mix_lock);
    // frames count from the audio's start; the rings begin at `start`, and before it is the lead-in's silence
    ring_start = start;
    song_frame = start - (int64_t)(lead_in * AUDIO_RATE);
    anchor_frame = song_frame;
    anchor_from_output = false;
    anchor_us = plat_now_us();
    anchor_playing = true;
    last_time = -1e9;
    song_active = true;
    song_playing = true;
    pthread_mutex_unlock(&mix_lock);
}

void audio_song_unload(void)
{
    pthread_mutex_lock(&load_lock);
    preview_pending = false;            // a preview not opened yet won't be
    song_unload();
    pthread_mutex_unlock(&load_lock);
}

bool audio_song_load(const char * dir, const char * const * stems, int count)
{
    pthread_mutex_lock(&load_lock);
    preview_pending = false;
    previewing = false;
    bool ok = song_load(dir, stems, count);
    pthread_mutex_unlock(&load_lock);
    return ok;
}

void audio_song_play(double from, double lead_in)
{
    pthread_mutex_lock(&load_lock);
    song_play(from, lead_in);
    pthread_mutex_unlock(&load_lock);
}

void audio_preview(const char * dir, const char * const * stems, int count, double from, float volume, float fade_in)
{
    pthread_mutex_lock(&load_lock);
    song_unload();
    snprintf(preview.dir, sizeof(preview.dir), "%s", dir);
    preview.count = count < MAX_STEMS ? count : MAX_STEMS;
    for (int i = 0; i < preview.count; i++)
        snprintf(preview.stems[i], sizeof(preview.stems[i]), "%s", stems[i]);
    preview.from = from;
    preview.volume = volume;
    preview.fade_in = fade_in;
    preview_pending = true;
    pthread_mutex_unlock(&load_lock);
}

// on the decode thread
static void start_preview(void)
{
    pthread_mutex_lock(&load_lock);
    if (preview_pending) {
        preview_pending = false;
        const char * stems[MAX_STEMS];
        for (int i = 0; i < preview.count; i++)
            stems[i] = preview.stems[i];
        previewing = true;
        if (song_load(preview.dir, stems, preview.count)) {
            audio_song_fade(0, 0);
            song_play(preview.from, 0);
            audio_song_fade(preview.volume, preview.fade_in);
        }
    }
    pthread_mutex_unlock(&load_lock);
}

void audio_song_speed(float speed)
{
    pthread_mutex_lock(&mix_lock);
    song_speed = speed;
    slow_pos = 0;
    memset(slowed, 0, 4 * sizeof(float));
    pthread_mutex_unlock(&mix_lock);
}

void audio_song_pause(bool pause)
{
    double now = audio_song_time();
    pthread_mutex_lock(&mix_lock);
    song_playing = !pause;
    if (pause) {
        anchor_playing = false;
        last_time = now;
    } else {
        // carry on from where it stopped
        anchor_frame = (int64_t)(now * AUDIO_RATE);
        anchor_us = plat_now_us();
        anchor_playing = true;
        anchor_from_output = false;
        last_time = now;
    }
    pthread_mutex_unlock(&mix_lock);
}

double audio_song_time(void)
{
    pthread_mutex_lock(&mix_lock);
    int64_t af = anchor_frame, au = anchor_us;
    bool playing = anchor_playing, from_output = anchor_from_output;
    double last = last_time;
    pthread_mutex_unlock(&mix_lock);
    if (!playing)
        return last;
    double t = af / (double)AUDIO_RATE + (plat_now_us() - au) / 1e6 * song_speed;
    // the audio thread reports every buffer: past a few of them it has stalled, so the song has too
    if (from_output && t > af / (double)AUDIO_RATE + 0.08 * song_speed)
        t = af / (double)AUDIO_RATE + 0.08 * song_speed;
    if (t < last)
        t = last;
    last_time = t;
    return t;
}

bool audio_song_finished(void)
{
    if (!song_active)
        return true;
    for (int i = 0; i < stream_count; i++)
        if (streams[i].open && (!streams[i].eof || streams[i].wpos != streams[i].rpos))
            return false;
    return song_frame > ring_start;
}

bool audio_song_active(void)
{
    return stream_count > 0 && !previewing;
}

void audio_stem_bend(int stem, float amount)
{
    if (stem >= 0 && stem < MAX_STEMS)
        streams[stem].bend_target = amount * MAX_BEND;
}

void audio_stem_volume(int stem, float volume)
{
    if (stem >= 0 && stem < MAX_STEMS)
        streams[stem].vol_target = volume;
}

void audio_song_fade(float volume, float seconds)
{
    pthread_mutex_lock(&mix_lock);
    fade_target = volume;
    if (seconds <= 0) {
        fade_vol = volume;
        fade_step = 0;
    } else
        fade_step = (volume - fade_vol) / (seconds * AUDIO_RATE);
    pthread_mutex_unlock(&mix_lock);
}

void audio_set_volumes(float music, float effects)
{
    music_vol = music;
    effects_vol = effects;
}

int audio_underruns(void)
{
    return underruns;
}

Sound * sound_load(const char * path)
{
    Decoder * d = dec_open(path);
    if (!d)
        return NULL;
    int64_t total = dec_length(d);
    if (total <= 0)
        total = AUDIO_RATE * 10;
    Sound * s = calloc(1, sizeof(Sound));
    s->pcm = malloc((total + 16) * 2 * sizeof(int16_t));
    for (;;) {
        int room = (int)(total + 16 - s->frames);
        if (room <= 0)
            break;
        int n = dec_read(d, s->pcm + s->frames * 2, room < 4096 ? room : 4096);
        if (n <= 0)
            break;
        s->frames += n;
    }
    dec_close(d);
    return s;
}

int sound_play(Sound * s, float volume, bool loop)
{
    if (!s || s->frames == 0)
        return -1;
    pthread_mutex_lock(&mix_lock);
    int best = -1, best_pos = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) {
            best = i;
            break;
        }
        // all busy: take the one-shot that's furthest along
        if (!voices[i].loop && voices[i].pos > best_pos) {
            best = i;
            best_pos = voices[i].pos;
        }
    }
    if (best >= 0) {
        Voice * v = &voices[best];
        v->sound = s;
        v->pos = 0;
        v->vol = v->vol_target = volume;
        v->loop = loop;
        v->active = true;
    }
    pthread_mutex_unlock(&mix_lock);
    return best;
}

void sound_stop(int voice)
{
    if (voice < 0 || voice >= MAX_VOICES)
        return;
    pthread_mutex_lock(&mix_lock);
    voices[voice].active = false;
    pthread_mutex_unlock(&mix_lock);
}

void sound_volume(int voice, float volume)
{
    if (voice >= 0 && voice < MAX_VOICES)
        voices[voice].vol_target = volume;
}
