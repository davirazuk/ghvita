#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __vita__
#include <vitaGL.h>
#include <psp2/gxm.h>
#elif defined(__ANDROID__) || defined(GHV_GLES)
// a phone's OpenGL ES 1.1: the same fixed-function calls
#include <GLES/gl.h>
#define GHV_ES 1
#define glOrtho glOrthof
#else
#include <GL/gl.h>
#endif
// Windows' own gl.h stops at OpenGL 1.1: these two came after
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#include "gfx.h"
#include "plat.h"
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "ext/stb_image.h"

#define MAX_QUADS 4096

typedef struct {
    float x, y, z;
    float u, v;
    uint32_t c;
} Vtx;

static Vtx verts[MAX_QUADS * 4];
static uint16_t indices[MAX_QUADS * 6];
static int quad_count;
static Tex * current_tex;
static Tex * white;
static int current_blend = -1;
static int draw_calls;

void gfx_init(void)
{
    for (int i = 0; i < MAX_QUADS; i++) {
        indices[i * 6 + 0] = i * 4 + 0;
        indices[i * 6 + 1] = i * 4 + 1;
        indices[i * 6 + 2] = i * 4 + 2;
        indices[i * 6 + 3] = i * 4 + 2;
        indices[i * 6 + 4] = i * 4 + 3;
        indices[i * 6 + 5] = i * 4 + 0;
    }
    uint8_t px[4 * 4 * 4];
    memset(px, 255, sizeof(px));
    white = tex_rgba(px, 4, 4, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    gfx_blend(BLEND_ALPHA);
}

// the next mip level: a 2x2 box filter, the colours weighted by alpha so transparent edges don't darken the sprites
static uint8_t * half_size(const uint8_t * src, int w, int h)
{
    int w2 = w / 2, h2 = h / 2;
    uint8_t * dst = malloc((size_t)w2 * h2 * 4);
    for (int y = 0; y < h2; y++) {
        const uint8_t * r0 = src + (size_t)y * 2 * w * 4, * r1 = r0 + (size_t)w * 4;
        uint8_t * d = dst + (size_t)y * w2 * 4;
        for (int x = 0; x < w2; x++, d += 4) {
            const uint8_t * p[4] = {r0 + x * 8, r0 + x * 8 + 4, r1 + x * 8, r1 + x * 8 + 4};
            unsigned a = p[0][3] + p[1][3] + p[2][3] + p[3][3];
            for (int c = 0; c < 3; c++)
                d[c] = a ? (p[0][c] * p[0][3] + p[1][c] * p[1][3] + p[2][c] * p[2][3] + p[3][c] * p[3][3] + a / 2) / a
                         : (unsigned)(p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4;
            d[3] = (a + 2) / 4;
        }
    }
    return dst;
}

// how much of a picture an alpha test at `ref` keeps (an alpha a pixel, scaled by k)
static float coverage(const uint8_t * px, int n, float k, int ref)
{
    int kept = 0;
    for (int i = 0; i < n; i++)
        kept += px[i * 4 + 3] * k > ref;
    return (float)kept / (n ? n : 1);
}

// a smaller level's alpha scaled so the alpha test keeps as much of it as of the full size: averaged, thin cut-out
// strands (hair, lashes) fall under the test and vanish when the texture's drawn small or at a slant
static void keep_coverage(uint8_t * px, int w, int h, float want)
{
    const int ref = 115;                // the band's alpha test (0.45)
    int n = w * h;
    if (want <= 0 || want >= 1)
        return;
    float lo = 1, hi = 4;
    if (coverage(px, n, 1, ref) >= want)
        return;
    for (int it = 0; it < 10; it++) {
        float mid = (lo + hi) / 2;
        if (coverage(px, n, mid, ref) < want)
            lo = mid;
        else
            hi = mid;
    }
    for (int i = 0; i < n; i++) {
        int a = (int)(px[i * 4 + 3] * hi + 0.5f);
        px[i * 4 + 3] = (uint8_t)(a > 255 ? 255 : a);
    }
}

// every level down to where a side is 1 pixel, as vitaGL lays its chains out
static void upload_mipmaps(const uint8_t * px, int w, int h)
{
    // the cut-out share of the full picture, kept on every level (none to keep for an opaque one)
    float cover = 0;
    {
        int n = w * h, part = 0;
        for (int i = 0; i < n; i++)
            part += px[i * 4 + 3] > 8 && px[i * 4 + 3] < 247;
        if (part > n / 200)
            cover = coverage(px, n, 1, 115);
    }
#ifdef __vita__
    // vitaGL makes the chain with its own quick downscale (the top level just point-sampled), then gets ours
    glGenerateMipmap(GL_TEXTURE_2D);
    sceGxmTransferFinish();
#endif
    const uint8_t * cur = px;
    uint8_t * owned = NULL;
    int level = 0;
#ifdef GHV_ES
    // ES wants the whole chain, down to one pixel (it has no "last level"): a side that's reached 1 stays there
    while (w > 1 || h > 1) {
        uint8_t * next;
        if (w > 1 && h > 1)
            next = half_size(cur, w, h);
        else {
            int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
            next = malloc((size_t)nw * nh * 4);
            for (int i = 0; i < nw * nh; i++)
                memcpy(next + i * 4, cur + (size_t)i * 2 * 4, 4);
        }
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        level++;
        keep_coverage(next, w, h, cover);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, next);
        free(owned);
        owned = next;
        cur = next;
    }
    free(owned);
    return;
#endif
    while (w > 1 && h > 1) {
        uint8_t * next = half_size(cur, w, h);
        w /= 2;
        h /= 2;
        level++;
        keep_coverage(next, w, h, cover);
#ifdef __vita__
        glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, next);
#else
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, next);
#endif
        free(owned);
        owned = next;
        cur = next;
    }
    free(owned);
