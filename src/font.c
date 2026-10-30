#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "plat.h"

typedef struct {
    uint32_t cp;
    uint16_t x, y, w, h;
    int16_t ox, oy;
    uint16_t adv4;      // advance x4
    uint16_t pad;
} Glyph;

struct Font {
    Tex * tex;
    int size, line, ascent;
    int count;
    Glyph * glyphs;
};

Font * font_load(const char * png, const char * fnt)
{
    FILE * f = fopen(fnt, "rb");
    if (!f) {
        plat_log("font: can't open %s", fnt);
        return NULL;
    }
    char magic[4];
    uint16_t h[4];
    uint32_t count;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VFN1", 4) != 0 || fread(h, 2, 4, f) != 4 ||
        fread(&count, 4, 1, f) != 1) {
        fclose(f);
        return NULL;
    }
    Font * font = calloc(1, sizeof(Font));
    font->size = h[0];
    font->line = h[1];
    font->ascent = h[2];
    font->count = count;
    font->glyphs = calloc(count, sizeof(Glyph));
    if (fread(font->glyphs, sizeof(Glyph), count, f) != count)
        font->count = 0;
    fclose(f);
    font->tex = tex_load(png, TEX_MIPMAP);
    return font;
}

static uint32_t next_cp(const char ** s)
{
    const unsigned char * p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
        p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = ((c & 31) << 6) | (p[0] & 63);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

static const Glyph * find(const Font * f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp)
            return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static const Glyph * glyph(const Font * f, uint32_t cp)
{
    const Glyph * g = find(f, cp);
    if (!g && cp >= 'a' && cp <= 'z')
        g = find(f, cp - 32);           // a capitals-only face
    if (!g)
        g = find(f, '?');
    return g;
}

float font_width(Font * f, const char * text, float size)
{
    if (!text)
        return 0;
    if (!f)
        return 0;
    float scale = size / f->line, w = 0;
    while (*text) {
        const Glyph * g = glyph(f, next_cp(&text));
        if (g)
            w += g->adv4 * 0.25f * scale;
    }
    return w;
}

float font_draw(Font * f, const char * text, float x, float y, float size, uint32_t color, int align)
{
    if (!f || !f->tex)
        return 0;
    float w = font_width(f, text, size);
    if (align == ALIGN_CENTER)
        x -= w / 2;
    else if (align == ALIGN_RIGHT)
        x -= w;
    float scale = size / f->line;
    float iw = 1.0f / f->tex->w, ih = 1.0f / f->tex->h;
    gfx_tex(f->tex);
    while (*text) {
        const Glyph * g = glyph(f, next_cp(&text));
        if (!g)
            continue;
        if (g->w && g->h) {
            float gx = x + g->ox * scale, gy = y + g->oy * scale, gw = g->w * scale, gh = g->h * scale;
            const float p[4][3] = {{gx, gy, 0}, {gx + gw, gy, 0}, {gx + gw, gy + gh, 0}, {gx, gy + gh, 0}};
            float u0 = g->x * iw, v0 = g->y * ih, u1 = (g->x + g->w) * iw, v1 = (g->y + g->h) * ih;
            const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
            const uint32_t c[4] = {color, color, color, color};
            gfx_quad(p, uv, c);
        }
        x += g->adv4 * 0.25f * scale;
    }
    return w;
}

float font_draw_shadow(Font * f, const char * text, float x, float y, float size, uint32_t color, int align)
{
    float o = size > 30 ? 3 : 2;
    font_draw(f, text, x + o, y + o, size, RGBA(0, 0, 0, (color >> 24) * 3 / 4), align);
    return font_draw(f, text, x, y, size, color, align);
}

float font_draw_fit(Font * f, const char * text, float x, float y, float size, float max_w, uint32_t color, int align)
{
    if (font_width(f, text, size) <= max_w)
        return font_draw(f, text, x, y, size, color, align);
    char buf[512];
    size_t n = strlen(text);
    if (n > sizeof(buf) - 4)
        n = sizeof(buf) - 4;
    memcpy(buf, text, n);
    buf[n] = 0;
    while (n > 0) {
        // back off one UTF-8 character
        do
            n--;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80);
        strcpy(buf + n, "...");
        if (font_width(f, buf, size) <= max_w)
            break;
    }
    return font_draw(f, buf, x, y, size, color, align);
}
