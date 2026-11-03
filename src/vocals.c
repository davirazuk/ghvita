#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "gfx.h"
#include "plat.h"
#include "res.h"
#include "vocals.h"

#define LYRIC 24.0f             // the size the lyric widths are measured at
#define WINDOW 1024             // 64 ms of 16 kHz: two periods of the lowest voice
#define GRAIN 512
#define RATE 16000
// the pitch is looked for at half that: a voice needs no more, and it's four times less work
#define PITCH_RATE 8000
#define TAU_MIN 7               // 1140 Hz
#define TAU_MAX 160             // 50 Hz

typedef struct {
    int t, len;                 // ms
    int pitch;                  // MIDI, 0: spoken, any voice counts
    bool hyphen;                // the next syllable joins this one
    bool perc;                  // no lyric at all: the line's clap, any noise plays it
    char text[28];              // with the space after it
    float w;                    // its width at LYRIC, measured once
} VNote;

typedef struct {
    int start, end;             // ms
    int first, count;           // notes
    int low, high;              // their pitches
    float w;                    // the lyric line's width at LYRIC
} VPhrase;

static struct {
    VNote * notes;
    VPhrase * phrases;
    int note_count, phrase_count;
    int cur;                    // the phrase being sung
    float sung, total;          // of it, seconds
    double last_t;
    float tolerance;
    float need;                 // how much of a phrase has to be in tune for full marks
    double base;
    float * marks;              // how each note went, 0..1
    float shown;                // the arrow's pitch on screen
    volatile float mic_pitch;   // MIDI, -1 nothing sung
    volatile float mic_level;   // how loud the room is, for the claps
    volatile int target;        // autoplay's: what its voice sings
    volatile bool run, running, mic_ok, autoplay;
    bool sung_now;              // a note is being sung right, this frame (singing starts Star Power with it)
} V;

float vox_pitch(const short * in, int n)
{
    static float d[TAU_MAX + 2];
    static short half[WINDOW / 2];
    n = n / 2 > WINDOW / 2 ? WINDOW / 2 : n / 2;
    double energy = 0;
    for (int i = 0; i < n; i++) {
        half[i] = (short)((in[i * 2] + in[i * 2 + 1]) / 2);
        energy += (double)half[i] * half[i];
    }
    const short * s = half;
    int w = n - TAU_MAX - 1;
    V.mic_level = sqrtf(energy / n);
    if (w < 64 || V.mic_level < 160)
        return -1;
    // YIN: the difference of the signal with itself, normalised by its running mean
    float sum = 0, best = 0;
    int tau = -1;
    d[0] = 1;
    for (int t = 1; t <= TAU_MAX; t++) {
        int64_t diff = 0;
        for (int i = 0; i < w; i++) {
            int x = (s[i] - s[i + t]) >> 2;
            diff += x * x;
        }
        sum += (float)diff;
        d[t] = sum > 0 ? (float)diff * t / sum : 1;
    }
    for (int t = TAU_MIN; t < TAU_MAX; t++)
        if (d[t] < 0.18f) {
            while (t + 1 < TAU_MAX && d[t + 1] < d[t])
                t++;
            tau = t;
            break;
        }
    if (tau < 0)
        return -1;
    float a = d[tau - 1], b = d[tau], c = d[tau + 1], den = a - 2 * b + c;
    best = tau + (den > 0 ? (a - c) / (2 * den) : 0);
    return 69 + 12 * log2f((float)PITCH_RATE / best / 440);
}

