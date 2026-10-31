#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "rbcon.h"
#include "moggdec.h"
#include "shrink.h"

#define BLOCK 0x1000
#define MAX_FILES 512

typedef struct {
    char name[42];
    bool consecutive, dir;
    uint32_t first, blocks, size;
    int parent;
} Entry;

typedef struct {
    FILE * f;
    uint32_t base;
    int shift;
    Entry e[MAX_FILES];
    int n;
} Con;

static uint32_t be32(const uint8_t * p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint32_t le24(const uint8_t * p)
{
    return p[0] | p[1] << 8 | (uint32_t)p[2] << 16;
}

// data blocks are numbered without the hash tables between them
static long block_at(const Con * c, uint32_t b)
{
    uint32_t r = b + (((b + 0xAA) / 0xAA) << c->shift);
    if (b >= 0xAA)
        r += ((b + 0x70E4) / 0x70E4) << c->shift;
    if (b >= 0x70E4)
        r += ((b + 0x4AF768) / 0x4AF768) << c->shift;
    return (long)c->base + ((long)r << 12);
}

// the block after b, from its hash table
static bool next_block(const Con * c, uint32_t b, uint32_t * next)
{
    uint32_t step = 0xAB + (c->shift ? 1 : 0), tab = b / 0xAA * step;
    if (b >= 0xAA)
        tab += (b / 0x70E4 + 1) << c->shift;
    if (b >= 0x70E4)
        tab += 1u << c->shift;
    uint8_t h[3];
    long off = (long)c->base + ((long)tab << 12) + (b % 0xAA) * 0x18 + 0x15;
    if (fseek(c->f, off, SEEK_SET) || fread(h, 1, 3, c->f) != 3)
        return false;
    *next = (uint32_t)h[0] << 16 | h[1] << 8 | h[2];
    return true;
}

static bool read_table(Con * c, uint32_t start, int blocks)
{
    uint32_t b = start;
    for (int i = 0; i < blocks; i++, b++) {
        static uint8_t t[BLOCK];
        if (fseek(c->f, block_at(c, b), SEEK_SET) || fread(t, 1, BLOCK, c->f) != BLOCK)
            return false;
        for (int k = 0; k < BLOCK / 0x40; k++) {
            const uint8_t * p = t + k * 0x40;
            int len = p[0x28] & 0x3F;
            if (!p[0] || !len || len > 40)
                continue;
            if (c->n == MAX_FILES)
                return true;
            Entry * e = &c->e[c->n++];
            memcpy(e->name, p, len);
            e->name[len] = 0;
            e->consecutive = p[0x28] & 0x40;
            e->dir = p[0x28] & 0x80;
            e->blocks = le24(p + 0x29);
            e->first = le24(p + 0x2F);
            e->parent = (int16_t)(p[0x32] << 8 | p[0x33]);
            e->size = be32(p + 0x34);
        }
    }
    return true;
}

// a file's bytes from skip on, into out or a new buffer
typedef void (*XorFn)(void *, uint8_t *, size_t, uint32_t);

static bool extract_ex(const Con * c, const Entry * e, FILE * out, uint8_t * mem, uint32_t skip, uint32_t limit,
                       XorFn fn, void * ctx)
{
    static uint8_t buf[BLOCK];
    uint32_t b = e->first, left = e->size, pos = 0;
    while (left) {
        uint32_t want = left < BLOCK ? left : BLOCK;
        if (fseek(c->f, block_at(c, b), SEEK_SET) || fread(buf, 1, want, c->f) != want)
            return false;
        if (fn)
            fn(ctx, buf, want, pos);
        if (mem) {
            uint32_t k = pos + want > limit ? (pos < limit ? limit - pos : 0) : want;
            memcpy(mem + pos, buf, k);
        } else if (pos + want > skip) {
            uint32_t from = pos < skip ? skip - pos : 0;
            if (fwrite(buf + from, 1, want - from, out) != want - from)
                return false;
        }
        pos += want;
        left -= want;
        if (left) {
            if (e->consecutive)
                b++;
            else if (!next_block(c, b, &b))
                return false;
        }
    }
    return true;
}

static bool extract(const Con * c, const Entry * e, FILE * out, uint8_t * mem, uint32_t skip, uint32_t limit)
{
    return extract_ex(c, e, out, mem, skip, limit, NULL, NULL);
}

static const Entry * find_ext(const Con * c, const char * ext, const char * stem)
{
    size_t el = strlen(ext);
    for (int i = 0; i < c->n; i++) {
        const Entry * e = &c->e[i];
        size_t l = strlen(e->name);
        if (e->dir || l <= el || strcasecmp(e->name + l - el, ext))
            continue;
        if (!stem || (strlen(stem) == l - el && !strncasecmp(e->name, stem, l - el)))
            return e;
    }
    return NULL;
}

// a quoted or bare value after a key of the song's entry: (name "Waltz #2")
static void dta_value(const char * s, const char * end, const char * key, char * out, size_t size)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char * p = s; p && p < end; p = strchr(p + 1, '(')) {
        const char * q = p + 1;
        while (q < end && (isspace((unsigned char)*q) || *q == '\''))
            q++;
        if (strncmp(q, key, kl) != 0)
            continue;
        q += kl;
        if (*q == '\'')
            q++;
        if (!isspace((unsigned char)*q))
            continue;
        while (q < end && isspace((unsigned char)*q))
            q++;
        size_t k = 0;
        if (*q == '\'')
            q++;
        if (*q == '"') {
            for (q++; q < end && *q != '"' && k + 1 < size; q++)
                out[k++] = *q;
        } else
            for (; q < end && !isspace((unsigned char)*q) && *q != ')' && *q != '\'' && k + 1 < size; q++)
                out[k++] = *q;
        out[k] = 0;
        // a song's (name "songs/x/x") isn't its title
        if (strncmp(out, "songs/", 6) != 0)
            return;
        out[0] = 0;
    }
}

