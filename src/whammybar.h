// How far down the whammy bar is, from the right stick or a finger rubbing the rear touchpad. Only a hand that means it counts:
// a stick that doesn't come back to the middle, or the fingers holding the Vita, leave the bar up
#ifndef WHAMMYBAR_H
#define WHAMMYBAR_H
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define BAR_FINGERS 4
#define BAR_DEAD 0.12f                  // of the stick, round where it rests
#define BAR_REST_MAX 0.4f               // no stick rests further out than this: past it, it's being pushed
#define BAR_REST_SPEED 0.05f            // per frame, while nothing's ringing
#define BAR_STILL 0.01f                 // a stick moving less than this a frame is lying still
#define BAR_RUB 0.06f                   // the travel (a sliding sum of about 10 frames) that makes a touch a rub
#define BAR_RUB_HOLD 18                 // frames a rub lasts after the finger stops
#define BAR_RUB_GAIN 3                  // a third of the pad's height is the bar all the way down
#define BAR_JUMP 0.2f                   // no finger goes this far in a frame: the panel's lost one and found another

typedef struct {
    int id;
    bool on;
    float from[2], last[2], travel;
    int hold;
} BarFinger;

typedef struct {
    bool started;
    float rest[2], stick[2];
    BarFinger finger[BAR_FINGERS];
} Bar;

// fingers are this frame's rear touches. ringing: a sustain is held, so the stick's rest stays put. moved: the bar's being worked
static inline float bar_read(Bar * b, float rx, float ry, int n, const int * ids, const float * xs, const float * ys, bool ringing, float * moved)
{
    if (!b->started) {
        b->started = true;
        b->rest[0] = b->stick[0] = rx;
        b->rest[1] = b->stick[1] = ry;
        if (hypotf(rx, ry) > BAR_REST_MAX)
            b->rest[0] = b->rest[1] = 0;
    }
    float step = fabsf(rx - b->stick[0]) + fabsf(ry - b->stick[1]);
    b->stick[0] = rx;
    b->stick[1] = ry;
    float off = hypotf(rx - b->rest[0], ry - b->rest[1]);
    if (!ringing && step < BAR_STILL && hypotf(rx, ry) < BAR_REST_MAX) {
        b->rest[0] += (rx - b->rest[0]) * BAR_REST_SPEED;
        b->rest[1] += (ry - b->rest[1]) * BAR_REST_SPEED;
    }
    float bar = (off - BAR_DEAD) / 0.7f;
    bar = bar < 0 ? 0 : bar;
    float work = bar > 0 ? step : 0;

    bool seen[BAR_FINGERS] = {false};
    for (int i = 0; i < n; i++) {
        int s = -1;
        for (int k = 0; k < BAR_FINGERS && s < 0; k++)
            if (b->finger[k].on && b->finger[k].id == ids[i] && !seen[k])
                s = k;
        for (int k = 0; k < BAR_FINGERS && s < 0; k++)
            if (!b->finger[k].on && !seen[k]) {
                s = k;
                memset(&b->finger[k], 0, sizeof(b->finger[k]));
                b->finger[k].on = true;
                b->finger[k].id = ids[i];
                b->finger[k].from[0] = b->finger[k].last[0] = xs[i];
                b->finger[k].from[1] = b->finger[k].last[1] = ys[i];
            }
        if (s < 0)
            continue;
        seen[s] = true;
        BarFinger * f = &b->finger[s];
        float d = fabsf(xs[i] - f->last[0]) + fabsf(ys[i] - f->last[1]);
        if (d > BAR_JUMP) {
            f->from[0] = xs[i];
            f->from[1] = ys[i];
            f->travel = 0;
            f->hold = 0;
            d = 0;
        }
        f->last[0] = xs[i];
        f->last[1] = ys[i];
        f->travel = f->travel * 0.9f + d;
        if (f->travel > BAR_RUB)
            f->hold = BAR_RUB_HOLD;
        else if (f->hold)
            f->hold--;
        if (f->hold) {
            float rub = hypotf(xs[i] - f->from[0], ys[i] - f->from[1]) * BAR_RUB_GAIN;
            bar = rub > bar ? rub : bar;
            work += d;
        } else {                        // it isn't a rub, so it's the grip: wherever it's crept to is where a rub would start
            f->from[0] = xs[i];
            f->from[1] = ys[i];
        }
    }
    for (int k = 0; k < BAR_FINGERS; k++)
        if (!seen[k])
            b->finger[k].on = false;
    *moved = work;
    return bar > 1 ? 1 : bar;
}
#endif