#if !defined(__vita__) && !defined(GHV_ES)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level);
#endif
}

static bool power_of_two(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

Tex * tex_rgba(const uint8_t * px, int w, int h, int flags)
{
    Tex * t = calloc(1, sizeof(Tex));
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
#ifdef GHV_NO_MIPMAPS
    bool mip = false;               // Vita3K hangs in vitaGL's mipmap downscale: its test builds go without
#else
    bool mip = (flags & TEX_MIPMAP) && power_of_two(w) && power_of_two(h);
#endif
    GLint mag = flags & TEX_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : mag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    GLint wrap = flags & TEX_REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (mip)
        upload_mipmaps(px, w, h);
    current_tex = NULL;
    return t;
}

// The game's own pictures as tools/rawtex.py lays them out in assets/raw/: reading those is several times quicker than
// decoding them on the Vita. Without them (or for any other picture) it decodes as usual
static uint8_t * load_raw(const char * path, int * w, int * h)
{
    char raw[400];
    const char * dir = plat_asset_dir(), * dot = strrchr(path, '.');
    size_t len = strlen(dir);
    if (!dot || strncmp(path, dir, len) != 0)
        return NULL;
    snprintf(raw, sizeof(raw), "%sraw/%.*s.rgba", dir, (int)(dot - path - len), path + len);
    FILE * f = fopen(raw, "rb");
    if (!f)
        return NULL;
    uint32_t head[2];
    uint8_t * px = NULL;
    if (fread(head, sizeof(head), 1, f) == 1 && head[0] && head[1] && head[0] <= 4096 && head[1] <= 4096) {
        size_t size = (size_t)head[0] * head[1] * 4;
        px = malloc(size);
        if (px && fread(px, 1, size, f) != size) {
            free(px);
            px = NULL;
        }
        *w = (int)head[0];
        *h = (int)head[1];
    }
    fclose(f);
    return px;
}

// The pictures loaded next have their colours turned round the wheel by this many degrees (0: as they are): a band
// member's clothes in another colour. Skin (and wood) keeps its own: what's in skin's range of hue and not vivid is
// left alone, and so is anything near grey
static int load_hue;

void gfx_load_hue(int degrees)
{
    load_hue = ((degrees % 360) + 360) % 360;
}

// the hair's places in the next picture (its pixels; all of it for a hair texture of its own), and its dye (0: none)
static struct { int x, y, w, h; } hair_rect[32];
static int hair_n, hair_dye;
static bool hair_all;

void gfx_load_hair(int dye, const int * rects, int n, bool all)
{
    hair_dye = dye;
    hair_all = all;
    hair_n = n > 32 ? 32 : n;
    for (int i = 0; i < hair_n; i++) {
        hair_rect[i].x = rects[i * 4];
        hair_rect[i].y = rects[i * 4 + 1];
        hair_rect[i].w = rects[i * 4 + 2];
        hair_rect[i].h = rects[i * 4 + 3];
    }
}

// a dye: the hair's own light and shade, in the dye's colour (black hair comes out dark, but the colour shows)
static const uint8_t dyes[10][3] = {{0, 0, 0}, {236, 196, 112}, {196, 34, 28}, {212, 104, 40}, {40, 96, 236},
                                    {244, 88, 184}, {56, 204, 84}, {136, 60, 212}, {236, 236, 242}, {26, 26, 32}};

static uint8_t * hair_mask(int w, int h)
{
    if (!hair_n && !hair_all)
        return NULL;
    uint8_t * m = calloc((size_t)w * h, 1);
    if (!m)
        return NULL;
    if (hair_all)
        memset(m, 1, (size_t)w * h);
    for (int i = 0; i < hair_n; i++)
        for (int y = hair_rect[i].y; y < hair_rect[i].y + hair_rect[i].h && y < h; y++)
            for (int x = hair_rect[i].x; x < hair_rect[i].x + hair_rect[i].w && x < w; x++)
                if (x >= 0 && y >= 0)
                    m[(size_t)y * w + x] = 1;
    return m;
}

static void dye_hair(uint8_t * px, const uint8_t * mask, int w, int h)
{
    if (!mask || hair_dye <= 0 || hair_dye > 9)
        return;
    const uint8_t * d = dyes[hair_dye];
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
        if (!mask[i])
            continue;
        uint8_t * p = px + i * 4;
        float lum = (0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]) / 255, k = 0.16f + 1.15f * lum;
        for (int c = 0; c < 3; c++) {
            float v = d[c] * k;
            p[c] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

static void turn_hue(uint8_t * px, int w, int h, const uint8_t * skip)
{
    // a rotation about the grey axis (YIQ), as a 3 x 3 in 8.8 fixed point
    float a = (float)load_hue * 3.14159265f / 180, c = cosf(a), s = sinf(a);
    const float M[9] = {0.299f + 0.701f * c + 0.168f * s, 0.587f - 0.587f * c + 0.330f * s, 0.114f - 0.114f * c - 0.497f * s,
                        0.299f - 0.299f * c - 0.328f * s, 0.587f + 0.413f * c + 0.035f * s, 0.114f - 0.114f * c + 0.292f * s,
                        0.299f - 0.300f * c + 1.250f * s, 0.587f - 0.588f * c - 1.050f * s, 0.114f + 0.886f * c - 0.203f * s};
    int m[9];
    for (int i = 0; i < 9; i++)
        m[i] = (int)(M[i] * 256);
    for (size_t i = 0, n = (size_t)w * h; i < n; i++, px += 4) {
        if (skip && skip[i])
            continue;                   // hair: its own colour, or its dye
        int r = px[0], g = px[1], b = px[2];
        int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b), sat = hi - lo;
        if (sat < 24)
            continue;                   // grey: nothing to turn
        // skin (and wood): a hue from rose round to orange-yellow (about -25 to 50 degrees) and not a vivid colour. Pale
        // skin is often a touch more blue than green, which the old test ("red, then green, then blue") turned blue
        if (r == hi && sat * 100 < hi * 66) {
            int hue6 = g >= b ? (g - b) * 60 / (sat ? sat : 1) : -((b - g) * 60 / (sat ? sat : 1));   // -60..60, 0 red
            if (hue6 >= -25 && hue6 <= 50)
                continue;
        }
        int nr = (m[0] * r + m[1] * g + m[2] * b) >> 8, ng = (m[3] * r + m[4] * g + m[5] * b) >> 8;
        int nb = (m[6] * r + m[7] * g + m[8] * b) >> 8;
        px[0] = (uint8_t)(nr < 0 ? 0 : nr > 255 ? 255 : nr);
        px[1] = (uint8_t)(ng < 0 ? 0 : ng > 255 ? 255 : ng);
        px[2] = (uint8_t)(nb < 0 ? 0 : nb > 255 ? 255 : nb);
    }
}

Tex * tex_load(const char * path, int flags)
{
    int w, h, n;
    uint8_t * px = load_hue || hair_dye ? NULL : load_raw(path, &w, &h);
    if (px) {
        Tex * t = tex_rgba(px, w, h, flags);
        free(px);
        return t;
    }
    px = stbi_load(path, &w, &h, &n, 4);
    if (!px) {
        plat_log("tex: can't load %s", path);
        return NULL;
    }
    if (load_hue || hair_dye) {
        uint8_t * mask = hair_mask(w, h);
        if (load_hue)
            turn_hue(px, w, h, mask);
        dye_hair(px, mask, w, h);
        free(mask);
    }
    Tex * t = tex_rgba(px, w, h, flags);
    stbi_image_free(px);
    return t;
}

// a picture file's bytes, already in memory (a venue's textures come packed in one file)
Tex * tex_load_mem(const uint8_t * data, int size, int flags)
{
    int w, h, n;
    uint8_t * px = stbi_load_from_memory(data, size, &w, &h, &n, 4);
    if (!px)
        return NULL;
    Tex * t = tex_rgba(px, w, h, flags);
    stbi_image_free(px);
    return t;
}

// A 256-colour PNG (8-bit indices, PLTE + tRNS, not interlaced) as its palette (RGBA) and indices; NULL when it's
// anything else. The band's sheets are made that way (tools/band.py), and the Vita keeps them as they are: a byte a
// pixel instead of four
static uint32_t be32(const uint8_t * p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t * load_png_p8(const char * path, uint8_t pal[256][4], int * w, int * h)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t * file = size > 8 ? malloc((size_t)size) : NULL;
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(file);
        return NULL;
    }
    fclose(f);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t * idat = NULL, * out = NULL;
    size_t idat_n = 0;
    int width = 0, height = 0;
    bool ok = memcmp(file, sig, 8) == 0;
    memset(pal, 0, 256 * 4);
    for (int i = 0; i < 256; i++)
        pal[i][3] = 255;
    for (long at = 8; ok && at + 12 <= size;) {
        uint32_t len = be32(file + at);
        const uint8_t * type = file + at + 4, * d = file + at + 8;
        if (len > (uint32_t)(size - at - 12)) {
            ok = false;
            break;
        }
        if (!memcmp(type, "IHDR", 4)) {
            width = (int)be32(d);
            height = (int)be32(d + 4);
            // 8-bit indexed, no interlace; anything else goes the usual way
            ok = len >= 13 && d[8] == 8 && d[9] == 3 && d[12] == 0 && width > 0 && height > 0 && width <= 4096 &&
                 height <= 4096;
        } else if (!memcmp(type, "PLTE", 4)) {
            for (uint32_t i = 0; i < len / 3 && i < 256; i++) {
                pal[i][0] = d[i * 3];
                pal[i][1] = d[i * 3 + 1];
                pal[i][2] = d[i * 3 + 2];
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            for (uint32_t i = 0; i < len && i < 256; i++)
                pal[i][3] = d[i];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t * more = realloc(idat, idat_n + len);
            if (!more) {
                ok = false;
                break;
            }
            idat = more;
            memcpy(idat + idat_n, d, len);
            idat_n += len;
        } else if (!memcmp(type, "IEND", 4))
            break;
        at += 12 + (long)len;
    }
    free(file);
    int raw_n = 0;
    uint8_t * raw = ok && idat ? (uint8_t *)stbi_zlib_decode_malloc((const char *)idat, (int)idat_n, &raw_n) : NULL;
    free(idat);
    if (raw && raw_n >= (width + 1) * height) {
        // the scanlines' filters undone, one byte a pixel
        out = malloc((size_t)width * height);
        for (int y = 0; out && y < height; y++) {
            const uint8_t * line = raw + (size_t)y * (width + 1);
            uint8_t * row = out + (size_t)y * width, * up = y ? row - width : NULL;
            int ft = line[0];
            for (int x = 0; x < width; x++) {
                int a = x ? row[x - 1] : 0, b = up ? up[x] : 0, c = x && up ? up[x - 1] : 0, v = line[1 + x];
                if (ft == 1)
                    v += a;
                else if (ft == 2)
                    v += b;
                else if (ft == 3)
                    v += (a + b) / 2;
                else if (ft == 4) {
                    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                    v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                }
                row[x] = (uint8_t)v;
            }
        }
        *w = width;
        *h = height;
    }
    free(raw);
    return out;
}

Tex * tex_load_paletted(const char * path, int flags)
{
    static uint8_t pal[256][4];
    int w, h;
    uint8_t * idx = load_png_p8(path, pal, &w, &h);
    if (!idx)
        return tex_load(path, flags);
#ifdef __vita__
    // vitaGL keeps GL_PALETTE8_RGBA8_OES as a real paletted texture: the palette, then an index a pixel
    size_t n = (size_t)w * h;
    uint8_t * buf = malloc(sizeof(pal) + n);
    if (!buf) {
        free(idx);
        return tex_load(path, flags);
    }
    memcpy(buf, pal, sizeof(pal));
    memcpy(buf + sizeof(pal), idx, n);
    free(idx);
    Tex * t = calloc(1, sizeof(Tex));
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    GLint mag = flags & TEX_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGetError();
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE8_RGBA8_OES, w, h, 0, (GLsizei)(sizeof(pal) + n), buf);
    GLenum err = glGetError();
    free(buf);
    current_tex = NULL;
    if (err != GL_NO_ERROR) {
        plat_log("tex: paletted %s refused (0x%x), as RGBA", path, err);
        tex_free(t);
        return tex_load(path, flags);
    }
    return t;
#else
    // the desktop has the memory: as RGBA
    uint8_t * px = malloc((size_t)w * h * 4);
    for (size_t i = 0; px && i < (size_t)w * h; i++)
        memcpy(px + i * 4, pal[idx[i]], 4);
    free(idx);
    if (px && getenv("GHV_P8CHECK")) {
        // tests: the indices and palette against stb_image's own decoding
        int sw, sh, sn;
        uint8_t * ref = stbi_load(path, &sw, &sh, &sn, 4);
        size_t bad = 0;
        for (size_t i = 0; ref && sw == w && sh == h && i < (size_t)w * h * 4; i++)
            bad += px[i] != ref[i];
        plat_log("p8check: %s %dx%d %s, %zu bytes differ", path, w, h, ref ? "decoded" : "no ref", bad);
        stbi_image_free(ref);
    }
    if (!px)
        return tex_load(path, flags);
    Tex * t = tex_rgba(px, w, h, flags);
    free(px);
    return t;
#endif
}

