// Renders a decoded GHWT venue (YargLook's venue_ghwt_<zone>.wtch/.json/.parts.json, made by
// refs/yarg/ghwt-tools/export_venues.py) from the audience, for GH Vita to draw behind the highway:
//   <out>_base.png  the stage, its lighting baked in (opaque, alpha-tested and see-through parts)
//   <out>_glow.png  its lights alone (GHWT's additive parts, hidden where the stage is in front of them) on black, for
//                   the game to pulse with the music
//   <out>_marks.txt where the band's marks land on the picture (x y scale per role, in output pixels)
// The parts are drawn the way YargLook's CharacterLoader draws them: opaque parts alpha-tested at 0.45 and self-lit,
// soft-alpha "opaque" art (beams, haze) blended at 35%, mostly-clear art blended (or as light when bright and coloured),
// GHWT's darkening passes left out.
// usage: venue_render <textures dir> <zone> <out prefix> [back up look fov shift]  (camera: back/up metres from the drummer
// toward the audience, look = height looked at above the drummer's mark, fov = vertical degrees, shift = lens shift up in half screens)
// Build: gcc -O2 -o build-tools/venue_render tools/venue_render.c -Isrc -lSDL2 -lGL -lm ; run on any GL 2.1 display.
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "ext/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ext/stb_image_write.h"

#define W 1920
#define H 1088

typedef struct {
    float p[3], n[3], uv[2], a;
    uint16_t j[4];
    float w[4];
} Vert;

typedef struct {
    char tex[33];
    int vc, tc;
    Vert * v;
    uint32_t * t;
    char blend[24];
    char cls[8];                    // what its texture looks like: opaque, soft, blend or glowpm (see classify)
    bool glare;                     // a lens-glare sprite (grey, all soft edges): GHWT fades these with the view
    bool solid;                     // its texture is mostly opaque (a see-through part that's really scenery)
    float gain;                     // strength cap from its share of the picture (see probe)
    bool hidden;
    GLuint gl, light;               // the texture, and its light version (alpha = alpha x brightness)
    bool has_tex;
    float cx, cy, cz;               // centre, for sorting see-through parts
} Mesh;

static Mesh * meshes;
static int mesh_count;
static const char * tex_dir;

static void * slurp(const char * path, size_t * len)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char * d = malloc(n + 1);
    if (fread(d, 1, n, f) != (size_t)n) {
        fclose(f);
        free(d);
        return NULL;
    }
    d[n] = 0;
    fclose(f);
    if (len)
        *len = n;
    return d;
}

static bool load_wtch(const char * path)
{
    size_t len;
    unsigned char * d = slurp(path, &len);
    if (!d || memcmp(d, "WTCH", 4) != 0)
        return false;
    size_t o = 8;
    uint32_t nb;
    memcpy(&nb, d + o, 4);
    o += 4;
    o += nb * (32 + 4 + 12 + 16 + 64);              // bones: a venue has its one root
    uint32_t nm;
    memcpy(&nm, d + o, 4);
    o += 4;
    meshes = calloc(nm, sizeof(Mesh));
    mesh_count = nm;
    for (uint32_t k = 0; k < nm; k++) {
        Mesh * m = &meshes[k];
        memcpy(m->tex, d + o, 32);
        o += 32;
        uint32_t vc, tc;
        memcpy(&vc, d + o, 4);
        memcpy(&tc, d + o + 4, 4);
        o += 8;
        m->vc = vc;
        m->tc = tc;
        m->v = malloc(vc * sizeof(Vert));
        for (uint32_t i = 0; i < vc; i++) {
            Vert * v = &m->v[i];
            memcpy(v->p, d + o, 12);
            v->p[2] = -v->p[2];         // Unity's left-handed space back to GHWT's (else the stage comes out mirrored)
            memcpy(v->n, d + o + 12, 12);
            memcpy(v->uv, d + o + 24, 8);
            memcpy(&v->a, d + o + 32, 4);
            memcpy(v->j, d + o + 36, 8);
            memcpy(v->w, d + o + 44, 16);
            o += 60;
        }
        m->t = malloc(tc * 12);
        memcpy(m->t, d + o, tc * 12);
        o += tc * 12;
        double sx = 0, sy = 0, sz = 0;
        for (uint32_t i = 0; i < vc; i++) {
            sx += m->v[i].p[0];
            sy += m->v[i].p[1];
            sz += m->v[i].p[2];
        }
        if (vc) {
            m->cx = sx / vc;
            m->cy = sy / vc;
            m->cz = sz / vc;
        }
        strcpy(m->blend, "opaque");
    }
    free(d);
    return true;
}

