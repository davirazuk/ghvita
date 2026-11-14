#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chart.h"
#include "plat.h"

static void * read_file(const char * path, long * size)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void * data = malloc(n > 0 ? n : 1);
    if (data && fread(data, 1, n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = n;
    return data;
}

Chart * chart_load(const char * path)
{
    long size;
    uint8_t * d = read_file(path, &size);
    if (!d)
        return NULL;
    Chart * c = calloc(1, sizeof(Chart));
    c->blob = d;
    uint32_t h[6];
    if (size < 28 || memcmp(d, "VHC1", 4) != 0)
        goto bad;
    memcpy(h, d + 4, sizeof(h));
    c->version = h[0];
    c->tempo_count = h[1];
    c->beat_count = h[2];
    c->section_count = h[3];
    c->part_count = h[4];
    c->length_ms = h[5];
    long o = 28;
#define TAKE(ptr, count, type) \
    if (o + (long)(count) * (long)sizeof(type) > size) goto bad; \
    ptr = (const type *)(d + o); \
    o += (long)(count) * (long)sizeof(type);
    TAKE(c->tempo, c->tempo_count, TempoPoint)
    TAKE(c->beats, c->beat_count, BeatLine)
    TAKE(c->sections, c->section_count, Section)
    c->parts = calloc(c->part_count ? c->part_count : 1, sizeof(Part));
    for (int i = 0; i < c->part_count; i++) {
        if (o + 16 > size)
            goto bad;
        Part * p = &c->parts[i];
        uint32_t counts[3];
        p->part = d[o];
        p->diff = d[o + 1];
        memcpy(counts, d + o + 4, sizeof(counts));
        o += 16;
        p->note_count = counts[0];
        p->sp_count = counts[1];
        p->solo_count = counts[2];
        TAKE(p->notes, p->note_count, Note)
        TAKE(p->sp, p->sp_count, Range)
        TAKE(p->solos, p->solo_count, Range)
    }
#undef TAKE
    // cues ride with the sections, marked by a leading \1: apart, the sections are only the song's own
    int ncue = 0;
    for (int i = 0; i < c->section_count; i++)
        ncue += c->sections[i].name[0] == 1;
    if (ncue) {
        c->cues = malloc(ncue * sizeof(Section));
        c->own_sections = malloc((c->section_count - ncue + 1) * sizeof(Section));
        if (!c->cues || !c->own_sections)
            goto bad;
        int a = 0, b = 0;
        for (int i = 0; i < c->section_count; i++) {
            const Section * s = &c->sections[i];
            if (s->name[0] == 1) {
                c->cues[a] = *s;
                memmove(c->cues[a].name, s->name + 1, sizeof(s->name) - 1);
                a++;
            } else
                c->own_sections[b++] = *s;
        }
        c->cue_count = a;
        c->sections = c->own_sections;
        c->section_count = b;
    }
    return c;
bad:
    plat_log("chart: %s is damaged", path);
    chart_free(c);
    return NULL;
}

void chart_free(Chart * c)
{
    if (!c)
        return;
    free(c->parts);
    free(c->cues);
    free(c->own_sections);
    free(c->blob);
    free(c);
}

const Part * chart_part(const Chart * c, int part, int diff)
{
    for (int i = 0; i < c->part_count; i++)
        if (c->parts[i].part == part && c->parts[i].diff == diff)
            return &c->parts[i];
    return NULL;
}

double chart_quarters(const Chart * c, double seconds)
{
    double us = seconds * 1e6;
    if (c->tempo_count == 0)
        return us / 500000.0;
    // the tempo points are at their own times: quarters add up segment by segment
    double q = 0;
    int i = 0;
    if (us < c->tempo[0].time)
        return (us - c->tempo[0].time) / c->tempo[0].us_per_quarter;
    for (; i + 1 < c->tempo_count && c->tempo[i + 1].time <= us; i++)
        q += (double)(c->tempo[i + 1].time - c->tempo[i].time) / c->tempo[i].us_per_quarter;
    return q + (us - c->tempo[i].time) / c->tempo[i].us_per_quarter;
}
