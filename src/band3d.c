// The band as World Tour's models: see band3d.h. The files are tools/band.py --3d's:
//   <name>.b3d  "B3D2", parts, bones, frames, layer bones, layer grooves, frets, chords; then each part: texture
//               name[32], vertex and triangle counts, wraps; vertices (B3Vert), u16 indices; then frames x bones skin
//               matrices (3 x 4 floats, rows); the layer's bone numbers; grooves x frets x chords x layer bones
//               matrices; the camera's anchors
//   far/<name>.b3d  "B3F1", parts, then the parts: the same body in a quarter of the triangles with a small texture,
//               for when they're small on the screen (the whole stage, the band in a line)
//   alt/<name>.b3a  "B3A2", bones, frames, the four layer counts, the matrices, the layer's bones and matrices
// Model space: metres, y up from the feet, the loop's centre at x = z = 0, facing -z (the audience).
// The layers are the fret hand apart from the body (the arm, the hand, the fingers): at each step of the groove, the
// hand at each of a few places along the neck in each of a few chord shapes. pose_model blends the ones the chart asks
// for over the frame's own arm.
// The CPU skins in the model's own space (a worker thread, a frame or so behind: only the animation is late); the
// camera is all in the matrix the GPU is given at the draw, so a cut or a camera move is never a frame behind the
// picture it's drawn over.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "band3d.h"
#include "gfx.h"
#include "plat.h"

#define CELL_H 2.13f            // the light's fall from head to toe is over this many metres

typedef struct {
    float p[3], n[3], uv[2];
    uint8_t j[4], w[4];         // two bones and their weights; w[3] the vertex's own alpha
} B3Vert;

typedef struct {
    Tex * tex;
    int vc, tc;
    B3Vert * v;
    float * uv;                 // the texture places packed tight (vitaGL copies what it's given every draw)
    uint16_t * idx;
} B3Part;

typedef struct {
    B3Part * parts;
    int nparts, total_v;
} B3Mesh;

struct Band3D {
    int nbones, nframes, alt_frames;
    B3Mesh lod[2];              // the whole body, and the far one (none: the whole one always)
    // two posings (pose_model): positions (3 floats a vertex) and colours, of the mesh in posed_lod. The worker
    // thread fills the back one while the front one's drawn
    float * pose[2];
    uint32_t * col[2];
    int posed_lod[2];
    int front;
    bool posed;                 // the front one has something in it
    int job;                    // the worker's slot posing it, -1 none
    float * mats, * alt;        // frames x bones x 12
    // the fret hand's layers: [0] the main loop's, [1] the wilder one's
    int lbones, lg, lp, lc;
    int32_t * lbone;
    float * layer[2];
    int anchor_bone[3];         // the head, the chest, the pelvis: the bone and its joint's place in the mesh
    float anchor_at[3][3];
    // the fret hand's fingers: the palm's bone, the fingers' bones, and 16 poses of them (a bit a finger: index, middle,
    // ring, little) as matrices that follow the palm's
    int palm, nfing;
    int32_t * fing;
    float * fing_pose;          // 16 x nfing x 12
};

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

