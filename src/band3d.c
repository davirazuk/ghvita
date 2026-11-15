// The band as World Tour's models: see band3d.h. The files are tools/band.py --3d's:
//   <name>.b3d  "B3D1", parts, bones, frames, (2 floats unused), then each part: texture name[32], vertex and triangle
//               counts, vertices (B3Vert), u16 indices; then frames x bones skin matrices (3 x 4 floats, rows)
//   alt/<name>.b3a  "B3A1", bones, frames, the matrices
// Model space: metres, y up from the feet, the loop's centre at x = z = 0, facing -z (the camera). Seen as the sheets
// were rendered: orthographic, looking down PITCH.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "band3d.h"
#include "gfx.h"
#include "plat.h"
#ifndef __vita__
#include <SDL2/SDL.h>
double band3d_skin_us;     // tests: time spent skinning (not drawing)
static uint64_t now_us(void) { return SDL_GetPerformanceCounter() * 1000000 / SDL_GetPerformanceFrequency(); }
#endif

#define PITCH 0.2618f           // 15 degrees, tools/band.py
#define CELL_H 2.13f            // what a sheet's lean and dip were measured against (a cell's height in metres)

typedef struct {
    float p[3], n[3], uv[2];
    uint8_t j[4], w[4];
} B3Vert;

typedef struct {
    Tex * tex;
    int vc, tc;
    B3Vert * v;
    float * uv;                 // the texture places packed tight (vitaGL copies what it's given every draw)
    uint16_t * idx;
} B3Part;

struct Band3D {
    int nparts, nbones, nframes, alt_frames, maxv, total_v;
    // two posings (pose_model): positions, the ink line's (3 floats a vertex) and colours. The worker thread fills the
    // back one while the front one's drawn
    float * pose[2], * ink[2];
    uint32_t * col[2];
    int front;
    bool posed;                 // the front one has something in it
    int job;                    // the worker's slot posing it, -1 none
    B3Part * parts;
    float * mats, * alt;        // frames x bones x 12
};

static uint32_t * ink_col, * shadow_col;
static int buf_cap;

void band3d_release(Band3D * m);

