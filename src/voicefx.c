// The singing kept with a replay, mixed to sit in the song: put back in time with the chart, the rumble and the hiss
// filtered off, the gaps between lines quietened, gently tuned to the notes when it's close, evened out, brought to
// the level of a lead vocal and given a little room (a stereo reverb). Done once, when the replay loads.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "vocals.h"
#include "voicefx.h"

#define R VOICE_RATE
#define HOP 160                 // 10 ms
#define LAG 0.076               // a frame's pitch was heard this long before the frame (the mic's window and grain)

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} Biquad;

static void bq_set(Biquad * q, int type, float hz, float qv, float gain_db)
{
    float w = 2 * (float)M_PI * hz / R, c = cosf(w), s = sinf(w), al = s / (2 * qv), A = powf(10, gain_db / 40);
    float b0, b1, b2, a0, a1, a2;
    if (type == 0) {                            // high pass
        b0 = (1 + c) / 2, b1 = -(1 + c), b2 = (1 + c) / 2, a0 = 1 + al, a1 = -2 * c, a2 = 1 - al;
    } else if (type == 1) {                     // low pass
        b0 = (1 - c) / 2, b1 = 1 - c, b2 = (1 - c) / 2, a0 = 1 + al, a1 = -2 * c, a2 = 1 - al;
    } else {                                    // peak
        b0 = 1 + al * A, b1 = -2 * c, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * c, a2 = 1 - al / A;
    }
    *q = (Biquad){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0};
}

static inline float bq(Biquad * q, float x)
{
    x += 1e-18f;                // silence never decays into denormals (slow, on the Vita most of all)
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static int cmp_float(const void * a, const void * b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

// the pitch the mic heard for the singing at song time s (recorded time, before any shift), -1 none
static float heard_at(const RFrame * f, int nf, double s)
{
    double want = s + LAG;
    int lo = 0, hi = nf - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f[mid].t <= want) {
            at = mid;
            lo = mid + 1;
        } else
            hi = mid - 1;
    }
    if (at < 0 || !(f[at].flags & RF_SING) || want - f[at].t > 0.1)
        return -1;
    return f[at].pitch;
}

// --------------------------------------------------------------------------------------------- the reverb

#define NCOMB 4
#define NAP 2
typedef struct {
    float * buf;
    int len, at;
    float store;
} Line;

static const int comb_len[NCOMB] = {405, 463, 516, 565}, ap_len[NAP] = {202, 160};
enum { SPREAD = 11 };

static bool line_new(Line * l, int len)
{
    l->buf = calloc((size_t)len, sizeof(float));
    l->len = len;
    l->at = 0;
    l->store = 0;
    return l->buf != NULL;
}

static inline float comb(Line * l, float x, float fb, float damp)
{
    float y = l->buf[l->at];
    l->store = y * (1 - damp) + l->store * damp;
    l->buf[l->at] = x + l->store * fb + 1e-18f;
    if (++l->at == l->len)
        l->at = 0;
    return y;
}

static inline float allpass(Line * l, float x)
{
    float b = l->buf[l->at], y = b - x;
    l->buf[l->at] = x + b * 0.5f;
    if (++l->at == l->len)
        l->at = 0;
    return y;
}

static inline int16_t clip(float v)
{
    // soft above a knee, so a shout rounds off rather than cracks
    float a = fabsf(v), k = 22000;
    if (a > k)
        a = k + (a - k) / (1 + (a - k) / 10000);
    a = a > 32767 ? 32767 : a;
    return (int16_t)(v < 0 ? -a : a);
}

// -------------------------------------------------------------------------------------------------- the mix

// natural, studio, big hall, hard tune
static const struct {
    float tune, range, speed, wet, fb, damp;
    int predelay;
    const char * name;
} styles[VOICE_STYLES] = {
    {0.5f, 1.0f, 0.004f, 0.08f, 0.78f, 0.40f, 400, "Natural"},
    {0.8f, 1.5f, 0.004f, 0.16f, 0.80f, 0.35f, 400, "Studio"},
    {0.8f, 1.5f, 0.004f, 0.30f, 0.87f, 0.25f, 640, "Big hall"},
    {1.0f, 3.0f, 0.05f, 0.14f, 0.80f, 0.35f, 400, "Hard tune"},
};

const char * voicefx_name(int style)
{
    return styles[style < 0 || style >= VOICE_STYLES ? 1 : style].name;
}