// a file's parts from `at` on into a mesh; where they end, or -1: damaged
static long read_parts(B3Mesh * mesh, int nparts, int nbones, const uint8_t * d, long size, long at, bool mipmaps)
{
    char path[512];
    if (nparts <= 0 || nparts >= 256)
        return -1;
    mesh->parts = calloc((size_t)nparts, sizeof(B3Part));
    if (!mesh->parts)
        return -1;
    mesh->nparts = nparts;
    for (int i = 0; i < nparts; i++) {
        B3Part * p = &mesh->parts[i];
        if (at + 44 > size)
            return -1;
        char tex[33] = {0};
        memcpy(tex, d + at, 32);
        int32_t c[3];
        memcpy(c, d + at + 32, 12);
        at += 44;
        p->vc = c[0];
        p->tc = c[1];
        size_t vb = (size_t)p->vc * sizeof(B3Vert), ib = (size_t)p->tc * 6;
        if (p->vc <= 0 || p->vc > 65535 || p->tc <= 0 || at + (long)(vb + ib) > size)
            return -1;
        p->v = malloc(vb);
        p->idx = malloc(ib);
        p->uv = malloc((size_t)p->vc * 8);
        if (!p->v || !p->idx || !p->uv)
            return -1;
        memcpy(p->v, d + at, vb);
        for (int k = 0; k < p->vc; k++) {
            p->uv[k * 2] = p->v[k].uv[0];
            p->uv[k * 2 + 1] = p->v[k].uv[1];
        }
        memcpy(p->idx, d + at + vb, ib);
        at += (long)(vb + ib);
        for (int k = 0; k < p->vc; k++)
            for (int j = 0; j < 2; j++)
                if (p->v[k].j[j] >= nbones) {
                    p->v[k].j[j] = 0;
                    p->v[k].w[j] = j ? 0 : p->v[k].w[j];
                }
        for (int k = 0; k < p->tc * 3; k++)
            if (p->idx[k] >= p->vc)
                p->idx[k] = 0;
        if (tex[0]) {
            snprintf(path, sizeof(path), "%sband3d/tex/%s.png", plat_asset_dir(), tex);
            p->tex = tex_load(path, (mipmaps ? TEX_MIPMAP : 0) | (c[2] ? TEX_REPEAT : 0));
        }
        mesh->total_v += p->vc;
    }
    return at;
}