// the few fields of the venue's .json and .parts.json this needs
static float mark[4][3];
static bool has_mark[4];
static const char * roles[4] = {"drummer", "vocalist", "guitarist", "bassist"};
static float facing[3] = {0, 0, -1};

static const char * json_vec(const char * s, const char * key, float out[3])
{
    char k[64];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char * p = strstr(s, k);
    if (!p)
        return NULL;
    p = strchr(p, '[');
    if (!p || sscanf(p, "[%f , %f , %f", &out[0], &out[1], &out[2]) != 3)
        return NULL;
    return p;
}

static void load_json(const char * zone)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/venue_ghwt_%s.json", tex_dir, zone);
    char * j = slurp(path, NULL);
    if (j) {
        for (int r = 0; r < 4; r++) {
            has_mark[r] = json_vec(j, roles[r], mark[r]) != NULL;
            mark[r][2] = -mark[r][2];
        }
        json_vec(j, "facing", facing);
        facing[2] = -facing[2];
        const char * h = strstr(j, "\"hide\"");
        if (h && (h = strchr(h, '['))) {
            h++;
            while (*h && *h != ']') {
                char * e;
                long k = strtol(h, &e, 10);
                if (e == h)
                    break;
                if (k >= 0 && k < mesh_count)
                    meshes[k].hidden = true;
                h = e;
                while (*h == ',' || *h == ' ')
                    h++;
            }
        }
        free(j);
    }
    snprintf(path, sizeof(path), "%s/venue_ghwt_%s.parts.json", tex_dir, zone);
    j = slurp(path, NULL);
    if (j) {
        const char * p = strchr(j, '[');
        for (int k = 0; p && k < mesh_count; k++) {
            p = strchr(p, '"');
            if (!p)
                break;
            const char * e = strchr(p + 1, '"');
            if (!e)
                break;
            size_t n = e - p - 1 < sizeof(meshes[k].blend) - 1 ? (size_t)(e - p - 1) : sizeof(meshes[k].blend) - 1;
            memcpy(meshes[k].blend, p + 1, n);
            meshes[k].blend[n] = 0;
            p = e + 1;
        }
        free(j);
    }
    for (int k = 0; k < mesh_count; k++)
        if (strstr(meshes[k].tex, "_crowd_at"))
            meshes[k].hidden = true;
    const char * extra = getenv("VR_HIDE");
    while (extra && *extra) {
        char * e;
        long k = strtol(extra, &e, 10);
        if (e == extra)
            break;
        if (k >= 0 && k < mesh_count)
            meshes[k].hidden = true;
        extra = *e ? e + 1 : e;
    }
    // VR_HIDE_TEX="a b": parts whose texture name contains any of these
    const char * ht = getenv("VR_HIDE_TEX");
    char word[64];
    int used;
    while (ht && sscanf(ht, "%63s%n", word, &used) == 1) {
        for (int k = 0; k < mesh_count; k++)
            if (strstr(meshes[k].tex, word))
                meshes[k].hidden = true;
        ht += used;
    }
    snprintf(path, sizeof(path), "%s/venue_ghwt_%s.moods.txt", tex_dir, zone);
    FILE * f = fopen(path, "r");
    char line[128];
    while (f && fgets(line, sizeof(line), f)) {
        int k;
        char mood[64];
        if (sscanf(line, "%d %63s", &k, mood) == 2 && k >= 0 && k < mesh_count && strcmp(mood, "exposition") != 0)
            meshes[k].hidden = true;
    }
    if (f)
        fclose(f);
}