static int mic_thread(void * arg)
{
    (void)arg;
    static short win[WINDOW];
    short grain[GRAIN];
    float phase = 0;
    int port = -1, runs = 0, worst_us = 0;
    int64_t total_us = 0;
    bool tried = false;
    while (V.run) {
        if (!V.autoplay && !tried) {
            tried = true;
            port = plat_mic_open(GRAIN, RATE);
            V.mic_ok = port >= 0;
            if (port < 0)
                plat_log("vocals: no microphone");
        }
        if (V.autoplay) {
            // a voice: the note with its overtones
            float hz = V.target > 0 ? 440 * powf(2, (V.target - 69) / 12.0f) : 0;
            for (int i = 0; i < GRAIN; i++) {
                phase += hz / RATE;
                phase -= floorf(phase);
                float p = phase * 6.2831853f;
                grain[i] = hz > 0 ? (short)(9000 * sinf(p) + 3500 * sinf(2 * p) + 1500 * sinf(3 * p)) : 0;
            }
            plat_sleep_us(GRAIN * 1000000 / RATE);
        } else if (port >= 0)
            plat_mic_read(port, grain);
        else {
            plat_sleep_us(50000);
            continue;
        }
        memmove(win, win + GRAIN, (WINDOW - GRAIN) * sizeof(short));
        memcpy(win + WINDOW - GRAIN, grain, sizeof(grain));
        int64_t t0 = plat_now_us();
        V.mic_pitch = vox_pitch(win, WINDOW);
        int us = (int)(plat_now_us() - t0);
        total_us += us;
        worst_us = us > worst_us ? us : worst_us;
        runs++;
    }
    if (runs)
        plat_log("vocals: pitch avg %.2f ms, slowest %.2f ms of 32 ms, %d runs", total_us / 1000.0 / runs,
                 worst_us / 1000.0, runs);
    if (port >= 0)
        plat_mic_close(port);
    V.mic_pitch = -1;
    V.running = false;
    return 0;
}

static void mic_stop(void)
{
    V.run = false;
    while (V.running)
        plat_sleep_us(2000);
}

bool vox_exists(const char * folder)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/vocals.vhv", folder);
    FILE * f = fopen(path, "r");
    if (f)
        fclose(f);
    return f != NULL;
}

bool vox_load(const char * folder)
{
    vox_free();
    char path[400], line[160];
    snprintf(path, sizeof(path), "%s/vocals.vhv", folder);
    FILE * f = fopen(path, "r");
    if (!f)
        return false;
    int nc = 0, pc = 0;
    while (fgets(line, sizeof(line), f)) {
        nc += line[0] == 'N';
        pc += line[0] == 'P';
    }
    rewind(f);
    V.notes = calloc(nc + 1, sizeof(VNote));
    V.marks = calloc(nc + 1, sizeof(float));
    V.phrases = calloc(pc + nc + 1, sizeof(VPhrase));
    while (V.notes && V.phrases && V.marks && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        int a, b, c, used = 0;
        if (line[0] == 'N' && sscanf(line + 1, "%d %d %d%n", &a, &b, &c, &used) == 3) {
            VNote * n = &V.notes[V.note_count++];
            n->t = a;
            n->len = b;
            n->pitch = c;
            const char * s = line + 1 + used;
            snprintf(n->text, sizeof(n->text), "%s", *s == ' ' ? s + 1 : s);
            size_t l = strlen(n->text);
            // the charts' marks: # spoken, ^ spoken shortly, - joins the next, = a hyphen that shows, + slides
            while (l && strchr("#^$", n->text[l - 1])) {
                n->text[--l] = 0;
                n->pitch = 0;
            }
            if (l && n->text[l - 1] == '-') {
                n->text[--l] = 0;
                n->hyphen = true;
            } else if (l && n->text[l - 1] == '=') {
                n->text[l - 1] = '-';
                n->hyphen = true;
            }
            if (!strcmp(n->text, "+"))
                n->text[0] = 0;
            n->perc = !n->text[0] && !n->pitch;
            if (n->text[0] && !n->hyphen)
                strncat(n->text, " ", sizeof(n->text) - strlen(n->text) - 1);
            n->w = font_width(res.ui, n->text, LYRIC);
        } else if (line[0] == 'P' && sscanf(line + 1, "%d %d", &a, &b) == 2) {
            V.phrases[V.phrase_count].start = a;
            V.phrases[V.phrase_count++].end = b;
        }
    }
    fclose(f);
    if (!V.note_count || !V.notes || !V.phrases || !V.marks) {
        vox_free();
        return false;
    }
    // charts without phrases: a rest of over a second ends one
    if (!V.phrase_count)
        for (int i = 0; i < V.note_count; i++)
            if (!i || V.notes[i].t - (V.notes[i - 1].t + V.notes[i - 1].len) > 1000) {
                V.phrases[V.phrase_count].start = V.notes[i].t;
                V.phrases[V.phrase_count].end = V.notes[i].t;
                V.phrase_count++;
            }
    int k = 0, kept = 0;
    V.base = 0;
    for (int p = 0; p < V.phrase_count; p++) {
        VPhrase ph = V.phrases[p];
        int end = p + 1 < V.phrase_count ? V.phrases[p + 1].start : 0x7fffffff;
        if (ph.end <= ph.start)
            ph.end = end;
        ph.first = k;
        ph.count = 0;
        ph.low = 127;
        ph.high = 0;
        while (k < V.note_count && V.notes[k].t < ph.end) {
            const VNote * n = &V.notes[k++];
            ph.count++;
            ph.w += n->w;
            V.base += n->len / 1000.0 * 500;
            if (n->pitch && n->pitch < ph.low)
                ph.low = n->pitch;
            if (n->pitch > ph.high)
                ph.high = n->pitch;
        }
        if (ph.count) {
            ph.end = V.notes[ph.first + ph.count - 1].t + V.notes[ph.first + ph.count - 1].len;
            V.phrases[kept++] = ph;
        }
    }
    V.phrase_count = kept;
    plat_log("vocals: %d notes, %d phrases", V.note_count, kept);
    return kept > 0;
}

