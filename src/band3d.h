// The band as World Tour's own 3D models (tools/band.py --3d): each member's mesh, skinned on the CPU by the same posed
// frames the picture sheets have, so game.c's band logic picks frames the same way for either.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Band3D Band3D;

Band3D * band3d_load(const char * name);        // assets/band3d/<name>.b3d (+ alt/<name>.b3a); NULL: none
void band3d_free(Band3D * m);
int band3d_frames(const Band3D * m, bool alt);  // 0: no such frames

typedef struct {
    bool alt_a, alt_b;          // which frames: the main loop's or the wilder one's
    int fa, fb;                 // two frames, blended: fb by wb
    float wb;
    float x, feet, ppm;         // where the feet are on the screen, pixels per metre
    float yaw;                  // turned about their own up (radians), 0 facing the camera
    float squash;               // across the screen (1: as is)
    float lean, dip;            // the top of them moved by this many pixels (sideways, down)
    float top[3], bot[3];       // the light from head to toe, per channel
    uint32_t tint;
    float rim[3], rim_k;        // the backlight's colour and how strong
    float depth;                // 0 the furthest member .. 3 the nearest (each its own depth range)
    bool ink;                   // the cut-out look's ink line round them
    bool shadow;                // their shape lying on the floor in front of them, as the sheets' cut-outs had
} Band3DDraw;

void band3d_begin(bool clear);  // before a member (clear: the first, the depth buffer cleared)
void band3d_draw(Band3D * m, const Band3DDraw * d);
void band3d_end(void);