static void * read_all(const char * path, long * size)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void * d = *size > 0 ? malloc((size_t)*size) : NULL;
    if (d && fread(d, 1, (size_t)*size, f) != (size_t)*size) {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

Band3D * band3d_load(const char * name)
{
    char path[512];
    long size = 0;
    snprintf(path, sizeof(path), "%sband3d/%s.b3d", plat_asset_dir(), name);
    uint8_t * d = read_all(path, &size);
    if (!d)
        return NULL;
    Band3D * m = calloc(1, sizeof(Band3D));
    m->job = -1;
    int32_t head[3];
    bool ok = size >= 24 && memcmp(d, "B3D1", 4) == 0;
    long at = 24;
    if (ok) {
        memcpy(head, d + 4, 12);
        m->nparts = head[0];
        m->nbones = head[1];
        m->nframes = head[2];
        ok = m->nparts > 0 && m->nparts < 256 && m->nbones > 0 && m->nbones <= 256 && m->nframes > 0;
    }
    if (ok)
        m->parts = calloc((size_t)m->nparts, sizeof(B3Part));
    for (int i = 0; ok && i < m->nparts; i++) {
        B3Part * p = &m->parts[i];
        if (at + 40 > size) {
            ok = false;
            break;
        }
        char tex[33] = {0};
        memcpy(tex, d + at, 32);
        int32_t c[2];
        memcpy(c, d + at + 32, 8);
        at += 40;
        p->vc = c[0];
        p->tc = c[1];
        size_t vb = (size_t)p->vc * sizeof(B3Vert), ib = (size_t)p->tc * 6;
        if (p->vc <= 0 || p->vc > 65535 || p->tc <= 0 || at + (long)(vb + ib) > size) {
            ok = false;
            break;
        }
        p->v = malloc(vb);
        p->idx = malloc(ib);
        p->uv = malloc((size_t)p->vc * 8);
        if (!p->v || !p->idx || !p->uv) {
            ok = false;
            break;
        }
        memcpy(p->v, d + at, vb);
        for (int k = 0; k < p->vc; k++) {
            p->uv[k * 2] = p->v[k].uv[0];
            p->uv[k * 2 + 1] = p->v[k].uv[1];
        }
        memcpy(p->idx, d + at + vb, ib);
        at += (long)(vb + ib);
        for (int k = 0; k < p->vc; k++)
            for (int j = 0; j < 4; j++)
                if (p->v[k].j[j] >= m->nbones)
                    p->v[k].w[j] = 0;
        for (int k = 0; k < p->tc * 3; k++)
            if (p->idx[k] >= p->vc)
                p->idx[k] = 0;
        if (tex[0]) {
            snprintf(path, sizeof(path), "%sband3d/tex/%s.png", plat_asset_dir(), tex);
            p->tex = tex_load(path, TEX_MIPMAP);
        }
        if (p->vc > m->maxv)
            m->maxv = p->vc;
        m->total_v += p->vc;
    }
    size_t mb = (size_t)m->nframes * m->nbones * 12 * sizeof(float);
    if (ok && at + (long)mb <= size) {
        m->mats = malloc(mb);
        memcpy(m->mats, d + at, mb);
    } else
        ok = false;
    free(d);
    if (!ok) {
        plat_log("band3d: %s is damaged", name);
        band3d_free(m);
        return NULL;
    }
    // the wilder loop, when there is one
    snprintf(path, sizeof(path), "%sband3d/alt/%s.b3a", plat_asset_dir(), name);
    d = read_all(path, &size);
    if (d && size >= 12 && memcmp(d, "B3A1", 4) == 0) {
        memcpy(head, d + 4, 8);
        size_t ab = (size_t)head[1] * head[0] * 12 * sizeof(float);
        if (head[0] == m->nbones && head[1] > 0 && 12 + (long)ab <= size) {
            m->alt = malloc(ab);
            memcpy(m->alt, d + 12, ab);
            m->alt_frames = head[1];
        }
    }
    free(d);
    return m;
}

void band3d_free(Band3D * m)
{
    if (!m)
        return;
    for (int i = 0; m->parts && i < m->nparts; i++) {
        tex_free(m->parts[i].tex);
        free(m->parts[i].v);
        free(m->parts[i].uv);
        free(m->parts[i].idx);
    }
    free(m->parts);
    free(m->mats);
    free(m->alt);
    band3d_release(m);
    for (int i = 0; i < 2; i++) {
        free(m->pose[i]);
        free(m->ink[i]);
        free(m->col[i]);
    }
    free(m);
}

int band3d_frames(const Band3D * m, bool alt)
{
    return !m ? 0 : alt ? m->alt_frames : m->nframes;
}

void band3d_begin(bool clear)
{
    gfx_mesh_begin(clear);
}

void band3d_end(void)
{
    gfx_mesh_end();
}

static const float * frame_mats(const Band3D * m, bool alt, int f)
{
    if (alt && m->alt && m->alt_frames > 0) {
        f = f < 0 ? 0 : f >= m->alt_frames ? m->alt_frames - 1 : f;
        return m->alt + (size_t)f * m->nbones * 12;
    }
    f = f < 0 ? 0 : f >= m->nframes ? m->nframes - 1 : f;
    return m->mats + (size_t)f * m->nbones * 12;
}

static uint8_t to8(float v)
{
    return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255 + 0.5f);
}