static void free_mesh(B3Mesh * mesh)
{
    for (int i = 0; mesh->parts && i < mesh->nparts; i++) {
        tex_free(mesh->parts[i].tex);
        free(mesh->parts[i].v);
        free(mesh->parts[i].uv);
        free(mesh->parts[i].idx);
    }
    free(mesh->parts);
    memset(mesh, 0, sizeof(*mesh));
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

Band3D * band3d_load(const char * name, int hue)
{
    gfx_load_hue(hue);
    char path[512];
    long size = 0;
    snprintf(path, sizeof(path), "%sband3d/%s.b3d", plat_asset_dir(), name);
    uint8_t * d = read_all(path, &size);
    if (!d) {
        gfx_load_hue(0);
        return NULL;
    }
    Band3D * m = calloc(1, sizeof(Band3D));
    m->job = -1;
    int32_t head[7] = {0};
    bool ok = size >= 32 && memcmp(d, "B3D2", 4) == 0;
    long at = 32;
    if (ok) {
        memcpy(head, d + 4, 28);
        m->nbones = head[1];
        m->nframes = head[2];
        ok = m->nbones > 0 && m->nbones <= 256 && m->nframes > 0;
    }
    if (ok) {
        at = read_parts(&m->lod[0], head[0], m->nbones, d, size, at, true);
        ok = at > 0;
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
        long fa = aa + 4 + (long)na * 16;
        if (ok && na >= 0 && na <= 8 && fa + 12 <= size && memcmp(d + fa, "FNG1", 4) == 0) {
            int32_t fh[2];
            memcpy(fh, d + fa + 4, 8);
            size_t ib = (size_t)fh[1] * 4, pb = (size_t)fh[1] * 16 * 12 * sizeof(float);
            if (fh[0] >= 0 && fh[0] < m->nbones && fh[1] > 0 && fh[1] <= 32 && fa + 12 + (long)(ib + pb) <= size) {
                m->palm = fh[0];
                m->nfing = fh[1];
                m->fing = malloc(ib);
                m->fing_pose = malloc(pb);
                memcpy(m->fing, d + fa + 12, ib);
                memcpy(m->fing_pose, d + fa + 12 + ib, pb);
                for (int i = 0; i < m->nfing; i++)
                    if (m->fing[i] < 0 || m->fing[i] >= m->nbones)
                        m->nfing = 0;
            }
        }
    } else
        ok = false;
    free(d);
    if (!ok) {
        plat_log("band3d: %s is damaged", name);
        band3d_free(m);
        gfx_load_hue(0);
        return NULL;
    }
    // the far body, when there is one
    snprintf(path, sizeof(path), "%sband3d/far/%s.b3d", plat_asset_dir(), name);
    d = read_all(path, &size);
    if (d && size >= 8 && memcmp(d, "B3F1", 4) == 0) {
        int32_t np = 0;
        memcpy(&np, d + 4, 4);
        if (read_parts(&m->lod[1], np, m->nbones, d, size, 8, false) < 0)
            free_mesh(&m->lod[1]);
    }
    free(d);
    gfx_load_hue(0);
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
    band3d_release(m);
    free_mesh(&m->lod[0]);
    free_mesh(&m->lod[1]);
    free(m->mats);
    free(m->alt);
    free(m->lbone);
    free(m->layer[0]);
    free(m->layer[1]);
    free(m->fing);
    free(m->fing_pose);
    for (int i = 0; i < 2; i++) {
        free(m->pose[i]);
        free(m->col[i]);
    }
    free(m);
}

int band3d_frames(const Band3D * m, bool alt)
{
    return !m ? 0 : alt ? m->alt_frames : m->nframes;
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

// The camera. The model (facing -z) on the stage (x to the audience's right, y up, z toward the audience), turned by
// yaw and stood at pos; the camera's orbit and pitch about the target; then the lens: w = 1 - z / dist, so what's
// nearer is bigger, the target landing on (cx, cy) at ppm pixels a metre; the depth as gfx_2d's ortho has it (the
// nearer, the smaller). CW: model to the camera's axes (x right, y up, z toward it); model: false for things given in
// the stage's own axes (the floor)
static void camera(const Band3DDraw * d, bool model, float mv[16], float CW[3][3])
{
    float cy = model ? cosf(d->yaw) : 1, sy = model ? sinf(d->yaw) : 0, cp = cosf(d->cam_pitch), sp = sinf(d->cam_pitch);
    float cc = cosf(d->cam_yaw), sc = sinf(d->cam_yaw);
    const float W[3][3] = {{cy, 0, sy}, {0, 1, 0}, {sy, 0, -cy}}, I[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float C[3][3] = {{cc, 0, -sc}, {-sp * sc, cp, -sp * cc}, {cp * sc, sp, cp * cc}};
    const float (*M)[3] = model ? W : I;
    float T[3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++)
            CW[r][c] = C[r][0] * M[0][c] + C[r][1] * M[1][c] + C[r][2] * M[2][c];
        T[r] = C[r][0] * d->pos[0] + C[r][1] * d->pos[1] + C[r][2] * d->pos[2];
    }
    // the depth: what's between 0.25 m and 400 m from the lens, as a perspective projection lays it out (the venue's far
    // walls and a guitar's head by the lens in the same buffer); gfx_2d's ortho turns the sign
    float k = d->ppm, D = d->cam_dist > 0.5f ? d->cam_dist : 0.5f, w0 = 1 - T[2] / D;
    const float zn = 0.25f, zf = 400, za = (zf + zn) / (zf - zn), zb = 2 * zf * zn / (zf - zn);
    for (int c = 0; c < 3; c++) {
        mv[c * 4 + 0] = k * CW[0][c] - d->cx / D * CW[2][c];
        mv[c * 4 + 1] = -k * CW[1][c] - d->cy / D * CW[2][c];
        mv[c * 4 + 2] = za / D * CW[2][c];
        mv[c * 4 + 3] = -CW[2][c] / D;
    }
    mv[12] = k * T[0] + d->cx * w0;
    mv[13] = -k * T[1] + d->cy * w0;
    mv[14] = -za + zb / D + za / D * T[2];
    mv[15] = w0;
}

typedef struct {
    float M[256][12], LS[256][12];
    uint8_t layered[256];
} Scratch;
static Scratch scratch_main, scratch_worker;    // pose_model runs on two threads: each its own

static void pose_model(Band3D * m, const Band3DDraw * d, int buf, Scratch * sc)
{
    float (*M)[12] = sc->M, (*LS)[12] = sc->LS;
    uint8_t * layered = sc->layered;
    const float * A = frame_mats(m, d->alt_a, d->fa), * B = frame_mats(m, d->alt_b, d->fb);
    float wb = d->wb < 0 ? 0 : d->wb > 1 ? 1 : d->wb, wa = 1 - wb;
    float w2 = d->w2 < 0 ? 0 : d->w2 > 1 ? 1 : d->w2;
    const float * A2 = frame_mats(m, d->alt_a, d->fa2), * B2 = frame_mats(m, d->alt_b, d->fb2);
    memset(layered, 0, (size_t)m->nbones);
    const Band3DHands * hd = &d->hands;
    if (m->lbones > 0 && hd->ga >= 0) {
        // the fret hand where the chart has it: two places along the neck and two chord shapes, at the frame's two
        // groove steps
        const float * LA = m->layer[d->alt_a && m->layer[1] ? 1 : 0], * LB = m->layer[d->alt_b && m->layer[1] ? 1 : 0];
        float fp = hd->fret < 0 ? 0 : hd->fret > m->lp - 1 ? (float)(m->lp - 1) : hd->fret;
        int p0 = (int)fp, p1 = p0 + 1 < m->lp ? p0 + 1 : p0;
        float wp = fp - (float)p0, wc = hd->wc < 0 ? 0 : hd->wc > 1 ? 1 : hd->wc;
        // with the fingers' own poses the layers carry the arm only: their one hand shape
        int c0 = m->nfing ? 0 : hd->ca % m->lc, c1 = m->nfing ? 0 : hd->cb % m->lc, ga = hd->ga % m->lg, gb = hd->gb % m->lg;
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
        if (layered[b])
            memcpy(M[b], LS[b], sizeof(M[b]));
        else if (w2 > 0)
            for (int k = 0; k < 12; k++)
                M[b][k] = (A[b * 12 + k] * wa + B[b * 12 + k] * wb) * (1 - w2) + (A2[b * 12 + k] * wa + B2[b * 12 + k] * wb) * w2;
        else
            for (int k = 0; k < 12; k++)
                M[b][k] = A[b * 12 + k] * wa + B[b * 12 + k] * wb;
    }
    if (m->nfing && m->lbones > 0 && hd->ga >= 0) {
        // the fingers the notes ask for, hung on the palm where the layers put it: two sets of them, blended
        const float * P = M[m->palm];
        const float * Ca = m->fing_pose + (size_t)(hd->ca & 15) * m->nfing * 12, * Cb = m->fing_pose + (size_t)(hd->cb & 15) * m->nfing * 12;
        float wc = hd->wc < 0 ? 0 : hd->wc > 1 ? 1 : hd->wc;
        for (int i = 0; i < m->nfing; i++) {
            float C[12], * o = M[m->fing[i]];
            for (int k = 0; k < 12; k++)
                C[k] = Ca[i * 12 + k] * (1 - wc) + Cb[i * 12 + k] * wc;
            for (int r = 0; r < 3; r++) {
                for (int k = 0; k < 4; k++)
                    o[r * 4 + k] = P[r * 4] * C[k] + P[r * 4 + 1] * C[4 + k] + P[r * 4 + 2] * C[8 + k];
                o[r * 4 + 3] += P[r * 4 + 3];
            }
        }
    }
    // the light and the camera's line of sight, brought into the model's space (the light's a frame late with the
    // rest of the pose: nothing anyone sees)
    float mv[16], CW[3][3];
    camera(d, true, mv, CW);
    float Lv[3] = {d->key[0], d->key[1], d->key[2]}, ll = sqrtf(Lv[0] * Lv[0] + Lv[1] * Lv[1] + Lv[2] * Lv[2]);
    if (ll < 1e-3f) {
        Lv[0] = 0.25f;
        Lv[1] = 0.45f;
        Lv[2] = 0.85f;
        ll = sqrtf(Lv[0] * Lv[0] + Lv[1] * Lv[1] + Lv[2] * Lv[2]);
    }
    float L[3], E[3];
    for (int j = 0; j < 3; j++) {
        L[j] = (Lv[0] * CW[0][j] + Lv[1] * CW[1][j] + Lv[2] * CW[2][j]) / ll * 0.5f;
        E[j] = CW[2][j];
    }
    float fill = d->fill < 0 ? 0 : d->fill > 1 ? 1 : d->fill, lift = 1 - fill;
    float tint[3] = {(d->tint & 255) / 255.0f, ((d->tint >> 8) & 255) / 255.0f, ((d->tint >> 16) & 255) / 255.0f};
    float bot[3], span[3], rimc[3];
    for (int ch = 0; ch < 3; ch++) {
        bot[ch] = d->bot[ch] * tint[ch] * 255;
        span[ch] = (d->top[ch] - d->bot[ch]) * tint[ch] * 255 / CELL_H;
        rimc[ch] = d->rim[ch] * d->rim_k * 255;
    }
    uint32_t ta = d->tint >> 24;
    int lod = d->far && m->lod[1].nparts ? 1 : 0;
    const B3Mesh * mesh = &m->lod[lod];
    float * o = m->pose[buf];
    uint32_t * col = m->col[buf];
    for (int pi = 0; pi < mesh->nparts; pi++) {
        const B3Part * p = &mesh->parts[pi];
        const B3Vert * v = p->v;
        for (int i = 0; i < p->vc; i++, v++, o += 3, col++) {
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
            // half-lambert: the lit side full, the far side down to the fill
            float nl = 0.5f + nx * L[0] + ny * L[1] + nz * L[2];
            float lit = fill + lift * nl * nl;
            float g = y < 0 ? 0 : y > CELL_H ? CELL_H : y;
            float edge = 1 - fabsf(nx * E[0] + ny * E[1] + nz * E[2]), rim = edge * edge;
            float r0 = lit * (bot[0] + span[0] * g) + rim * rimc[0];
            float g0 = lit * (bot[1] + span[1] * g) + rim * rimc[1];
            float b0 = lit * (bot[2] + span[2] * g) + rim * rimc[2];
            uint32_t ri = r0 > 255 ? 255u : (uint32_t)r0, gi = g0 > 255 ? 255u : (uint32_t)g0, bi = b0 > 255 ? 255u : (uint32_t)b0;
            *col = ri | gi << 8 | bi << 16 | (v->w[3] == 255 ? ta : ta * v->w[3] / 255) << 24;
        }
    }
    m->posed_lod[buf] = lod;
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
    uint64_t spent = 0;
    int posed = 0, verts = 0;
    for (;;) {
        bool did = false;
        for (int i = 0; i < 4; i++)
            if (__atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE) == 1) {
                Band3D * m = jobs[i].m;
                uint64_t t0 = (uint64_t)plat_now_us();
                pose_model(m, &jobs[i].d, m->front ^ 1, &scratch_worker);
                spent += (uint64_t)plat_now_us() - t0;
                verts += m->lod[m->posed_lod[m->front ^ 1]].total_v;
                if (++posed == 3000) {
                    // in the log (the Vita's too): what the band costs
                    plat_log("band3d: %d us a pose, %d vertices each", (int)(spent / 3000), verts / 3000);
                    spent = 0;
                    posed = verts = 0;
                }
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

void band3d_begin(bool clear)
{
    gfx_mesh_begin(clear);
}

void band3d_end(void)
{
    gfx_mesh_end();
}

void band3d_draw(Band3D * m, const Band3DDraw * d)
{
    if (!m)
        return;
    if (!m->pose[0]) {
        int cap = m->lod[0].total_v > m->lod[1].total_v ? m->lod[0].total_v : m->lod[1].total_v;
        for (int i = 0; i < 2; i++) {
            m->pose[i] = malloc((size_t)cap * 12);
            m->col[i] = malloc((size_t)cap * 4);
        }
    }
    for (int i = 0; i < 2; i++)
        if (!m->pose[i] || !m->col[i])
            return;
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
        pose_model(m, d, m->front, &scratch_main);      // the first time: now, here
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
    // the pose as the worker last left it, through this frame's camera
    float mv[16], CW[3][3];
    camera(d, true, mv, CW);
    const B3Mesh * mesh = &m->lod[m->posed_lod[m->front]];
    size_t at = 0;
    for (int pi = 0; pi < mesh->nparts; pi++) {
        const B3Part * p = &mesh->parts[pi];
        gfx_mesh_xf(mv, p->tex, m->pose[m->front] + at * 3, p->uv, 8, m->col[m->front] + at, p->idx, p->tc * 3);
        at += (size_t)p->vc;
    }
}

// a flat four-cornered thing on the stage (its own axes: x right, y up, z toward the audience, metres from what the
// camera looks at) through a camera: the floor, a shadow on it. Between band3d_begin and band3d_end; it neither reads
// nor writes the depth
void band3d_quad(const Band3DDraw * cam, const float p[4][3], Tex * tex, const float uv[4][2], const uint32_t col[4])
{
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    float mv[16], CW[3][3];
    camera(cam, false, mv, CW);
    gfx_mesh_state(false, 0.004f);
    gfx_mesh_depth(false);
    gfx_mesh_xf(mv, tex, &p[0][0], &uv[0][0], 8, col, idx, 6);
    gfx_mesh_depth(true);
    gfx_mesh_state(true, 0.45f);
}

// ---------------------------------------------------------------------------------------------------- the venue
// World Tour's venue as a model (tools/venue3d.py, assets/venue3d/<zone>.v3d): in the stage's own axes, drawn through
// the same camera as the band, behind it. Unlit as the game has it (its light is in its textures); the show's colour and
// the dark away from the stage are a colour a vertex, made here from the light the tool left on each.
typedef struct {
    float p[3], uv[2];
    uint8_t l[4];
} V3Vert;

typedef struct {
    Tex * tex;
    int vc, tc;
    float * xyz, * uv;
    uint8_t * light;
    uint32_t * col;
    uint16_t * idx;
} V3Part;

struct Venue3D {
    int nparts;
    V3Part * parts;
    float marks[4][3];
    float tint[3];              // what col was last made for
};

void venue3d_free(Venue3D * v)
{
    if (!v)
        return;
    for (int i = 0; v->parts && i < v->nparts; i++) {
        tex_free(v->parts[i].tex);
        free(v->parts[i].xyz);
        free(v->parts[i].uv);
        free(v->parts[i].light);
        free(v->parts[i].col);
        free(v->parts[i].idx);
    }
    free(v->parts);
    free(v);
}

Venue3D * venue3d_load(const char * zone)
{
    char path[512];
    long size = 0;
    snprintf(path, sizeof(path), "%svenue3d/%s.v3d", plat_asset_dir(), zone);
    uint8_t * d = read_all(path, &size);
    if (!d)
        return NULL;
    Venue3D * v = calloc(1, sizeof(Venue3D));
    bool ok = size >= 56 && memcmp(d, "V3D1", 4) == 0;
    // its textures, packed beside it (<zone>.v3t); without the pack, loose in venue3d/tex as they first were
    long tsize = 0;
    snprintf(path, sizeof(path), "%svenue3d/%s.v3t", plat_asset_dir(), zone);
    uint8_t * td = read_all(path, &tsize);
    int32_t tn = 0;
    if (td && tsize >= 8 && memcmp(td, "V3T1", 4) == 0)
        memcpy(&tn, td + 4, 4);
    if (tn < 0 || tn > 4096 || 8 + (long)tn * 36 > tsize)
        tn = 0;
    long at = 56;
    int32_t np = 0;
    if (ok) {
        memcpy(&np, d + 4, 4);
        memcpy(v->marks, d + 8, 48);
        ok = np > 0 && np < 1024;
    }
    if (ok) {
        v->parts = calloc((size_t)np, sizeof(V3Part));
        v->nparts = np;
    }
    for (int i = 0; ok && i < np; i++) {
        V3Part * p = &v->parts[i];
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
        size_t vb = (size_t)p->vc * sizeof(V3Vert), ib = (size_t)p->tc * 6;
        if (p->vc <= 0 || p->vc > 65535 || p->tc <= 0 || at + (long)(vb + ib) > size) {
            ok = false;
            break;
        }
        p->xyz = malloc((size_t)p->vc * 12);
        p->uv = malloc((size_t)p->vc * 8);
        p->light = malloc((size_t)p->vc);
        p->col = malloc((size_t)p->vc * 4);
        p->idx = malloc(ib);
        if (!p->xyz || !p->uv || !p->light || !p->col || !p->idx) {
            ok = false;
            break;
        }
        const V3Vert * src = (const V3Vert *)(d + at);
        for (int k = 0; k < p->vc; k++) {
            V3Vert one;
            memcpy(&one, &src[k], sizeof(one));
            memcpy(p->xyz + k * 3, one.p, 12);
            memcpy(p->uv + k * 2, one.uv, 8);
            p->light[k] = one.l[0];
        }
        memcpy(p->idx, d + at + vb, ib);
        for (int k = 0; k < p->tc * 3; k++)
            if (p->idx[k] >= p->vc)
                p->idx[k] = 0;
        at += (long)(vb + ib);
        if (tex[0]) {
            int flags = TEX_MIPMAP | (c[2] ? TEX_REPEAT : 0);
            long blob = 8 + (long)tn * 36;
            for (int k = 0; k < tn && !p->tex; k++) {
                uint32_t len;
                memcpy(&len, td + 8 + k * 36 + 32, 4);
                if (blob + (long)len > tsize)
                    break;
                if (strncmp((const char *)td + 8 + k * 36, tex, 32) == 0)
                    p->tex = tex_load_mem(td + blob, (int)len, flags);
                blob += (long)len;
            }
            if (!p->tex && !tn) {
                snprintf(path, sizeof(path), "%svenue3d/tex/%s.png", plat_asset_dir(), tex);
                p->tex = tex_load(path, flags);
            }
        }
    }
    free(d);
    free(td);
    if (!ok) {
        plat_log("venue3d: %s is damaged", zone);
        venue3d_free(v);
        return NULL;
    }
    v->tint[0] = -1;
    return v;
}

const float * venue3d_mark(const Venue3D * v, int role)
{
    return v->marks[role & 3];
}

// the venue through a camera: cam->pos is where the stage's origin (the floor under the drummer) is from what the
// camera looks at. Writes the depth: the band drawn after it stands in it
void venue3d_draw(Venue3D * v, const Band3DDraw * cam, const float tint[3])
{
    if (!v)
        return;
    if (fabsf(tint[0] - v->tint[0]) + fabsf(tint[1] - v->tint[1]) + fabsf(tint[2] - v->tint[2]) > 0.012f) {
        // the show's colour moved: every vertex's colour again (its own light times it)
        memcpy(v->tint, tint, sizeof(v->tint));
        int t[3];
        for (int c = 0; c < 3; c++)
            t[c] = (int)(tint[c] * 256);
        for (int i = 0; i < v->nparts; i++) {
            V3Part * p = &v->parts[i];
            for (int k = 0; k < p->vc; k++) {
                int l = p->light[k], r = l * t[0] >> 8, g = l * t[1] >> 8, b = l * t[2] >> 8;
                p->col[k] = (uint32_t)(r > 255 ? 255 : r) | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b) << 16 | 0xff000000u;
            }
        }
    }
    float mv[16], CW[3][3];
    camera(cam, false, mv, CW);
    for (int i = 0; i < v->nparts; i++) {
        const V3Part * p = &v->parts[i];
        gfx_mesh_xf(mv, p->tex, p->xyz, p->uv, 8, p->col, p->idx, p->tc * 3);
    }
}

// whether the venue stands between two places on the stage (its axes): a camera there wouldn't see who it's on
bool venue3d_blocked(const Venue3D * v, const float from[3], const float to[3])
{
    if (!v)
        return false;
    float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    for (int i = 0; i < v->nparts; i++) {
        const V3Part * p = &v->parts[i];
        for (int t = 0; t < p->tc; t++) {
            const float * a = p->xyz + p->idx[t * 3] * 3, * b = p->xyz + p->idx[t * 3 + 1] * 3, * c = p->xyz + p->idx[t * 3 + 2] * 3;
            float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            float h[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
            if (det > -1e-7f && det < 1e-7f)
                continue;
            float f = 1 / det, s[3] = {from[0] - a[0], from[1] - a[1], from[2] - a[2]};
            float u = f * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);
            if (u < 0 || u > 1)
                continue;
            float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
            float w = f * (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]);
            if (w < 0 || u + w > 1)
                continue;
            float k = f * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);
            if (k > 0.02f && k < 0.9f)
                return true;            // short of them: the last tenth is their own stand, their monitor
        }
    }
    return false;
}