#define WAKE 40
static float wake_y[WAKE];
static bool wake_hit[WAKE];

void vox_free(void)
{
    mic_stop();
    free(V.notes);
    free(V.phrases);
    free(V.marks);
    memset(&V, 0, sizeof(V));
}

void vox_start(int diff, bool autoplay)
{
    for (int i = 0; i < WAKE; i++)
        wake_y[i] = -1;
    static const float tolerance[4] = {3.0f, 2.5f, 1.75f, 1.0f};
    // YARG's: low, but a voice always has breaths and consonants the mic can't place
    static const float need[4] = {0.325f, 0.4f, 0.45f, 0.575f};
    mic_stop();
    V.cur = 0;
    V.sung = V.total = 0;
    V.last_t = -100;
    V.tolerance = tolerance[diff];
    V.need = need[diff];
    V.autoplay = autoplay;
    V.mic_pitch = -1;
    V.mic_ok = true;
    V.target = 0;
    memset(V.marks, 0, V.note_count * sizeof(float));
    V.run = V.running = true;
    plat_thread("mic", mic_thread, NULL, PLAT_BACKGROUND);       // never in the way of the game or the audio
}

void vox_seek(double t)
{
    int ms = (int)(t * 1000);
    while (V.cur < V.phrase_count && V.phrases[V.cur].end < ms)
        V.cur++;
}

void vox_autoplay(bool on)
{
    V.autoplay = on;
}

double vox_end(void)
{
    const VNote * n = &V.notes[V.note_count - 1];
    return (n->t + n->len) / 1000.0;
}

double vox_base_score(void)
{
    return V.base;
}

bool vox_singing(double t)
{
    if (!V.note_count)
        return t > 0;
    int ms = (int)(t * 1000), lo = 0, hi = V.note_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (V.notes[mid].t > ms)
            hi = mid;
        else
            lo = mid + 1;
    }
    // on through a breath between lines
    if (lo && ms < V.notes[lo - 1].t + V.notes[lo - 1].len + 900)
        return true;
    return lo < V.note_count && V.notes[lo].t - ms < 200;
}

int vox_phrases(void)
{
    return V.phrase_count;
}