// the pose: the two frames blended, and the view in each bone's matrix (turned by yaw, looked down on by the pitch:
// x right, y up the picture, z toward the camera, metres). The CPU skins it (two bones a vertex, the normal turned by
// the main one) into the positions, the ink line's positions and the colours; the screen is the GPU's (band3d_draw's matrices)
static void pose_model(Band3D * m, const Band3DDraw * d, int buf)
{
    static float M[256][12];
    const float * A = frame_mats(m, d->alt_a, d->fa), * B = frame_mats(m, d->alt_b, d->fb);
    float wb = d->wb < 0 ? 0 : d->wb > 1 ? 1 : d->wb, wa = 1 - wb;
    float cy = cosf(d->yaw), sy = sinf(d->yaw), cp = cosf(PITCH), sp = sinf(PITCH);
    const float V[3][3] = {{cy, 0, sy}, {-sy * sp, cp, cy * sp}, {sy * cp, sp, -cy * cp}};
    for (int b = 0; b < m->nbones; b++) {
        float S[12];
        for (int k = 0; k < 12; k++)
            S[k] = A[b * 12 + k] * wa + B[b * 12 + k] * wb;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++)
                M[b][r * 4 + c] = V[r][0] * S[c] + V[r][1] * S[4 + c] + V[r][2] * S[8 + c];
    }
    // the light from the audience (right, up, toward the camera), as tools/band_render.c's
    const float L[3] = {0.25f, 0.45f, 0.85f};
    float tint[3] = {(d->tint & 255) / 255.0f, ((d->tint >> 8) & 255) / 255.0f, ((d->tint >> 16) & 255) / 255.0f};
    uint32_t ta = (d->tint >> 24) << 24;
    float ink_m = 2.3f / (d->ppm > 1 ? d->ppm : 1);      // the ink line's width in metres at this size
    float * o = m->pose[buf], * q = m->ink[buf];
    uint32_t * col = m->col[buf];
    for (int pi = 0; pi < m->nparts; pi++) {
        const B3Part * p = &m->parts[pi];
        for (int i = 0; i < p->vc; i++, o += 3, q += 3, col++) {
            const B3Vert * v = &p->v[i];
            const float * R = M[v->j[0]];
            float px = v->p[0], py = v->p[1], pz = v->p[2];
            float x = R[0] * px + R[1] * py + R[2] * pz + R[3];
            float y = R[4] * px + R[5] * py + R[6] * pz + R[7];
            float z = R[8] * px + R[9] * py + R[10] * pz + R[11];
            if (v->w[1]) {
                const float * Q = M[v->j[1]];
                float w1 = v->w[1] * (1 / 255.0f), w0 = 1 - w1;
                x = x * w0 + w1 * (Q[0] * px + Q[1] * py + Q[2] * pz + Q[3]);
                y = y * w0 + w1 * (Q[4] * px + Q[5] * py + Q[6] * pz + Q[7]);
                z = z * w0 + w1 * (Q[8] * px + Q[9] * py + Q[10] * pz + Q[11]);
            }
            float nx = R[0] * v->n[0] + R[1] * v->n[1] + R[2] * v->n[2];
            float ny = R[4] * v->n[0] + R[5] * v->n[1] + R[6] * v->n[2];
            float nz = R[8] * v->n[0] + R[9] * v->n[1] + R[10] * v->n[2];
            o[0] = x;
            o[1] = y;
            o[2] = z;
            // the ink line: the body a couple of pixels fatter along its outline, a little behind
            float sl = sqrtf(nx * nx + ny * ny) + 1e-6f, push = ink_m * (1 - fabsf(nz) * 0.5f) / sl;
            q[0] = x + nx * push;
            q[1] = y + ny * push;
            q[2] = z - 0.12f;
            float lit = 0.62f + 0.38f * fabsf(nx * L[0] + ny * L[1] + nz * L[2]);
            float g = y / CELL_H;
            g = g < 0 ? 0 : g > 1 ? 1 : g;
            float edge = 1 - fabsf(nz), rim = edge * edge * d->rim_k;
            float c[3];
            for (int ch = 0; ch < 3; ch++)
                c[ch] = lit * (d->bot[ch] + (d->top[ch] - d->bot[ch]) * g) * tint[ch] + rim * d->rim[ch];
            *col = (uint32_t)to8(c[0]) | (uint32_t)to8(c[1]) << 8 | (uint32_t)to8(c[2]) << 16 | ta;
        }
    }
}

// The worker: four slots (one a member). The main thread puts a model and how it's to be posed in a free slot (1),
// the worker poses it into its back buffer (2), the main thread swaps it to the front when it next draws it (0)
typedef struct {
    Band3D * m;
    Band3DDraw d;
    int state;                  // 0 free, 1 asked, 2 done; __atomic
} Job;

static Job jobs[4];
static bool worker_on;

static int worker(void * arg)
{
    (void)arg;
    for (;;) {
        bool did = false;
        for (int i = 0; i < 4; i++)
            if (__atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE) == 1) {
                Band3D * m = jobs[i].m;
                pose_model(m, &jobs[i].d, m->front ^ 1);
                __atomic_store_n(&jobs[i].state, 2, __ATOMIC_RELEASE);
                did = true;
            }
        if (!did)
            plat_sleep_us(1000);
    }
    return 0;
}

// a model about to go: off the worker's hands first
void band3d_release(Band3D * m)
{
    if (!m || m->job < 0)
        return;
    while (__atomic_load_n(&jobs[m->job].state, __ATOMIC_ACQUIRE) == 1)
        plat_sleep_us(200);
    __atomic_store_n(&jobs[m->job].state, 0, __ATOMIC_RELEASE);
    jobs[m->job].m = NULL;
    m->job = -1;
}

