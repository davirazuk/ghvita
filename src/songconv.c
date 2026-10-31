// The chart half of tools/pack.py in C (same rules, same notes.vhc), so the Vita can take songs it downloads or
// finds in songs/ as they come: the audio stays as the song ships it (decoder.c plays Vorbis, Opus, MP3 and WAV)
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "songconv.h"
#include "audio.h"
#include "chart.h"
#include "decoder.h"
#include "plat.h"
#include "ext/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ext/stb_image_write.h"

#define F_HOPO NOTE_HOPO
#define F_TAP NOTE_TAP
#define F_SP NOTE_SP
#define F_SP_END NOTE_SP_END
#define OPEN FRET_OPEN

// bump with pack.py's CHART_VERSION when the chart output changes
#define CHART_VERSION 2

#define VEC(T) struct { T * v; int n, cap; }
#define PUSH(vec, x) do { \
        if ((vec).n == (vec).cap) { \
            (vec).cap = (vec).cap ? (vec).cap * 2 : 16; \
            (vec).v = realloc((vec).v, (vec).cap * sizeof(*(vec).v)); \
        } \
        (vec).v[(vec).n++] = (x); \
    } while (0)
#define FREE(vec) do { free((vec).v); (vec).v = NULL; (vec).n = (vec).cap = 0; } while (0)

static const char * const audio_ext[] = {".ogg", ".opus", ".mp3", ".wav"};     // in pack.py's order of preference
// the stems Clone Hero plays (anything else in the folder, like preview.ogg, isn't the song)
static const char * const stem_names[] = {"song", "guitar", "bass", "rhythm", "keys", "vocals", "vocals_1", "vocals_2",
                                          "drums", "drums_1", "drums_2", "drums_3", "drums_4", "crowd"};
static const char * const chart_names[PART_COUNT] = {"Single", "DoubleBass", "DoubleRhythm", "DoubleGuitar", "Keyboard", "Drums", "-"};
static const char * const mid_names[PART_COUNT] = {"PART GUITAR", "PART BASS", "PART RHYTHM", "PART GUITAR COOP",
                                                   "PART KEYS", "PART DRUMS", "PART VOCALS"};
static const char * const part_stems[PART_COUNT] = {"guitar", "bass", "rhythm", "guitar", "keys", "drums", "vocals"};
static const char * const part_keys[PART_COUNT] = {"guitar", "bass", "rhythm", "coop", "keys", "drums", "vocals"};
static const char * const diff_names[DIFF_COUNT] = {"Easy", "Medium", "Hard", "Expert"};

// ------------------------------------------------------------------------------------------------------- strings

static char * trim(char * s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char * e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static bool all_digits(const char * s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return false;
    return true;
}

// Python's int(): optional sign and digits, surrounding spaces allowed
static bool parse_int(const char * s, long * out)
{
    while (isspace((unsigned char)*s))
        s++;
    char * end;
    long v = strtol(s, &end, 10);
    if (end == s)
        return false;
    while (isspace((unsigned char)*end))
        end++;
    if (*end)
        return false;
    *out = v;
    return true;
}

// pack.py's ms(): int(float(v)), or the default
static long ms_value(const char * s, long def)
{
    if (!s)
        return def;
    char * end;
    double v = strtod(s, &end);
    if (end == s)
        return def;
    while (isspace((unsigned char)*end))
        end++;
    return *end || !isfinite(v) ? def : (long)v;
}

static bool utf8_valid(const uint8_t * s, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i];
        int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || i + k >= n + (k == 0))
            return false;
        for (int j = 1; j <= k; j++)
            if ((s[i + j] & 0xC0) != 0x80)
                return false;
        i += k + 1;
    }
    return true;
}

static void put_utf8(char ** o, unsigned cp)
{
    char * p = *o;
    if (cp < 0x80)
        *p++ = (char)cp;
    else if (cp < 0x800) {
        *p++ = (char)(0xC0 | cp >> 6);
        *p++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *p++ = (char)(0xE0 | cp >> 12);
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *p++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *p++ = (char)(0xF0 | cp >> 18);
        *p++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *p++ = (char)(0x80 | (cp & 0x3F));
    }
    *o = p;
}

// a text file as UTF-8 (UTF-16 with its mark, else UTF-8, else Windows-1252 as pack.py reads song.ini), BOM dropped
static char * to_utf8(const uint8_t * d, size_t n)
{
    static const unsigned short cp1252[32] = {
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,
        0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
        0x0153, 0x9D, 0x017E, 0x0178};
    char * out = malloc(n * 3 + 4), * o = out;
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
        for (size_t i = 2; i + 1 < n; i += 2) {
            unsigned cp = d[i] | d[i + 1] << 8;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) {
                unsigned lo = d[i + 2] | d[i + 3] << 8;
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            put_utf8(&o, cp);
        }
    } else {
        if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
            d += 3;
            n -= 3;
        }
        if (utf8_valid(d, n)) {
            memcpy(o, d, n);
            o += n;
        } else
            for (size_t i = 0; i < n; i++)
                put_utf8(&o, d[i] >= 0x80 && d[i] < 0xA0 ? cp1252[d[i] - 0x80] : d[i]);
    }
    *o = 0;
    return out;
}

// pack.py's clean(): tags like <color=#FF0000> out, then trimmed
static void clean(char * out, size_t size, const char * in)
{
    size_t k = 0;
    bool tag = false;
    for (; in && *in && k + 1 < size; in++) {
        if (*in == '<' && strchr(in, '>'))
            tag = true;
        else if (tag && *in == '>')
            tag = false;
        else if (!tag)
            out[k++] = *in;
    }
    out[k] = 0;
    char * t = trim(out);
    memmove(out, t, strlen(t) + 1);
}

void songconv_safe_name(char * out, size_t size, const char * in)
{
    size_t k = 0;
    int chars = 0;
    for (const unsigned char * p = (const unsigned char *)in; *p && k + 5 < size; p++) {
        if (strchr("\\/:*?\"<>|", *p) || *p < 0x20)
            continue;
        if ((*p & 0xC0) != 0x80 && ++chars > 80)       // pack.py keeps 80 characters
            break;
        out[k++] = (char)*p;
    }
    out[k] = 0;
    // no spaces or dots at either end
    char * s = out;
    while (*s == ' ' || *s == '.')
        s++;
    memmove(out, s, strlen(s) + 1);
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.'))
        out[--n] = 0;
    if (!*out)
        snprintf(out, size, "song");
}

// ---------------------------------------------------------------------------------------------------- the source

typedef struct {
    char name[96];                  // lower case
    char path[600];                 // a folder's file
    uint64_t off, size;             // a .sng's
} SrcFile;

typedef struct {
    char key[64];
    char * value;
} IniEntry;

typedef struct {
    char path[600];
    bool sng;
    uint8_t mask[16];
    VEC(SrcFile) files;
    VEC(IniEntry) ini;
} Source;

static const char * ini_get(const Source * s, const char * key)
{
    for (int i = 0; i < s->ini.n; i++)
        if (strcmp(s->ini.v[i].key, key) == 0)
            return s->ini.v[i].value;
    return NULL;
}