bool vox_mic_ok(void)
{
    return V.mic_ok;
}

bool vox_sung_now(void)
{
    return V.sung_now;
}

// how far the voice is from the note in semitones: any octave counts
static float off_pitch(float sung, int pitch)
{
    float d = fmodf(sung - pitch, 12);
    if (d > 6)
        d -= 12;
    if (d < -6)
        d += 12;
    return d;
}

bool vox_update(double t, float * grade, float * points)
{
    double dt = V.last_t < -50 ? 0 : t - V.last_t;
    V.last_t = t;
    if (dt < 0 || dt > 0.25)
        dt = 0;
    V.sung_now = false;
    // the voice heard now began a window ago
    int ms = (int)(t * 1000) - 70;
    if (V.cur >= V.phrase_count) {
        V.target = 0;
        return false;
    }
    VPhrase * ph = &V.phrases[V.cur];
    float voice = V.mic_pitch;
    int want = 0;
    for (int i = ph->first; i < ph->first + ph->count; i++) {
        const VNote * n = &V.notes[i];
        if (ms < n->t || ms >= n->t + n->len)
            continue;
        want = n->pitch ? n->pitch : 60;
        V.total += (float)dt;
        // a clap is any noise at all, a spoken line any voice, the rest the note in tune
        bool ok;
        if (n->perc)
            ok = voice >= 0 || V.mic_level > 600;
        else if (!n->pitch)
            ok = voice >= 0;
        else
            ok = voice >= 0 && fabsf(off_pitch(voice, n->pitch)) <= V.tolerance;
        if (ok) {
            V.sung += (float)dt;
            V.marks[i] += (float)dt / (n->len / 1000.0f);
            V.sung_now = true;
        }
    }
    V.target = want;
    if (ms <= ph->end + 40)
        return false;
    float got = V.total > 0 ? V.sung / V.total / V.need : 0;
    *grade = got > 1 ? 1 : got;
    *points = (got >= 1 ? V.total : V.sung) * 500;
    V.sung = V.total = 0;
    V.cur++;
    return true;
}

// ---------------------------------------------------------------------------------------------------------- the lane

// a strip over the highway's far end: the notes, then the lyric lines
#define LANE_Y 4
#define LANE_H 64
#define NOW_X 96.0f
#define PX_PER_S 150.0f

static float pitch_y(float pitch, const VPhrase * ph)
{
    float mid = (ph->low + ph->high) / 2.0f, span = ph->high - ph->low + 4.0f;
    if (span < 12)
        span = 12;
    return LANE_Y + LANE_H / 2.0f - (pitch - mid) / span * (LANE_H - 18);
}

static void draw_lyrics(const VPhrase * ph, int ms, float y, float size, bool live)
{
    float k = size / LYRIC;
    if (ph->w * k > SCREEN_W - 32)
        k = (SCREEN_W - 32) / ph->w;
    float x = (SCREEN_W - ph->w * k) / 2;
    size = LYRIC * k;
    for (int i = ph->first; i < ph->first + ph->count; i++) {
        const VNote * n = &V.notes[i];
        uint32_t c = RGBA(165, 165, 180, 255);
        if (live) {
            c = RGBA(245, 245, 245, 255);
            if (ms >= n->t)
                c = V.marks[i] > 0.5f ? RGBA(120, 240, 170, 255) : RGBA(255, 200, 80, 255);
        }
        if (n->text[0])
            font_draw_shadow(res.ui, n->text, x, y, size, c, ALIGN_LEFT);
        x += n->w * k;
    }
}