void band3d_draw(Band3D * m, const Band3DDraw * d)
{
    if (!m)
        return;
    if (!m->pose[0]) {
        for (int i = 0; i < 2; i++) {
            m->pose[i] = malloc((size_t)m->total_v * 12);
            m->ink[i] = malloc((size_t)m->total_v * 12);
            m->col[i] = malloc((size_t)m->total_v * 4);
        }
    }
    if (m->maxv > buf_cap) {
        buf_cap = m->maxv;
        ink_col = realloc(ink_col, (size_t)buf_cap * 4);
        shadow_col = realloc(shadow_col, (size_t)buf_cap * 4);
        for (int i = 0; ink_col && shadow_col && i < buf_cap; i++) {
            ink_col[i] = 0xff1c0910u;           // (16, 9, 28), tools/band_ink.py's ink
            shadow_col[i] = 0x780c0306u;        // RGBA(6, 3, 12, 120), the sheets' shadow
        }
    }
    for (int i = 0; i < 2; i++)
        if (!m->pose[i] || !m->ink[i] || !m->col[i])
            return;
    if (!ink_col || !shadow_col)
        return;
#ifndef __vita__
    uint64_t t0 = now_us();
#endif
    if (!worker_on) {
        worker_on = true;
        plat_thread("band3d", worker, NULL, 1);
    }
    // the worker's last posing to the front, and this frame's asked for (drawn next frame: a frame late, unseen)
    if (m->job >= 0 && __atomic_load_n(&jobs[m->job].state, __ATOMIC_ACQUIRE) == 2) {
        m->front ^= 1;
        m->posed = true;
        __atomic_store_n(&jobs[m->job].state, 0, __ATOMIC_RELEASE);
    }
    if (!m->posed) {
        pose_model(m, d, m->front);     // the first time: now, here
        m->posed = true;
    }
    if (m->job < 0)
        for (int i = 0; i < 4; i++)
            if (!jobs[i].m) {
                jobs[i].m = m;
                m->job = i;
                break;
            }
    if (m->job >= 0 && __atomic_load_n(&jobs[m->job].state, __ATOMIC_ACQUIRE) == 0) {
        jobs[m->job].d = *d;
        __atomic_store_n(&jobs[m->job].state, 1, __ATOMIC_RELEASE);
    }
#ifndef __vita__
    band3d_skin_us += (double)(now_us() - t0);
#endif
    // onto the screen (the GPU's work): x right and y up in metres to pixels at the feet, leaning and dipping more the
    // higher up, depth into this member's slice (gfx_2d's ortho: the larger z, the nearer; slices 0.4 deep)
    float k = d->ppm, z0 = 0.95f - 0.47f * d->depth;
    const float body[16] = {k * d->squash, 0, 0, 0,
                            d->lean / CELL_H, -k + d->dip / CELL_H, 0, 0,
                            0, 0, 0.4f / 2.4f, 0,
                            d->x, d->feet, -z0 + 0.2f, 1};
    // the shadow: laid down toward the camera, a fifth as tall, behind everything of theirs
    const float flat[16] = {k * d->squash, 0, 0, 0,
                            -d->lean * 0.18f / CELL_H, k * 0.18f, 0, 0,
                            0, 0, 0, 0,
                            d->x, d->feet, -(z0 + 0.04f), 1};
    size_t at = 0;
    for (int pi = 0; pi < m->nparts; pi++) {
        const B3Part * p = &m->parts[pi];
        const float * uv = p->uv;
        if (d->shadow) {
            gfx_mesh_state(false, 0.2f);
            gfx_mesh_xf(flat, p->tex, m->pose[m->front] + at * 3, uv, 8, shadow_col, p->idx, p->tc * 3);
            gfx_mesh_state(true, 0.45f);
        }
        if (d->ink)
            gfx_mesh_xf(body, p->tex, m->ink[m->front] + at * 3, uv, 8, ink_col, p->idx, p->tc * 3);
        gfx_mesh_xf(body, p->tex, m->pose[m->front] + at * 3, uv, 8, m->col[m->front] + at, p->idx, p->tc * 3);
        at += (size_t)p->vc;
    }
}
