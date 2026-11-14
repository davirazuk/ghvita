// Any rate to 48 kHz with a windowed-sinc filter (Kaiser, 32 taps). The cheap ones it replaces (straight lines,
// Catmull-Rom) folded a 10 kHz cymbal back as a 13.9 kHz whistle only 23-28 dB under it: downloads came out gritty
#pragma once
#include <stdint.h>

#define RS_HALF 16                      // taps each side of the point
#define RS_PHASES 512                   // the filter is kept at this many points between two samples (the nearest is used)

typedef struct {
    const float * coef;                 // (RS_PHASES + 1) rows of RS_HALF * 2, shared by everything at one rate
} RsKernel;

// for a file at rate; false when there's no memory. The tables are made once per rate and kept
int rs_kernel(RsKernel * k, int rate);
void rs_kernel_free(RsKernel * k);
// one output frame at t (0..1) past frame [RS_HALF - 1] of the window: win holds RS_HALF * 2 frames, interleaved
// stereo, step frames apart
void rs_point(const RsKernel * k, const int16_t * win, float t, float out[2]);

// a stream of float stereo frames at rate in, 48 kHz out through emit
typedef struct {
    RsKernel k;
    double pos, step;
    int count;
    int64_t in_total, out_total, out_max;
    int16_t buf[4096 * 2];
} Resamp;

int rs_open(Resamp * r, int rate);
void rs_push(Resamp * r, const int * lr, int frames, void (*emit)(void * ctx, int l, int r), void * ctx);
// the end: the last of it out
void rs_finish(Resamp * r, void (*emit)(void * ctx, int l, int r), void * ctx);
void rs_close(Resamp * r);