// the YargLook rules for "opaque" art that isn't (see CharacterLoader.SoftAlpha / MostlyClear)
static void classify(Mesh * m, const unsigned char * px, int w, int h)
{
    int n = 0, mid = 0, low = 0, clear = 0, seen = 0;
    double lum = 0, sat = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const unsigned char * c = px + (y * w + x) * 4;
            n++;
            if (c[3] > 20 && c[3] < 235)
                mid++;
            if (c[3] < 235)
                low++;
            if (c[3] < 16) {
                clear++;
                continue;
            }
            if (c[3] < 128)
                continue;
            seen++;
            int mx = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
            int mn = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
            lum += mx;
            sat += mx - mn;
        }
    strcpy(m->cls, "opaque");
    // (exported as huge fixed quads, ten of them whited out the Forum, the Ice Cave and Donington)
    double all_sat = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const unsigned char * c = px + (y * w + x) * 4;
            int mx = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
            int mn = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
            all_sat += mx - mn;
        }
    m->glare = n && all_sat / n < 14 && mid > n * 0.5;
    // (a plain white card isn't scenery: it's a light panel GHWT tints, Nashville's windows and riser)
    m->solid = n && low < n * 0.2 && !(seen && lum / seen > 235 && all_sat / n < 10);
    if (n && mid > n * 0.3 && low > n * 0.4)
        strcpy(m->cls, "soft");
    else if (n && clear > n * 0.4)
        strcpy(m->cls, seen && lum / seen > 170 && sat / seen > 90 ? "glowpm" : "blend");
}

static GLuint upload(const unsigned char * px, int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

static void load_textures(void)
{
    for (int k = 0; k < mesh_count; k++) {
        Mesh * m = &meshes[k];
        // the same texture for many parts: share it
        for (int i = 0; i < k; i++)
            if (meshes[i].has_tex && strcmp(meshes[i].tex, m->tex) == 0) {
                m->gl = meshes[i].gl;
                m->light = meshes[i].light;
                m->has_tex = true;
                strcpy(m->cls, meshes[i].cls);
                m->glare = meshes[i].glare;
                m->solid = meshes[i].solid;
                break;
            }
        if (m->has_tex || !m->tex[0])
            continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s.png", tex_dir, m->tex);
        int w, h, c;
        unsigned char * px = stbi_load(path, &w, &h, &c, 4);
        // the export's names stop at 31 characters ("..._crowd_atlas0_d" for "..._crowd_atlas0_dds.png")
        if (!px && strlen(m->tex) == 31) {
            DIR * d = opendir(tex_dir);
            struct dirent * e;
            while (d && (e = readdir(d)))
                if (strncmp(e->d_name, m->tex, 31) == 0 && strstr(e->d_name, ".png")) {
                    snprintf(path, sizeof(path), "%s/%s", tex_dir, e->d_name);
                    px = stbi_load(path, &w, &h, &c, 4);
                    break;
                }
            if (d)
                closedir(d);
        }
        if (!px)
            continue;
        classify(m, px, w, h);
        m->gl = upload(px, w, h);
        // light art: alpha = alpha x brightness, colour brought up to full
        for (int i = 0; i < w * h; i++) {
            unsigned char * q = px + i * 4;
            int mx = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]);
            float s = mx > 0 ? 255.0f / mx : 0;
            for (int ch = 0; ch < 3; ch++) {
                float v = q[ch] * s;
                q[ch] = v > 255 ? 255 : (unsigned char)v;
            }
            q[3] = (unsigned char)(q[3] * mx / 255);
        }
        m->light = upload(px, w, h);
        m->has_tex = true;
        stbi_image_free(px);
    }
    for (int k = 0; k < mesh_count; k++)
        if (!strcmp(meshes[k].blend, "opaque") && meshes[k].cls[0])
            strcpy(meshes[k].blend, meshes[k].cls);
}

