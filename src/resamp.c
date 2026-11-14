#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "resamp.h"

static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 30; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

static pthread_mutex_t kern_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int rate;
    float * coef;
} kerns[6];

static float * make_kernel(int rate)
{
    const int taps = RS_HALF * 2;
    float * coef = malloc((RS_PHASES + 1) * taps * sizeof(float));
    if (!coef)
        return NULL;
    // the passband ends a little under the lower of the two Nyquists; past 48 kHz in, the filter widens to match
    double cut = 0.5 * (rate > 48000 ? 48000.0 / rate : 1.0) * 0.91, beta = 8.0, norm = bessel_i0(beta);
    for (int p = 0; p <= RS_PHASES; p++) {
        double t = (double)p / RS_PHASES, sum = 0;
        float * row = coef + p * taps;
        for (int j = 0; j < taps; j++) {
            // tap j sits at frame j - (RS_HALF - 1), the point at t
            double x = j - (RS_HALF - 1) - t, w = x / RS_HALF;
            double win = fabs(w) >= 1 ? 0 : bessel_i0(beta * sqrt(1 - w * w)) / norm;
            double s = x == 0 ? 2 * cut : sin(2 * M_PI * cut * x) / (M_PI * x);
            row[j] = (float)(s * win);
            sum += row[j];
        }
        for (int j = 0; j < taps; j++)
            row[j] = (float)(row[j] / sum);
    }
    return coef;
}

int rs_kernel(RsKernel * k, int rate)
{
    pthread_mutex_lock(&kern_lock);
    k->coef = NULL;
    int free_slot = -1;
    for (int i = 0; i < 6 && !k->coef; i++) {
        if (kerns[i].rate == rate)
            k->coef = kerns[i].coef;
        else if (!kerns[i].rate && free_slot < 0)
            free_slot = i;
    }
    if (!k->coef) {
        float * c = make_kernel(rate);
        if (c && free_slot >= 0) {
            kerns[free_slot].rate = rate;
            kerns[free_slot].coef = c;
        }
        // all the slots taken by odd rates: this one is made and kept by no one (64 KB, once per such file)
        k->coef = c;
    }
    pthread_mutex_unlock(&kern_lock);
    return k->coef != NULL;
}

void rs_kernel_free(RsKernel * k)
{
    k->coef = NULL;
}

void rs_point(const RsKernel * k, const int16_t * win, float t, float out[2])
{
    const int taps = RS_HALF * 2;
    int p = (int)(t * RS_PHASES + 0.5f);
    if (p > RS_PHASES)
        p = RS_PHASES;
    const float * a = k->coef + p * taps;
    float l = 0, r = 0;
    for (int j = 0; j < taps; j++) {
        l += a[j] * win[j * 2];
        r += a[j] * win[j * 2 + 1];
    }
    out[0] = l;
    out[1] = r;
}

#define BUF_FRAMES (int)(sizeof(((Resamp *)0)->buf) / sizeof(int16_t) / 2)

int rs_open(Resamp * r, int rate)
{
    memset(r, 0, sizeof(*r));
    if (!rs_kernel(&r->k, rate))
        return 0;
    r->step = rate / 48000.0;
    // silence before the start, so the first point has its taps
    r->count = RS_HALF - 1;
    r->pos = RS_HALF - 1;
    return 1;
}

static void drain(Resamp * r, void (*emit)(void *, int, int), void * ctx, int64_t limit)
{
    for (;;) {
        int i = (int)r->pos;
        if (i + RS_HALF >= r->count || r->out_total >= limit)
            break;
        float v[2];
        rs_point(&r->k, r->buf + (i - (RS_HALF - 1)) * 2, (float)(r->pos - i), v);
        emit(ctx, (int)lrintf(v[0]), (int)lrintf(v[1]));
        r->out_total++;
        r->pos += r->step;
    }
    // keep what the next points still need
    int keep_from = (int)r->pos - (RS_HALF - 1);
    if (keep_from > 0) {
        memmove(r->buf, r->buf + keep_from * 2, (size_t)(r->count - keep_from) * 2 * sizeof(int16_t));
        r->count -= keep_from;
        r->pos -= keep_from;
    }
}

static int16_t c16(int v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void rs_push(Resamp * r, const int * lr, int frames, void (*emit)(void *, int, int), void * ctx)
{
    while (frames > 0) {
        int room = BUF_FRAMES - r->count, n = frames < room ? frames : room;
        for (int i = 0; i < n; i++) {
            r->buf[(r->count + i) * 2] = c16(lr[i * 2]);
            r->buf[(r->count + i) * 2 + 1] = c16(lr[i * 2 + 1]);
        }
        r->count += n;
        r->in_total += n;
        lr += n * 2;
        frames -= n;
        drain(r, emit, ctx, INT64_MAX);
    }
}

void rs_finish(Resamp * r, void (*emit)(void *, int, int), void * ctx)
{
    // as many out as the input's length at 48 kHz, the filter's tail fed silence
    int64_t want = (int64_t)ceil(r->in_total / r->step);
    for (int k = 0; k < 4 && r->out_total < want; k++) {
        int room = BUF_FRAMES - r->count, n = RS_HALF * 2 < room ? RS_HALF * 2 : room;
        memset(r->buf + r->count * 2, 0, (size_t)n * 2 * sizeof(int16_t));
        r->count += n;
        drain(r, emit, ctx, want);
    }
}

void rs_close(Resamp * r)
{
    rs_kernel_free(&r->k);
}
