// The song taken back out of the singing. With the Vita's own speakers on, its mic hears the song as well as the
// singer; played back over the song in a replay that's a muddy second copy, and the replay's mix then turns it up. The
// mixer kept what it played (ref, the mic's rate and timeline), so: find how late the mic heard it, learn how the
// speakers and the case colour it (an adaptive filter, block frequency domain), take that away, and then hush what's
// left of it where it still outweighs the voice (a spectral gain). Done once, after the song, on a quiet thread.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "aec.h"
#include "plat.h"

typedef struct {
    float r, i;
} Cpx;

// ------------------------------------------------------------------------------------------------------ FFT

#define TW_N 8192
static Cpx tw[TW_N / 2 + 1];                 // e^(-2 pi i k / TW_N)

static void tw_init(void)
{
    static bool done;
    if (done)
        return;
    for (int k = 0; k <= TW_N / 2; k++)
        tw[k] = (Cpx){(float)cos(-2 * M_PI * k / TW_N), (float)sin(-2 * M_PI * k / TW_N)};
    done = true;
}

// complex, in place, n a power of two up to TW_N / 2; inverse unscaled
static void cfft(Cpx * a, int n, bool inv)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            Cpx t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = TW_N / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; k++) {
                Cpx w = tw[k * step];
                if (inv)
                    w.i = -w.i;
                Cpx * p = &a[i + k], * q = &a[i + k + half];
                float vr = q->r * w.r - q->i * w.i, vi = q->r * w.i + q->i * w.r;
                q->r = p->r - vr;
                q->i = p->i - vi;
                p->r += vr;
                p->i += vi;
            }
    }
}

// real input x[n] -> X[0..n/2] (n/2 + 1 bins); tmp holds n/2
static void rfft(const float * x, Cpx * X, int n, Cpx * tmp)
{
    int h = n / 2, step = TW_N / n;
    for (int k = 0; k < h; k++)
        tmp[k] = (Cpx){x[2 * k], x[2 * k + 1]};
    cfft(tmp, h, false);
    for (int k = 0; k <= h; k++) {
        Cpx a = tmp[k % h], b = tmp[(h - k) % h];
        b.i = -b.i;                                         // conj(Z[h-k])
        Cpx fe = {(a.r + b.r) / 2, (a.i + b.i) / 2};
        Cpx fo = {(a.i - b.i) / 2, -(a.r - b.r) / 2};       // (a - b) / 2i
        Cpx w = tw[k * step];
        X[k] = (Cpx){fe.r + w.r * fo.r - w.i * fo.i, fe.i + w.r * fo.i + w.i * fo.r};
    }
}

// X[0..n/2] -> real x[n], scaled (the true inverse)
static void irfft(const Cpx * X, float * x, int n, Cpx * tmp)
{
    int h = n / 2, step = TW_N / n;
    for (int k = 0; k < h; k++) {
        Cpx a = X[k], b = X[h - k];
        b.i = -b.i;
        Cpx fe = {(a.r + b.r) / 2, (a.i + b.i) / 2};
        Cpx d = {(a.r - b.r) / 2, (a.i - b.i) / 2};
        Cpx w = tw[k * step];
        Cpx fo = {d.r * w.r + d.i * w.i, d.i * w.r - d.r * w.i};   // d * conj(w)
        tmp[k] = (Cpx){fe.r - fo.i, fe.i + fo.r};                   // fe + i fo
    }
    cfft(tmp, h, true);
    for (int k = 0; k < h; k++) {
        x[2 * k] = tmp[k].r / h;
        x[2 * k + 1] = tmp[k].i / h;
    }
}

// ------------------------------------------------------------------------------------------ how late the mic is

#define DN 8192
#define MAX_LAG 4000                            // +-250 ms

