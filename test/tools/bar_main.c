// the whammy bar's input on its own: what mustn't bend, and what must
#include <stdio.h>
#include <stdlib.h>
#include "../../src/whammybar.h"
static float noise(float a) { return a * ((rand() % 2001) / 1000.0f - 1); }
static int fails;
static void check(const char * what, float got, float lo, float hi)
{
    bool ok = got >= lo && got <= hi;
    fails += !ok;
    printf("%-4s %-52s %.3f (want %.2f..%.2f)\n", ok ? "ok" : "FAIL", what, got, lo, hi);
}
int main(void)
{
    float m, peak;
    int id[4] = {3, 7, 0, 0};
    float x[4], y[4];
    Bar b;
    // a stick resting well off the middle, jittering, through a sustain
    memset(&b, 0, sizeof(b));
    peak = 0;
    for (int i = 0; i < 600; i++) {
        float v = bar_read(&b, 0.28f + noise(0.012f), -0.1f + noise(0.012f), 0, 0, 0, 0, i > 120, &m);
        peak = v > peak ? v : peak;
    }
    check("drifted stick at rest, bar", peak, 0, 0);
    // the drift arrives during play, between sustains
    memset(&b, 0, sizeof(b));
    peak = 0;
    for (int i = 0; i < 600; i++) {
        float r = i < 60 ? 0 : (i - 60) * 0.0006f;
        float v = bar_read(&b, r > 0.3f ? 0.3f : r, 0, 0, 0, 0, 0, i % 200 > 150, &m);
        peak = v > peak ? v : peak;
    }
    check("stick settling off-centre slowly, bar", peak, 0, 0.02f);
    // pushed: all the way, and half way and held through a sustain
    memset(&b, 0, sizeof(b));
    for (int i = 0; i < 60; i++)
        bar_read(&b, 0.1f, 0, 0, 0, 0, 0, false, &m);
    float v = 0;
    for (int i = 0; i < 30; i++)
        v = bar_read(&b, 0.1f + 0.9f * (i + 1) / 30, 0, 0, 0, 0, 0, true, &m);
    check("stick pushed fully", v, 0.95f, 1);
    for (int i = 0; i < 300; i++)
        v = bar_read(&b, 0.1f + 0.47f, 0, 0, 0, 0, 0, true, &m);
    check("stick held half down for 5 s of sustain", v, 0.45f, 0.55f);
    // a held stick at the start of a song isn't taken for its rest
    memset(&b, 0, sizeof(b));
    v = bar_read(&b, 0.9f, 0, 0, 0, 0, 0, true, &m);
    check("stick already down on the first frame", v, 0.95f, 1);
    // grip: two fingers lying on the pad, jittering
    memset(&b, 0, sizeof(b));
    peak = 0;
    for (int i = 0; i < 600; i++) {
        x[0] = 0.2f + noise(0.002f); y[0] = 0.5f + noise(0.002f); x[1] = 0.8f + noise(0.002f); y[1] = 0.6f + noise(0.002f);
        float k = bar_read(&b, 0, 0, 2, id, x, y, true, &m);
        peak = k > peak ? k : peak;
    }
    check("two fingers gripping, bar", peak, 0, 0);
    // grip creeping across the pad as the hands shift
    memset(&b, 0, sizeof(b));
    peak = 0;
    for (int i = 0; i < 600; i++) {
        x[0] = 0.2f + i * 0.0008f; y[0] = 0.5f + i * 0.0003f;
        float k = bar_read(&b, 0, 0, 1, id, x, y, true, &m);
        peak = k > peak ? k : peak;
    }
    check("a grip finger creeping 0.5 of the pad in 10 s, bar", peak, 0, 0);
    // the panel swaps which finger comes first, and a lifted one's id comes back somewhere else
    memset(&b, 0, sizeof(b));
    peak = 0;
    for (int i = 0; i < 600; i++) {
        int swap = (i / 40) & 1, ids[2] = {swap ? 7 : 3, swap ? 3 : 7};
        x[0] = swap ? 0.8f : 0.2f; y[0] = swap ? 0.6f : 0.5f; x[1] = swap ? 0.2f : 0.8f; y[1] = swap ? 0.5f : 0.6f;
        float k = bar_read(&b, 0, 0, 2, ids, x, y, true, &m);
        peak = k > peak ? k : peak;
    }
    check("report order swapping between fingers, bar", peak, 0, 0);
    memset(&b, 0, sizeof(b));
    peak = 0;
    for (int i = 0; i < 600; i++) {
        x[0] = (i / 30) & 1 ? 0.9f : 0.1f; y[0] = 0.5f;               // same id, teleporting
        float k = bar_read(&b, 0, 0, 1, id, x, y, true, &m);
        peak = k > peak ? k : peak;
    }
    check("one id jumping across the pad, bar", peak, 0, 0);
    // a real rub, with a grip finger down as well
    memset(&b, 0, sizeof(b));
    peak = 0;
    float work = 0;
    for (int i = 0; i < 120; i++) {
        x[0] = 0.2f; y[0] = 0.5f; x[1] = 0.6f + 0.2f * sinf(i * 0.4f); y[1] = 0.5f;
        float k = bar_read(&b, 0, 0, 2, id, x, y, true, &m);
        peak = k > peak ? k : peak;
        work += m;
    }
    check("rubbing beside a grip finger, bar", peak, 0.5f, 1);
    check("  and it counts as working the bar", work, 1, 100);
    for (int i = 0; i < 60; i++)
        v = bar_read(&b, 0, 0, 2, id, x, y, true, &m);
    check("finger stopped for a second, bar", v, 0, 0);
    printf("%d failed\n", fails);
    return fails != 0;
}