static void draw_mesh(const Mesh * m, float r, float g, float b, float a, bool light)
{
    if (m->has_tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, light ? m->light : m->gl);
    } else {
        glDisable(GL_TEXTURE_2D);
        r = g = b = 0.17f;              // an untextured part: an off screen, not a white card
    }
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < m->tc * 3; i++) {
        const Vert * v = &m->v[m->t[i]];
        glColor4f(r, g, b, a * (v->a > 0 ? v->a : 1));
        glTexCoord2f(v->uv[0], v->uv[1]);
        glVertex3f(v->p[0], v->p[1], v->p[2]);
    }
    glEnd();
}

static float eye[3];

static int by_distance(const void * x, const void * y)
{
    const Mesh * a = *(Mesh * const *)x, * b = *(Mesh * const *)y;
    float da = (a->cx - eye[0]) * (a->cx - eye[0]) + (a->cy - eye[1]) * (a->cy - eye[1]) + (a->cz - eye[2]) * (a->cz - eye[2]);
    float db = (b->cx - eye[0]) * (b->cx - eye[0]) + (b->cy - eye[1]) * (b->cy - eye[1]) + (b->cz - eye[2]) * (b->cz - eye[2]);
    return da < db ? 1 : da > db ? -1 : 0;
}

static bool is(const Mesh * m, const char * b)
{
    return strcmp(m->blend, b) == 0;
}

static bool darkening(const Mesh * m)
{
    return is(m, "subtract") || is(m, "dstminussrc") || is(m, "dstsubsrcmulinvdst") || is(m, "multiply") || is(m, "mod");
}

static bool is_light(const Mesh * m)
{
    return is(m, "additive") || is(m, "brighten") || is(m, "glowpm");
}

static void save(const char * prefix, const char * what)
{
    static unsigned char px[W * H * 3], flip[W * H * 3];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < H; y++)
        memcpy(flip + y * W * 3, px + (H - 1 - y) * W * 3, W * 3);
    char path[600];
    snprintf(path, sizeof(path), "%s_%s.png", prefix, what);
    stbi_write_png(path, W, H, 3, flip, W * 3);
}