// the numbers after a key: ('pans' (-1.0 1.0)), ('crowd_channels' 12 13)
static int dta_numbers(const char * s, const char * end, const char * key, float * out, int max)
{
    size_t kl = strlen(key);
    for (const char * p = s; p && p + kl < end; p = strstr(p + 1, key)) {
        const char * q = strstr(p, key);
        if (!q || q + kl >= end)
            return 0;
        q += kl;
        if (*q == '\'')
            q++;
        while (q < end && (isspace((unsigned char)*q) || *q == '('))
            q++;
        int n = 0;
        while (q < end && n < max && (isdigit((unsigned char)*q) || *q == '-' || *q == '.')) {
            char * e;
            out[n++] = strtof(q, &e);
            q = e;
            while (q < end && isspace((unsigned char)*q))
                q++;
        }
        if (n)
            return n;
    }
    return 0;
}

// the .mogg is one track per instrument side: how they mix, for the decoder. The crowd stays, behind the band
static bool write_mix(const char * path, const char * s, const char * end)
{
    float pans[64], vols[64], crowd[8];
    int n = dta_numbers(s, end, "pans", pans, 64), nv = dta_numbers(s, end, "vols", vols, 64);
    int nc = dta_numbers(s, end, "crowd_channels", crowd, 8);
    if (!n)
        return true;
    FILE * f = fopen(path, "w");
    if (!f)
        return false;
    for (int c = 0; c < n; c++) {
        float g = powf(10, (c < nv ? vols[c] : 0) / 20), p = pans[c] < -1 ? -1 : pans[c] > 1 ? 1 : pans[c];
        for (int k = 0; k < nc; k++)
            if ((int)crowd[k] == c)
                g *= 0.6f;
        fprintf(f, "%d %d\n", (int)(g * sqrtf((1 - p) / 2) * 3480), (int)(g * sqrtf((1 + p) / 2) * 3480));
    }
    // which tracks are which instrument: tools/shrink_songs.py splits them by this
    static const char * const names[5][2] = {{"drum", "drums"}, {"bass", "bass"}, {"guitar", "guitar"},
                                             {"keys", "keys"}, {"vocals", "vocals"}};
    const char * pan_at = strstr(s, "pans"), * tracks = strstr(s, "tracks");
    while (tracks && !strncmp(tracks, "tracks_count", 12))
        tracks = strstr(tracks + 1, "tracks");
    if (pan_at && pan_at < end && tracks && tracks < pan_at)
        for (int i = 0; i < 5; i++) {
            float ch[64];
            int m = dta_numbers(tracks, pan_at, names[i][0], ch, 64);
            if (!m)
                continue;
            fprintf(f, "part %s", names[i][1]);
            for (int k = 0; k < m; k++)
                fprintf(f, " %d", (int)ch[k]);
            fprintf(f, "\n");
        }
    return !fclose(f);
}

static void clean_name(char * s)
{
    for (; *s; s++)
        if (strchr("/\\:*?\"<>|", *s))
            *s = '_';
}