int16_t * voicefx_mix(int16_t * x, int n, float t0, const RFrame * fr, int nf, int style, float * out_t0)
{
    style = style < 0 || style >= VOICE_STYLES ? 1 : style;
    uint64_t start = plat_now_us();
    int hops = n / HOP;
    if (hops < 100)
        return NULL;
    // ---- how loud each 10 ms is (after the rumble's gone), the room's own noise, what counts as singing
    float * env = malloc((size_t)hops * sizeof(float)), * sorted = malloc((size_t)hops * sizeof(float));
    if (!env || !sorted) {
        free(env);
        free(sorted);
        return NULL;
    }
    Biquad hp;
    bq_set(&hp, 0, 90, 0.707f, 0);
    for (int k = 0; k < hops; k++) {
        double sum = 0;
        for (int i = 0; i < HOP; i++) {
            float v = bq(&hp, x[k * HOP + i]);
            sum += v * v;
        }
        env[k] = sorted[k] = sqrtf((float)(sum / HOP));
    }
    qsort(sorted, (size_t)hops, sizeof(float), cmp_float);
    // above the quiet (the room), but never above a quarter of the loud parts (someone singing nearly all the way)
    float floor_ = sorted[hops / 6], loud90 = sorted[hops * 9 / 10];
    float thr = fminf(fmaxf(floor_ * 3, 120), fmaxf(loud90 * 0.25f, 60));
    // ---- in time: the shift (+-250 ms) that lines the singing up best with the chart's notes
    double best_e = 0;
    long best = -(1L << 40);
    for (int e10 = -25; e10 <= 25; e10++) {
        double e = e10 * 0.01;
        long score = 0;
        for (int k = 0; k < hops; k++) {
            if (env[k] <= thr)
                continue;
            int p = vox_target_at(t0 + (k + 0.5) * 0.01 - e, 0);
            score += p >= 0 ? 10 : -3;
        }
        // the guess (the mic's known delay) wins a tie
        score -= abs(e10) / 5;
        if (score > best) {
            best = score;
            best_e = e;
        }
    }
    double t0a = t0 - best_e;
    // the singing's usual loudness, where the compressor starts to work
    int voiced = 0;
    for (int k = 0; k < hops; k++)
        if (env[k] > thr)
            sorted[voiced++] = env[k];
    qsort(sorted, (size_t)voiced, sizeof(float), cmp_float);
    float typical = voiced ? sorted[voiced / 2] : 2000;

    // ---- pass one, in place: filters, tuning, the gate, the compressor
    Biquad lp, presence;
    bq_set(&hp, 0, 90, 0.707f, 0);
    bq_set(&lp, 1, 6800, 0.707f, 0);
    bq_set(&presence, 2, 3000, 1.0f, 3);
    enum { W = 512, RING = 1024 };
    float ring[RING] = {0};
    int w = 0;
    float d = 0, ratio = 1, follow = 0, gate = 0, det = 0, comp = 1;
    float c_thr = typical * 1.4f;
    const float att_f = expf(-1.0f / (0.002f * R)), rel_f = expf(-1.0f / (0.06f * R));
    const float att_g = expf(-1.0f / (0.004f * R)), rel_g = expf(-1.0f / (0.15f * R));
    const float det_k = expf(-1.0f / (0.015f * R));
    const float att_c = expf(-1.0f / (0.005f * R)), rel_c = expf(-1.0f / (0.12f * R));
    float r_target = 1, mask = 1, c_want = 1;
    static float win[W + 1];                // sin^2 across the window, the read heads' fades
    for (int k = 0; k <= W; k++) {
        float sn = sinf((float)M_PI * k / W);
        win[k] = sn * sn;
    }
    for (int i = 0; i < n; i++) {
        if (!(i & 63)) {
            // every 4 ms: the note meant then, the pitch heard, how far to pull it in
            double meant = t0a + (double)i / R, heard_t = t0 + (double)i / R;
            int note = vox_target_at(meant, 0);
            float p = heard_at(fr, nf, heard_t);
            float corr = 0;
            if (note > 0 && p > 0) {
                float diff = note - p;
                diff -= 12 * floorf(diff / 12 + 0.5f);      // an octave off is still the note
                if (fabsf(diff) <= styles[style].range)
                    corr = diff * styles[style].tune;
            }
            r_target = powf(2, corr / 12);
            mask = vox_target_at(meant, 0.2) >= 0 ? 1 : 0.3f;
        }
        float v = bq(&presence, bq(&lp, bq(&hp, x[i])));
        // the gate: open on the voice, down between lines
        float a = fabsf(v);
        follow = a > follow ? a + (follow - a) * att_f : a + (follow - a) * rel_f;
        float g_want = (follow > thr * 0.8f ? 1 : 0.1f) * mask;
        gate = g_want > gate ? g_want + (gate - g_want) * att_g : g_want + (gate - g_want) * rel_g;
        // the tuning: two read heads sweeping a short delay, crossfaded; back to a plain delay (W/2) when it's not
        // pulling, quickly when nothing's being sung
        ratio += (r_target - ratio) * styles[style].speed;
        float r = ratio;
        if (fabsf(r_target - 1) < 1e-4f) {
            float to = d < W / 4.0f ? 0 : d < 3 * W / 4.0f ? W / 2.0f : W;
            float nudge = gate < 0.2f ? 0.03f : 0.004f;
            r = 1 + (d > to ? nudge : d < to ? -nudge : 0);
            if (fabsf(d - to) < nudge)
                d = to, r = 1;
        }
        ring[w] = v;
        d += 1 - r;
        while (d >= W)
            d -= W;
        while (d < 0)
            d += W;
        float d2 = d + W / 2.0f;
        if (d2 >= W)
            d2 -= W;
        float out = 0;
        for (int h = 0; h < 2; h++) {
            float dl = h ? d2 : d, g2 = win[(int)(dl + 0.5f)];
            float pos = w - dl;
            int i0 = (int)floorf(pos);
            float f = pos - i0;
            float s0 = ring[i0 & (RING - 1)], s1 = ring[(i0 + 1) & (RING - 1)];
            out += (s0 + (s1 - s0) * f) * g2;
        }
        w = (w + 1) & (RING - 1);
        out *= gate;
        // the compressor: 3 to 1 above the singing's usual loudness
        det = out * out + (det - out * out) * det_k;
        if (!(i & 15)) {
            float lvl = sqrtf(det);
            c_want = lvl > c_thr ? powf(c_thr / lvl, 2.0f / 3) : 1;
        }
        comp = c_want < comp ? c_want + (comp - c_want) * att_c : c_want + (comp - c_want) * rel_c;
        out *= comp * 0.5f;                 // headroom till the level's set
        x[i] = (int16_t)(out > 32767 ? 32767 : out < -32768 ? -32768 : out);
    }
    // ---- the level of a lead vocal (about -19 dB RMS where singing), the output a plain W/2 later than the input
    voiced = 0;
    for (int k = 0; k < hops; k++) {
        double sum = 0;
        for (int i = 0; i < HOP; i++)
            sum += (double)x[k * HOP + i] * x[k * HOP + i];
        float rms = sqrtf((float)(sum / HOP));
        if (env[k] > thr && rms > 30)
            sorted[voiced++] = rms;
    }
    qsort(sorted, (size_t)voiced, sizeof(float), cmp_float);
    float loud = voiced > 10 ? sorted[voiced * 3 / 4] : 0;
    float makeup = loud > 0 ? 3600 / loud : 1;
    makeup = makeup < 0.5f ? 0.5f : makeup > 16 ? 16 : makeup;
    free(env);
    free(sorted);

    // ---- pass two: the level and a room, into stereo
    int16_t * st = malloc((size_t)n * 2 * sizeof(int16_t));
    Line cl[NCOMB] = {{0}}, cr[NCOMB] = {{0}}, al[NAP] = {{0}}, ar[NAP] = {{0}}, pre = {0};
    bool ok = st && line_new(&pre, styles[style].predelay);    // 25-40 ms before the room answers
    for (int k = 0; k < NCOMB; k++)
        ok &= line_new(&cl[k], comb_len[k]) & line_new(&cr[k], comb_len[k] + SPREAD);
    for (int k = 0; k < NAP; k++)
        ok &= line_new(&al[k], ap_len[k]) & line_new(&ar[k], ap_len[k] + SPREAD);
    if (ok) {
        const float fb = styles[style].fb, damp = styles[style].damp, wet = styles[style].wet;
        for (int i = 0; i < n; i++) {
            float dry = x[i] * makeup;
            float in = pre.buf[pre.at] * 0.25f;
            pre.buf[pre.at] = dry;
            if (++pre.at == pre.len)
                pre.at = 0;
            float l = 0, r = 0;
            for (int k = 0; k < NCOMB; k++) {
                l += comb(&cl[k], in, fb, damp);
                r += comb(&cr[k], in, fb, damp);
            }
            for (int k = 0; k < NAP; k++) {
                l = allpass(&al[k], l);
                r = allpass(&ar[k], r);
            }
            st[i * 2] = clip(dry + l * wet * 4);
            st[i * 2 + 1] = clip(dry + r * wet * 4);
        }
    }
    free(pre.buf);
    for (int k = 0; k < NCOMB; k++)
        free(cl[k].buf), free(cr[k].buf);
    for (int k = 0; k < NAP; k++)
        free(al[k].buf), free(ar[k].buf);
    if (!ok) {
        free(st);
        return NULL;
    }
    *out_t0 = (float)(t0a - (double)(W / 2) / R);
    plat_log("voicefx: %s, %d s mixed in %.2f s: moved %+d ms, %d%% sung, level x%.1f", styles[style].name, n / R,
             (plat_now_us() - start) / 1e6, (int)(best_e * 1000), hops ? voiced * 100 / hops : 0, makeup);
    return st;
}
