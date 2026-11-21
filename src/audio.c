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
#define LOOK_FRAMES 16000               // how far ahead a stem's level is read (a third of a second)
#define CHUNK_FRAMES 4096
#define MAX_MIX 2048
#define MAX_VOICES 24
#define HISTORY 32
#define WORKERS 2                       // decode threads (on the Vita, cores 2 and 1): a stem goes to stem % WORKERS
// the whammy, the pitch shifter YARG has from BASS_FX: an FFT of 2048 and a semitone with the bar all the way down
#define BEND_FFT 2048
#ifndef BEND_OVERLAP
#define BEND_OVERLAP 4                  // YARG's 8 would take a third of the Vita's audio core
#endif
#define BEND_HOP (BEND_FFT / BEND_OVERLAP)
#define BEND_BINS 768                   // to 18 kHz: nothing a guitar stem has above that is missed in a bend
#define BEND_SEMITONES 1.0f
#define BEND_EASE 0.0014f               // per frame: the bar's 60 steps a second become a slope (15 ms)
#define MAX_BEND BEND_FFT               // frames kept behind the read position, to start from

typedef struct {
    Decoder * dec;
    bool open, eof;
    int16_t * ring;
    volatile uint32_t wpos, rpos;       // frames written by the decoder / read by the mixer
    volatile uint32_t skip;             // frames the mixer had to go without: the decoder drops them to stay in step
    float vol_target, vol;
    float bend_target, bend;            // the whammy: how far the bar is down, 0..1
    float power;                        // how loud it's been, the mute not counted
    float level, ahead;                 // how loud it is now, and a third of a second on (mean squares, smoothed)
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
static int underruns_logged;           // the count at the last song's log line
static float song_speed = 1;            // not 1: the song plays slower and lower, or faster and higher, like a tape (practice)
static float slow_pos;                  // where the next output frame falls in the slowed buffer
#define MAX_SPEED 2               // the fastest the stretch goes
// the song before it's stretched (up to MAX_SPEED x a buffer's frames); its first two frames are the last block's
static float slowed[(MAX_MIX * MAX_SPEED + 4) * 2];

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

// Phase vocoder: each frame of the stem goes to frequencies, they're moved down, and the phases are carried on from what
// each frequency was doing the frame before. Frames come from ahead of the read position, so what comes out is complete
// the moment it's played: nothing else has to wait for the bent stem, as YARG's stems do
static struct {
    const Stream * st;
    uint32_t next;                      // the ring position the next sample out belongs to
    int have;                           // of out, used
    bool fresh;                         // the next frame starts the phases
    int warm;                           // hops until what comes out is whole
    float wet;                          // how much of it is heard
    float out[2][BEND_HOP], ola[2][BEND_FFT];
    float last[2][BEND_BINS * 2], sent[2][BEND_BINS * 2];     // the last frame's bins, as they came in and went out
} bender;
static volatile int anchor_jumps;       // seeks and pauses: the song's time moved in a way a buffer's report doesn't tell
static int64_t bend_us;                 // what the whammy has cost: the song's log line
static int bend_frames, bend_worst;
static float bend_win[BEND_FFT], fft_cos[BEND_FFT / 2], fft_sin[BEND_FFT / 2];
static uint16_t fft_rev[BEND_FFT / 2];

static void bend_init(void)
{
    const int half = BEND_FFT / 2;
    int bits = 0;
    while (1 << bits < half)
        bits++;
    for (int i = 0; i < BEND_FFT; i++)
        bend_win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / BEND_FFT);
    for (int i = 0; i < half; i++) {
        fft_cos[i] = cosf(2 * (float)M_PI * i / BEND_FFT);
        fft_sin[i] = sinf(2 * (float)M_PI * i / BEND_FFT);
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        fft_rev[i] = (uint16_t)r;
    }
}

