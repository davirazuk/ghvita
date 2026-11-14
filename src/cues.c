// A chart's light show for songs converted before notes.vhc carried it: the lighting events read straight from the
// song's own notes.mid (its VENUE, EVENTS and LIGHTING tracks) or notes.chart ([Events]), timed by the converted
// chart's tempo map. Names as chart.c keeps them: "strobe fast", "verse", "bonusfx", "next" ...
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cues.h"

typedef struct {
    Section * v;
    int n, cap;
} Cues;

static void * slurp(const char * path, long * size)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char * d = n > 0 && n < 64 * 1024 * 1024 ? malloc(n + 1) : NULL;
    if (d && fread(d, 1, n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    if (d)
        d[n] = 0;
    *size = n;
    return d;
}

// a tick of the source chart in microseconds, by the converted chart's tempo points (time and quarter length)
static double tick_us(const Chart * c, double res, long tick)
{
    double t0 = 0, us = 500000, tk = 0;
    if (c->tempo_count) {
        t0 = c->tempo[0].time;
        us = c->tempo[0].us_per_quarter;
    }
    for (int i = 1; i < c->tempo_count; i++) {
        double next = tk + (c->tempo[i].time - t0) * res / us;
        if (next > tick)
            break;
        tk = next;
        t0 = c->tempo[i].time;
        us = c->tempo[i].us_per_quarter;
    }
    return t0 + (tick - tk) * us / res;
}

// "[lighting (strobe_fast)]" as "strobe fast"; false: not a cue
bool cue_name(const char * t, char * out, size_t size)
{
    while (*t == '[' || *t == '"' || isspace((unsigned char)*t))
        t++;
    char buf[64];
    size_t n = 0;
    for (; *t && *t != ']' && *t != '"' && n + 1 < sizeof(buf); t++)
        buf[n++] = *t;
    while (n && isspace((unsigned char)buf[n - 1]))
        n--;
    buf[n] = 0;
    const char * body = NULL;
    if (strncmp(buf, "lighting", 8) == 0) {
        char * a = strchr(buf, '('), * b = strrchr(buf, ')');
        if (!a || !b || b < a)
            return false;
        *b = 0;
        body = a[1] ? a + 1 : "default";
    } else if (!strcmp(buf, "bonusfx") || !strcmp(buf, "bonusfx_optional"))
        body = "bonusfx";
    else if (!strcmp(buf, "next") || !strcmp(buf, "prev") || !strcmp(buf, "first"))
        body = buf;
    if (!body)
        return false;
    snprintf(out, size, "%s", body);
    for (char * q = out; *q; q++)
        if (*q == '_')
            *q = ' ';
    return true;
}

static void add(Cues * q, const Chart * c, double res, long tick, const char * text)
{
    char name[60];
    if (!cue_name(text, name, sizeof(name)))
        return;
    if (q->n == q->cap) {
        int cap = q->cap ? q->cap * 2 : 64;
        Section * v = realloc(q->v, cap * sizeof(Section));
        if (!v)
            return;
        q->v = v;
        q->cap = cap;
    }
    Section * s = &q->v[q->n++];
    memset(s, 0, sizeof(*s));
    s->time = (int32_t)tick_us(c, res, tick);
    snprintf(s->name, sizeof(s->name), "%s", name);
}

static uint32_t be32(const uint8_t * p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static bool varlen(const uint8_t * d, long n, long * o, uint32_t * v)
{
    *v = 0;
    for (int k = 0; k < 4; k++) {
        if (*o >= n)
            return false;
        uint8_t b = d[(*o)++];
        *v = *v << 7 | (b & 0x7F);
        if (!(b & 0x80))
            return true;
    }
    return false;
}

static void from_mid(Cues * q, const Chart * c, const uint8_t * d, long n)
{
    if (n < 14 || memcmp(d, "MThd", 4))
        return;
    int division = d[12] << 8 | d[13];
    if (division <= 0 || (division & 0x8000))
        return;
    long o = 8 + be32(d + 4);
    while (o + 8 <= n) {
        long len = be32(d + o + 4), end = o + 8 + len;
        if (memcmp(d + o, "MTrk", 4) || end > n)
            break;
        // a first pass for the name, then the texts if it's one of the light show's tracks
        bool want = false;
        for (int pass = 0; pass < 2; pass++) {
            long p = o + 8, tick = 0;
            uint8_t status = 0;
            while (p < end) {
                uint32_t delta;
                if (!varlen(d, end, &p, &delta))
                    break;
                tick += delta;
                if (p >= end)
                    break;
                uint8_t b = d[p];
                if (b == 0xFF) {
                    if (p + 2 > end)
                        break;
                    uint8_t type = d[p + 1];
                    p += 2;
                    uint32_t l;
                    if (!varlen(d, end, &p, &l) || p + (long)l > end)
                        break;
                    char text[128];
                    size_t k = l < sizeof(text) - 1 ? l : sizeof(text) - 1;
                    memcpy(text, d + p, k);
                    text[k] = 0;
                    if (pass == 0 && type == 3)
                        want = !strcmp(text, "VENUE") || !strcmp(text, "EVENTS") || !strcmp(text, "LIGHTING");
                    else if (pass == 1 && type == 1)
                        add(q, c, division, tick, text);
                    p += l;
                } else if (b == 0xF0 || b == 0xF7) {
                    p++;
                    uint32_t l;
                    if (!varlen(d, end, &p, &l))
                        break;
                    p += l;
                } else {
                    if (b & 0x80) {
                        status = b;
                        p++;
                    }
                    int kind = status & 0xF0;
                    p += kind == 0xC0 || kind == 0xD0 ? 1 : 2;
                }
            }
            if (!want)
                break;
        }
        o = end;
    }
}

static void from_chart(Cues * q, const Chart * c, char * text)
{
    double res = 192;
    bool events = false;
    for (char * line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (isspace((unsigned char)*line))
            line++;
        if (*line == '[') {
            events = !strncmp(line, "[Events]", 8);
            continue;
        }
        char * eq = strchr(line, '=');
        if (!eq)
            continue;
        if (!strncasecmp(line, "Resolution", 10)) {
            double r = atof(eq + 1);
            if (r > 0)
                res = r;
            continue;
        }
        if (!events)
            continue;
        long tick = atol(line);
        char * v = eq + 1;
        while (isspace((unsigned char)*v))
            v++;
        if (*v != 'E')
            continue;
        add(q, c, res, tick, v + 1);
    }
}

static int by_time(const void * a, const void * b)
{
    int32_t x = ((const Section *)a)->time, y = ((const Section *)b)->time;
    return x < y ? -1 : x > y;
}

int cues_load(const char * folder, const Chart * c, Section ** out)
{
    Cues q = {0};
    char path[800];
    long n;
    snprintf(path, sizeof(path), "%s/notes.mid", folder);
    uint8_t * d = slurp(path, &n);
    if (d)
        from_mid(&q, c, d, n);
    else {
        snprintf(path, sizeof(path), "%s/notes.chart", folder);
        if ((d = slurp(path, &n)))
            from_chart(&q, c, (char *)d);
    }
    free(d);
    if (q.n)
        qsort(q.v, q.n, sizeof(Section), by_time);
    *out = q.v;
    return q.n;
}