static void ini_set(Source * s, const char * key, const char * value, bool replace)
{
    for (int i = 0; i < s->ini.n; i++)
        if (strcmp(s->ini.v[i].key, key) == 0) {
            if (replace) {
                free(s->ini.v[i].value);
                s->ini.v[i].value = strdup(value);
            }
            return;
        }
    IniEntry e;
    snprintf(e.key, sizeof(e.key), "%s", key);
    for (char * p = e.key; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    e.value = strdup(value);
    PUSH(s->ini, e);
}

static const SrcFile * src_find(const Source * s, const char * name)
{
    for (int i = 0; i < s->files.n; i++)
        if (strcmp(s->files.v[i].name, name) == 0)
            return &s->files.v[i];
    return NULL;
}

static uint64_t le64(const uint8_t * p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static bool read_exact(FILE * f, void * buf, size_t n)
{
    return fread(buf, 1, n, f) == n;
}

// a .sng: "SNGPKG", version, the XOR mask, then the metadata (song.ini's keys) and the file table
static bool open_sng(Source * s)
{
    FILE * f = fopen(s->path, "rb");
    if (!f)
        return false;
    uint8_t h[42];
    bool ok = read_exact(f, h, 42) && memcmp(h, "SNGPKG", 6) == 0;
    if (ok) {
        memcpy(s->mask, h + 10, 16);
        uint64_t count = le64(h + 34);
        for (uint64_t i = 0; ok && i < count && i < 4096; i++) {
            uint8_t l[4];
            char key[64], * value;
            ok = read_exact(f, l, 4);
            int32_t kl = l[0] | l[1] << 8 | l[2] << 16 | (int32_t)l[3] << 24;
            if (!ok || kl < 0 || kl > 4096)
                break;
            char * k = malloc(kl + 1);
            ok = read_exact(f, k, kl) && read_exact(f, l, 4);
            k[kl] = 0;
            int32_t vl = l[0] | l[1] << 8 | l[2] << 16 | (int32_t)l[3] << 24;
            if (!ok || vl < 0 || vl > 1 << 20) {
                free(k);
                ok = false;
                break;
            }
            value = malloc(vl + 1);
            ok = read_exact(f, value, vl);
            value[vl] = 0;
            snprintf(key, sizeof(key), "%s", k);
            char * text = to_utf8((uint8_t *)value, vl);
            ini_set(s, key, text, true);
            free(text);
            free(value);
            free(k);
        }
        uint8_t fh[16];
        ok = ok && read_exact(f, fh, 16);
        uint64_t files = ok ? le64(fh + 8) : 0;
        for (uint64_t i = 0; ok && i < files && i < 4096; i++) {
            uint8_t nl;
            SrcFile sf;
            memset(&sf, 0, sizeof(sf));
            char name[256];
            uint8_t sz[16];
            ok = read_exact(f, &nl, 1) && read_exact(f, name, nl) && read_exact(f, sz, 16);
            name[nl] = 0;
            for (int j = 0; name[j] && j < (int)sizeof(sf.name) - 1; j++)
                sf.name[j] = (char)tolower((unsigned char)name[j]);
            sf.size = le64(sz);
            sf.off = le64(sz + 8);
            if (ok)
                PUSH(s->files, sf);
        }
    }
    fclose(f);
    return ok;
}

static void read_ini_text(Source * s, char * text)
{
    for (char * line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char * l = trim(line);
        if (!*l || *l == '[' || *l == ';' || *l == '#' || (l[0] == '/' && l[1] == '/'))
            continue;
        char * eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = 0;
        ini_set(s, trim(l), trim(eq + 1), false);       // the first one counts, as in pack.py
    }
}

static uint8_t * src_read(const Source * s, const char * name, size_t * size);

static bool open_folder(Source * s)
{
    DIR * d = opendir(s->path);
    if (!d)
        return false;
    struct dirent * e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        SrcFile sf;
        memset(&sf, 0, sizeof(sf));
        snprintf(sf.path, sizeof(sf.path), "%s/%s", s->path, e->d_name);
        struct stat st;
        if (stat(sf.path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        for (int j = 0; e->d_name[j] && j < (int)sizeof(sf.name) - 1; j++)
            sf.name[j] = (char)tolower((unsigned char)e->d_name[j]);
        sf.size = st.st_size;
        PUSH(s->files, sf);
    }
    closedir(d);
    size_t n;
    uint8_t * raw = src_read(s, "song.ini", &n);
    if (raw) {
        char * text = to_utf8(raw, n);
        read_ini_text(s, text);
        free(text);
        free(raw);
    }
    return true;
}

static void src_free(Source * s)
{
    for (int i = 0; i < s->ini.n; i++)
        free(s->ini.v[i].value);
    FREE(s->ini);
    FREE(s->files);
}

static void unmask(const Source * s, uint8_t * buf, size_t n, uint64_t at)
{
    for (size_t i = 0; i < n; i++) {
        uint64_t k = at + i;
        buf[i] ^= s->mask[k % 16] ^ (uint8_t)(k & 0xFF);
    }
}

static uint8_t * src_read(const Source * s, const char * name, size_t * size)
{
    const SrcFile * sf = src_find(s, name);
    if (!sf || sf->size > 64u << 20)
        return NULL;
    FILE * f = fopen(s->sng ? s->path : sf->path, "rb");
    if (!f)
        return NULL;
    uint8_t * buf = malloc(sf->size + 1);
    bool ok = !s->sng || fseek(f, (long)sf->off, SEEK_SET) == 0;
    ok = ok && read_exact(f, buf, sf->size);
    fclose(f);
    if (!ok) {
        free(buf);
        return NULL;
    }
    if (s->sng)
        unmask(s, buf, sf->size, 0);
    buf[sf->size] = 0;
    *size = sf->size;
    return buf;
}

// a .sng's file onto the card
static bool src_extract(const Source * s, const SrcFile * sf, const char * dest)
{
    FILE * in = fopen(s->path, "rb");
    if (!in)
        return false;
    FILE * out = fopen(dest, "wb");
    bool ok = out && fseek(in, (long)sf->off, SEEK_SET) == 0;
    static uint8_t buf[64 * 1024];
    for (uint64_t done = 0; ok && done < sf->size;) {
        size_t n = sf->size - done < sizeof(buf) ? (size_t)(sf->size - done) : sizeof(buf);
        ok = read_exact(in, buf, n);
        if (ok) {
            unmask(s, buf, n, done);
            ok = fwrite(buf, 1, n, out) == n;
        }
        done += n;
    }
    fclose(in);
    if (out && fclose(out) != 0)
        ok = false;
    if (!ok)
        remove(dest);
    return ok;
}

static bool copy_file(const char * from, const char * to)
{
    FILE * in = fopen(from, "rb");
    if (!in)
        return false;
    FILE * out = fopen(to, "wb");
    bool ok = out != NULL;
    static uint8_t buf[64 * 1024];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    if (out && fclose(out) != 0)
        ok = false;
    if (!ok)
        remove(to);
    return ok;
}

// ------------------------------------------------------------------------------------------------------ the chart

typedef struct { int32_t tick; int64_t us; } TempoIn;
typedef struct { int32_t tick; int num, den; } Sig;
typedef struct { int32_t tick; char * text; } Name;
typedef struct { int32_t a, b; } TR;
typedef struct { int32_t a, b; bool on; } Forced;
typedef struct { int32_t tick; int fret; int32_t len; int order; } RawNote;

typedef struct {
    bool present;
    VEC(RawNote) notes;             // fret 0-4, 7 open
    VEC(int32_t) forced_ticks;      // .chart: the HOPO flip
    VEC(Forced) forced;             // .mid: forced on / off ranges
    VEC(TR) taps, sp, solos;
    VEC(TR) cyms;                   // .chart drums: tick, pad (0 yellow, 1 blue, 2 green) of a cymbal
} Track;

typedef struct { int32_t tick, len; int pitch; char * text; } VoxIn;

typedef struct {
    bool mid;
    int res;
    VEC(VoxIn) vox;                 // PART VOCALS
    VEC(TR) vox_phrases;
    VEC(TR) toms[3];                // .mid drums: while yellow, blue, green are toms
    VEC(TR) disco[DIFF_COUNT];      // .mid drums: while a disco flip's on, red and yellow trade places
    VEC(TempoIn) tempos;
    VEC(Sig) sigs;
    VEC(Name) names;
    Track tracks[PART_COUNT][DIFF_COUNT];
} Parsed;

static void parsed_free(Parsed * p)
{
    FREE(p->tempos);
    FREE(p->sigs);
    for (int i = 0; i < p->vox.n; i++)
        free(p->vox.v[i].text);
    FREE(p->vox);
    FREE(p->vox_phrases);
    for (int i = 0; i < 3; i++)
        FREE(p->toms[i]);
    for (int i = 0; i < DIFF_COUNT; i++)
        FREE(p->disco[i]);
    for (int i = 0; i < p->names.n; i++)
        free(p->names.v[i].text);
    FREE(p->names);
    for (int a = 0; a < PART_COUNT; a++)
        for (int b = 0; b < DIFF_COUNT; b++) {
            Track * t = &p->tracks[a][b];
            FREE(t->notes);
            FREE(t->forced_ticks);
            FREE(t->forced);
            FREE(t->taps);
            FREE(t->sp);
            FREE(t->solos);
            FREE(t->cyms);
        }
}

static void track_reset(Track * t)
{
    FREE(t->cyms);
    FREE(t->notes);
    FREE(t->forced_ticks);
    FREE(t->forced);
    FREE(t->taps);
    FREE(t->sp);
    FREE(t->solos);
    t->present = false;
}

typedef struct {
    char * section;
    char * key, * value;
} ChartLine;

static bool parse_chart(char * text, Parsed * P)
{
    VEC(ChartLine) lines = {0};
    char * name = NULL;
    bool body = false;
    for (char * line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char * l = trim(line);
        size_t n = strlen(l);
        if (n >= 2 && l[0] == '[' && l[n - 1] == ']') {
            l[n - 1] = 0;
            name = l + 1;
            body = false;
        } else if (strcmp(l, "{") == 0)
            body = name != NULL;
        else if (strcmp(l, "}") == 0)
            body = false;
        else if (body) {
            char * eq = strchr(l, '=');
            if (!eq)
                continue;
            *eq = 0;
            ChartLine cl = {name, trim(l), trim(eq + 1)};
            PUSH(lines, cl);
        }
    }
    P->res = 192;
    for (int i = 0; i < lines.n; i++) {
        ChartLine * c = &lines.v[i];
        if (strcmp(c->section, "Song") == 0 && strcasecmp(c->key, "resolution") == 0) {
            char v[64];
            snprintf(v, sizeof(v), "%s", c->value);
            char * q = v;
            if (*q == '"')
                q++;
            q[strcspn(q, "\"")] = 0;
            double r = strtod(q, NULL);
            P->res = r >= 1 ? (int)r : 192;
        }
    }
    for (int i = 0; i < lines.n; i++) {
        ChartLine * c = &lines.v[i];
        if (!all_digits(c->key))
            continue;
        int32_t tick = (int32_t)atol(c->key);
        char v[256];
        snprintf(v, sizeof(v), "%s", c->value);
        char * parts[4] = {0};
        int np = 0;
        for (char * t = strtok(v, " \t"); t && np < 4; t = strtok(NULL, " \t"))
            parts[np++] = t;
        if (strcmp(c->section, "SyncTrack") == 0) {
            long a, b;
            if (np >= 2 && strcmp(parts[0], "B") == 0 && parse_int(parts[1], &a)) {
                TempoIn t = {tick, (int64_t)rint(60000000000.0 / (a > 1 ? a : 1))};
                PUSH(P->tempos, t);
            } else if (np >= 2 && strcmp(parts[0], "TS") == 0 && parse_int(parts[1], &a)) {
                Sig s = {tick, (int)a, 4};
                if (np > 2 && parse_int(parts[2], &b) && b >= 0 && b < 16)
                    s.den = 1 << b;
                PUSH(P->sigs, s);
            }
        } else if (strcmp(c->section, "Events") == 0) {
            // E "section Intro"
            const char * p = c->value;
            if (*p != 'E' || !isspace((unsigned char)p[1]))
                continue;
            p++;
            while (isspace((unsigned char)*p))
                p++;
            if (*p == '"')
                p++;
            if (strncmp(p, "section", 7) != 0 || !isspace((unsigned char)p[7]))
                continue;
            p += 7;
            while (isspace((unsigned char)*p))
                p++;
            Name nm = {tick, strdup(p)};
            size_t len = strlen(nm.text);
            if (len && nm.text[len - 1] == '"')
                nm.text[len - 1] = 0;
            PUSH(P->names, nm);
        }
    }
    for (int pid = 0; pid < PART_COUNT; pid++)
        for (int d = 0; d < DIFF_COUNT; d++) {
            char sec[48];
            snprintf(sec, sizeof(sec), "%s%s", diff_names[d], chart_names[pid]);
            Track * t = &P->tracks[pid][d];
            int32_t solo = -1;
            int order = 0;
            for (int i = 0; i < lines.n; i++) {
                ChartLine * c = &lines.v[i];
                if (strcmp(c->section, sec) != 0 || !all_digits(c->key))
                    continue;
                int32_t tick = (int32_t)atol(c->key);
                char v[256];
                snprintf(v, sizeof(v), "%s", c->value);
                char * parts[4] = {0};
                int np = 0;
                for (char * tok = strtok(v, " \t"); tok && np < 4; tok = strtok(NULL, " \t"))
                    parts[np++] = tok;
                long a, b;
                if (np >= 3 && strcmp(parts[0], "N") == 0 && parse_int(parts[1], &a) && parse_int(parts[2], &b)) {
                    // on drums 5 is the fifth pad, not a hammer-on flip
                    if ((a >= 0 && a <= 4) || a == 7 || (a == 5 && pid == PART_DRUMS)) {
                        RawNote n = {tick, (int)a, (int32_t)b, order++};
                        PUSH(t->notes, n);
                    } else if (a == 5)
                        PUSH(t->forced_ticks, tick);
                    else if (a == 6) {
                        TR r = {tick, tick + 1};
                        PUSH(t->taps, r);
                    } else if (a >= 66 && a <= 68) {
                        TR r = {tick, (int32_t)a - 66};
                        PUSH(t->cyms, r);
                    }
                } else if (np >= 3 && strcmp(parts[0], "S") == 0 && strcmp(parts[1], "2") == 0 &&
                           parse_int(parts[2], &b)) {
                    TR r = {tick, tick + (int32_t)(b > 1 ? b : 1)};
                    PUSH(t->sp, r);
                } else if (np >= 2 && strcmp(parts[0], "E") == 0) {
                    char * ev = parts[1];
                    while (*ev == '"')
                        ev++;
                    size_t len = strlen(ev);
                    while (len && ev[len - 1] == '"')
                        ev[--len] = 0;
                    if (strcmp(ev, "solo") == 0)
                        solo = tick;
                    else if (strcmp(ev, "soloend") == 0 && solo >= 0) {
                        TR r = {solo, tick + 1};
                        PUSH(t->solos, r);
                        solo = -1;
                    }
                }
            }
            if (t->notes.n)
                t->present = true;
            else
                track_reset(t);
        }
    FREE(lines);
    P->mid = false;
    return true;
}

static uint32_t vlq(const uint8_t * d, size_t n, size_t * o, bool * bad)
{
    uint32_t v = 0;
    for (;;) {
        if (*o >= n) {
            *bad = true;
            return 0;
        }
        uint8_t b = d[(*o)++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            return v;
    }
}

typedef struct { int32_t s, e; } Pair;
typedef VEC(Pair) PairVec;
typedef struct { int32_t tick; uint8_t diff, type, on; } Sysex;

static int sysex_cmp(const void * a, const void * b)
{
    const Sysex * x = a, * y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    if (x->diff != y->diff)
        return x->diff - y->diff;
    if (x->type != y->type)
        return x->type - y->type;
    return x->on - y->on;
}

static bool parse_mid(const uint8_t * d, size_t n, const Source * src, Parsed * P, char * err, size_t err_size)
{
    if (n < 14 || memcmp(d, "MThd", 4) != 0) {
        snprintf(err, err_size, "notes.mid isn't a MIDI file");
        return false;
    }
    uint32_t hl = (uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
    int ntr = d[10] << 8 | d[11], div = d[12] << 8 | d[13];
    if (div & 0x8000) {
        snprintf(err, err_size, "notes.mid uses SMPTE time");
        return false;
    }
    P->res = div;
    P->mid = true;
    const char * mn = ini_get(src, "multiplier_note");
    if (!mn)
        mn = ini_get(src, "star_power_note");
    char mnb[16] = "";
    if (mn) {
        snprintf(mnb, sizeof(mnb), "%s", mn);
        memmove(mnb, trim(mnb), strlen(trim(mnb)) + 1);
    }
    int sp_note = strcmp(mnb, "103") == 0 ? 103 : 116;
    size_t o = 8 + hl;
    for (int tr = 0; tr < ntr; tr++) {
        if (o + 8 > n || memcmp(d + o, "MTrk", 4) != 0)
            break;
        uint32_t tl = (uint32_t)d[o + 4] << 24 | d[o + 5] << 16 | d[o + 6] << 8 | d[o + 7];
        o += 8;
        size_t end = o + tl < n ? o + tl : n;
        int32_t tick = 0;
        uint8_t status = 0;
        char tname[64];
        bool named = false;
        PairVec notes[128];
        VEC(int32_t) open[128];
        memset(notes, 0, sizeof(notes));
        memset(open, 0, sizeof(open));
        VEC(Sysex) sysex = {0};
        VEC(Name) texts = {0};
        bool bad = false, fail = false;
        while (o < end && !bad) {
            tick += (int32_t)vlq(d, end, &o, &bad);
            if (bad || o >= end)
                break;
            uint8_t b = d[o];
            if (b == 0xFF) {
                if (o + 1 >= end)
                    break;
                uint8_t typ = d[o + 1];
                o += 2;
                uint32_t ln = vlq(d, end, &o, &bad);
                if (bad)
                    break;
                size_t avail = o + ln <= end ? ln : end - o;
                const uint8_t * data = d + o;
                if (typ == 0x03 && !named) {
                    size_t k = avail < sizeof(tname) - 1 ? avail : sizeof(tname) - 1;
                    memcpy(tname, data, k);
                    tname[k] = 0;
                    memmove(tname, trim(tname), strlen(trim(tname)) + 1);
                    named = true;
                } else if (typ == 0x51 && ln == 3) {
                    if (avail < 3)
                        break;
                    TempoIn t = {tick, (int64_t)(data[0] << 16 | data[1] << 8 | data[2])};
                    PUSH(P->tempos, t);
                } else if (typ == 0x58 && ln >= 2) {
                    if (avail < 2)
                        break;
                    Sig s = {tick, data[0], data[1] < 16 ? 1 << data[1] : 4};
                    PUSH(P->sigs, s);
                } else if (typ == 0x01 || typ == 0x05 || typ == 0x06 || typ == 0x07) {
                    Name t = {tick, malloc(avail + 1)};
                    memcpy(t.text, data, avail);
                    t.text[avail] = 0;
                    PUSH(texts, t);
                }
                o += ln;
                continue;
            }
            if (b == 0xF0 || b == 0xF7) {
                o++;
                uint32_t ln = vlq(d, end, &o, &bad);
                if (bad)
                    break;
                // Phase Shift: 50 53 00 00 <difficulty|FF> <01 open / 04 tap> <on> F7
                if (ln >= 7 && o + 7 <= end && d[o] == 0x50 && d[o + 1] == 0x53 && d[o + 2] == 0) {
                    Sysex sx = {tick, d[o + 4], d[o + 5], d[o + 6]};
                    PUSH(sysex, sx);
                }
                o += ln;
                continue;
            }
            if (b & 0x80) {
                status = b;
                o++;
            }
            uint8_t kind = status & 0xF0;
            if (kind == 0x80 || kind == 0x90) {
                if (o + 2 > end) {
                    bad = true;
                    break;
                }
                uint8_t pitch = d[o] & 0x7F, vel = d[o + 1];
                o += 2;
                if (kind == 0x90 && vel > 0)
                    PUSH(open[pitch], tick);
                else if (open[pitch].n) {
                    Pair p = {open[pitch].v[0], tick};
                    memmove(open[pitch].v, open[pitch].v + 1, (open[pitch].n - 1) * sizeof(int32_t));
                    open[pitch].n--;
                    PUSH(notes[pitch], p);
                }
            } else if (kind == 0xA0 || kind == 0xB0 || kind == 0xE0)
                o += 2;
            else if (kind == 0xC0 || kind == 0xD0)
                o += 1;
            else {
                fail = true;
                break;
            }
        }
        if (fail) {
            snprintf(err, err_size, "notes.mid has a bad MIDI event");
            for (int i = 0; i < 128; i++) {
                FREE(notes[i]);
                FREE(open[i]);
            }
            for (int i = 0; i < texts.n; i++)
                free(texts.v[i].text);
            FREE(texts);
            FREE(sysex);
            return false;
        }
        for (int p = 0; p < 128; p++)
            for (int i = 0; i < open[p].n; i++) {
                Pair pr = {open[p].v[i], open[p].v[i] + 1};
                PUSH(notes[p], pr);
            }
        o = end;
        if (named && strcmp(tname, "EVENTS") == 0) {
            for (int i = 0; i < texts.n; i++) {
                // [section Intro] or [prc_intro]
                char * t = trim(texts.v[i].text);
                size_t len = strlen(t);
                if (len < 2 || t[0] != '[' || t[len - 1] != ']')
                    continue;
                const char * body = NULL;
                if (strncmp(t + 1, "section", 7) == 0 && (t[8] == ' ' || t[8] == '_'))
                    body = t + 9;
                else if (strncmp(t + 1, "prc", 3) == 0 && (t[4] == ' ' || t[4] == '_'))
                    body = t + 5;
                if (!body || body > t + len - 1)
                    continue;
                Name nm = {texts.v[i].tick, strndup(body, t + len - 1 - body)};
                PUSH(P->names, nm);
            }
        }
        int pid = -1;
        for (int p = 0; named && p < PART_COUNT; p++)
            if (strcmp(tname, mid_names[p]) == 0)
                pid = p;
        if (pid == PART_VOCALS) {
            // notes 36-84 are the pitches, 105/106 the phrases; each note's lyric is at its start
            pid = -1;
            for (int p = 36; p <= 84; p++)
                for (int i = 0; i < notes[p].n; i++) {
                    VoxIn v = {notes[p].v[i].s, notes[p].v[i].e - notes[p].v[i].s, p, NULL};
                    for (int k = 0; k < texts.n && !v.text; k++)
                        if (texts.v[k].tick == v.tick && texts.v[k].text[0] != '[')
                            v.text = strdup(texts.v[k].text);
                    PUSH(P->vox, v);
                }
            for (int p = 105; p <= 106; p++)
                for (int i = 0; i < notes[p].n; i++) {
                    TR r = {notes[p].v[i].s, notes[p].v[i].e};
                    PUSH(P->vox_phrases, r);
                }
        }
        bool enhanced = false;
        for (int i = 0; i < texts.n; i++)
            if (strstr(texts.v[i].text, "ENHANCED_OPENS"))
                enhanced = true;
        if (sysex.n)
            qsort(sysex.v, sysex.n, sizeof(Sysex), sysex_cmp);
        for (int di = 0; pid >= 0 && di < DIFF_COUNT; di++) {
            int base = 60 + 12 * di;
            Track tmp, * t = &tmp;
            memset(t, 0, sizeof(tmp));
            int order = 0;
            // drums: a sixth, the green of 5-lane charts
            for (int fret = 0; fret < (pid == PART_DRUMS ? 6 : 5); fret++)
                for (int i = 0; i < notes[base + fret].n; i++) {
                    Pair pr = notes[base + fret].v[i];
                    RawNote rn = {pr.s, fret, pr.e - pr.s, order++};
                    PUSH(t->notes, rn);
                }
            if (enhanced)
                for (int i = 0; i < notes[base - 1].n; i++) {
                    Pair pr = notes[base - 1].v[i];
                    RawNote rn = {pr.s, 7, pr.e - pr.s, order++};
                    PUSH(t->notes, rn);
                }
            if (!t->notes.n) {
                track_reset(t);
                continue;
            }
            t->present = true;
            track_reset(&P->tracks[pid][di]);
            for (int i = 0; i < notes[base + 5].n; i++) {
                Forced f = {notes[base + 5].v[i].s, notes[base + 5].v[i].e, true};
                PUSH(t->forced, f);
            }
            for (int i = 0; i < notes[base + 6].n; i++) {
                Forced f = {notes[base + 6].v[i].s, notes[base + 6].v[i].e, false};
                PUSH(t->forced, f);
            }
            for (int i = 0; i < notes[104].n; i++) {
                TR r = {notes[104].v[i].s, notes[104].v[i].e};
                PUSH(t->taps, r);
            }
            for (int i = 0; i < notes[sp_note].n; i++) {
                TR r = {notes[sp_note].v[i].s, notes[sp_note].v[i].e};
                PUSH(t->sp, r);
            }
            if (sp_note == 116)
                for (int i = 0; i < notes[103].n; i++) {
                    TR r = {notes[103].v[i].s, notes[103].v[i].e};
                    PUSH(t->solos, r);
                }
            // Phase Shift sysex phrases: open notes (green turns open, done in build) and taps
            for (int typ = 1; typ <= 4; typ += 3) {
                int32_t start = -1;
                for (int i = 0; i < sysex.n; i++) {
                    Sysex * x = &sysex.v[i];
                    if (x->type != typ || (x->diff != di && x->diff != 0xFF))
                        continue;
                    if (x->on && start < 0)
                        start = x->tick;
                    else if (!x->on && start >= 0) {
                        TR r = {start, x->tick + 1};
                        if (typ == 4)
                            PUSH(t->taps, r);
                        else {
                            // marks for build_chords: an open phrase
                            RawNote rn = {r.a, -1, r.b - r.a, order++};
                            PUSH(t->notes, rn);
                        }
                        start = -1;
                    }
                }
            }
            P->tracks[pid][di] = tmp;
        }
        for (int k = 0; pid == PART_DRUMS && k < 3; k++)
            for (int i = 0; i < notes[110 + k].n; i++) {
                TR r = {notes[110 + k].v[i].s, notes[110 + k].v[i].e};
                PUSH(P->toms[k], r);
            }
        // [mix 3 drums2d]: from here to the next mix event of that difficulty, a disco flip
        int32_t flip_from[DIFF_COUNT] = {-1, -1, -1, -1};
        for (int i = 0; pid == PART_DRUMS && i < texts.n; i++) {
            const char * m = strstr(texts.v[i].text, "mix ");
            int di, layout;
            char how[16] = "";
            if (!m || sscanf(m, "mix %d drums%d%15[a-z]", &di, &layout, how) < 2 || di < 0 || di >= DIFF_COUNT)
                continue;
            if (flip_from[di] >= 0) {
                TR r = {flip_from[di], texts.v[i].tick};
                PUSH(P->disco[di], r);
            }
            flip_from[di] = strcmp(how, "d") ? -1 : texts.v[i].tick;
        }
        for (int di = 0; pid == PART_DRUMS && di < DIFF_COUNT; di++)
            if (flip_from[di] >= 0) {
                TR r = {flip_from[di], INT32_MAX};
                PUSH(P->disco[di], r);
            }
        for (int i = 0; i < 128; i++) {
            FREE(notes[i]);
            FREE(open[i]);
        }
        for (int i = 0; i < texts.n; i++)
            free(texts.v[i].text);
        FREE(texts);
        FREE(sysex);
    }
    return true;
}

// ------------------------------------------------------------------------------------------ chords, then notes

typedef struct {
    int32_t tick;
    uint8_t has;                    // bit per fret (0-4, 7 open)
    int32_t len[8];
} Chord;

typedef struct {
    int32_t tick;
    uint8_t mask, flags;
    int32_t len;
} OutNote;

static int rawnote_cmp(const void * a, const void * b)
{
    const RawNote * x = a, * y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    return x->order - y->order;
}

static bool in_ranges(const TR * r, int n, int32_t tick)
{
    for (int i = 0; i < n; i++)
        if (r[i].a <= tick && tick < r[i].b)
            return true;
    return false;
}

static int popcount(unsigned v)
{
    int c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

// the chords of one part on one difficulty, as pack.py's build_notes
static int build_notes(const Parsed * P, Track * t, const Source * src, OutNote ** out_notes)
{
    // fret -1 entries are Phase Shift open phrases (see parse_mid)
    VEC(TR) open_phrases = {0};
    VEC(RawNote) real = {0};
    for (int i = 0; i < t->notes.n; i++) {
        if (t->notes.v[i].fret < 0) {
            TR r = {t->notes.v[i].tick, t->notes.v[i].tick + t->notes.v[i].len};
            PUSH(open_phrases, r);
        } else
            PUSH(real, t->notes.v[i]);
    }
    qsort(real.v, real.n, sizeof(RawNote), rawnote_cmp);
    VEC(Chord) chords = {0};
    for (int i = 0; i < real.n;) {
        Chord c;
        memset(&c, 0, sizeof(c));
        c.tick = real.v[i].tick;
        for (; i < real.n && real.v[i].tick == c.tick; i++) {
            c.len[real.v[i].fret] = real.v[i].len;
            c.has |= 1 << real.v[i].fret;
        }
        // a Phase Shift open phrase turns green notes open
        if (P->mid && (c.has & 1) && in_ranges(open_phrases.v, open_phrases.n, c.tick)) {
            c.len[7] = c.len[0];
            c.has = (c.has & ~1) | 0x80;
        }
        PUSH(chords, c);
    }
    FREE(real);
    FREE(open_phrases);

    long threshold = 0;
    const char * hf = ini_get(src, "hopo_frequency");
    if (!hf || !parse_int(hf, &threshold))
        threshold = 0;
    if (threshold <= 0) {
        char eb[16] = "";
        const char * e = ini_get(src, "eighthnote_hopo");
        if (e) {
            snprintf(eb, sizeof(eb), "%s", e);
            char * t2 = trim(eb);
            for (char * q = t2; *q; q++)
                *q = (char)tolower((unsigned char)*q);
            memmove(eb, t2, strlen(t2) + 1);
        }
        bool eighth = strcmp(eb, "1") == 0 || strcmp(eb, "true") == 0;
        threshold = P->res / (eighth ? 2 : 3);
        if (!P->mid)
            threshold += P->res / 192;
    }
    long cutoff = -1;
    const char * sc = ini_get(src, "sustain_cutoff_threshold");
    if (!sc || !parse_int(sc, &cutoff))
        cutoff = -1;
    if (cutoff < 0)
        cutoff = P->mid ? P->res / 3 : 0;

    OutNote * out = malloc((chords.n ? chords.n : 1) * sizeof(OutNote));
    int n = 0;
    bool have_prev = false;
    int32_t prev_tick = 0;
    uint8_t prev_mask = 0;
    for (int i = 0; i < chords.n; i++) {
        Chord * c = &chords.v[i];
        uint8_t mask = 0;
        int32_t length = INT32_MIN;
        for (int f = 0; f < 8; f++) {
            if (!(c->has & (1 << f)))
                continue;
            mask |= f == 7 ? OPEN : 1 << f;
            if (c->len[f] > length)
                length = c->len[f];
        }
        // an open note charted with frets: keep the frets (on drums this bit is the sixth pad)
        if ((mask & OPEN) && mask != OPEN && !(t >= P->tracks[PART_DRUMS] && t < P->tracks[PART_DRUMS + 1]))
            mask &= ~OPEN;
        if (length <= cutoff)
            length = 0;
        bool single = popcount(mask) == 1, natural = false;
        if (have_prev && single && c->tick - prev_tick <= threshold && mask != prev_mask)
            natural = !(popcount(prev_mask) > 1 && (mask & prev_mask));
        bool hopo;
        if (!P->mid) {
            bool forced = false;
            for (int k = 0; k < t->forced_ticks.n; k++)
                if (t->forced_ticks.v[k] == c->tick)
                    forced = true;
            hopo = natural != forced;
        } else {
            hopo = natural;
            for (int k = 0; k < t->forced.n; k++)
                if (t->forced.v[k].a <= c->tick && c->tick < t->forced.v[k].b)
                    hopo = t->forced.v[k].on;
        }
        uint8_t flags = 0;
        if (in_ranges(t->taps.v, t->taps.n, c->tick) && !(mask & OPEN))
            flags |= F_TAP;
        else if (hopo)
            flags |= F_HOPO;
        if (in_ranges(t->sp.v, t->sp.n, c->tick))
            flags |= F_SP;
        out[n].tick = c->tick;
        out[n].mask = mask;
        out[n].flags = flags;
        out[n].len = length;
        n++;
        have_prev = true;
        prev_tick = c->tick;
        prev_mask = mask;
    }
    FREE(chords);
    // the last note of each Star Power phrase completes it
    for (int k = 0; k < t->sp.n; k++) {
        int last = -1;
        for (int i = 0; i < n; i++)
            if (t->sp.v[k].a <= out[i].tick && out[i].tick < t->sp.v[k].b)
                last = i;
        if (last >= 0)
            out[last].flags |= F_SP_END;
    }
    // sustains stop short of the next note on the same frets (as Clone Hero trims them)
    for (int i = 0; i < n; i++) {
        if (!out[i].len)
            continue;
        for (int j = i + 1; j < n; j++) {
            if ((out[j].mask & out[i].mask) || out[j].mask == OPEN || out[i].mask == OPEN) {
                int32_t gap = out[j].tick - out[i].tick, room = gap - P->res / 16;
                if (out[i].len > room)
                    out[i].len = room > 0 ? room : 0;
                break;
            }
        }
    }
    *out_notes = out;
    return n;
}

// ------------------------------------------------------------------------------------------------- the tempo map

typedef struct {
    int32_t tick;
    double sec;
    int64_t us;
} TPoint;

typedef struct {
    TPoint * p;
    int n, res;
    double delay;
} Tempo;

static int tempo_cmp(const void * a, const void * b)
{
    const TempoIn * x = a, * y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    return x->us < y->us ? -1 : x->us > y->us;
}

static void tempo_build(Tempo * T, Parsed * P, double delay)
{
    qsort(P->tempos.v, P->tempos.n, sizeof(TempoIn), tempo_cmp);
    int extra = !P->tempos.n || P->tempos.v[0].tick != 0;
    T->n = P->tempos.n + extra;
    T->p = malloc(T->n * sizeof(TPoint));
    T->res = P->res;
    T->delay = delay;
    double sec = 0;
    int32_t last_tick = 0;
    int64_t last_us = extra ? 500000 : P->tempos.v[0].us;
    for (int i = 0; i < T->n; i++) {
        TempoIn t = extra && i == 0 ? (TempoIn){0, 500000} : P->tempos.v[i - extra];
        sec += (double)((int64_t)(t.tick - last_tick) * last_us) / 1e6 / P->res;
        T->p[i] = (TPoint){t.tick, sec, t.us};
        last_tick = t.tick;
        last_us = t.us;
    }
}

static double tempo_seconds(const Tempo * T, int64_t tick)
{
    int lo = 0, hi = T->n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (T->p[mid].tick <= tick)
            lo = mid;
        else
            hi = mid - 1;
    }
    const TPoint * p = &T->p[lo];
    return p->sec + (double)((tick - p->tick) * p->us) / 1e6 / T->res;
}

static int32_t tempo_us(const Tempo * T, int64_t tick)
{
    return (int32_t)rint((tempo_seconds(T, tick) + T->delay) * 1e6);
}

// ----------------------------------------------------------------------------------------------- notes.vhc out

typedef struct {
    uint8_t * d;
    size_t n, cap;
} Buf;

static void put(Buf * b, const void * p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->d = realloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void put32(Buf * b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, x, 4);
}

static void put16(Buf * b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(b, x, 2);
}

static int sig_cmp(const void * a, const void * b)
{
    const Sig * x = a, * y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    if (x->num != y->num)
        return x->num - y->num;
    return x->den - y->den;
}

static int name_cmp(const void * a, const void * b)
{
    const Name * x = a, * y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    return strcmp(x->text, y->text);
}

static int tr_cmp(const void * a, const void * b)
{
    const TR * x = a, * y = b;
    if (x->a != y->a)
        return x->a < y->a ? -1 : 1;
    return x->b < y->b ? -1 : x->b > y->b;
}

// sorted copies of the ranges that have notes of this chart in them
static int ranges_with_notes(const TR * r, int n, const OutNote * notes, int nn, TR ** out)
{
    *out = malloc((n ? n : 1) * sizeof(TR));
    int k = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < nn; j++)
            if (r[i].a <= notes[j].tick && notes[j].tick < r[i].b) {
                (*out)[k++] = r[i];
                break;
            }
    qsort(*out, k, sizeof(TR), tr_cmp);
    return k;
}

static int vox_cmp(const void * a, const void * b)
{
    const VoxIn * x = a, * y = b;
    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

static int range_cmp(const void * a, const void * b)
{
    const TR * x = a, * y = b;
    return x->a < y->a ? -1 : x->a > y->a;
}

// vocals.vhv, what vocals.c reads: "N <ms> <len ms> <pitch> <lyric>" and "P <ms> <end ms>"
static bool write_vocals(const char * folder, Parsed * P, const Tempo * T)
{
    char path[800];
    snprintf(path, sizeof(path), "%s/vocals.vhv", folder);
    FILE * f = fopen(path, "w");
    if (!f)
        return false;
    qsort(P->vox.v, P->vox.n, sizeof(VoxIn), vox_cmp);
    qsort(P->vox_phrases.v, P->vox_phrases.n, sizeof(TR), range_cmp);
    for (int i = 0; i < P->vox.n; i++) {
        const VoxIn * v = &P->vox.v[i];
        int t = tempo_us(T, v->tick) / 1000, e = tempo_us(T, v->tick + v->len) / 1000;
        fprintf(f, "N %d %d %d %s\n", t, e - t, v->pitch, v->text ? v->text : "");
    }
    for (int i = 0; i < P->vox_phrases.n; i++) {
        const TR * r = &P->vox_phrases.v[i];
        fprintf(f, "P %d %d\n", tempo_us(T, r->a) / 1000, tempo_us(T, r->b) / 1000);
    }
    return fclose(f) == 0;
}

static bool write_vhc(const char * path, Parsed * P, const Tempo * T, OutNote * notes[PART_COUNT][DIFF_COUNT],
                      int counts[PART_COUNT][DIFF_COUNT], double audio_len)
{
    int res = P->res;
    int64_t end_tick = 0;
    int built = 0;
    for (int p = 0; p < PART_COUNT; p++)
        for (int d = 0; d < DIFF_COUNT; d++)
            if (counts[p][d] > 0) {
                built++;
                OutNote * last = &notes[p][d][counts[p][d] - 1];
                if (last->tick + last->len > end_tick)
                    end_tick = last->tick + last->len;
            }
    // beat lines run to the end of the audio
    int64_t last_tick = end_tick + (int64_t)res * 8;
    while (tempo_seconds(T, last_tick) < audio_len + 2 && last_tick < end_tick + (int64_t)res * 4000)
        last_tick += (int64_t)res * 4;
    if (P->sigs.n)
        qsort(P->sigs.v, P->sigs.n, sizeof(Sig), sig_cmp);
    VEC(Sig) sigs = {0};
    if (!P->sigs.n || P->sigs.v[0].tick != 0) {
        Sig s = {0, 4, 4};
        PUSH(sigs, s);
    }
    for (int i = 0; i < P->sigs.n; i++)
        PUSH(sigs, P->sigs.v[i]);
    VEC(TR) beats = {0};                // tick, kind
    for (int i = 0; i < sigs.n; i++) {
        int64_t stop = i + 1 < sigs.n ? sigs.v[i + 1].tick : last_tick;
        int den = sigs.v[i].den > 1 ? sigs.v[i].den : 1, num = sigs.v[i].num > 1 ? sigs.v[i].num : 1;
        int64_t step = (int64_t)res * 4 / den;
        if (step < 1)
            step = 1;
        int64_t k = 0;
        for (int64_t t = sigs.v[i].tick; t < stop; t += step, k++) {
            TR b = {(int32_t)t, k % num == 0 ? 0 : 1};
            PUSH(beats, b);
        }
    }
    FREE(sigs);
    if (P->names.n)
        qsort(P->names.v, P->names.n, sizeof(Name), name_cmp);

    Buf b = {0};
    put(&b, "VHC1", 4);
    put32(&b, 2);
    put32(&b, T->n);
    put32(&b, beats.n);
    put32(&b, P->names.n);
    put32(&b, built);
    put32(&b, (uint32_t)(int64_t)(audio_len * 1000));
    for (int i = 0; i < T->n; i++) {
        put32(&b, (uint32_t)tempo_us(T, T->p[i].tick));
        put32(&b, (uint32_t)T->p[i].us);
    }
    for (int i = 0; i < beats.n; i++) {
        put32(&b, (uint32_t)tempo_us(T, beats.v[i].a));
        uint8_t k[4] = {(uint8_t)beats.v[i].b, 0, 0, 0};
        put(&b, k, 4);
    }
    FREE(beats);
    for (int i = 0; i < P->names.n; i++) {
        put32(&b, (uint32_t)tempo_us(T, P->names.v[i].tick));
        char label[60];
        memset(label, 0, sizeof(label));
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", P->names.v[i].text);
        for (char * q = tmp; *q; q++)
            if (*q == '_')
                *q = ' ';
        char * t = trim(tmp);
        size_t len = strlen(t);
        memcpy(label, t, len < 59 ? len : 59);
        put(&b, label, 60);
    }
    for (int p = 0; p < PART_COUNT; p++)
        for (int d = 0; d < DIFF_COUNT; d++) {
            int nn = counts[p][d];
            if (nn <= 0)
                continue;
            Track * t = &P->tracks[p][d];
            TR * sp, * solos;
            int nsp = ranges_with_notes(t->sp.v, t->sp.n, notes[p][d], nn, &sp);
            int nsolo = ranges_with_notes(t->solos.v, t->solos.n, notes[p][d], nn, &solos);
            uint8_t h[4] = {(uint8_t)p, (uint8_t)d, 0, 0};
            put(&b, h, 4);
            put32(&b, nn);
            put32(&b, nsp);
            put32(&b, nsolo);
            for (int i = 0; i < nn; i++) {
                OutNote * x = &notes[p][d][i];
                int32_t start = tempo_us(T, x->tick);
                int64_t end = x->len ? (int64_t)tempo_us(T, (int64_t)x->tick + x->len) - start : 0;
                int32_t ln = end < 0 || end > INT32_MAX ? 0 : (int32_t)end;
                put32(&b, (uint32_t)start);
                put32(&b, (uint32_t)ln);
                uint8_t mf[2] = {x->mask, x->flags};
                put(&b, mf, 2);
                double beats100 = rint((double)x->len * 100 / res);
                put16(&b, (uint16_t)(beats100 > 65535 ? 65535 : beats100 < 0 ? 0 : beats100));
            }
            for (int i = 0; i < nsp; i++) {
                put32(&b, (uint32_t)tempo_us(T, sp[i].a));
                put32(&b, (uint32_t)tempo_us(T, sp[i].b));
            }
            for (int i = 0; i < nsolo; i++) {
                put32(&b, (uint32_t)tempo_us(T, solos[i].a));
                put32(&b, (uint32_t)tempo_us(T, solos[i].b));
            }
            free(sp);
            free(solos);
        }
    FILE * f = fopen(path, "wb");
    bool ok = f && fwrite(b.d, 1, b.n, f) == b.n;
    if (f && fclose(f) != 0)
        ok = false;
    free(b.d);
    return ok;
}

// ------------------------------------------------------------------------------------------------------ the cover

bool songconv_cover(const uint8_t * data, size_t size, const char * path)
{
    int w, h, n;
    uint8_t * px = stbi_load_from_memory(data, (int)size, &w, &h, &n, 3);
    if (!px)
        return false;
    const int S = 256;
    uint8_t * out = malloc(S * S * 3);
    // each output pixel averages the source pixels it covers (or picks the nearest one when growing)
    for (int oy = 0; oy < S; oy++) {
        int y0 = oy * h / S, y1 = (oy + 1) * h / S;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int ox = 0; ox < S; ox++) {
            int x0 = ox * w / S, x1 = (ox + 1) * w / S;
            if (x1 <= x0)
                x1 = x0 + 1;
            unsigned sum[3] = {0, 0, 0}, cnt = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const uint8_t * p = px + (y * w + x) * 3;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    cnt++;
                }
            for (int c = 0; c < 3; c++)
                out[(oy * S + ox) * 3 + c] = (uint8_t)(sum[c] / cnt);
        }
    }
    stbi_image_free(px);
    bool ok = stbi_write_jpg(path, S, S, 3, out, 88) != 0;
    free(out);
    return ok;
}

// ------------------------------------------------------------------------------------------------------ install

static bool is_dir(const char * path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char * path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static const char * ext_of(const char * name)
{
    const char * dot = strrchr(name, '.');
    return dot ? dot : "";
}

bool songconv_is_raw(const char * folder)
{
    char p[700];
    snprintf(p, sizeof(p), "%s/ghvita.ini", folder);
    if (exists(p))
        return false;
    // pack.py's own folders have notes.vhc and a song.ini of theirs
    snprintf(p, sizeof(p), "%s/notes.vhc", folder);
    if (exists(p))
        return false;
    DIR * d = opendir(folder);
    if (!d)
        return false;
    bool chart = false;
    struct dirent * e;
    while ((e = readdir(d)))
        if (strcasecmp(e->d_name, "notes.chart") == 0 || strcasecmp(e->d_name, "notes.mid") == 0)
            chart = true;
    closedir(d);
    return chart;
}

// the ghvita.ini source line of an installed song
static bool same_source(const char * folder, const char * id)
{
    if (!id)
        return false;
    char p[700], line[400];
    snprintf(p, sizeof(p), "%s/ghvita.ini", folder);
    FILE * f = fopen(p, "r");
    if (!f)
        return false;
    bool same = false;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "source = ", 9) == 0 && strcmp(trim(line + 9), id) == 0)
            same = true;
    fclose(f);
    return same;
}

static void empty_folder(const char * folder)
{
    DIR * d = opendir(folder);
    if (!d)
        return;
    struct dirent * e;
    char p[700];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s", folder, e->d_name);
        if (!is_dir(p))
            remove(p);
    }
    closedir(d);
}

bool songconv_install(const char * src_path, const char * dest_root, const ConvOptions * opt, char * out,
                      size_t out_size, char * err, size_t err_size)
{
    ConvOptions defaults = {0};
    if (!opt)
        opt = &defaults;
    Source S;
    memset(&S, 0, sizeof(S));
    snprintf(S.path, sizeof(S.path), "%s", src_path);
    S.sng = !is_dir(src_path);
    if (!(S.sng ? open_sng(&S) : open_folder(&S))) {
        snprintf(err, err_size, S.sng ? "not a .sng package" : "can't read the folder");
        src_free(&S);
        return false;
    }
    Parsed P;
    memset(&P, 0, sizeof(P));
    bool ok = true;
    size_t n;
    uint8_t * raw;
    if ((raw = src_read(&S, "notes.mid", &n))) {
        ok = parse_mid(raw, n, &S, &P, err, err_size);
        free(raw);
    } else if ((raw = src_read(&S, "notes.chart", &n))) {
        char * text = to_utf8(raw, n);
        free(raw);
        ok = parse_chart(text, &P);
        free(text);
    } else {
        snprintf(err, err_size, "no chart in it");
        ok = false;
    }
    bool any = false;
    for (int p = 0; ok && p < PART_COUNT; p++)
        for (int d = 0; d < DIFF_COUNT; d++)
            any |= P.tracks[p][d].present;
    any |= P.vox.n > 0;
    if (ok && !any) {
        snprintf(err, err_size, "no chart for any instrument in it");
        ok = false;
    }
    // the stems: one file per name, Vorbis first (pack.py's order)
    char stems[MAX_STEMS][64];
    int nstems = 0;
    for (int i = 0; ok && i < S.files.n; i++) {
        const char * name = S.files.v[i].name, * ext = ext_of(name);
        int rank = -1;
        for (int k = 0; k < 4; k++)
            if (strcmp(ext, audio_ext[k]) == 0)
                rank = k;
        size_t base_len = ext - name;
        bool known = false;
        for (size_t k = 0; k < sizeof(stem_names) / sizeof(stem_names[0]); k++)
            known |= strlen(stem_names[k]) == base_len && strncmp(name, stem_names[k], base_len) == 0;
        if (rank < 0 || !known)
            continue;
        int have = -1;
        for (int k = 0; k < nstems; k++)
            if (strlen(stems[k]) - strlen(ext_of(stems[k])) == base_len && strncmp(stems[k], name, base_len) == 0)
                have = k;
        if (have >= 0) {
            int old = 0;
            for (int k = 0; k < 4; k++)
                if (strcmp(ext_of(stems[have]), audio_ext[k]) == 0)
                    old = k;
            if (rank < old)
                snprintf(stems[have], sizeof(stems[have]), "%s", name);
        } else if (nstems < MAX_STEMS)
            snprintf(stems[nstems++], sizeof(stems[0]), "%s", name);
    }
    if (ok && !nstems) {
        snprintf(err, err_size, "no audio it can play (Vorbis, Opus, MP3 or WAV)");
        ok = false;
    }
    if (!ok) {
        parsed_free(&P);
        src_free(&S);
        return false;
    }

    // where it goes
    char name[128], artist[128], folder_name[200], folder[700];
    clean(name, sizeof(name), ini_get(&S, "name"));
    clean(artist, sizeof(artist), ini_get(&S, "artist"));
    if (!*name) {
        const char * b = strrchr(src_path, '/');
        snprintf(name, sizeof(name), "%s", b ? b + 1 : src_path);
        if (S.sng && strlen(name) > 4 && strcasecmp(name + strlen(name) - 4, ".sng") == 0)
            name[strlen(name) - 4] = 0;
    }
    if (!*artist)
        snprintf(artist, sizeof(artist), "Unknown Artist");
    if (opt->in_place)
        snprintf(folder, sizeof(folder), "%s", src_path);
    else {
        char both[300];
        snprintf(both, sizeof(both), "%s - %s", artist, name);
        songconv_safe_name(folder_name, sizeof(folder_name), both);
        snprintf(folder, sizeof(folder), "%s/%s", dest_root, folder_name);
        // another chart of the same song: a folder of its own
        for (int k = 2; is_dir(folder) && !same_source(folder, opt->source_id) && k < 100; k++)
            snprintf(folder, sizeof(folder), "%s/%s (%d)", dest_root, folder_name, k);
        if (!is_dir(folder))
            mkdir(folder, 0777);
        else if (same_source(folder, opt->source_id))
            empty_folder(folder);           // this chart again: a fresh copy
        else
            folder[0] = 0;
        if (!folder[0] || !is_dir(folder)) {
            snprintf(err, err_size, "can't make the song's folder");
            parsed_free(&P);
            src_free(&S);
            return false;
        }
    }

    // the audio into the folder (a .sng's unpacked, a folder's moved or copied; in place they keep their names)
    for (int i = 0; i < nstems && ok; i++) {
        const SrcFile * sf = src_find(&S, stems[i]);
        if (opt->in_place) {
            const char * b = strrchr(sf->path, '/');
            snprintf(stems[i], sizeof(stems[i]), "%s", b ? b + 1 : sf->path);
            continue;
        }
        char dest[800];
        snprintf(dest, sizeof(dest), "%s/%s", folder, stems[i]);
        if (S.sng)
            ok = src_extract(&S, sf, dest);
        else {
            ok = opt->move && rename(sf->path, dest) == 0;
            if (!ok)
                ok = copy_file(sf->path, dest);
        }
        if (!ok)
            snprintf(err, err_size, "couldn't write %s (card full?)", stems[i]);
    }
    // stems that can't be read are left out (and the song with them if none can)
    double audio_len = 0;
    for (int i = 0; i < nstems && ok; i++) {
        char p[800];
        snprintf(p, sizeof(p), "%s/%s", folder, stems[i]);
        double s = dec_file_seconds(p);
        if (s <= 0) {
            plat_log("songconv: %s can't be played, left out", p);
            memmove(stems[i], stems[i + 1], (nstems - i - 1) * sizeof(stems[0]));
            nstems--;
            i--;
            continue;
        }
        if (s > audio_len)
            audio_len = s;
    }
    if (ok && audio_len <= 0) {
        snprintf(err, err_size, "its audio files can't be read");
        ok = false;
    }

    // the chart
    Tempo T = {0};
    OutNote * notes[PART_COUNT][DIFF_COUNT];
    int counts[PART_COUNT][DIFF_COUNT];
    memset(notes, 0, sizeof(notes));
    memset(counts, 0, sizeof(counts));
    if (ok) {
        tempo_build(&T, &P, ms_value(ini_get(&S, "delay"), 0) / 1000.0);
        for (int p = 0; p < PART_COUNT; p++)
            for (int d = 0; d < DIFF_COUNT; d++)
                if (P.tracks[p][d].present)
                {
                    counts[p][d] = build_notes(&P, &P.tracks[p][d], &S, &notes[p][d]);
                    // drums use the guitar's note numbers, kick where green is: pads on the first lanes, the kick is
                    // the open bar. Every hit is struck: no sustains, no hammer-ons
                    const Track * dt = &P.tracks[p][d];
                    const char * pd = ini_get(&S, "pro_drums");
                    bool pro = (pd && strchr("Tt1", *pd)) || P.toms[0].n + P.toms[1].n + P.toms[2].n + dt->cyms.n;
                    for (int i = 0; p == PART_DRUMS && i < counts[p][d]; i++) {
                        OutNote * o = &notes[p][d][i];
                        o->mask = (o->mask >> 1 & 0x1f) | (o->mask & 1 ? OPEN : 0);
                        o->flags &= ~(F_HOPO | F_TAP);
                        o->len = 0;
                        // cymbals unless the chart says otherwise (.chart marks the cymbals instead). A chart that
                        // doesn't tell them apart: blue's a tom
                        for (int k = 0; k < 3; k++) {
                            bool tom = P.mid ? in_ranges(P.toms[k].v, P.toms[k].n, o->tick) : dt->cyms.n > 0;
                            for (int c = 0; !P.mid && c < dt->cyms.n; c++)
                                if (dt->cyms.v[c].a == o->tick && dt->cyms.v[c].b == k)
                                    tom = false;
                            if (tom)
                                o->flags |= NOTE_TOM_Y << k;
                        }
                        if (!pro)
                            o->flags |= NOTE_TOM_B;
                        // as YARG plays one: red's a hi-hat on yellow, yellow the snare on red
                        if (in_ranges(P.disco[d].v, P.disco[d].n, o->tick)) {
                            o->mask = (o->mask & ~3) | (o->mask & 1) << 1 | (o->mask >> 1 & 1);
                            o->flags &= ~NOTE_TOM_Y;
                        }
                    }
                }
        char path[800];
        snprintf(path, sizeof(path), "%s/notes.vhc", folder);
        ok = write_vhc(path, &P, &T, notes, counts, audio_len);
        if (ok && P.vox.n)
            ok = write_vocals(folder, &P, &T);
        if (!ok)
            snprintf(err, err_size, "couldn't write the chart (card full?)");
    }

    // the cover
    if (ok) {
        static const char * const covers[] = {"album.png", "album.jpg", "album.jpeg"};
        for (int i = 0; i < 3; i++) {
            if (!(raw = src_read(&S, covers[i], &n)))
                continue;
            char path[800];
            snprintf(path, sizeof(path), "%s/album.jpg", folder);
            if (!songconv_cover(raw, n, path))
                plat_log("songconv: can't read %s's %s", name, covers[i]);
            free(raw);
            break;
        }
    }

    // what the song list reads
    if (ok) {
        bool parts[PART_COUNT] = {false};
        for (int p = 0; p < PART_COUNT; p++)
            for (int d = 0; d < DIFF_COUNT; d++)
                if (P.tracks[p][d].present)
                    parts[p] = true;
        parts[PART_VOCALS] = P.vox.n > 0;
        // the stem each part mutes on a miss (Clone Hero puts the bass on rhythm when there's no rhythm part)
        int part_stem[PART_COUNT];
        for (int p = 0; p < PART_COUNT; p++) {
            part_stem[p] = -1;
            if (!parts[p])
                continue;
            const char * want = part_stems[p];
            bool has_bass = false, has_rhythm = false;
            for (int i = 0; i < nstems; i++) {
                size_t bl = strlen(stems[i]) - strlen(ext_of(stems[i]));
                has_bass |= bl == 4 && strncasecmp(stems[i], "bass", 4) == 0;
                has_rhythm |= bl == 6 && strncasecmp(stems[i], "rhythm", 6) == 0;
            }
            if (p == PART_BASS && !has_bass && has_rhythm && !parts[PART_RHYTHM])
                want = "rhythm";
            for (int i = 0; i < nstems; i++) {
                size_t bl = strlen(stems[i]) - strlen(ext_of(stems[i]));
                if (bl == strlen(want) && strncasecmp(stems[i], want, bl) == 0)
                    part_stem[p] = i;
            }
        }
        char path[800];
        snprintf(path, sizeof(path), "%s/ghvita.ini", folder);
        FILE * f = fopen(path, "w");
        if (f) {
            char album[128], genre[64], year[32], charter[96];
            clean(album, sizeof(album), ini_get(&S, "album"));
            clean(genre, sizeof(genre), ini_get(&S, "genre"));
            clean(year, sizeof(year), ini_get(&S, "year"));
            const char * y = year;
            while (*y == ',' || *y == ' ')
                y++;
            const char * ch = ini_get(&S, "charter");
            clean(charter, sizeof(charter), ch && *ch ? ch : ini_get(&S, "frets"));
            long preview = ms_value(ini_get(&S, "preview_start_time"), -1);
            if (preview <= 0)
                preview = (long)(audio_len * 300);         // 30% in
            fprintf(f, "[song]\nname = %s\nartist = %s\nalbum = %s\ngenre = %s\nyear = %s\ncharter = %s\n"
                       "song_length = %d\npreview_start_time = %ld\nparts = ", name, artist, album, genre, y,
                    charter, (int)(audio_len * 1000), preview);
            bool first = true;
            for (int p = 0; p < PART_COUNT; p++)
                if (parts[p]) {
                    fprintf(f, "%s%s", first ? "" : ",", part_keys[p]);
                    first = false;
                }
            // the stems no part mutes first, as pack.py's backing
            fprintf(f, "\nstems = ");
            first = true;
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < nstems; i++) {
                    bool own = false;
                    for (int p = 0; p < PART_COUNT; p++)
                        own |= part_stem[p] == i;
                    if (own == (pass == 1)) {
                        fprintf(f, "%s%s", first ? "" : ",", stems[i]);
                        first = false;
                    }
                }
            fprintf(f, "\n");
            for (int p = 0; p < PART_COUNT; p++)
                if (part_stem[p] >= 0)
                    fprintf(f, "stem_%d = %s\n", p, stems[part_stem[p]]);
            static const char * const diff_keys[PART_COUNT] = {"diff_guitar", "diff_bass", "diff_rhythm",
                                                               "diff_guitar_coop", "diff_keys", "diff_drums", "diff_vocals"};
            for (int p = 0; p < PART_COUNT; p++) {
                long v = ms_value(ini_get(&S, diff_keys[p]), -1);
                if (v >= 0)
                    fprintf(f, "%s = %ld\n", diff_keys[p], v);
            }
            if (opt->source_id)
                fprintf(f, "source = %s\n", opt->source_id);
            fprintf(f, "chart = %d\n", CHART_VERSION);
            ok = fclose(f) == 0;
        } else
            ok = false;
        if (!ok)
            snprintf(err, err_size, "couldn't write ghvita.ini (card full?)");
    }
    for (int p = 0; p < PART_COUNT; p++)
        for (int d = 0; d < DIFF_COUNT; d++)
            free(notes[p][d]);
    free(T.p);
    parsed_free(&P);
    src_free(&S);
    if (ok)
        snprintf(out, out_size, "%s", folder);
    return ok;
}