// half-size complex FFT, in place on pairs; sign -1 forward, 1 back
static void fft_complex(float * z, int sign)
{
    const int n = BEND_FFT / 2;
    for (int i = 0; i < n; i++) {
        int j = fft_rev[i];
        if (j > i) {
            float a = z[i * 2], b = z[i * 2 + 1];
            z[i * 2] = z[j * 2];
            z[i * 2 + 1] = z[j * 2 + 1];
            z[j * 2] = a;
            z[j * 2 + 1] = b;
        }
    }
    for (int size = 2; size <= n; size *= 2) {
        int h = size / 2, step = BEND_FFT / size;
        for (int i = 0; i < n; i += size)
            for (int k = 0; k < h; k++) {
                float wr = fft_cos[k * step], wi = sign * fft_sin[k * step];
                float * a = z + (i + k) * 2, * b = a + h * 2;
                float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
    }
}

// x[BEND_FFT] of samples becomes the bins 0..BEND_FFT/2 as re, im pairs (x needs room for two more)
static void rfft(float * x)
{
    const int n = BEND_FFT / 2;
    fft_complex(x, -1);
    x[n * 2] = x[0] - x[1];
    x[n * 2 + 1] = 0;
    x[0] += x[1];
    x[1] = 0;
    for (int k = 1; k <= n / 2; k++) {
        float * a = x + k * 2, * b = x + (n - k) * 2;
        float er = (a[0] + b[0]) * 0.5f, ei = (a[1] - b[1]) * 0.5f;
        float or_ = (a[1] + b[1]) * 0.5f, oi = (b[0] - a[0]) * 0.5f;
        float wr = fft_cos[k], wi = -fft_sin[k];
        float tr = or_ * wr - oi * wi, ti = or_ * wi + oi * wr;
        a[0] = er + tr;
        a[1] = ei + ti;
        b[0] = er - tr;
        b[1] = ti - ei;
    }
}

static void irfft(float * x)
{
    const int n = BEND_FFT / 2;
    float x0 = x[0], xn = x[n * 2];
    x[0] = (x0 + xn) * 0.5f;
    x[1] = (x0 - xn) * 0.5f;
    for (int k = 1; k <= n / 2; k++) {
        float * a = x + k * 2, * b = x + (n - k) * 2;
        float er = (a[0] + b[0]) * 0.5f, ei = (a[1] - b[1]) * 0.5f;
        float tr = (a[0] - b[0]) * 0.5f, ti = (a[1] + b[1]) * 0.5f;
        float wr = fft_cos[k], wi = fft_sin[k];
        float or_ = tr * wr - ti * wi, oi = tr * wi + ti * wr;
        a[0] = er - oi;
        a[1] = ei + or_;
        b[0] = er + oi;
        b[1] = or_ - ei;
    }
    fft_complex(x, 1);
}

static inline float wrap_pi(float d)
{
    int q = (int)(d / (float)M_PI);
    q += q >= 0 ? q & 1 : -(q & 1);
    return d - (float)M_PI * q;
}

static inline float fast_atan2(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    if (ax == 0 && ay == 0)
        return 0;
    bool swap = ay > ax;
    float a = swap ? ax / ay : ay / ax, a2 = a * a;
    float r = a * (0.99997726f + a2 * (-0.33262347f + a2 * (0.19354346f + a2 * (-0.11643287f + a2 * (0.05265332f - 0.0117212f * a2)))));
    if (swap)
        r = (float)M_PI_2 - r;
    if (x < 0)
        r = (float)M_PI - r;
    return y < 0 ? -r : r;
}

// x in -pi..pi
static inline void fast_sincos(float x, float * s, float * c)
{
    bool flip = false;
    if (x > (float)M_PI_2) {
        x = (float)M_PI - x;
        flip = true;
    } else if (x < -(float)M_PI_2) {
        x = -(float)M_PI - x;
        flip = true;
    }
    float x2 = x * x;
    *s = x * (1 + x2 * (-0.16666667f + x2 * (0.0083333f + x2 * (-0.00019841f + x2 * 2.7526e-6f))));
    float co = 1 + x2 * (-0.5f + x2 * (0.041666667f + x2 * (-0.0013888889f + x2 * 2.4801e-5f)));
    *c = flip ? -co : co;
}

// the frame of the stem that starts at at, a hop further on in out. Each peak's whole lobe moves as one, so a note stays as
// loud and as solid as it was: the phases of its bins aren't left to drift apart (Laroche & Dolson's peak shifting)
static void bend_frame(const Stream * st, uint32_t at, float ratio)
{
    static float x[BEND_FFT + 2], power[BEND_BINS], y[BEND_BINS * 2];
    static int peak[BEND_BINS / 2 + 1];
    const float per = BEND_OVERLAP / (2 * (float)M_PI);
    uint32_t have = st->wpos - at;      // the end of a song, or a decoder behind: silence, as what isn't there
    for (int c = 0; c < 2; c++) {
        for (int j = 0; j < BEND_FFT; j++)
            x[j] = (uint32_t)j < have ? st->ring[((at + j) % RING_FRAMES) * 2 + c] * bend_win[j] : 0;
        rfft(x);
        float * last = bender.last[c], * sent = bender.sent[c];
        if (bender.fresh)
            memcpy(y, x, sizeof(y));
        else {
            for (int k = 0; k < BEND_BINS; k++)
                power[k] = x[k * 2] * x[k * 2] + x[k * 2 + 1] * x[k * 2 + 1];
            // from bin 1: a detuned bass's bottom string is down there, at 33 Hz. Below about 80 Hz a note's harmonics share
            // a lobe and move by the same Hz, up to 2/3 of a semitone out at the top of the bar. Following each bin on its
            // own tunes them and costs real bass stems 3-4 dB (tools/whammy.py), so they stay together
            int peaks = 0;
            for (int k = 1; k < BEND_BINS - 2; k++)
                if (power[k] > power[k - 1] && power[k] >= power[k + 1] && (k < 2 || power[k] > power[k - 2]) && power[k] >= power[k + 2]) {
                    peak[peaks++] = k;
                    k += 2;
                }
            memset(y, 0, sizeof(y));
            if (!peaks)                 // a tone below every bin, or nothing: as it is
                memcpy(y, x, sizeof(y));
            for (int i = 0, from = 0; i < peaks; i++) {
                int p = peak[i], to = i + 1 < peaks ? (p + peak[i + 1] + 1) / 2 : BEND_BINS;
                // what the peak really is, from how far its phase went since the last frame, and where that goes
                float ph = fast_atan2(x[p * 2 + 1], x[p * 2]);
                float d = wrap_pi(ph - fast_atan2(last[p * 2 + 1], last[p * 2]) - p / per);
                // the lobe moves by whole bins, as near as they get from the note's own frequency: taken from the peak's
                // bin it could land most of a bin off, and the frames then cancel a little (2 dB at 82 Hz)
                float was = p + d * per, f = was * ratio;
                int shift = (int)floorf(f - was + 0.5f), q = p + shift;
                // on from the phase its new bin went out with: rot takes the lobe there
                float rot = 0;
                if (q >= 0 && q < BEND_BINS)
                    rot = wrap_pi(fast_atan2(sent[q * 2 + 1], sent[q * 2]) + wrap_pi(f / per) - ph);
                float sn, cs;
                fast_sincos(rot, &sn, &cs);
                for (int k = from; k < to; k++) {
                    int o = k + shift;
                    if (o >= 0 && o < BEND_BINS) {
                        y[o * 2] += x[k * 2] * cs - x[k * 2 + 1] * sn;
                        y[o * 2 + 1] += x[k * 2] * sn + x[k * 2 + 1] * cs;
                    }
                }
                from = to;
            }
        }
        memcpy(last, x, sizeof(bender.last[c]));
        memcpy(sent, y, sizeof(bender.sent[c]));
        memcpy(x, y, sizeof(y));
        memset(x + BEND_BINS * 2, 0, (BEND_FFT + 2 - BEND_BINS * 2) * sizeof(float));
        irfft(x);
        float * ola = bender.ola[c];
        const float gain = 2.0f / BEND_FFT / (0.375f * BEND_OVERLAP);
        for (int j = 0; j < BEND_FFT; j++)
            ola[j] += x[j] * bend_win[j] * gain;
        memcpy(bender.out[c], ola, sizeof(bender.out[c]));
        memmove(ola, ola + BEND_HOP, (BEND_FFT - BEND_HOP) * sizeof(float));
        memset(ola + BEND_FFT - BEND_HOP, 0, BEND_HOP * sizeof(float));
    }
    bender.fresh = false;
}

// The stem at the bar's pitch. A bend starts by filling the overlap, a frame to a hop so no buffer pays for all of them, and
// meanwhile the stem plays as it is; it slides over, in time and in phase, and back the same way
static void mix_bent(Stream * st, uint32_t r, float * dst, int take, float v, float dv)
{
    float bend = st->bend;
    for (int i = 0; i < take; i++) {
        uint32_t at = r + i;
        if (bender.st != st || bender.next != at) {
            bender.st = st;
            bender.fresh = true;
            bender.warm = BEND_OVERLAP - 1;
            bender.wet = 0;
            bender.have = BEND_HOP;
            memset(bender.ola, 0, sizeof(bender.ola));
        }
        if (bender.have == BEND_HOP) {
            int64_t t0 = plat_now_us();
            bend_frame(st, at, powf(2, -bend * BEND_SEMITONES / 12));
            int us = (int)(plat_now_us() - t0);
            bend_us += us;
            bend_worst = us > bend_worst ? us : bend_worst;
            bend_frames++;
            bender.have = 0;
            if (bender.warm)
                bender.warm--;
        }
        bend += (st->bend_target - bend) * BEND_EASE;
        float want = bender.warm ? 0 : bend * 32 > 1 ? 1 : bend * 32;
        bender.wet += (want - bender.wet) * 0.003f;
        const int16_t * smp = &st->ring[(at % RING_FRAMES) * 2];
        dst[i * 2] += (smp[0] + (bender.out[0][bender.have] - smp[0]) * bender.wet) * v;
        dst[i * 2 + 1] += (smp[1] + (bender.out[1][bender.have] - smp[1]) * bender.wet) * v;
        bender.have++;
        bender.next = at + 1;
        v += dv;
    }
    st->bend = bend;
}

// a voice over the song (a replay's singing): mono, its first sample at song time voice_t0
static const int16_t * voice_pcm;
static int voice_n, voice_rate, voice_ch;
static double voice_t0;
static float voice_vol;

// what the speakers play, kept for the echo canceller (aec.c): mono, 16 kHz, cap_buf[0] at song time cap_t0
static int16_t * cap_buf;
static int cap_n;
static double cap_t0;
#ifndef __vita__
// tests (plat_sdl's speaker echo): everything played, 16 kHz mono by song time, the last 4 minutes (the disk driver
// the tests use mixes well ahead of the clock)
#define HEARD (1 << 22)
static int16_t heard[HEARD];

void audio_test_heard(double t, int16_t * out, int n)
{
    long k = (long)floor(t * 16000);
    for (int i = 0; i < n; i++, k++)
        out[i] = k >= 0 ? heard[k % HEARD] : 0;
}
#endif

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
    int16_t * cap = song_active && song_playing ? cap_buf : NULL;
    int cap_len = cap_n;
    double cap_at = ((double)song_frame / AUDIO_RATE - cap_t0) * 16000, cap_step = 16000.0 / AUDIO_RATE * song_speed;
    if (song_active && song_playing) {
        // the frames of the song this buffer takes, and where they go
        // any speed but 1 goes through the stretch: above 1 the buffer takes more of the song than it plays
        bool slow = song_speed != 1;
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
            float now = 0, next = 0;
            int got = 0;
            bool see = avail > LOOK_FRAMES + (uint32_t)take;     // (decoded that far ahead)
            for (int i = 0; i < take; i += 16) {
                const int16_t * smp = &st->ring[((r + i) % RING_FRAMES) * 2];
                float e = (float)smp[0] * smp[0] + (float)smp[1] * smp[1];
                st->power += e;
                now += e;
                if (see) {
                    const int16_t * a = &st->ring[((r + LOOK_FRAMES + i) % RING_FRAMES) * 2];
                    next += (float)a[0] * a[0] + (float)a[1] * a[1];
                }
                got++;
            }
            if (got) {
                st->level += (now / got - st->level) * 0.2f;
                st->ahead += ((see ? next : now) / got - st->ahead) * 0.2f;
            }
            if (st->bend > 0.0005f || st->bend_target > 0) {
                mix_bent(st, r, dst + offset * 2, take, v, dv);
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
        if (voice_pcm && voice_vol > 0 && n > 0) {
            double step = (double)voice_rate / AUDIO_RATE;
            double pos = ((double)(song_frame + offset) / AUDIO_RATE - voice_t0) * voice_rate;
            float v = voice_vol * music_vol * fade_vol;
            for (int i = 0; i < n; i++, pos += step) {
                int k = (int)pos;
                if (pos < 0 || k + 1 >= voice_n)
                    continue;
                float f = (float)(pos - k);
                if (voice_ch == 2) {
                    const int16_t * a = voice_pcm + k * 2;
                    dst[(offset + i) * 2] += (a[0] + (a[2] - a[0]) * f) * v;
                    dst[(offset + i) * 2 + 1] += (a[1] + (a[3] - a[1]) * f) * v;
                } else {
                    float smp = (voice_pcm[k] + (voice_pcm[k + 1] - voice_pcm[k]) * f) * v;
                    dst[(offset + i) * 2] += smp;
                    dst[(offset + i) * 2 + 1] += smp;
                }
            }
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
#ifndef __vita__
    if (song_active && song_playing) {
        // averaged like the capture (a mic filters what it can't keep; picking every third would alias)
        double at = (double)h->song_frame / AUDIO_RATE * 16000;
        static float hs;
        static int hn;
        static long hk = -1;
        for (int i = 0; i < frames; i++, at += 16000.0 / AUDIO_RATE * song_speed) {
            long k = (long)floor(at);
            if (k != hk) {
                if (hk >= 0 && hn)
                    heard[hk % HEARD] = (int16_t)(hs / hn);
                hs = 0, hn = 0, hk = k;
            }
            float l = acc[i * 2], r = acc[i * 2 + 1];
            l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
            r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
            hs += (l + r) * 0.5f;
            hn++;
        }
    }
#endif
    if (cap) {
        // each 16 kHz sample the mean of the output frames that fall in it (3 at normal speed), clipped as played;
        // under the lock: audio_capture(NULL) may free the buffer straight after
        static float sum;
        static int count, last = -1;
        static int16_t * last_buf;
        if (cap != last_buf)
            last_buf = cap, last = -1, sum = 0, count = 0;
        for (int i = 0; i < frames; i++, cap_at += cap_step) {
            int k = (int)floor(cap_at);
            if (k != last) {
                if (last >= 0 && last < cap_len && count)
                    cap[last] = (int16_t)(sum / count);
                sum = 0, count = 0, last = k;
            }
            float l = acc[i * 2], r = acc[i * 2 + 1];
            l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
            r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
            sum += (l + r) * 0.5f;
            count++;
        }
    }
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
    bend_init();
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
    anchor_jumps++;
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
        st->bend = st->bend_target = st->power = st->level = st->ahead = 0;
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
    anchor_jumps++;
    bender.st = NULL;                   // a seek's a new start, even if it lands where the last frame would have gone on
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
    song_speed = speed < 0.25f ? 0.25f : speed > MAX_SPEED ? (float)MAX_SPEED : speed;
    slow_pos = 0;
    memset(slowed, 0, 4 * sizeof(float));
    pthread_mutex_unlock(&mix_lock);
}

void audio_song_pause(bool pause)
{
    double now = audio_song_time();
    pthread_mutex_lock(&mix_lock);
    anchor_jumps++;
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

void audio_voice(const int16_t * pcm, int n, int channels, int rate, double t0, float volume)
{
    pthread_mutex_lock(&mix_lock);
    voice_ch = channels == 2 ? 2 : 1;
    voice_pcm = n > 1 && rate > 0 ? pcm : NULL;
    voice_n = n;
    voice_rate = rate;
    voice_t0 = t0;
    voice_vol = volume;
    pthread_mutex_unlock(&mix_lock);
}

void audio_capture(int16_t * buf, int n, double t0)
{
    pthread_mutex_lock(&mix_lock);
    cap_buf = buf;
    cap_n = n;
    cap_t0 = t0;
    pthread_mutex_unlock(&mix_lock);
}

void audio_voice_volume(float volume)
{
    voice_vol = volume;
}

double audio_song_clock(void)
{
    // audio_song_time for other threads: no smoothing, nothing kept between calls
    pthread_mutex_lock(&mix_lock);
    int64_t af = anchor_frame, au = anchor_us;
    bool playing = anchor_playing;
    pthread_mutex_unlock(&mix_lock);
    if (!playing)
        return -1e9;
    return af / (double)AUDIO_RATE + (plat_now_us() - au) / 1e6 * song_speed;
}

double audio_song_time(void)
{
    // The game asks every frame, and the mixer can hold this lock for all of a whammy frame: rather than wait, go on from what
    // it said a moment ago. That's a buffer old at most, and the anchor is a time as well as a frame, so it still holds. Only
    // after a seek or a pause does the wait matter
    static int64_t af, au;
    static bool playing, from_output;
    static int seen = -1;
    int jump = anchor_jumps;
    if (jump != seen ? pthread_mutex_lock(&mix_lock) == 0 : pthread_mutex_trylock(&mix_lock) == 0) {
        seen = jump;
        af = anchor_frame;
        au = anchor_us;
        playing = anchor_playing;
        from_output = anchor_from_output;
        pthread_mutex_unlock(&mix_lock);
    }
    double last = last_time;
    if (!playing)
        return last;
    double t = af / (double)AUDIO_RATE + (plat_now_us() - au) / 1e6 * song_speed;
    // the audio thread reports every buffer: past a few of them it has stalled, so the song has too
    if (from_output && t > af / (double)AUDIO_RATE + 0.08 * song_speed)
        t = af / (double)AUDIO_RATE + 0.08 * song_speed;
    // The anchor is when the audio thread last handed a buffer over, and the Vita's output takes them with a few ms of
    // wobble: notes would judder by it. So the clock runs on its own at the song's speed and leans a little toward what
    // the audio says each frame (a third of a second to settle); a seek, a stall or a jump over 40 ms resets it
    static double sm_t, sm_us;
    static int sm_jump = -1;
    double now_us = (double)plat_now_us();
    double pred = sm_t + (now_us - sm_us) / 1e6 * song_speed;
    if (sm_jump != jump || fabs(t - pred) > 0.04)
        pred = t;
    else
        pred += (t - pred) * 0.06;
    sm_jump = jump;
    sm_t = pred;
    sm_us = now_us;
    t = pred;
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

void audio_log_cost(void)
{
    if (bend_frames)
        plat_log("whammy: %d frames, avg %.0f us, slowest %d us; buffer %d us, underruns %d this song", bend_frames,
                 (double)bend_us / bend_frames, bend_worst, 512 * 1000000 / AUDIO_RATE, underruns - underruns_logged);
    underruns_logged = underruns;
    bend_us = bend_frames = bend_worst = 0;
}

void audio_stem_bend(int stem, float amount)
{
    if (stem >= 0 && stem < MAX_STEMS)
        streams[stem].bend_target = amount;
}

float audio_stems_share(const int * stems, int count)
{
    float mine = 0, all = 0;
    for (int i = 0; i < count; i++)
        if (stems[i] >= 0 && stems[i] < stream_count)
            mine += streams[stems[i]].power;
    for (int i = 0; i < stream_count; i++)
        all += streams[i].power;
    return all > 0 ? mine / all : 0;
}

float audio_stem_level(int stem, bool ahead)
{
    if (stem < 0 || stem >= stream_count)
        return 0;
    float ms = ahead ? streams[stem].ahead : streams[stem].level;
    return ms > 0 ? sqrtf(ms / 2) / 32768.0f : 0;
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
