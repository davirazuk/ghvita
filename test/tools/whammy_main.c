// The whammy through the real mixer, with no game and no sound card. whammytest IN.raw OUT.raw SCHEDULE [BAR]
// IN and OUT are 48 kHz stereo s16. tools/whammy.py makes the stems and reads what comes out.
// Schedules: dry, hold, wobble, fast, bursts, pause, seek, mute, underrun, end, cost
#include "../../src/audio.c"
#include <time.h>

Decoder * dec_open(const char * p) { (void)p; return 0; }
int dec_read(Decoder * d, int16_t * o, int n) { (void)d; (void)o; (void)n; return 0; }
void dec_close(Decoder * d) { (void)d; }
bool dec_seek(Decoder * d, int64_t t) { (void)d; (void)t; return false; }
int64_t dec_length(const Decoder * d) { (void)d; return 0; }
int64_t plat_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000ll + t.tv_nsec / 1000; }
void plat_sleep_us(int us) { (void)us; }
void plat_thread(const char * n, int (*fn)(void *), void * a, int core) { (void)n; (void)fn; (void)a; (void)core; }
void plat_audio_start(void (*m)(int16_t *, int), void (*s)(int64_t, int64_t)) { (void)m; (void)s; }
void plat_log(const char * f, ...) { (void)f; }

#define GRAIN 512
static int16_t * pcm;
static int frames;
static uint32_t fed;                    // of pcm, what's gone into the ring

static void feed(Stream * st, uint32_t ahead)
{
    while (st->wpos - st->rpos < ahead && st->wpos - st->rpos < RING_FRAMES - 4096 && (int)fed < frames) {
        int w = st->wpos % RING_FRAMES;
        st->ring[w * 2] = pcm[fed * 2];
        st->ring[w * 2 + 1] = pcm[fed * 2 + 1];
        st->wpos++;
        fed++;
    }
    if ((int)fed >= frames)
        st->eof = true;
}

static float wave_bar(const char * s, double t, float full)
{
    if (!strcmp(s, "dry")) return 0;
    if (!strcmp(s, "hold") || !strcmp(s, "pause")) return full;
    if (!strcmp(s, "wobble")) return fmod(t, 3) < 2 ? 0.5f + 0.5f * sinf(t * 18) : 0;
    if (!strcmp(s, "fast")) return (long)(t * 60) & 1 ? 1 : 0;     // every 60 fps frame, end to end
    if (!strcmp(s, "zero")) return fabsf(sinf(t * 6));              // down and up, touching the top each time, as a wobble does
    if (!strcmp(s, "bursts")) return fmod(t, 0.4) < 0.1 ? 1 : 0;   // a bend hardly under way before it's let go
    return fmod(t, 3) < 2 ? 1 : 0;
}

int main(int argc, char ** argv)
{
    if (argc < 4)
        return 2;
    FILE * f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    frames = (int)(size / 4);
    pcm = malloc(size);
    if (fread(pcm, 1, size, f) != (size_t)size)
        return 2;
    fclose(f);
    const char * sched = argv[3];
    float full = argc > 4 ? (float)atof(argv[4]) : 1;
    bend_init();
    Stream * st = &streams[0];
    st->ring = calloc(RING_FRAMES * 2, sizeof(int16_t));
    st->open = true;
    st->vol = st->vol_target = 1;
    stream_count = 1;
    song_active = song_playing = true;
    fade_vol = fade_target = 1;
    music_vol = 1;
    song_speed = 1;
    FILE * o = fopen(argv[2], "wb");
    int16_t out[GRAIN * 2];
    int64_t worst = 0, total = 0;
    int buffers = 0, spent = 0;
    bool paused = false;
    for (int b = 0; (b + 1) * GRAIN < frames + GRAIN * 8; b++) {
        double t = b * GRAIN / 48000.0;
        bool stall = !strcmp(sched, "underrun") && fmod(t, 4) > 3 && fmod(t, 4) < 3.2;
        // the mixer's own clock doesn't stop: a pause just stops taking from the stems
        if (!strcmp(sched, "pause")) {
            bool now = fmod(t, 5) > 4;
            if (now != paused) {
                paused = now;
                song_playing = !now;
            }
        }
        if (!strcmp(sched, "seek") && b && b % 800 == 0) {   // practice looping back, mid-bend
            fed = (uint32_t)(b / 800 * 48000);
            st->wpos = st->rpos = st->skip = 0;
            st->eof = false;
            song_frame = ring_start = 0;
        }
        if (!strcmp(sched, "mute") && b % 200 == 100)         // a miss while bent, the next hit a moment later
            st->vol_target = 0;
        if (!strcmp(sched, "mute") && b % 200 == 140)
            st->vol_target = 1;
        if (!stall)
            feed(st, 4096 + GRAIN + 2048);
        st->bend_target = wave_bar(sched, t, full);
        int64_t t0 = plat_now_us();
        mix(out, GRAIN);
        int64_t dt = plat_now_us() - t0;
        if (st->bend > 0.0005f) {
            total += dt;
            worst = dt > worst ? dt : worst;
            spent++;
        }
        buffers++;
        for (int i = 0; i < GRAIN * 2; i++)
            if (out[i] != out[i])
                puts("NaN");
        if (!paused || strcmp(sched, "pause"))    // a pause's silence is left out: the rest should join up exactly
            fwrite(out, 4, GRAIN, o);
    }
    fclose(o);
    printf("%s: %d buffers, %d bent; bent buffer avg %.1f us, worst %lld us (a buffer is %.0f us); underruns %d; bender memory %zu bytes\n",
           sched, buffers, spent, spent ? (double)total / spent : 0, (long long)worst, GRAIN * 1e6 / 48000, underruns,
           sizeof(bender) + sizeof(bend_win) + sizeof(fft_cos) + sizeof(fft_sin) + sizeof(fft_rev));
    return 0;
}