static void draw_note(const VNote * n, int i, int ms, const VPhrase * scale)
{
    float x = NOW_X + (n->t - ms) / 1000.0f * PX_PER_S, w = n->len / 1000.0f * PX_PER_S;
    if (x > SCREEN_W || x + w < 0)
        return;
    w = w < 4 ? 4 : w;
    if (n->perc) {
        // a clap: a burst at the lane's centre that swells as it comes, green once any of it landed
        float cx = x + w / 2, cy = LANE_Y + LANE_H / 2.0f;
        float s = 7 + 3 * (0.5f + 0.5f * sinf(ms * 0.02f));
        uint32_t c = V.marks[i] > 0.02f ? RGBA(120, 240, 170, 255) : RGBA(235, 245, 255, 255);
        gfx_rect(cx - s, cy - 2, s * 2, 4, c);
        gfx_rect(cx - 2, cy - s, 4, s * 2, c);
        return;
    }
    float y = n->pitch ? pitch_y((float)n->pitch, scale) : LANE_Y + LANE_H / 2.0f;
    uint32_t top = n->pitch ? RGBA(150, 240, 245, 245) : RGBA(205, 205, 215, 190);
    uint32_t low = n->pitch ? RGBA(20, 130, 150, 245) : RGBA(110, 110, 125, 190);
    // a tube, its ends rounded; the one you're on burns brighter
    const int h = 5;
    static const float inset[5] = {3, 1.5f, 0.6f, 0, 0};
    for (int k = 0; k < h; k++) {
        float in = inset[k] < w / 2 ? inset[k] : w / 2 - 1;
        uint32_t c = k < 2 ? top : low;
        gfx_rect(x + in, y - h + k, w - in * 2, 1, c);
        gfx_rect(x + in, y + h - 1 - k, w - in * 2, 1, low);
    }
    if (ms >= n->t && ms < n->t + n->len) {
        gfx_blend(BLEND_ADD);
        gfx_rect(x, y - h - 2, w, h * 2 + 4, RGBA(70, 190, 200, 70));
        gfx_blend(BLEND_ALPHA);
    }
    // what's been sung of it lights up
    if (ms > n->t && V.marks[i] > 0.02f) {
        float f = (ms - n->t) / (float)n->len;
        f = f > 1 ? 1 : f;
        bool good = V.marks[i] / (f > 0.05f ? f : 1) > 0.55f;
        gfx_rect_grad(x + 1, y - 5, (w - 2) * f, 10, good ? RGBA(180, 255, 190, 250) : RGBA(255, 225, 130, 245),
                      good ? RGBA(40, 175, 90, 250) : RGBA(205, 130, 30, 245));
    }
}

// the words along the top while you play something else: only around a line, with the next one under it. Nothing to do
// with the mic
void vox_lyrics(double t)
{
    if (!V.phrase_count || !V.notes)
        return;
    int ms = (int)(t * 1000), cur = -1;
    for (int p = 0; p < V.phrase_count; p++)
        if (ms < V.phrases[p].end + 500) {
            cur = p;
            break;
        }
    if (cur < 0 || ms < V.phrases[cur].start - 1500)
        return;
    gfx_blend(BLEND_ALPHA);
    gfx_rect_grad(0, 0, SCREEN_W, 60, RGBA(0, 0, 0, 190), RGBA(0, 0, 0, 0));
    draw_lyrics(&V.phrases[cur], ms, 6, LYRIC, true);
    if (cur + 1 < V.phrase_count && V.phrases[cur + 1].start - V.phrases[cur].end < 1500)
        draw_lyrics(&V.phrases[cur + 1], -1, 32, 15, false);
}