// mic[i] hears ref[i - lag]; *sure: how far the peak stands out (under ~6: no song in the mic)
static int find_lag(const int16_t * mic, const int16_t * ref, int n, float * sure)
{
    float * x = malloc(DN * sizeof(float));
    Cpx * A = malloc((DN / 2 + 1) * sizeof(Cpx)), * B = malloc((DN / 2 + 1) * sizeof(Cpx));
    Cpx * S = calloc(DN / 2 + 1, sizeof(Cpx)), * tmp = malloc(DN / 2 * sizeof(Cpx));
    int lag = 0;
    *sure = 0;
    if (!x || !A || !B || !S || !tmp)
        goto out;
    int blocks = n / (DN / 2) - 1, every = blocks > 64 ? blocks / 64 : 1, used = 0;
    for (int b = 0; b < blocks; b += every) {
        const int16_t * m = mic + b * (DN / 2), * r = ref + b * (DN / 2);
        double er = 0;
        for (int i = 0; i < DN / 2; i++)
            er += (double)r[i] * r[i];
        if (er / (DN / 2) < 200.0 * 200)
            continue;                           // the song's quiet here
        for (int i = 0; i < DN; i++)
            x[i] = i < DN / 2 ? m[i] : 0;
        rfft(x, A, DN, tmp);
        for (int i = 0; i < DN; i++)
            x[i] = i < DN / 2 ? r[i] : 0;
        rfft(x, B, DN, tmp);
        for (int k = 1; k < DN / 2; k++) {
            float cr = A[k].r * B[k].r + A[k].i * B[k].i, ci = A[k].i * B[k].r - A[k].r * B[k].i;
            float mag = sqrtf(cr * cr + ci * ci) + 1e-9f;
            S[k].r += cr / mag;
            S[k].i += ci / mag;
        }
        used++;
    }
    if (used < 4)
        goto out;
    irfft(S, x, DN, tmp);
    float best = 0, sum = 0;
    for (int l = -MAX_LAG; l <= MAX_LAG; l++) {
        float v = fabsf(x[(l + DN) % DN]);
        sum += v;
        if (v > best)
            best = v, lag = l;
    }
    *sure = best / (sum / (2 * MAX_LAG + 1) + 1e-9f);
out:
    free(x), free(A), free(B), free(S), free(tmp);
    return lag;
}

// ------------------------------------------------------------------------------------------- the canceller

#define BL 256                          // a block: 16 ms
#define FN (2 * BL)
#define NB (BL + 1)                     // bins
#define PARTS 8                         // the filter: 128 ms of speaker, case and room
#define AHEAD 320                       // samples of the filter before the main path (the lag's never exact)