void tex_free(Tex * t)
{
    if (!t)
        return;
    gfx_flush();
    if (current_tex == t)
        current_tex = NULL;
    glDeleteTextures(1, &t->id);
    free(t);
}

Sprite sprite_whole(Tex * t)
{
    Sprite s = {t, 0, 0, 1, 1, t ? t->w : 0, t ? t->h : 0};
    return s;
}

void gfx_begin(float r, float g, float b)
{
    draw_calls = 0;
#ifdef __vita__
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
#else
    // a window or a phone's screen of any shape: black all over, then the picture as big as fits, in the middle
    int vx, vy, vw, vh, fw, fh, ps[4];
    plat_view(&vx, &vy, &vw, &vh, &fw, &fh);
    bool pass = plat_pass_scissor(ps);      // a second view of a menu: only its own stretch of the screen
    if (pass) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(ps[0], ps[1], ps[2], ps[3]);
    }
    glViewport(0, 0, fw, fh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(vx, vy, vw, vh);
    if (r > 0 || g > 0 || b > 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, vy, vw, vh);
        if (pass)
            glScissor(ps[0], ps[1], ps[2], ps[3]);
        glClearColor(r, g, b, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!pass)
            glDisable(GL_SCISSOR_TEST);
    }
#endif
}

void gfx_end(void)
{
    gfx_flush();
}

