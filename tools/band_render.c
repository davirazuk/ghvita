// Draws the frames tools/band.py posed (a GHWT band member, skinned and IK-solved) into PNGs with alpha: orthographic,
// x/y in metres from the feet, textured, alpha-tested like YargLook's cards, lit by one soft light from the front.
// usage: band_render <textures dir> <frames.bin> <out prefix> <cell w> <cell h> <pixels per metre> <bottom y (m)>
// Build: gcc -O2 -o build-tools/band_render tools/band_render.c -Isrc -lSDL2 -lGL -lm
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

typedef struct {
    char tex[33];
    int vc, tc;
    float * uv;
    uint32_t * idx;
    GLuint gl;
} Part;

static GLuint load_texture(const char * dir, const char * name)
{
    char path[700];
    int w, h, c;
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    unsigned char * px = stbi_load(path, &w, &h, &c, 4);
    // the export's names stop at 31 characters
    if (!px && strlen(name) == 31) {
        DIR * d = opendir(dir);
        struct dirent * e;
        while (d && (e = readdir(d)))
            if (strncmp(e->d_name, name, 31) == 0 && strstr(e->d_name, ".png")) {
                snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
                px = stbi_load(path, &w, &h, &c, 4);
                break;
            }
        if (d)
            closedir(d);
    }
    if (!px)
        return 0;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return t;
}

int main(int argc, char ** argv)
{
    if (argc < 8)
        return 2;
    int cw = atoi(argv[4]), ch = atoi(argv[5]);
    float ppm = atof(argv[6]);
    FILE * f = fopen(argv[2], "rb");
    if (!f)
        return 1;
    int np, nf;
    if (fread(&np, 4, 1, f) != 1 || fread(&nf, 4, 1, f) != 1)
        return 1;
    Part * parts = calloc(np, sizeof(Part));
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_Window * win = SDL_CreateWindow("band", 0, 0, cw, ch, SDL_WINDOW_OPENGL);
    if (!win || !SDL_GL_CreateContext(win))
        return 1;
    for (int k = 0; k < np; k++) {
        Part * p = &parts[k];
        if (fread(p->tex, 32, 1, f) != 1 || fread(&p->vc, 4, 1, f) != 1 || fread(&p->tc, 4, 1, f) != 1)
            return 1;
        p->uv = malloc(p->vc * 8);
        p->idx = malloc(p->tc * 12);
        if (fread(p->uv, 8, p->vc, f) != (size_t)p->vc || fread(p->idx, 12, p->tc, f) != (size_t)p->tc)
            return 1;
        if (p->tex[0])
            p->gl = load_texture(argv[1], p->tex);
    }
    glViewport(0, 0, cw, ch);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float mx = cw / ppm / 2, my0 = atof(argv[7]), my1 = my0 + ch / ppm;
    glOrtho(-mx, mx, my0, my1, -10, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.45f);
    glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 0);
    unsigned char * px = malloc(cw * ch * 4), * flip = malloc(cw * ch * 4);
    float * pos = NULL;
    int cap = 0;
    for (int fr = 0; fr < nf; fr++) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int k = 0; k < np; k++) {
            Part * p = &parts[k];
            if (p->vc * 6 > cap) {
                cap = p->vc * 6;
                pos = realloc(pos, cap * 4);
            }
            if (fread(pos, 24, p->vc, f) != (size_t)p->vc)
                return 1;
            if (p->gl) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, p->gl);
            } else
                glDisable(GL_TEXTURE_2D);
            glBegin(GL_TRIANGLES);
            for (int i = 0; i < p->tc * 3; i++) {
                int v = p->idx[i];
                float * q = pos + v * 6;
                // self-lit like the venue, shaped by a soft light from the audience (x,y,z = right, up, toward camera)
                float l = 0.62f + 0.38f * fabsf(q[3] * 0.25f + q[4] * 0.45f + q[5] * 0.85f);
                if (p->gl)
                    glColor4f(l, l, l, 1);
                else
                    glColor4f(0.17f * l, 0.17f * l, 0.18f * l, 1);
                glTexCoord2f(p->uv[v * 2], p->uv[v * 2 + 1]);
                glVertex3f(q[0], q[1], q[2]);
            }
            glEnd();
        }
        glFinish();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int y = 0; y < ch; y++)
            memcpy(flip + y * cw * 4, px + (ch - 1 - y) * cw * 4, cw * 4);
        char path[700];
        snprintf(path, sizeof(path), "%s_%02d.png", argv[3], fr);
        stbi_write_png(path, cw, ch, 4, flip, cw * 4);
    }
    SDL_Quit();
    return 0;
}
