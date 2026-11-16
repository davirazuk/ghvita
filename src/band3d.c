// The band as World Tour's models: see band3d.h. The files are tools/band.py --3d's:
//   <name>.b3d  "B3D2", parts, bones, frames, layer bones, layer grooves, frets, chords; then each part: texture
//               name[32], vertex and triangle counts, wraps; vertices (B3Vert), u16 indices; then frames x bones skin
//               matrices (3 x 4 floats, rows); the layer's bone numbers; grooves x frets x chords x layer bones matrices
//   alt/<name>.b3a  "B3A2", bones, frames, the four layer counts, the matrices, the layer's bones and matrices
// Model space: metres, y up from the feet, the loop's centre at x = z = 0, facing -z (the audience).
// The layers are the fret hand apart from the body (the arm, the hand, the fingers): at each step of the groove, the
// hand at each of a few places along the neck in each of a few chord shapes. band3d_draw blends the ones the chart
// asks for over the frame's own arm.
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
    float view_t[2][3];         // where each posing put the feet, as the camera sees it
    int front;
    bool posed;                 // the front one has something in it
    int job;                    // the worker's slot posing it, -1 none
    B3Part * parts;
    float * mats, * alt;        // frames x bones x 12
    // the fret hand's layers: [0] the main loop's, [1] the wilder one's
    int lbones, lg, lp, lc;
    int32_t * lbone;
    float * layer[2];
    int anchor_bone[3];         // the head, the chest, the pelvis: the bone and its joint's place in the mesh
    float anchor_at[3][3];
};

static uint32_t * ink_col, * shadow_col;
static int buf_cap;

void band3d_release(Band3D * m);
static const float * frame_mats(const Band3D * m, bool alt, int f);

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