float aec_clean(int16_t * mic, const int16_t * ref, int n)
{
    tw_init();
    uint64_t start = plat_now_us();
    float sure;
    int lag = find_lag(mic, ref, n, &sure);
    if (sure < 8) {
        plat_log("aec: no song heard in the mic (%.1f), left as it was", sure);
        return 0;
    }
    int shift = lag - AHEAD;           // x[i] = ref[i - shift]
    Cpx * X = calloc((size_t)PARTS * NB, sizeof(Cpx)), * W = calloc((size_t)PARTS * NB, sizeof(Cpx));
    Cpx * F = malloc(NB * sizeof(Cpx)), * E = malloc(NB * sizeof(Cpx)), * tmp = malloc(BL * sizeof(Cpx));
    float * Px = calloc(NB, sizeof(float)), * Py = calloc(NB, sizeof(float)), * Pe = calloc(NB, sizeof(float));
    float * gain = malloc(NB * sizeof(float)), * mE = calloc(NB, sizeof(float)), * mY = calloc(NB, sizeof(float));
    double Pey = 0, Pyy = 0;
    float leak = 1;
    float * t = malloc(FN * sizeof(float)), * ewin = calloc(FN, sizeof(float)), * ola = calloc(FN, sizeof(float));
    float * win = malloc(FN * sizeof(float));
    float erle = 0;
    if (!X || !W || !F || !E || !tmp || !Px || !Py || !Pe || !gain || !mE || !mY || !t || !ewin || !ola || !win)
        goto out;
    for (int i = 0; i < FN; i++)
        win[i] = sinf((float)M_PI * (i + 0.5f) / FN);         // sqrt-Hann, both ways: 50% overlap adds to one
    for (int k = 0; k < NB; k++)
        gain[k] = 1;
    double e_in = 0, e_out = 0;                 // where the song's loud and the voice isn't: before and after
    // a first go over the start (20 s), only learning, so the filter knows the speakers from the first note
    int blocks = n / BL, warm = blocks < 1250 ? blocks : 1250;
    for (int step = 0; step < warm + blocks; step++) {
        int b = step < warm ? step : step - warm, i0 = b * BL;
        bool writing = step >= warm;
        if (step == warm) {
            memset(X, 0, (size_t)PARTS * NB * sizeof(Cpx));
            memset(ewin, 0, FN * sizeof(float));
            memset(ola, 0, FN * sizeof(float));
        }
        // the speakers' last two blocks, newest spectrum first
        memmove(X + NB, X, (PARTS - 1) * NB * sizeof(Cpx));
        for (int i = 0; i < FN; i++) {
            int j = i0 - BL + i - shift;
            t[i] = j >= 0 && j < n ? ref[j] : 0;
        }
        rfft(t, X, FN, tmp);
        // the echo the filter expects: the second half (overlap-save)
        memset(F, 0, NB * sizeof(Cpx));
        for (int p = 0; p < PARTS; p++) {
            const Cpx * x = X + p * NB, * w = W + p * NB;
            for (int k = 0; k < NB; k++) {
                F[k].r += x[k].r * w[k].r - x[k].i * w[k].i;
                F[k].i += x[k].r * w[k].i + x[k].i * w[k].r;
            }
        }
        for (int k = 0; k < NB; k++)
            Py[k] = 0.8f * Py[k] + 0.2f * (F[k].r * F[k].r + F[k].i * F[k].i);
        irfft(F, t, FN, tmp);
        float e[BL];
        double eb_d = 0, eb_e = 0, eb_y = 0;
        for (int i = 0; i < BL; i++) {
            float d = mic[i0 + i], y = t[BL + i];
            e[i] = d - y;
            eb_d += d * d, eb_e += e[i] * e[i], eb_y += y * y;
        }
        for (int i = 0; i < FN; i++)
            t[i] = i < BL ? 0 : e[i - BL];
        rfft(t, E, FN, tmp);
        // how much of the echo the filter expects is still in the error ("leak", as Speex has it): the error's
        // power going up and down with the echo's, over all bins. Learning goes at that share of the error: fast
        // while it's all song, slow once the error's mostly the singer (learning from the voice would teach the
        // filter the voice)
        double dey = 0, dyy = 0, sy = 0, se = 0;
        for (int k = 0; k < NB; k++) {
            float px = 0;
            for (int p = 0; p < PARTS; p++)
                px += X[p * NB + k].r * X[p * NB + k].r + X[p * NB + k].i * X[p * NB + k].i;
            Px[k] = 0.7f * Px[k] + 0.3f * px;
            float pe = E[k].r * E[k].r + E[k].i * E[k].i, py = F[k].r * F[k].r + F[k].i * F[k].i;
            Pe[k] = 0.8f * Pe[k] + 0.2f * pe;
            float de = pe - mE[k], dy = py - mY[k];
            mE[k] = 0.95f * mE[k] + 0.05f * pe;
            mY[k] = 0.95f * mY[k] + 0.05f * py;
            dey += (double)de * dy, dyy += (double)dy * dy;
            sy += py, se += pe;
        }
        Pey = 0.97 * Pey + 0.03 * dey;
        Pyy = 0.97 * Pyy + 0.03 * dyy;
        leak = Pyy > 0 && step > 30 ? (float)fmin(fmax(Pey / Pyy, 0.002), 1) : 1;
        float learn = fminf(0.5f, fmaxf(0.01f, 0.5f * leak * (float)(sy / (se + 1e3))));
        for (int p = 0; p < PARTS; p++) {
            Cpx * x = X + p * NB, * w = W + p * NB;
            for (int k = 0; k < NB; k++) {
                float s = learn / (Px[k] + FN * 1e4f);
                w[k].r += s * (x[k].r * E[k].r + x[k].i * E[k].i);       // conj(x) * E
                w[k].i += s * (x[k].r * E[k].i - x[k].i * E[k].r);
            }
        }
        // one part a block kept a true (linear, not circular) filter
        {
            Cpx * w = W + (b % PARTS) * NB;
            irfft(w, t, FN, tmp);
            for (int i = BL; i < FN; i++)
                t[i] = 0;
            rfft(t, w, FN, tmp);
        }
        // what's left of the song, hushed where it outweighs the voice: the echo that got away is the leak's share
        // of the echo the filter expects (a phone speaker's never quite linear), doubled to be sure
        for (int k = 0; k < NB; k++) {
            float g = 1 - 2.0f * leak * Py[k] / (Pe[k] + 1);
            g = g < 0.08f ? 0.08f : g;
            gain[k] = g < gain[k] ? g : 0.6f * gain[k] + 0.4f * g;           // down fast, up gently
        }
        memmove(ewin, ewin + BL, BL * sizeof(float));
        memcpy(ewin + BL, e, sizeof(e));
        for (int i = 0; i < FN; i++)
            t[i] = ewin[i] * win[i];
        rfft(t, F, FN, tmp);
        for (int k = 0; k < NB; k++)
            F[k].r *= gain[k], F[k].i *= gain[k];
        irfft(F, t, FN, tmp);
        for (int i = 0; i < FN; i++)
            ola[i] += t[i] * win[i];
        // a block out, a block behind: written where the mic's been read already
        double eb_o = 0;
        if (writing && b > 0)
            for (int i = 0; i < BL; i++) {
                float v = ola[i];
                eb_o += v * v;
                mic[i0 - BL + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        memmove(ola, ola + BL, BL * sizeof(float));
        memset(ola + BL, 0, BL * sizeof(float));
        if (writing && eb_y > 30 * eb_e) {                    // nearly all echo
            e_in += eb_d;
            e_out += eb_o;
        }
    }
    for (int i = (blocks - 1) * BL; i < n; i++)
        mic[i] = 0;
    erle = e_out > 0 ? 10 * log10f((float)(e_in / e_out)) : 0;
    plat_log("aec: the mic heard the song %+d ms late (%.0f), %.0f dB quieter, %.2f s", lag * 1000 / AEC_RATE, sure,
             erle, (plat_now_us() - start) / 1e6);
out:
    free(X), free(W), free(F), free(E), free(tmp), free(Px), free(Py), free(Pe), free(gain), free(mE), free(mY), free(t), free(ewin);
    free(ola), free(win);
    return erle;
}