int main(int argc, char ** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: venue_render <textures dir> <zone> <out prefix> [back up look fov shift]\n");
        return 2;
    }
    tex_dir = argv[1];
    const char * zone = argv[2], * prefix = argv[3];
    float back = argc > 4 ? atof(argv[4]) : 9.75f, up = argc > 5 ? atof(argv[5]) : 1.8f;
    float look_h = argc > 6 ? atof(argv[6]) : 1.0f, fov = argc > 7 ? atof(argv[7]) : 36.0f;
    char path[600];
    snprintf(path, sizeof(path), "%s/venue_ghwt_%s.wtch", tex_dir, zone);
    if (!load_wtch(path)) {
        fprintf(stderr, "can't read %s\n", path);
        return 1;
    }
    load_json(zone);
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window * win = SDL_CreateWindow("venue", 0, 0, W, H, SDL_WINDOW_OPENGL);
    if (!win) {
        fprintf(stderr, "no window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    (void)ctx;
    load_textures();

    // the camera: back from the drummer toward the audience (facing), up, looking at the drummer's chest
    float * dm = mark[0];
    float fl = sqrtf(facing[0] * facing[0] + facing[2] * facing[2]);
    float fx = facing[0] / fl, fz = facing[2] / fl;
    eye[0] = dm[0] + fx * back;
    eye[1] = dm[1] + up;
    eye[2] = dm[2] + fz * back;
    float at[3] = {dm[0], dm[1] + look_h, dm[2]};
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    float fn = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; i++)
        f[i] /= fn;
    float s[3] = {f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0};     // f x up(0,1,0)
    float sn = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (int i = 0; i < 3; i++)
        s[i] /= sn;
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    float view[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
                      -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]), -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                      f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2], 1};
    float zn = 0.1f, zf = 600.0f, t = 1.0f / tanf(fov * (float)M_PI / 360.0f), aspect = (float)W / H;
    float proj[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1, 0, 0, 2 * zf * zn / (zn - zf), 0};
    // lens shift: the picture slides up without changing the perspective (the band stands clear of the highway)
    proj[9] = argc > 8 ? -atof(argv[8]) : 0;
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    Mesh ** order = malloc(mesh_count * sizeof(Mesh *));
    int n = 0;
    for (int k = 0; k < mesh_count; k++) {
        meshes[k].gain = 1;
        if (!meshes[k].hidden && !is(&meshes[k], "opaque") && !darkening(&meshes[k]))
            order[n++] = &meshes[k];
    }
    qsort(order, n, sizeof(Mesh *), by_distance);

    // GHWT tints its effect cards (haze, glare, light shafts, fire sheets) with material colours the export doesn't
    // carry, so at full strength the big ones whited out whole stages. Each card's share of the picture is measured
    // alone (behind the stage where the stage is in front) and capped: a bulb stays a bulb, a 30 m haze sheet a haze.
    enum { PW = W / 4, PH = H / 4 };
    static unsigned char probe[PW * PH * 3];
    glViewport(0, 0, PW, PH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.45f);
    for (int k = 0; k < mesh_count; k++)
        if (!meshes[k].hidden && is(&meshes[k], "opaque"))
            draw_mesh(&meshes[k], 1, 1, 1, 1, false);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int i = 0; i < n; i++) {
        Mesh * m = order[i];
        if (m->solid && !is_light(m))
            continue;
        glClear(GL_COLOR_BUFFER_BIT);
        draw_mesh(m, 1, 1, 1, is(m, "soft") ? 0.35f : 1, is_light(m));
        glReadPixels(0, 0, PW, PH, GL_RGB, GL_UNSIGNED_BYTE, probe);
        double sum = 0;
        for (int q = 0; q < PW * PH * 3; q++)
            sum += probe[q];
        double mean = sum / (PW * PH * 3), cap = m->glare ? 2 : 5;
        if (mean > cap)
            m->gain = cap / mean;
        if (getenv("VR_PROBE") && mean > 1)
            printf("probe part %d %s %s%s mean %.1f gain %.2f\n", (int)(m - meshes), m->tex, m->blend,
                   m->glare ? " glare" : "", mean, m->gain);
    }
    // and all the lights together, and all the haze together
    for (int pass = 0; pass < 2; pass++) {
        glClear(GL_COLOR_BUFFER_BIT);
        int count = 0;
        for (int i = 0; i < n; i++) {
            Mesh * m = order[i];
            if (is_light(m) != (pass == 0) || (m->solid && !is_light(m)))
                continue;
            draw_mesh(m, 1, 1, 1, (is(m, "soft") ? 0.35f : 1) * m->gain, is_light(m));
            count++;
        }
        if (!count)
            continue;
        glReadPixels(0, 0, PW, PH, GL_RGB, GL_UNSIGNED_BYTE, probe);
        double sum = 0;
        for (int q = 0; q < PW * PH * 3; q++)
            sum += probe[q];
        double mean = sum / (PW * PH * 3), cap = pass == 0 ? 14 : 14;
        if (getenv("VR_PROBE"))
            printf("probe all %s mean %.1f\n", pass == 0 ? "lights" : "haze", mean);
        if (mean > cap)
            for (int i = 0; i < n; i++)
                if (is_light(order[i]) == (pass == 0) && !(order[i]->solid && !is_light(order[i])))
                    order[i]->gain *= cap / mean;
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // the stage: opaque parts, alpha-tested, self-lit like GHWT's baked lighting
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.45f);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    for (int k = 0; k < mesh_count; k++)
        if (!meshes[k].hidden && is(&meshes[k], "opaque"))
            draw_mesh(&meshes[k], 0.92f, 0.9f, 0.87f, 1, false);
    glDisable(GL_ALPHA_TEST);
    // see-through parts, far to near
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (int i = 0; i < n; i++)
        if (!is_light(order[i]))
            draw_mesh(order[i], 1, 1, 1, (is(order[i], "soft") ? 0.35f : 1) * order[i]->gain, false);
    glFinish();
    save(prefix, "base");
    // the lights alone, where they show
    glClear(GL_COLOR_BUFFER_BIT);
    for (int i = 0; i < n; i++)
        if (is_light(order[i]))
            draw_mesh(order[i], 0.9f, 0.9f, 0.9f, order[i]->gain, true);
    glFinish();
    save(prefix, "glow");

    if (getenv("VR_PICK")) {
        glDepthMask(GL_TRUE);           // (before the clear: a masked depth buffer isn't cleared)
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_BLEND);
        for (int k = 0; k < mesh_count; k++) {
            if (meshes[k].hidden || darkening(&meshes[k]))
                continue;
            int id = k + 1;
            glDisable(GL_TEXTURE_2D);
            glBegin(GL_TRIANGLES);
            glColor3ub(id & 255, id >> 8, 0);
            for (int i = 0; i < meshes[k].tc * 3; i++)
                glVertex3fv(meshes[k].v[meshes[k].t[i]].p);
            glEnd();
        }
        const char * q = getenv("VR_PICK");
        int x, y, used;
        while (sscanf(q, "%d,%d%n", &x, &y, &used) == 2) {
            unsigned char c[3];
            glReadPixels(x, H - 1 - y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, c);
            int id = c[0] | c[1] << 8;
            printf("pick %d,%d: part %d %s %s\n", x, y, id - 1, id ? meshes[id - 1].tex : "-", id ? meshes[id - 1].blend : "");
            q += used;
            while (*q == ' ')
                q++;
        }
    }
    // where the marks land (the game puts its band there)
    snprintf(path, sizeof(path), "%s_marks.txt", prefix);
    FILE * mf = fopen(path, "w");
    for (int r = 0; r < 4 && mf; r++) {
        if (!has_mark[r])
            continue;
        float p[4] = {mark[r][0], mark[r][1], mark[r][2], 1}, c[4], q[4];
        for (int i = 0; i < 4; i++)
            c[i] = view[i] * p[0] + view[4 + i] * p[1] + view[8 + i] * p[2] + view[12 + i];
        for (int i = 0; i < 4; i++)
            q[i] = proj[i] * c[0] + proj[4 + i] * c[1] + proj[8 + i] * c[2] + proj[12 + i] * c[3];
        if (q[3] <= 0)
            continue;
        // pixels per metre at the mark (a 1.8 m figure is 1.8 x this tall)
        float ppm = t / -c[2] * H / 2;
        fprintf(mf, "%s %.1f %.1f %.2f\n", roles[r], (q[0] / q[3] * 0.5f + 0.5f) * W, (0.5f - q[1] / q[3] * 0.5f) * H, ppm);
    }
    if (mf)
        fclose(mf);
    int vis = 0, tris = 0;
    for (int k = 0; k < mesh_count; k++)
        if (!meshes[k].hidden) {
            vis++;
            tris += meshes[k].tc;
        }
    printf("%s: %d parts (%d shown, %d triangles), %d see-through\n", zone, mesh_count, vis, tris, n);
    SDL_Quit();
    return 0;
}