static bool unpack_song(Con * c, const Entry * mogg, const char * dta, size_t dta_len, const char * dest, int index,
                        char * err, size_t err_size)
{
    char stem[42];
    snprintf(stem, sizeof(stem), "%s", mogg->name);
    stem[strlen(stem) - 5] = 0;
    const Entry * mid = find_ext(c, ".mid", stem);
    if (!mid)
        mid = find_ext(c, ".mid", NULL);
    if (!mid)
        return false;
    // the header of a .mogg: its version, where the Ogg starts. Past 10 the audio is encrypted
    uint8_t head[20];
    if (!extract(c, mogg, NULL, head, 0, sizeof(head)))
        return false;
    uint32_t version = head[0] | head[1] << 8 | head[2] << 16 | (uint32_t)head[3] << 24;
    uint32_t ogg = head[4] | head[5] << 8 | head[6] << 16 | (uint32_t)head[7] << 24;
    if (ogg >= mogg->size) {
        snprintf(err, err_size, "its audio header is broken");
        return false;
    }
    bool encrypted = version != 10;
    MoggCipher cipher;
    uint8_t * keys = NULL;
    if (encrypted) {
        uint32_t hmx = head[16] | head[17] << 8 | head[18] << 16 | (uint32_t)head[19] << 24;
        size_t need = 20 + (size_t)hmx * 8 + 80;
        if (version > 17 || hmx > 0x10000 || need > mogg->size) {
            snprintf(err, err_size, "its audio is encrypted in a way this can't read");
            return false;
        }
        keys = malloc(need);
        if (!keys || !extract(c, mogg, NULL, keys, 0, (uint32_t)need)) {
            free(keys);
            snprintf(err, err_size, "couldn't read its audio header");
            return false;
        }
        if (!mogg_cipher_init(&cipher, keys, need, err, err_size)) {
            free(keys);
            return false;
        }
    }
    char title[96] = "", artist[96] = "", album[96] = "", genre[48] = "", year[12] = "", author[64] = "";
    const char * mix_s = NULL, * mix_end = NULL;
    float preview[2] = {-1, 0};
    int tier[5] = {-1, -1, -1, -1, -1};
    if (dta) {
        // this song's part of songs.dta: from the entry that names it to the next at the left margin
        char want[100];
        snprintf(want, sizeof(want), "songs/%s/%s", stem, stem);
        const char * at = strstr(dta, want), * s = dta, * end = dta + dta_len;
        if (at) {
            for (const char * p = at; p > dta; p--)
                if (p[-1] == '\n' && *p == '(') {
                    s = p;
                    break;
                }
            const char * n = strstr(at, "\n(");
            end = n ? n : end;
        }
        mix_s = s;
        mix_end = end;
        dta_value(s, end, "name", title, sizeof(title));
        dta_value(s, end, "artist", artist, sizeof(artist));
        dta_value(s, end, "album_name", album, sizeof(album));
        dta_value(s, end, "genre", genre, sizeof(genre));
        dta_value(s, end, "year_released", year, sizeof(year));
        dta_value(s, end, "author", author, sizeof(author));
        dta_numbers(s, end, "preview", preview, 2);
        // how hard each part is: Rock Band's ranks in the game's own steps
        static const char * const names[5] = {"guitar", "bass", "keys", "drum", "vocals"};
        static const short steps[5][6] = {{139, 176, 221, 267, 333, 409}, {135, 181, 228, 293, 364, 436},
                                          {153, 211, 269, 327, 385, 443}, {124, 151, 178, 242, 345, 448},
                                          {132, 175, 218, 279, 353, 427}};
        const char * rank = strstr(s, "'rank'");
        rank = rank ? rank : strstr(s, "(rank");
        for (int p = 0; rank && rank < end && p < 5; p++) {
            float v;
            if (dta_numbers(rank, end, names[p], &v, 1) && v > 0)
                for (tier[p] = 0; tier[p] < 6 && v >= steps[p][tier[p]]; tier[p]++)
                    ;
        }
    }
    char folder[600], path[700];
    snprintf(path, sizeof(path), "%s", artist[0] ? artist : "Rock Band");
    clean_name(path);
    char safe[96];
    snprintf(safe, sizeof(safe), "%s", title[0] ? title : stem);
    clean_name(safe);
    snprintf(folder, sizeof(folder), "%s/%d %.100s - %.90s", dest, index, path, safe);
    mkdir(dest, 0777);
    mkdir(folder, 0777);
    snprintf(path, sizeof(path), "%s/song.ogg", folder);
    FILE * f = fopen(path, encrypted ? "wb+" : "wb");
    bool ok = f && (encrypted ? extract_ex(c, mogg, f, NULL, ogg, 0, (XorFn)mogg_cipher_xor, &cipher)
                              : extract(c, mogg, f, NULL, ogg, 0));
    bool cipher_err = false;
    if (ok && encrypted) {
        uint8_t first[64];
        ok = fseek(f, 0, SEEK_SET) == 0 && fread(first, 1, sizeof(first), f) == sizeof(first) &&
             mogg_fix_ogg(&cipher, first, sizeof(first), err, err_size);
        if (ok)
            ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(first, 1, sizeof(first), f) == sizeof(first);
        cipher_err = !ok;
    }
    free(keys);
    if (f)
        ok = !fclose(f) && ok;
    snprintf(path, sizeof(path), "%s/song.mix", folder);
    if (ok && mix_s)
        ok = write_mix(path, mix_s, mix_end);
    snprintf(path, sizeof(path), "%s/notes.mid", folder);
    f = ok ? fopen(path, "wb") : NULL;
    ok = f && extract(c, mid, f, NULL, 0, 0);
    if (f)
        ok = !fclose(f) && ok;
    snprintf(path, sizeof(path), "%s/song.ini", folder);
    f = ok ? fopen(path, "w") : NULL;
    if (f) {
        fprintf(f, "[song]\nname = %s\nartist = %s\nalbum = %s\ngenre = %s\nyear = %s\ncharter = %s\n", title[0] ? title : stem,
                artist, album, genre, year, author[0] ? author : "Rock Band");
        if (preview[0] >= 0)
            fprintf(f, "preview_start_time = %d\n", (int)preview[0]);
        fprintf(f, "pro_drums = True\n");
        static const char * const keys[5] = {"diff_guitar", "diff_bass", "diff_keys", "diff_drums", "diff_vocals"};
        for (int p = 0; p < 5; p++)
            if (tier[p] >= 0)
                fprintf(f, "%s = %d\n", keys[p], tier[p]);
        ok = !fclose(f);
    } else
        ok = false;
    if (!ok) {
        if (!cipher_err)
            snprintf(err, err_size, "couldn't write the song (card full?)");
    } else
        shrink_mogg(folder);
    return ok;
}