void gfx_flush(void)
{
    if (quad_count == 0)
        return;
    glBindTexture(GL_TEXTURE_2D, (current_tex ? current_tex : white)->id);
    glVertexPointer(3, GL_FLOAT, sizeof(Vtx), &verts[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(Vtx), &verts[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vtx), &verts[0].c);
    glDrawElements(GL_TRIANGLES, quad_count * 6, GL_UNSIGNED_SHORT, indices);
    quad_count = 0;
    draw_calls++;
}

int gfx_draw_calls(void)
{
    return draw_calls;
}

// drawing under the picture (a phone held upright): 960 across, `below` down, y from the picture's bottom edge
void gfx_below_begin(float below)
{
#ifndef __vita__
    gfx_flush();
    int vx, vy, vw, vh, fw, fh;
    plat_view(&vx, &vy, &vw, &vh, &fw, &fh);
    glViewport(vx, 0, vw, vy);         // as wide as the picture (its sides are off the screen too)
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_W, below, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
#else
    (void)below;
#endif
}

// the picture and the strip under it as one: 960 across, SCREEN_H + below down (a phone held upright)
void gfx_2d_tall(float below)
{
#ifndef __vita__
    gfx_flush();
    int vx, vy, vw, vh, fw, fh;
    plat_view(&vx, &vy, &vw, &vh, &fw, &fh);
    glViewport(vx, 0, vw, vy + vh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_W, SCREEN_H + below, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
#else
    (void)below;
#endif
}

void gfx_below_end(void)
{
#ifndef __vita__
    gfx_flush();
    int vx, vy, vw, vh, fw, fh;
    plat_view(&vx, &vy, &vw, &vh, &fw, &fh);
    glViewport(vx, vy, vw, vh);
#endif
}

void gfx_2d(void)
{
    gfx_flush();
#ifndef __vita__
    {
        int vx, vy, vw, vh, fw, fh;
        plat_view(&vx, &vy, &vw, &vh, &fw, &fh);
        glViewport(vx, vy, vw, vh);         // (gfx_2d_tall widens it)
    }
#endif
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// only this part of the picture drawn to (x, y, w, h in its 960 x 544); gfx_clip_off ends it
void gfx_clip(float x, float y, float w, float h)
{
    gfx_flush();
    int vx = 0, vy = 0, vw = SCREEN_W, vh = SCREEN_H;
#ifndef __vita__
    int fw, fh;
    plat_view(&vx, &vy, &vw, &vh, &fw, &fh);
#endif
    glEnable(GL_SCISSOR_TEST);
    int sx = vx + (int)(x * vw / SCREEN_W), sy = vy + (int)((SCREEN_H - y - h) * vh / SCREEN_H);
    int sw = (int)(w * vw / SCREEN_W + 1), sh = (int)(h * vh / SCREEN_H + 1);
#ifndef __vita__
    int ps[4];
    if (plat_pass_scissor(ps)) {
        // within the view's own stretch as well
        int x1 = sx + sw < ps[0] + ps[2] ? sx + sw : ps[0] + ps[2], y1 = sy + sh < ps[1] + ps[3] ? sy + sh : ps[1] + ps[3];
        sx = sx > ps[0] ? sx : ps[0];
        sy = sy > ps[1] ? sy : ps[1];
        sw = x1 > sx ? x1 - sx : 0;
        sh = y1 > sy ? y1 - sy : 0;
    }
#endif
    glScissor(sx, sy, sw, sh);
}

void gfx_clip_off(void)
{
    gfx_flush();
#ifndef __vita__
    int ps[4];
    if (plat_pass_scissor(ps)) {
        glScissor(ps[0], ps[1], ps[2], ps[3]);
        return;
    }
#endif
    glDisable(GL_SCISSOR_TEST);
}

void gfx_3d(const float * proj, const float * view)
{
    gfx_flush();
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view);
}

void gfx_blend(int mode)
{
    if (mode == current_blend)
        return;
    gfx_flush();
    current_blend = mode;
    if (mode == BLEND_ADD)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void gfx_tex(Tex * t)
{
    if (!t)
        t = white;
    if (t == current_tex)
        return;
    gfx_flush();
    current_tex = t;
}

// a mesh in screen space with depth (src/band3d.c): xyz per vertex (z -1 near .. 1 far, as gfx_2d's ortho), uv with
// its own stride, colours; cut out where the texture's alpha is under half
void gfx_mesh_begin(bool clear_depth)
{
    gfx_flush();
    if (clear_depth) {
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.45f);
}

void gfx_mesh(Tex * t, const float * xyz, const float * uv, int uv_stride, const uint32_t * col, const uint16_t * idx,
              int n)
{
#ifndef __vita__
    static int skip = -1;
    if (skip < 0)
        skip = getenv("GHV_NO_MESH") != NULL;   // tests: the CPU's share of the 3D band, without the drawing
    if (skip)
        return;
#endif
    glBindTexture(GL_TEXTURE_2D, (t ? t : white)->id);
    glVertexPointer(3, GL_FLOAT, 12, xyz);
    glTexCoordPointer(2, GL_FLOAT, uv_stride, uv);
    glColorPointer(4, GL_UNSIGNED_BYTE, 4, col);
    glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_SHORT, idx);
    draw_calls++;
    current_tex = NULL;
}

// what a mesh drawn next does to the depth buffer, and how much of its alpha it needs to show
void gfx_mesh_state(bool depth_write, float alpha_ref)
{
    glDepthMask(depth_write ? GL_TRUE : GL_FALSE);
    glAlphaFunc(GL_GREATER, alpha_ref);
}

// whether the meshes drawn next are tested against the depth buffer
void gfx_mesh_depth(bool test)
{
    if (test)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
}

// the same with a modelview matrix (column-major) taking xyz onto the screen
void gfx_mesh_xf(const float mv[16], Tex * t, const float * xyz, const float * uv, int uv_stride, const uint32_t * col,
                 const uint16_t * idx, int n)
{
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(mv);
    gfx_mesh(t, xyz, uv, uv_stride, col, idx, n);
    glLoadIdentity();
}

void gfx_mesh_end(void)
{
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    current_tex = NULL;
}

void gfx_quad(const float p[4][3], const float uv[4][2], const uint32_t c[4])
{
    if (quad_count == MAX_QUADS)
        gfx_flush();
    Vtx * v = &verts[quad_count * 4];
    for (int i = 0; i < 4; i++) {
        v[i].x = p[i][0];
        v[i].y = p[i][1];
        v[i].z = p[i][2];
        v[i].u = uv[i][0];
        v[i].v = uv[i][1];
        v[i].c = c[i];
    }
    quad_count++;
}

void gfx_sprite_uv(const Sprite * s, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   uint32_t color)
{
    gfx_tex(s ? s->tex : NULL);
    float su0 = s ? s->u0 + (s->u1 - s->u0) * u0 : u0, su1 = s ? s->u0 + (s->u1 - s->u0) * u1 : u1;
    float sv0 = s ? s->v0 + (s->v1 - s->v0) * v0 : v0, sv1 = s ? s->v0 + (s->v1 - s->v0) * v1 : v1;
    const float p[4][3] = {{x, y, 0}, {x + w, y, 0}, {x + w, y + h, 0}, {x, y + h, 0}};
    const float uv[4][2] = {{su0, sv0}, {su1, sv0}, {su1, sv1}, {su0, sv1}};
    const uint32_t c[4] = {color, color, color, color};
    gfx_quad(p, uv, c);
}

void gfx_sprite(const Sprite * s, float x, float y, float w, float h, uint32_t color)
{
    gfx_sprite_uv(s, x, y, w, h, 0, 0, 1, 1, color);
}

void gfx_rect(float x, float y, float w, float h, uint32_t color)
{
    gfx_sprite_uv(NULL, x, y, w, h, 0, 0, 1, 1, color);
}

void gfx_rect_grad(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
    gfx_tex(NULL);
    const float p[4][3] = {{x, y, 0}, {x + w, y, 0}, {x + w, y + h, 0}, {x, y + h, 0}};
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32_t c[4] = {top, top, bottom, bottom};
    gfx_quad(p, uv, c);
}

void gfx_nine(const Sprite * s, float x, float y, float w, float h, float c, float scale, uint32_t color)
{
    float cu = c / s->w, cv = c / s->h;
    float cw = c * scale, ch = c * scale;
    if (cw * 2 > w)
        cw = w / 2;
    if (ch * 2 > h)
        ch = h / 2;
    float xs[4] = {x, x + cw, x + w - cw, x + w};
    float ys[4] = {y, y + ch, y + h - ch, y + h};
    float us[4] = {0, cu, 1 - cu, 1};
    float vs[4] = {0, cv, 1 - cv, 1};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            gfx_sprite_uv(s, xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j], us[i], vs[j], us[i + 1], vs[j + 1],
                          color);
}

void mat_identity(float * m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
}

void mat_perspective(float * m, float fovy_deg, float aspect, float znear, float zfar)
{
    float f = 1.0f / tanf(fovy_deg * (float)M_PI / 360.0f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1;
    m[14] = 2 * zfar * znear / (znear - zfar);
}

static void normalize(float * v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void mat_lookat(float * m, const float eye[3], const float at[3], const float up[3])
{
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    normalize(f);
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    normalize(s);
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    mat_identity(m);
    m[0] = s[0];
    m[4] = s[1];
    m[8] = s[2];
    m[1] = u[0];
    m[5] = u[1];
    m[9] = u[2];
    m[2] = -f[0];
    m[6] = -f[1];
    m[10] = -f[2];
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
}