// a file's layer block: the bone numbers (the main file's only) and the matrices; false: damaged
static bool read_layers(Band3D * m, const uint8_t * d, long size, long at, const int32_t n[4], int which)
{
    if (n[0] <= 0 || n[1] <= 0 || n[2] <= 0 || n[3] <= 0)
        return true;            // none: a drummer, a singer
    if (n[0] > 256 || n[1] > 64 || n[2] > 16 || n[3] > 16)
        return false;
    size_t lb = (size_t)n[0] * 4, mb = (size_t)n[0] * n[1] * n[2] * n[3] * 12 * sizeof(float);
    if (at + (long)(lb + mb) > size)
        return false;
    if (which == 0) {
        m->lbones = n[0];
        m->lg = n[1];
        m->lp = n[2];
        m->lc = n[3];
        m->lbone = malloc(lb);
        memcpy(m->lbone, d + at, lb);
        for (int i = 0; i < m->lbones; i++)
            if (m->lbone[i] < 0 || m->lbone[i] >= m->nbones)
                return false;
    } else if (n[0] != m->lbones || n[1] != m->lg || n[2] != m->lp || n[3] != m->lc || memcmp(m->lbone, d + at, lb) != 0)
        return true;            // not the main loop's layout: the wilder loop goes without
    m->layer[which] = malloc(mb);
    memcpy(m->layer[which], d + at + lb, mb);
    return true;
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
    int32_t head[7];
    bool ok = size >= 32 && memcmp(d, "B3D2", 4) == 0;
    long at = 32;
    if (ok) {
        memcpy(head, d + 4, 28);
        m->nparts = head[0];
        m->nbones = head[1];
        m->nframes = head[2];
        ok = m->nparts > 0 && m->nparts < 256 && m->nbones > 0 && m->nbones <= 256 && m->nframes > 0;
    }
    if (ok)
        m->parts = calloc((size_t)m->nparts, sizeof(B3Part));
    for (int i = 0; ok && i < m->nparts; i++) {
        B3Part * p = &m->parts[i];
        if (at + 44 > size) {
            ok = false;
            break;
        }
        char tex[33] = {0};
        memcpy(tex, d + at, 32);
        int32_t c[3];
        memcpy(c, d + at + 32, 12);
        at += 44;
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
            for (int j = 0; j < 2; j++)
                if (p->v[k].j[j] >= m->nbones)
                    p->v[k].w[j] = 0;
        for (int k = 0; k < p->tc * 3; k++)
            if (p->idx[k] >= p->vc)
                p->idx[k] = 0;
        if (tex[0]) {
            snprintf(path, sizeof(path), "%sband3d/tex/%s.png", plat_asset_dir(), tex);
            p->tex = tex_load(path, TEX_MIPMAP | (c[2] ? TEX_REPEAT : 0));
        }
        if (p->vc > m->maxv)
            m->maxv = p->vc;
        m->total_v += p->vc;
    }
    size_t mb = (size_t)m->nframes * m->nbones * 12 * sizeof(float);
    if (ok && at + (long)mb <= size) {
        m->mats = malloc(mb);
        memcpy(m->mats, d + at, mb);
        ok = read_layers(m, d, size, at + (long)mb, head + 3, 0);
        // the camera's anchors come last
        long aa = at + (long)mb;
        if (m->lbones)
            aa += (long)m->lbones * 4 + (long)((size_t)m->lbones * m->lg * m->lp * m->lc * 12 * sizeof(float));
        int32_t na = 0;
        if (ok && aa + 4 <= size)
            memcpy(&na, d + aa, 4);
        for (int i = 0; i < 3; i++) {
            m->anchor_at[i][1] = i == 0 ? 1.62f : i == 1 ? 1.3f : 1.0f;
            if (i < na && aa + 4 + (i + 1) * 16 <= size) {
                memcpy(&m->anchor_bone[i], d + aa + 4 + i * 16, 4);
                memcpy(m->anchor_at[i], d + aa + 8 + i * 16, 12);
                if (m->anchor_bone[i] < 0 || m->anchor_bone[i] >= m->nbones)
                    m->anchor_bone[i] = 0;
            }
        }
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
    if (d && size >= 28 && memcmp(d, "B3A2", 4) == 0) {
        memcpy(head, d + 4, 24);
        size_t ab = (size_t)head[1] * head[0] * 12 * sizeof(float);
        if (head[0] == m->nbones && head[1] > 0 && 28 + (long)ab <= size) {
            m->alt = malloc(ab);
            memcpy(m->alt, d + 28, ab);
            m->alt_frames = head[1];
            if (m->lbones)
                read_layers(m, d, size, 28 + (long)ab, head + 2, 1);
        }
    }
    free(d);
    return m;
}

bool band3d_has_hands(const Band3D * m)
{
    return m && m->lbones > 0 && m->layer[0];
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
    free(m->lbone);
    free(m->layer[0]);
    free(m->layer[1]);
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

// where the head (0), the chest (1) or the pelvis (2) is in the pose asked for: the model's own space (metres, y up
// from the feet, x to the audience's right when it faces them, z toward the audience)
void band3d_anchor(const Band3D * m, const Band3DDraw * d, int which, float out[3])
{
    which = which < 0 ? 0 : which > 2 ? 2 : which;
    int b = m->anchor_bone[which];
    const float * A = frame_mats(m, d->alt_a, d->fa) + b * 12, * B = frame_mats(m, d->alt_b, d->fb) + b * 12;
    float wb = d->wb < 0 ? 0 : d->wb > 1 ? 1 : d->wb, wa = 1 - wb;
    const float * p = m->anchor_at[which];
    float v[3];
    for (int r = 0; r < 3; r++)
        v[r] = (A[r * 4] * wa + B[r * 4] * wb) * p[0] + (A[r * 4 + 1] * wa + B[r * 4 + 1] * wb) * p[1] +
               (A[r * 4 + 2] * wa + B[r * 4 + 2] * wb) * p[2] + A[r * 4 + 3] * wa + B[r * 4 + 3] * wb;
    out[0] = v[0];
    out[1] = v[1];
    out[2] = -v[2];
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
    // model (facing -z) to the stage (z toward the audience), turned by yaw; then the camera's orbit and pitch
    float cy = cosf(d->yaw), sy = sinf(d->yaw), cp = cosf(d->cam_pitch), sp = sinf(d->cam_pitch);
    float cc = cosf(d->cam_yaw), sc = sinf(d->cam_yaw);
    const float W[3][3] = {{cy, 0, sy}, {0, 1, 0}, {sy, 0, -cy}};
    const float C[3][3] = {{cc, 0, -sc}, {-sp * sc, cp, -sp * cc}, {cp * sc, sp, cp * cc}};
    float V[3][3], T[3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++)
            V[r][c] = C[r][0] * W[0][c] + C[r][1] * W[1][c] + C[r][2] * W[2][c];
        T[r] = C[r][0] * d->pos[0] + C[r][1] * d->pos[1] + C[r][2] * d->pos[2];
    }
    // the fret hand where the chart has it: the layer's bones from the layers instead of the frame, two places along
    // the neck and two chord shapes blended, at the frame's two groove steps
    static float LS[256][12];
    static uint8_t layered[256];
    memset(layered, 0, (size_t)m->nbones);
    const Band3DHands * hd = &d->hands;
    if (m->lbones > 0 && hd->ga >= 0) {
        const float * LA = m->layer[d->alt_a && m->layer[1] ? 1 : 0], * LB = m->layer[d->alt_b && m->layer[1] ? 1 : 0];
        float fp = hd->fret < 0 ? 0 : hd->fret > m->lp - 1 ? (float)(m->lp - 1) : hd->fret;
        int p0 = (int)fp, p1 = p0 + 1 < m->lp ? p0 + 1 : p0;
        float wp = fp - (float)p0, wc = hd->wc < 0 ? 0 : hd->wc > 1 ? 1 : hd->wc;
        int c0 = hd->ca % m->lc, c1 = hd->cb % m->lc, ga = hd->ga % m->lg, gb = hd->gb % m->lg;
        size_t stride = (size_t)m->lbones * 12;
        const float * src[8];
        float w8[8];
        for (int i = 0; i < 8; i++) {
            int g = i & 4 ? gb : ga, p = i & 2 ? p1 : p0, c = i & 1 ? c1 : c0;
            src[i] = (i & 4 ? LB : LA) + (((size_t)g * m->lp + p) * m->lc + c) * stride;
            w8[i] = (i & 4 ? wb : wa) * (i & 2 ? wp : 1 - wp) * (i & 1 ? wc : 1 - wc);
        }
        for (int l = 0; l < m->lbones; l++) {
            float * o = LS[m->lbone[l]];
            for (int k = 0; k < 12; k++) {
                float v = 0;
                for (int i = 0; i < 8; i++)
                    v += src[i][l * 12 + k] * w8[i];
                o[k] = v;
            }
            layered[m->lbone[l]] = 1;
        }
    }
    for (int b = 0; b < m->nbones; b++) {
        float S[12];
        if (layered[b])
            memcpy(S, LS[b], sizeof(S));
        else
            for (int k = 0; k < 12; k++)
                S[k] = A[b * 12 + k] * wa + B[b * 12 + k] * wb;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++)
                M[b][r * 4 + c] = V[r][0] * S[c] + V[r][1] * S[4 + c] + V[r][2] * S[8 + c];
            M[b][r * 4 + 3] += T[r];
        }
    }
    m->view_t[buf][0] = T[0];
    m->view_t[buf][1] = T[1];
    m->view_t[buf][2] = T[2];
    float feet_y = T[1];
    // the light from the audience (right, up, toward the camera), as tools/band_render.c's
    float L[3] = {d->key[0], d->key[1], d->key[2]}, ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    if (ll < 1e-3f) {
        L[0] = 0.25f;
        L[1] = 0.45f;
        L[2] = 0.85f;
        ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    }
    for (int i = 0; i < 3; i++)
        L[i] /= ll;
    float fill = d->fill < 0 ? 0 : d->fill > 1 ? 1 : d->fill;
    float tint[3] = {(d->tint & 255) / 255.0f, ((d->tint >> 8) & 255) / 255.0f, ((d->tint >> 16) & 255) / 255.0f};
    uint32_t ta = (d->tint >> 24) << 24;
    float ink_m = 1.6f / (d->ppm > 1 ? d->ppm : 1);      // the ink line's width in metres at this size
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
            if (d->ink) {
                // the ink line: the body a couple of pixels fatter along its outline, a little behind
                float sl = sqrtf(nx * nx + ny * ny) + 1e-6f, push = ink_m * (1 - fabsf(nz) * 0.5f) / sl;
                q[0] = x + nx * push;
                q[1] = y + ny * push;
                q[2] = z - 0.06f;
            }
            // half-lambert: the lit side full, the far side down to the fill
            float nl = 0.5f + 0.5f * (nx * L[0] + ny * L[1] + nz * L[2]);
            float lit = fill + (1 - fill) * nl * nl;
            float g = (y - feet_y) / CELL_H;
            g = g < 0 ? 0 : g > 1 ? 1 : g;
            float edge = 1 - fabsf(nz), rim = edge * edge * d->rim_k;
            float c[3];
            for (int ch = 0; ch < 3; ch++)
                c[ch] = lit * (d->bot[ch] + (d->top[ch] - d->bot[ch]) * g) * tint[ch] + rim * d->rim[ch];
            *col = (uint32_t)to8(c[0]) | (uint32_t)to8(c[1]) << 8 | (uint32_t)to8(c[2]) << 16 |
                   (v->w[3] == 255 ? ta : ((ta >> 24) * v->w[3] / 255) << 24);
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
#ifndef __vita__
                static uint64_t spent;
                static int posed, verts;
                uint64_t t0 = now_us();