int rbcon_unpack(const char * path, const char * dest, char * err, size_t err_size)
{
    Con * c = calloc(1, sizeof(Con));
    if (!c)
        return 0;
    int done = 0;
    c->f = fopen(path, "rb");
    uint8_t h[0x400];
    if (!c->f || fread(h, 1, sizeof(h), c->f) != sizeof(h)) {
        snprintf(err, err_size, "couldn't read the package");
        goto out;
    }
    uint32_t header = be32(h + 0x340);
    c->base = (header + 0xFFF) & ~0xFFFu;
    int blocks = h[0x37C] | h[0x37D] << 8;
    uint32_t start = le24(h + 0x37E);
    // packages differ in hash tables per stretch of blocks: whichever gives a file table that reads
    for (c->shift = 0; c->shift < 2; c->shift++) {
        c->n = 0;
        if (read_table(c, start, blocks) && find_ext(c, ".mogg", NULL))
            break;
    }
    if (c->shift == 2) {
        snprintf(err, err_size, "no songs in the package");
        goto out;
    }
    char * dta = NULL;
    size_t dta_len = 0;
    for (int i = 0; i < c->n && !dta; i++)
        if (!strcasecmp(c->e[i].name, "songs.dta") && c->e[i].size < 4 * 1024 * 1024) {
            dta_len = c->e[i].size;
            dta = calloc(dta_len + 1, 1);
            if (dta && !extract(c, &c->e[i], NULL, (uint8_t *)dta, 0, (uint32_t)dta_len)) {
                free(dta);
                dta = NULL;
            }
        }
    for (int i = 0; i < c->n; i++) {
        size_t l = strlen(c->e[i].name);
        if (!c->e[i].dir && l > 5 && !strcasecmp(c->e[i].name + l - 5, ".mogg") &&
            unpack_song(c, &c->e[i], dta, dta_len, dest, done, err, err_size))
            done++;
    }
    free(dta);
out:
    if (c->f)
        fclose(c->f);
    free(c);
    return done;
}
