// The band as World Tour's own 3D models (tools/band.py --3d): each member's mesh, skinned on the CPU by the same posed
// frames the picture sheets have, so game.c's band logic picks frames the same way for either.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"

typedef struct Band3D Band3D;
// the fret hand, from the chart: the frame's two groove steps (ga < 0: as the frames have it), where along the neck
// (0 by the nut .. the last place, by the body; between two: blended), and two chord shapes, cb by wc
typedef struct Band3DHands {
    int ga, gb;
    float fret;
    int ca, cb;
    float wc;
} Band3DHands;

Band3D * band3d_load(const char * name, int hue);      // assets/band3d/<name>.b3d (+ far/, alt/); hue: their clothes' colour turned (degrees); NULL: none
void band3d_free(Band3D * m);
int band3d_frames(const Band3D * m, bool alt);  // 0: no such frames
bool band3d_has_hands(const Band3D * m);        // the fret hand can be driven (a guitar, a bass)

typedef struct {
    bool alt_a, alt_b;          // which frames: the main loop's or the wilder one's
    int fa, fb;                 // two frames, blended: fb by wb
    float wb;
    int fa2, fb2;               // and the same pair in another state (a guitar's strum further through), by w2 (0: none)
    float w2;
    // where they stand: metres from what the camera looks at (x to the right of the stage as the audience sees it, y up,
    // z toward the audience), turned about their own up (0 facing the audience)
    float pos[3], yaw;
    // the camera: round the target by cam_yaw (to the audience's right), above it by cam_pitch (radians), cam_dist
    // metres away; the target lands on the screen at (cx, cy), a metre there ppm pixels
    float cam_yaw, cam_pitch, cam_dist, cx, cy, ppm;
    float top[3], bot[3];       // the light from head to toe, per channel
    float key[3];               // where the key light comes from, as the camera sees it (x right, y up, z to the camera)
    float fill;                 // how much of the light reaches the unlit side (0 hard .. 1 flat)
    uint32_t tint;
    float rim[3], rim_k;        // the backlight's colour and how strong
    bool far;                   // small on the screen: the far body will do
    Band3DHands hands;          // the guitar's fret hand, driven by the chart (ga < 0: as the frames have it)
    double stamp;               // band3d_draw's own: when this was asked for (seconds)
} Band3DDraw;

void band3d_anchor(const Band3D * m, const Band3DDraw * d, int which, float out[3]);    // 0 head, 1 chest, 2 pelvis
void band3d_begin(bool clear);
// a flat thing on the stage through a camera (the stage's axes, metres from the camera's target): the floor, a shadow
void band3d_quad(const Band3DDraw * cam, const float p[4][3], Tex * tex, const float uv[4][2], const uint32_t col[4]);  // before a member (clear: the depth buffer cleared)
void band3d_draw(Band3D * m, const Band3DDraw * d);
void band3d_end(void);

// World Tour's venue as a model (assets/venue3d/<zone>.v3d), in the stage's axes: drawn through the band's camera
typedef struct Venue3D Venue3D;
Venue3D * venue3d_load(const char * zone);      // NULL: none
void venue3d_free(Venue3D * v);
const float * venue3d_mark(const Venue3D * v, int role);        // where a member stands: 0 drummer, 1 singer, 2 guitarist, 3 bassist
void venue3d_draw(Venue3D * v, const Band3DDraw * cam, const float tint[3]);    // cam->pos: the stage's origin from the camera's target
bool venue3d_blocked(const Venue3D * v, const float from[3], const float to[3]);   // the venue's in the way between two places
