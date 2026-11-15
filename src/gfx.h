// Drawing: every quad goes into one batch that's drawn when the texture or blending changes (vitaGL pays per draw)
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define WHITE RGBA(255, 255, 255, 255)
#define ALPHA(c, a) (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

// c with its alpha scaled by a (0..1)
static inline uint32_t fade_alpha(uint32_t c, float a)
{
    int v = (int)((c >> 24) * a + 0.5f);
    return ALPHA(c, v < 0 ? 0 : v > 255 ? 255 : v);
}

enum { TEX_MIPMAP = 1, TEX_REPEAT = 2, TEX_NEAREST = 4 };
enum { BLEND_ALPHA, BLEND_ADD };

typedef struct {
    unsigned int id;
    int w, h;
} Tex;

// a region of a texture (an atlas entry); w and h in pixels
typedef struct {
    Tex * tex;
    float u0, v0, u1, v1;
    int w, h;
} Sprite;

void gfx_init(void);
Tex * tex_load(const char * path, int flags);
Tex * tex_load_paletted(const char * path, int flags);   // a 256-colour PNG kept paletted on the Vita (else as tex_load)
Tex * tex_rgba(const uint8_t * px, int w, int h, int flags);
void tex_free(Tex * t);
Sprite sprite_whole(Tex * t);

void gfx_begin(float r, float g, float b);
void gfx_end(void);
// 960x544 pixels, y down
void gfx_2d(void);
// column-major 4x4 matrices
void gfx_3d(const float * proj, const float * view);
void gfx_blend(int mode);
void gfx_tex(Tex * t);
// p: four corners in order (x, y, z), uv per corner, colour per corner
void gfx_quad(const float p[4][3], const float uv[4][2], const uint32_t c[4]);
void gfx_flush(void);
int gfx_draw_calls(void);

// 2D helpers
void gfx_sprite(const Sprite * s, float x, float y, float w, float h, uint32_t color);
void gfx_sprite_uv(const Sprite * s, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   uint32_t color);
void gfx_rect(float x, float y, float w, float h, uint32_t color);
void gfx_rect_grad(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
// a sprite cut in 9 (corner size c in pixels of the sprite, drawn at scale)
void gfx_nine(const Sprite * s, float x, float y, float w, float h, float c, float scale, uint32_t color);

// matrices
void mat_identity(float * m);
void mat_perspective(float * m, float fovy_deg, float aspect, float znear, float zfar);
void mat_lookat(float * m, const float eye[3], const float at[3], const float up[3]);