void vox_draw(double t)
{
    if (!V.phrase_count)
        return;
    int ms = (int)(t * 1000);
    // the strip stays through the quiet: an instrumental break doesn't take the highway away, the next phrase
    // is always on its way along it
    gfx_blend(BLEND_ALPHA);
    // World Tour's strip: dark glass with a teal hairline, the words on a darker band under it
    gfx_rect_grad(0, LANE_Y, SCREEN_W, LANE_H, RGBA(6, 16, 24, 190), RGBA(4, 10, 16, 170));
    gfx_rect_grad(0, LANE_Y + LANE_H, SCREEN_W, 56, RGBA(0, 0, 0, 200), RGBA(0, 0, 0, 0));
    gfx_rect(0, LANE_Y + LANE_H - 2, SCREEN_W, 2, RGBA(70, 200, 210, 170));

    int cur = V.cur < V.phrase_count ? V.cur : V.phrase_count - 1;
    const VPhrase * scale = &V.phrases[cur];        // the scale holds still inside a phrase
    for (int p = cur > 0 ? cur - 1 : 0; p < V.phrase_count && p < cur + 3; p++) {
        const VPhrase * ph = &V.phrases[p];
        for (int i = ph->first; i < ph->first + ph->count; i++)
            draw_note(&V.notes[i], i, ms, scale);
    }
    // where each line begins
    for (int p = cur > 0 ? cur - 1 : 0; p < V.phrase_count && p < cur + 3; p++) {
        float x = NOW_X + (V.phrases[p].start - ms) / 1000.0f * PX_PER_S;
        if (x > 0 && x < SCREEN_W)
            gfx_rect(x, LANE_Y + 1, 1, LANE_H - 3, RGBA(120, 210, 220, 55));
    }
    gfx_rect(NOW_X - 3, LANE_Y + 1, 7, LANE_H - 2, RGBA(255, 255, 255, 36));
    gfx_rect(NOW_X - 1, LANE_Y + 1, 2, LANE_H - 2, RGBA(255, 255, 255, 230));

    // the voice, a point on the now line: folded to the octave of the note being asked for
    float voice = V.mic_pitch;
    bool hit = false;
    float voice_y = -1;
    if (voice >= 0) {
        // what the mic just heard was sung a detection window ago: the matching runs 70 ms behind, and so does the
        // arrow, or it would trail the note it's on
        int want = V.target ? V.target : (V.shown > 0 ? (int)(V.shown + 0.5f) : (scale->low + scale->high) / 2);
        float shown = want + off_pitch(voice, want);
        V.shown = V.shown > 0 ? V.shown + (shown - V.shown) * 0.4f : shown;
        voice_y = pitch_y(V.shown, scale);
        hit = V.target && fabsf(off_pitch(voice, V.target)) <= V.tolerance;
    } else
        V.shown = 0;
    float voice_x = NOW_X - 70.0f * PX_PER_S / 1000.0f;
    // the voice leaves a wake running back from the now line, green where it was on the note
    memmove(wake_y + 1, wake_y, sizeof(wake_y) - sizeof(wake_y[0]));
    memmove(wake_hit + 1, wake_hit, sizeof(wake_hit) - sizeof(wake_hit[0]));
    wake_y[0] = voice_y;
    wake_hit[0] = hit;
    for (int i = 1; i < WAKE; i++)
        if (wake_y[i] >= 0) {
            float a = 1 - (float)i / WAKE;
            uint32_t c = wake_hit[i] ? RGBA(140, 255, 160, 255) : RGBA(255, 130, 90, 255);
            gfx_rect(voice_x - i * 3.0f - 3, wake_y[i] - 2 * a - 1, 3, 4 * a + 2, (c & 0xFFFFFF) | ((uint32_t)(a * 170) << 24));
        }
    if (voice_y >= 0) {
        uint32_t c = hit ? RGBA(140, 255, 160, 255) : RGBA(255, 130, 90, 255);
        for (int i = 0; i < 6; i++)
            gfx_rect(voice_x - 18 + i * 3, voice_y - (6 - i), 3, (6 - i) * 2, c);
    }

    if (V.cur < V.phrase_count) {
        draw_lyrics(&V.phrases[cur], ms, LANE_Y + LANE_H + 1, LYRIC, true);
        if (cur + 1 < V.phrase_count)
            draw_lyrics(&V.phrases[cur + 1], -1, LANE_Y + LANE_H + 27, 15, false);
    }
    if (!V.mic_ok)
        font_draw_shadow(res.ui, "No microphone", SCREEN_W - 12.0f, LANE_Y + 6, 16, RGBA(255, 130, 100, 255), ALIGN_RIGHT);
}