#endif
                pose_model(m, &jobs[i].d, m->front ^ 1);
#ifndef __vita__
                spent += now_us() - t0;
                verts += m->total_v;
                if (++posed == 1000) {
                    plat_log("band3d: worker %.0f us a pose, %d vertices each", spent / 1000.0, verts / 1000);
                    spent = 0;
                    posed = verts = 0;
                }
#endif
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
    // onto the screen (the GPU's work): what the camera sees in metres (x right, y up, z toward it) through a lens
    // cam_dist from the target: w = 1 - z / dist, so what's nearer is bigger; the depth as gfx_2d's ortho has it (the
    // nearer, the smaller)
    float k = d->ppm, D = d->cam_dist > 0.5f ? d->cam_dist : 0.5f, zs = 0.1f;
    const float body[16] = {k, 0, 0, 0,
                            0, -k, 0, 0,
                            -d->cx / D, -d->cy / D, zs, -1 / D,
                            d->cx, d->cy, 0, 1};
    // the shadow: their shape laid down on the floor toward the camera, a fifth as tall, behind everything of theirs
    float fy = m->view_t[m->front][1];
    const float flat[16] = {k, 0, 0, 0,
                            0, k * 0.18f, 0, 0,
                            0, 0, 0, 0,
                            d->cx, d->floor_y - k * 0.18f * fy, -0.99f, 1};
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
