#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __vita__
#include <vitaGL.h>
#include <psp2/gxm.h>
#else
#include <GL/gl.h>
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

// every level down to where a side is 1 pixel, as vitaGL lays its chains out
static void upload_mipmaps(const uint8_t * px, int w, int h)
{
#ifdef __vita__
    // vitaGL makes the chain with its own quick downscale (the top level just point-sampled), then gets ours
    glGenerateMipmap(GL_TEXTURE_2D);
    sceGxmTransferFinish();
#endif
    const uint8_t * cur = px;
    uint8_t * owned = NULL;
    int level = 0;
    while (w > 1 && h > 1) {
        uint8_t * next = half_size(cur, w, h);
        w /= 2;
        h /= 2;
        level++;
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
#ifndef __vita__
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

Tex * tex_load(const char * path, int flags)
{
    int w, h, n;
    uint8_t * px = load_raw(path, &w, &h);
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
    Tex * t = tex_rgba(px, w, h, flags);
    stbi_image_free(px);
    return t;
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
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
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

void gfx_2d(void)
{
    gfx_flush();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
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
