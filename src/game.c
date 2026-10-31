#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "game.h"
#include "vocals.h"
#include "res.h"
#include "audio.h"

#define WINDOW 0.095            // seconds early or late a note can still be hit (Warriors of Rock-ish lenient)
#define Z_FAR 11.0f             // the highway's far end (the strikeline is at 0)
#define Z_NEAR -1.3f
#define HALF_W 2.62f            // half the highway's width (lanes are 1 wide)
#define MAX_SUS 8
#define MAX_ZONES 64
#define SP_PER_QUARTER (1.0f / 32)

enum { PENDING, HIT, MISSED };

static const uint32_t lane_colour[7] = {
    RGBA(40, 230, 50, 255), RGBA(250, 40, 30, 255), RGBA(255, 228, 20, 255), RGBA(40, 110, 255, 255),
    RGBA(255, 140, 20, 255), RGBA(120, 220, 255, 255), RGBA(190, 80, 255, 255),
};

typedef struct {
    int note;
    double end;
    uint8_t frets;
    bool sp;
    double last_q;
} Sustain;

typedef struct {
    const Part * part;                  // none: they just play to the music
    int next;
    double held;
    double hit_t, last_hit;             // the next stroke, the last
    int hit_frets, last_frets, cell;
    float mic;                  // the singer's: 0 down, 1 at the mouth
    float pulse;
    bool was;
} BandPart;

typedef struct {
    double from, to;
    int lane;                   // -1: right across
} Zone;

static struct {
    Song * song;
    Chart * chart;
    const Part * part;
    int part_id, diff;
    uint8_t * state;
    uint8_t * got;              // lanes already pressed for a chord
    int16_t * phrase;           // the Star Power phrase a note belongs to (-1)
    bool * phrase_ok;
    double judged_t[6];                 // when a note on each lane was last hit or missed
    bool * roll;                        // drums: gems the hits before them carry
    Zone rolls[MAX_ZONES], gaps[MAX_ZONES];     // where the highway marks them
    int roll_count, gap_count;
    double pad_t[6];                    // when each pad was last hit
    int free_hits;                      // in this gap
    double free_t;
    int16_t * solo;             // the solo a note belongs to (-1)
    int * solo_hit;
    int first;                  // notes before this are all judged
    Sustain sus[MAX_SUS];
    double score;
    int streak, best_streak, hits, misses, overhits, sp_hit;
    double off_sum, off_sq;             // how early or late the hits came, seconds
    int off_n;
    float sp, rock;
    bool sp_active, sp_ready_told;
    double sp_last_q;           // where Star Power's drain has got to, in quarter notes
    double base_score;
    double t, lead_in;
    double speed;               // highway units per second
    bool paused, failed, done;
    int my_stem;
    // looks
    bool drums;                 // every gem struck on its own, 4 or 5 pads and the kick
    int freestyle;                      // hits in the gaps, this song
    int lanes;
    Note * drum_notes;
    Part drum_part;
    float hit_pulse, miss_pulse, cheer;  // what the band shows of how it's going
    float band_loud;            // the song's loudness, smoothed
    int drum_stems[MAX_STEMS], drum_stem_count;
    double flame_t[6];
    uint32_t held;              // lanes held (bit per lane, LANE_OPEN = bit 5)
    float miss_flash, sp_flash;
    bool vocal;                         // the Vocals part: no highway, vocals.c has the notes
    bool sing;                          // the mic is on: the Vocals part, or singing along to an instrument
    int vox_stem;
    int vox_run;                        // phrases in a row sung well
    char popup[64];
    double popup_t;
    float whammy, last_rx, last_ry;
    float bend;                         // how far the whammy bends the part's stem
    bool back_on;                       // a finger on the rear touchpad, where it landed and was
    float back_from[2], back_last[2];
    float tilt_base[3];
    bool tilt_armed, tilt_init;
    int crowd_voice, crowd_mood;
    Tex * highway;
    bool autoplay;
    float view[16], proj[16];
    float right[3], up[3];
    int touch_lane[MAX_TOUCH];
    int touch_ids[MAX_TOUCH], touch_count;
    double last_note_end;
    int solo_index;
    double fps_t;
    int fps_frames, fps;
    // the venue and its light show
    Tex * venue, * venue_lights;
    const char * venue_title;
    Tex * band[4];                      // drummer, vocalist, guitarist, bassist
    const float (*marks)[3];
    float needle;                       // what the rock needle shows
    BandPart band_part[4];              // each member's own part of the chart
    float stage_col[3], light_col[3];   // what the stage and its lights are tinted now (they glide to the show's)
    double show_t;
    int section, section_pal;           // the chart's section now, and its colours
    double pyro_t;                      // when the jets along the stage last went off
    float cam_x, cam_y, cam_z;          // the camera's drift
} G;

static struct {
    bool on;
    double from, to;
    float speed;
    int notes;                          // in the stretch
} practice;

// -------------------------------------------------------------------------------------------------------- helpers

static int frets_count(uint8_t frets)
{
    int n = 0;
    for (int i = 0; i < 6; i++)
        n += (frets >> i) & 1;
    return n;
}

// the colour art of a lane: drums go red, yellow, blue, (orange,) green
static int lane_art(int l)
{
    static const int drums4[5] = {1, 2, 3, 0, 0}, drums5[5] = {1, 2, 3, 4, 0};
    return !G.drums || l > 4 ? l : G.lanes == 4 ? drums4[l] : drums5[l];
}

static int display_lane(int lane)
{
    return cfg.lefty && lane < G.lanes ? G.lanes - 1 - lane : lane;
}

// the camera looks down +z, so world +x is on the screen's left: green (lane 0) gets the largest x
static float lane_x(int lane)
{
    return ((G.lanes - 1) / 2.0f - display_lane(lane)) * 5.0f / G.lanes;
}

static int multiplier(void)
{
    int max = G.part_id == PART_BASS ? 6 : 4;
    int m = 1 + G.streak / 10;
    if (m > max)
        m = max;
    return G.sp_active ? m * 2 : m;
}

// ------------------------------------------------------------------------------------------------------ the venue

// the song's band: one of each role's members, the same ones every time you play the song (YargLook's Member)
static void pick_band(const Song * song, uint32_t h)
{
    static const char * roles[4] = {"drummer", "vocalist", "guitarist", "bassist"};
    for (int r = 0; r < 4; r++) {
        const char * found[24];
        int n = 0;
        size_t len = strlen(roles[r]);
        for (int i = 0; i < res.band_count; i++)
            if (strncmp(res.band_names[i], roles[r], len) == 0)
                found[n++] = res.band_names[i];
        G.band[r] = NULL;
        if (!n)
            continue;
        uint32_t k = h;
        for (const char * c = roles[r]; *c; c++)
            k = (k ^ (uint8_t)*c) * 16777619u;
        G.band[r] = res_band_sheet(r, found[k % n]);
    }
    (void)song;
}

// the song's venue: each song always plays the same one (like YargLook's pick), or as the options say
static void pick_venue(const Song * song)
{
    G.venue = G.venue_lights = NULL;
    G.venue_title = NULL;
    G.marks = NULL;
    if (!res.venue_count || strcmp(cfg.venue, "classic") == 0)
        return;
    const char * zone = NULL;
    uint32_t h = 2166136261u;
    for (const char * c = song->artist; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    h = (h ^ '|') * 16777619u;
    for (const char * c = song->name; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    if (strcmp(cfg.venue, "song") == 0) {
        zone = res.venue_zones[h % res.venue_count];
    } else if (strcmp(cfg.venue, "random") == 0)
        zone = res.venue_zones[rand() % res.venue_count];
    else
        zone = cfg.venue;
    if (res_venue(zone, &G.venue, &G.venue_lights)) {
        G.venue_title = res_venue_title(zone);
        G.marks = res_venue_marks(zone);
        if (cfg.no_band)
            memset(G.band, 0, sizeof(G.band));
        else
            pick_band(song, h);
    }
    for (int i = 0; i < 3; i++)
        G.stage_col[i] = G.light_col[i] = 0.8f;
    G.show_t = -1e9;
}

static void draw_band(double beat);
static uint32_t with_alpha(uint32_t c, float a);

// where the song's structure puts the show: every chorus in the same warm colours, every verse in the same cool ones.
// A new chorus sets the jets off, when the crowd is with you. False: the chart names no sections
static bool update_section(void)
{
    const Chart * c = G.chart;
    bool moved = false;
    while (G.section + 1 < c->section_count && c->sections[G.section + 1].time / 1e6 <= G.t) {
        G.section++;
        moved = true;
    }
    if (G.section < 0 || !strncasecmp(c->sections[G.section].name, "ugc", 3))
        return false;
    if (moved) {
        char low[60];
        uint32_t h = 2166136261u;
        size_t k = 0;
        for (const char * n = c->sections[G.section].name; *n && k + 1 < sizeof(low); n++)
            if (isalpha((unsigned char)*n)) {
                low[k] = (char)tolower((unsigned char)*n);
                h = (h ^ (uint8_t)low[k++]) * 16777619u;
            }
        low[k] = 0;
        bool big = strstr(low, "chorus") || strstr(low, "solo") || strstr(low, "outro");
        static const int cool[] = {1, 3, 4}, warm[] = {0, 2, 5};
        G.section_pal = (big ? warm : cool)[h % 3];
        if (strstr(low, "chorus") && G.rock >= 0.5f)
            G.pyro_t = G.t;
    }
    return true;
}

// a layer of the stage as the camera sees it
static void stage_layer(Tex * t, uint32_t col)
{
    const Sprite s = sprite_whole(t);
    float w = SCREEN_W * G.cam_z, h = SCREEN_H * G.cam_z;
    gfx_sprite(&s, (SCREEN_W - w) / 2 + G.cam_x, (SCREEN_H - h) / 2 + G.cam_y, w, h, col);
}

// a point of the stage as the camera sees it
static void stage_point(float * x, float * y)
{
    *x = SCREEN_W / 2 + (*x - SCREEN_W / 2) * G.cam_z + G.cam_x;
    *y = SCREEN_H / 2 + (*y - SCREEN_H / 2) * G.cam_z + G.cam_y;
}

// the jets along the front of the stage, either side of whoever stands there
static void draw_pyro(void)
{
    const float length = 0.55f;
    double since = G.t - G.pyro_t;
    if (since < 0 || since >= length || !G.marks)
        return;
    const Sprite * flame = atlas_get(res.game, "flame");
    int frame = (int)(since / length * 16);
    float u0 = (frame % 4) * 0.25f, v0 = (frame / 4) * 0.25f;
    gfx_blend(BLEND_ADD);
    for (int r = 2; r <= 3; r++)
        for (int side = -1; side <= 1; side += 2) {
            float x = G.marks[r][0] + side * G.marks[r][2] * 0.75f + (r == 3 ? 34 : 0), y = G.marks[r][1], w = 64 * G.marks[r][2] / 110;
            stage_point(&x, &y);
            gfx_sprite_uv(flame, x - w / 2, y - w * 2.6f, w, w * 2.6f, u0, v0, u0 + 0.25f, v0 + 0.25f, WHITE);
        }
    gfx_blend(BLEND_ALPHA);
}

// GHWT's moods: a key colour and a wash colour (StageLights.Palettes in YargLook)
static const float palettes[6][2][3] = {
    {{1, 0.55f, 0.15f}, {0.9f, 0.1f, 0.05f}},   // amber over red
    {{0.35f, 0.5f, 1}, {0.6f, 0.1f, 0.9f}},     // blue over violet
    {{1, 0.85f, 0.6f}, {1, 0.35f, 0.05f}},      // white-hot over orange
    {{0.2f, 1, 0.45f}, {0.05f, 0.35f, 0.9f}},   // green over blue
    {{1, 0.2f, 0.6f}, {0.4f, 0.05f, 1}},        // magenta over purple
    {{1, 0.9f, 0.3f}, {0.9f, 0.4f, 0.05f}},     // gold over amber
};

static uint32_t col3(const float c[3], float a)
{
    int v[3];
    for (int i = 0; i < 3; i++) {
        float x = c[i] * 255;
        v[i] = x < 0 ? 0 : x > 255 ? 255 : (int)x;
    }
    return RGBA(v[0], v[1], v[2], (int)(a * 255));
}

// The venue picture (its lighting baked in, the stage rendered from the audience by tools/venues.py) and its lights
// alone on top, added: the lights pulse on every beat in the show's colours, which are the section's (every two bars
// where the chart names none) and as bright as the song is loud; a solo goes through them on every beat, a streak at
// the top multiplier heats them up, Star Power floods the stage blue-white and a band that's failing plays under a
// guttering red. The stage itself takes a little of the show's colour, and the camera never quite stands still.
static void draw_stage(void)
{
    if (!G.venue) {
        draw_background(res.bg_stage, WHITE);
        return;
    }
    double dt = G.t - G.show_t;
    G.show_t = G.t;
    if (dt < 0 || dt > 0.25)
        dt = 0.25;
    double beat = G.t > 0 ? chart_quarters(G.chart, G.t) : G.t * 2;
    float frac = (float)(beat - floor(beat));
    float pulse = expf(-frac * 5);
    float energy = G.rock < 0.2f ? 0 : G.rock > 0.8f ? 1 : (G.rock - 0.2f) / 0.6f;
    bool solo = G.solo_index >= 0 && G.solo_index < G.part->solo_count &&
                G.t >= G.part->solos[G.solo_index].start / 1e6 && G.t < G.part->solos[G.solo_index].end / 1e6;
    bool hot = solo || multiplier() >= (G.part_id == PART_BASS ? 6 : 4);
    static const int cool[] = {1, 3, 4}, warm[] = {0, 2, 5};
    bool named = update_section();
    int step = solo ? (int)floor(beat) : (int)floor(beat / 8);
    int pal = solo ? warm[((step % 3) + 3) % 3] : named ? G.section_pal : (hot ? warm : cool)[((step % 3) + 3) % 3];
    float inten = hot ? 1.25f : 0.85f;
    float key[3], wash[3], stage[3], lights[3];
    memcpy(key, palettes[pal][0], sizeof(key));
    memcpy(wash, palettes[pal][1], sizeof(wash));
    float gl = (0.3f + 0.55f * G.band_loud) + 0.4f * pulse * (0.4f + 0.6f * inten);
    float lvl = (hot ? 0.64f : 0.5f) + 0.07f * pulse * (0.3f + energy);
    float t = (float)G.t;
    if (G.sp_active) {
        float sb = 0.35f + 0.65f * (1 - frac) * (1 - frac) * (1 - frac);
        const float spk[3] = {0.6f + 0.4f * sb, 0.85f + 0.15f * sb, 1};
        memcpy(key, spk, sizeof(key));
        const float spw[3] = {0.62f, 0.8f, 1.25f};
        memcpy(wash, spw, sizeof(wash));
        gl = 0.45f + 0.35f * sb;
        lvl = 0.52f + 0.08f * pulse;     // bright skies burn out above this
    } else if (G.rock < 0.33f) {
        float flick = 0.6f + 0.4f * (0.5f + 0.25f * sinf(t * 6.1f) + 0.25f * sinf(t * 13.7f + 1.3f));
        const float fk[3] = {1, 0.25f, 0.15f}, fw[3] = {1, 0.5f, 0.42f};
        memcpy(key, fk, sizeof(key));
        memcpy(wash, fw, sizeof(wash));
        gl = 0.35f * flick;
        lvl = 0.42f * (0.85f + 0.15f * flick);
    }
    if (G.paused) {
        gl *= 0.5f;
        lvl *= 0.7f;
    }
    // the stage: tinted a little toward the wash; the lights: toward the key
    float wmax = fmaxf(0.01f, fmaxf(wash[0], fmaxf(wash[1], wash[2])));
    for (int i = 0; i < 3; i++) {
        stage[i] = (0.6f + 0.4f * wash[i] / wmax) * lvl / 0.6f;
        lights[i] = (0.5f + 0.5f * key[i]) * (gl > 1 ? 1 : gl);
    }
    float k = 1 - expf((float)-dt * 9);
    for (int i = 0; i < 3; i++) {
        G.stage_col[i] += (stage[i] - G.stage_col[i]) * k;
        G.light_col[i] += (lights[i] - G.light_col[i]) * k;
    }
    // a slow drift over the stage, in a little on the big parts
    float zoom = 1.04f + 0.012f * G.band_loud;
    G.cam_z += (zoom - G.cam_z) * k * 0.1f;
    G.cam_x = sinf((float)G.t * 0.23f) * 13;
    G.cam_y = sinf((float)G.t * 0.17f + 1) * 6;
    stage_layer(G.venue, col3(G.stage_col, 1));
    // a whole screen of blending: not when it would add nothing
    if (G.venue_lights && fmaxf(G.light_col[0], fmaxf(G.light_col[1], G.light_col[2])) > 0.02f) {
        gfx_blend(BLEND_ADD);
        stage_layer(G.venue_lights, col3(G.light_col, 1));
        gfx_blend(BLEND_ALPHA);
    }
    draw_band(beat);
    draw_pyro();
}

// The band on its marks: 48 frames over 4 beats (tools/band.py), locked to the song's beat; a tempo outside the 70-150
// the loops suit plays at half or double, as GHWT folds it. Lit by what lights the stage.
// The band follows the song a loop at a time: half time while the part rests or the song is going badly, full out when
// the notes come. Only at a loop's end, so a change never jumps and the loop stays on the bars.
// a note the member's arm plays: the drummer's pads (a kick is the foot), a guitarist's strums (hammer-ons need none)
static bool band_strikes(int r, const Note * n, double last_hit)
{
    if (r == 0)
        return n->frets & 0x1f;
    return !(n->flags & (NOTE_HOPO | NOTE_TAP)) || n->time / 1e6 - last_hit > 0.3;
}

// the hand shape of a note in a guitarist's sheet: green to orange, then three chords (tools/band.py SHAPES)
static int band_shape(int frets)
{
    frets &= 0x1f;
    if (frets & (frets - 1))
        return 5 + (frets * 40503u >> 4) % 3;
    for (int i = 0; i < 5; i++)
        if (frets >> i & 1)
            return i;
    return 0;
}

// the strike of a note in the drummer's sheet: snare, hi-hat, tom, crash, floor tom, or hi-hat and snare together
static int band_strike(int frets)
{
    if ((frets & 3) == 3)
        return 5;
    for (int i = 0; i < 5; i++)
        if (frets >> i & 1)
            return i;
    return 0;
}

// what the chart's pads are on the kit, as band_strike's bits: yellow's the hi-hat, blue a ride and green a crash,
// unless the chart says they're toms
static int kit_pads(const Note * n)
{
    int f = n->frets & 1;
    if (n->frets & 2)
        f |= n->flags & NOTE_TOM_Y ? 4 : 2;
    if (n->frets & 4)
        f |= n->flags & NOTE_TOM_B ? 4 : 8;
    if (n->frets & 8)
        f |= n->flags & NOTE_TOM_G ? 16 : 8;
    return f | (n->frets & 16);
}

// the cell a member's on: the arm comes down to land on the next stroke, rebounds, and holds until the one after
static int band_cell(int r, const BandPart * b, double beat)
{
    bool drums = r == 0;
    int cols = drums ? 8 : 6, land = drums ? 3 : 2;
    double hit = beat - floor(beat);
    if (!b->part) {
        // no chart for them: on the beat
        int row = drums ? (beat > 0 && ((int)beat & 1) ? 5 : 1) : (int)fabs(beat) % 4;
        return row * cols + land + (int)((cols - 1 - land) * fmin(hit * 2.5, 1));
    }
    double lead = fmin(b->hit_t - b->last_hit, drums ? 0.12 : 0.09);
    if (G.t > b->hit_t - lead) {
        int row = drums ? band_strike(b->hit_frets) : band_shape(b->hit_frets);
        return row * cols + (int)(land * (1 - (b->hit_t - G.t) / lead));
    }
    int row = drums ? band_strike(b->last_frets) : band_shape(b->last_frets);
    double since = fmin((G.t - b->last_hit) / (drums ? 0.16 : 0.15), 1);
    return row * cols + land + (int)((cols - 1 - land) * since + 0.5);
}

// what a member of the band is playing right now, from their own part of the chart. Plays on through a short rest
static bool band_playing(int r, double beat_len)
{
    if (r == 1)
        return vox_singing(G.t);
    BandPart * b = &G.band_part[r];
    const Part * p = b->part;
    if (!p)
        return G.t > 0;
    bool passed = b->hit_t == 0;
    while (b->next < p->note_count && p->notes[b->next].time / 1e6 <= G.t) {
        const Note * n = &p->notes[b->next++];
        double end = (n->time + (double)n->len) / 1e6;
        if (end > b->held)
            b->held = end;
        b->pulse = 1;
        if (n->time / 1e6 >= b->hit_t && n->time / 1e6 > b->last_hit && band_strikes(r, n, b->last_hit)) {
            b->last_hit = n->time / 1e6;
            b->last_frets = r ? n->frets : kit_pads(n);
            passed = true;
        }
    }
    // the next stroke, known ahead: the arm comes down to land on it
    if (passed) {
        b->hit_t = 1e9;
        for (int i = b->next; i < p->note_count; i++)
            if (p->notes[i].time / 1e6 > b->last_hit && band_strikes(r, &p->notes[i], b->last_hit)) {
                b->hit_t = p->notes[i].time / 1e6;
                b->hit_frets = r ? p->notes[i].frets : kit_pads(&p->notes[i]);
                break;
            }
    }
    double next = b->next < p->note_count ? p->notes[b->next].time / 1e6 : 1e9;
    return G.t > 0 && (G.t < b->held + beat_len || next - G.t < beat_len * 0.5);
}

// A member's cell as a strip that bends: the feet stay on their mark (and the kit under the drummer), and from there up
// the body leans by `lean` and dips by `dip` (pixels, at the top of the cell), more the higher up: a spine and a pair of
// knees, for four more quads than a flat picture.
static void band_strip(Tex * t, float x, float bottom, float w, float h, const float uv[4], float lean, float dip,
                       uint32_t tint)
{
    enum { CUTS = 3 };
    const uint32_t c[4] = {tint, tint, tint, tint};
    gfx_tex(t);
    for (int i = 0; i < CUTS; i++) {
        float f0 = (float)i / CUTS, f1 = (float)(i + 1) / CUTS;
        float x0 = x + lean * f0 * f0, x1 = x + lean * f1 * f1;
        float y0 = bottom - h * f0 + dip * f0 * f0, y1 = bottom - h * f1 + dip * f1 * f1;
        float v0 = uv[3] + (uv[1] - uv[3]) * f0, v1 = uv[3] + (uv[1] - uv[3]) * f1;
        const float p[4][3] = {{x1 - w / 2, y1, 0}, {x1 + w / 2, y1, 0}, {x0 + w / 2, y0, 0}, {x0 - w / 2, y0, 0}};
        const float tc[4][2] = {{uv[0], v1}, {uv[2], v1}, {uv[2], v0}, {uv[0], v0}};
        gfx_quad(p, tc, c);
    }
}

static void band_frame(Tex * t, int f, float cw, float ch, float x, float bottom, float w, float h, float lean,
                       float dip, uint32_t tint)
{
    float u0 = (f % 8) * cw / t->w, v0 = (f / 8) * ch / t->h;
    const float uv[4] = {u0, v0, u0 + cw / t->w, v0 + ch / t->h};
    band_strip(t, x, bottom, w, h, uv, lean, dip, tint);
}

static void draw_band(double beat)
{
    if (!G.marks)
        return;
    static const int order[4] = {0, 1, 3, 2};
    double bpm = G.t > 0 ? (chart_quarters(G.chart, G.t + 0.5) - chart_quarters(G.chart, G.t)) * 120 : 120;
    double speed = 1;
    while (bpm > 1 && bpm * speed > 150)
        speed *= 0.5;
    while (bpm > 1 && bpm * speed < 70)
        speed *= 2;
    // the loop is locked to the song's beat
    double phase = beat > 0 ? fmod(beat * speed, 4) / 4 : 0, beat_len = 60 / (bpm > 1 ? bpm : 120);
    // they move to the song: how loud it is now, on the beat, more as the crowd comes round; Star Power and streaks
    // make everybody jump, and whoever plays your part answers each hit and flinches at each miss
    float pi = 3.14159265f, on_beat = fabsf(sinf((float)(beat * speed) * pi));
    float loud = audio_level() * 1.6f;
    loud = loud > 1 ? 1 : loud;
    G.band_loud += (loud - G.band_loud) * 0.12f;
    float mood = G.rock < 0.33f ? 0.3f : 0.6f + G.rock * 0.9f;
    float bob = (1 - on_beat) * (1 - on_beat) * (0.04f + 0.14f * G.band_loud) * mood;   // down on the beat
    float sway = sinf((float)(beat * speed) * pi * 0.5f) * (0.05f + 0.13f * G.band_loud) * mood;
    float slump = G.rock < 0.33f ? (0.33f - G.rock) * 0.5f : 0;   // losing the crowd, they sag
    // and the last chord rings with everybody in the air
    float hop = G.sp_active || G.cheer > 0.25f || (G.t > G.last_note_end && G.band_loud > 0.03f) ? on_beat * 0.3f : 0;
    int me = G.part_id == PART_DRUMS ? 0 : G.part_id == PART_VOCALS ? 1 : G.part_id == PART_BASS ? 3 : 2;
    float lit[3];
    for (int i = 0; i < 3; i++)
        lit[i] = 0.2f + G.stage_col[i] * 1.05f;
    uint32_t col = col3(lit, 1);
    for (int i = 0; i < 4; i++) {
        int r = order[i];
        Tex * t = G.band[r];
        if (!t)
            continue;
        float m[3] = {G.marks[r][0], G.marks[r][1], G.marks[r][2]};
        // the singer's mark is behind the highway: beside the kit they can be seen
        if (r == 3)
            m[0] += 34;         // clear of the rock meter
        if (r == 1) {
            m[0] = G.marks[0][0] - G.marks[0][2] * 1.35f;
            m[1] = G.marks[0][1] + 24;
            m[2] = G.marks[0][2] * 1.1f;
        }
        // cell size, pixels per metre and the cell's bottom edge in metres, as in tools/band.py
        float cw = r == 0 ? 266 : 170, ch = r == 0 ? 232 : 256, ppm = r == 0 ? 88 : 120, y0 = r == 0 ? -0.35f : -0.08f;
        // each plays when their part does (no music, no playing), and between parts holds still
        BandPart * bp = &G.band_part[r];
        bool playing = band_playing(r, beat_len) && G.band_loud > 0.03f;
#ifndef __vita__
        if (playing != bp->was)
            plat_log("band: member %d %s at %.2f", r, playing ? "plays" : "stops", G.t);
        bp->was = playing;
#endif
        if (playing && r != 1)
            bp->cell = band_cell(r, bp, beat);
        int f = bp->cell;
        stage_point(&m[0], &m[1]);
        m[2] *= G.cam_z;
        float s = m[2] / ppm, w = cw * s, h = ch * s;
        // the kit stays where it stands; what's on their feet moves from the knees up, and a jump leaves the stage
        float bottom = m[1] - y0 * m[2] - (r ? hop : 0) * m[2];
        float dip = (r ? bob * (playing ? 1 : 0.5f) + bp->pulse * 0.05f : bob * 0.3f + bp->pulse * 0.035f) * m[2] +
                    slump * m[2];
        float lean = (r ? (i & 1 ? sway : -sway) * (playing ? 1 : 0.4f) : 0) * m[2];
        uint32_t tint = col;
        if (r == me) {
            // the kit's in the drummer's picture, and it stays where it stands
            dip += G.hit_pulse * (r ? 0.08f : 0.015f) * m[2];
            lean += sinf((float)G.t * 70) * G.miss_pulse * (r ? 0.12f : 0.02f) * m[2];
            if (G.miss_pulse > 0.15f)
                tint = RGBA(255, 110, 90, 255);
        }
        bp->pulse *= 0.82f;
        if (r == 1) {
            // the singer keeps moving: 32 frames with the mic at the mouth for a line, 16 with it down for the rests,
            // one over the other for the moment it comes up
            float want = playing ? 1 : 0, step = want - bp->mic;
            bp->mic += step > 0.17f ? 0.17f : step < -0.17f ? -0.17f : step;
            int sing = (int)(phase * 32) % 32, down = 32 + (int)(phase * 16) % 16;
            if (bp->mic < 1)
                band_frame(t, down, cw, ch, m[0], bottom, w, h, lean, dip,
                           with_alpha(tint, bp->mic < 0.5f ? 1 : 2 * (1 - bp->mic)));
            if (bp->mic > 0)
                band_frame(t, sing, cw, ch, m[0], bottom, w, h, lean, dip,
                           with_alpha(tint, bp->mic > 0.5f ? 1 : 2 * bp->mic));
        } else
            band_frame(t, f, cw, ch, m[0], bottom, w, h, lean, dip, tint);
    }
}

static void popup(const char * text)
{
    snprintf(G.popup, sizeof(G.popup), "%s", text);
    G.popup_t = G.t;
}

static float fade(float z)
{
    if (z < Z_FAR * 0.6f)
        return 1;
    float a = 1 - (z - Z_FAR * 0.6f) / (Z_FAR * 0.4f);
    return a < 0 ? 0 : a * a * (3 - 2 * a);
}

static uint32_t with_alpha(uint32_t c, float a)
{
    float base = (c >> 24) / 255.0f;
    int v = (int)(base * a * 255 + 0.5f);
    return ALPHA(c, v < 0 ? 0 : v > 255 ? 255 : v);
}

// a quad lying on the highway, x0..x1 across, z0..z1 along (colours near and far)
static void flat(float x0, float x1, float z0, float z1, float y, const Sprite * s, float v0, float v1,
                 uint32_t near_c, uint32_t far_c)
{
    gfx_tex(s ? s->tex : NULL);
    float su0 = s ? s->u0 : 0, su1 = s ? s->u1 : 1;
    float sv0 = s ? s->v0 + (s->v1 - s->v0) * v0 : v0, sv1 = s ? s->v0 + (s->v1 - s->v0) * v1 : v1;
    const float p[4][3] = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
    const float uv[4][2] = {{su0, sv0}, {su1, sv0}, {su1, sv1}, {su0, sv1}};
    const uint32_t c[4] = {near_c, near_c, far_c, far_c};
    gfx_quad(p, uv, c);
}

// a strip the length of the highway, fading out at the far end like the board
static void strip(float x0, float x1, float y, const Sprite * s, uint32_t c)
{
    flat(x0, x1, Z_NEAR, Z_FAR * 0.6f, y, s, 0, 1, c, c);
    for (int i = 0; i < 4; i++) {
        float z0 = Z_FAR * (0.6f + 0.1f * i), z1 = z0 + Z_FAR * 0.1f;
        flat(x0, x1, z0, z1, y, s, 0, 1, with_alpha(c, fade(z0)), with_alpha(c, fade(z1)));
    }
}

// a quad facing the camera standing on (x, y, z), w wide and h tall; uv inside the sprite
static void board(float x, float y, float z, float w, float h, const Sprite * s, float u0, float v0, float u1,
                  float v1, uint32_t c)
{
    gfx_tex(s ? s->tex : NULL);
    float su0 = s->u0 + (s->u1 - s->u0) * u0, su1 = s->u0 + (s->u1 - s->u0) * u1;
    float sv0 = s->v0 + (s->v1 - s->v0) * v0, sv1 = s->v0 + (s->v1 - s->v0) * v1;
    float rx = G.right[0] * w / 2, ry = G.right[1] * w / 2, rz = G.right[2] * w / 2;
    float ux = G.up[0] * h, uy = G.up[1] * h, uz = G.up[2] * h;
    const float p[4][3] = {
        {x - rx + ux, y - ry + uy, z - rz + uz}, {x + rx + ux, y + ry + uy, z + rz + uz},
        {x + rx, y + ry, z + rz}, {x - rx, y - ry, z - rz},
    };
    const float uv[4][2] = {{su0, sv0}, {su1, sv0}, {su1, sv1}, {su0, sv1}};
    const uint32_t cc[4] = {c, c, c, c};
    gfx_quad(p, uv, cc);
}

// where a point of the highway lands on the screen
static void project(float x, float y, float z, float * sx, float * sy)
{
    const float * v = G.view, * p = G.proj;
    float ex = v[0] * x + v[4] * y + v[8] * z + v[12];
    float ey = v[1] * x + v[5] * y + v[9] * z + v[13];
    float ez = v[2] * x + v[6] * y + v[10] * z + v[14];
    float cx = p[0] * ex + p[8] * ez, cy = p[5] * ey + p[9] * ez, cw = -ez;
    *sx = (cx / cw * 0.5f + 0.5f) * SCREEN_W;
    *sy = (0.5f - cy / cw * 0.5f) * SCREEN_H;
}

static void setup_camera(void)
{
    const float eye[3] = {0, 4.0f, -5.2f}, at[3] = {0, 0, 4.8f}, up[3] = {0, 1, 0};
    mat_perspective(G.proj, 40, (float)SCREEN_W / SCREEN_H, 0.5f, 60);
    mat_lookat(G.view, eye, at, up);
    // the highway sits a little low on the screen, as in the games
    G.right[0] = G.view[0];
    G.right[1] = G.view[4];
    G.right[2] = G.view[8];
    G.up[0] = G.view[1];
    G.up[1] = G.view[5];
    G.up[2] = G.view[9];
}

// ------------------------------------------------------------------------------------------------------- the song

// a drum chart's chords are hits at the same moment: each gem is its own note, so a missed kick costs the kick only
#define GAP_MIN 2.0                     // seconds between gems for freestyle
#define GAP_AFTER 0.3
#define PRESS_GRACE 0.13                // a press this soon after its lane's note was judged belongs to that note
#define NEXT_LATE 0.05                  // a drum hit this late on a gem may be meant for the next on that pad
#define ROLL_GAP 0.11                   // gems this close on a pad are a roll: no thumb keeps up with them
#define ROLL_RUN 4                      // that many in a row
#define ROLL_HOLD 0.18                  // a roll goes on while its pad was hit this recently

// songs converted before the kick had the open bit keep it in bit 7, and their charts are gone
static uint8_t drum_frets(const Note * n)
{
    return n->frets & 0x80 ? (n->frets & 0x1f) | FRET_OPEN : n->frets;
}

static void split_drums(void)
{
    const Part * p = G.part;
    int total = 0, used = 0;
    for (int i = 0; i < p->note_count; i++)
        total += frets_count(drum_frets(&p->notes[i]));
    G.drum_notes = calloc(total + 1, sizeof(Note));
    if (!G.drum_notes)
        return;
    for (int i = 0; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        uint8_t frets = drum_frets(n);
        int left = frets_count(frets);
        for (int b = 0; b < 6; b++) {
            if (!(frets & (1 << b)))
                continue;
            Note * o = &G.drum_notes[used++];
            *o = *n;
            o->frets = (uint8_t)(1 << b);
            // the phrase ends once, on its last gem
            if (--left)
                o->flags &= ~NOTE_SP_END;
        }
    }
    G.drums = true;
    G.lanes = 4;
    for (int i = 0; i < used; i++)
        if (G.drum_notes[i].frets & 0x10)
            G.lanes = 5;
    G.drum_part = *p;
    G.drum_part.notes = G.drum_notes;
    G.drum_part.note_count = used;
    G.part = &G.drum_part;
    G.freestyle = 0;
    G.gap_count = 0;
    for (int i = 0; i < used && G.gap_count < MAX_ZONES; i++) {
        double a = i ? G.drum_notes[i - 1].time / 1e6 : 0, b = G.drum_notes[i].time / 1e6;
        if (b - a >= GAP_MIN) {
            G.gaps[G.gap_count++] = (Zone){i ? a + GAP_AFTER : 0, b - WINDOW, -1};
            if (G.gap_count <= 4)
                plat_log("drums: gap %.2f to %.2f", a, b);
        }
    }
    plat_log("drums: %d gaps for freestyle", G.gap_count);
    // rolls: as YARG's lanes, the first gem is played and the rest come while the pad keeps going. Worked out from the
    // gems, not the chart's markers: a downloaded song's chart is gone once it's converted
    G.roll = calloc(used + 1, sizeof(bool));
    G.roll_count = 0;
    for (int b = 0; G.roll && b < 6; b++) {
        int first = -1, prev = -1, run = 0;
        for (int i = 0; i <= used; i++) {
            if (i < used && !(G.drum_notes[i].frets & (1 << b)))
                continue;
            if (i < used && prev >= 0 && (G.drum_notes[i].time - G.drum_notes[prev].time) / 1e6 <= ROLL_GAP)
                run++;
            else {
                if (run >= ROLL_RUN) {
                    for (int k = first + 1; k <= prev; k++)
                        G.roll[k] = G.drum_notes[k].frets & (1 << b);
                    if (G.roll_count < MAX_ZONES)
                        G.rolls[G.roll_count++] = (Zone){G.drum_notes[first].time / 1e6,
                                                         G.drum_notes[prev].time / 1e6, b};
                }
                first = i;
                run = 1;
            }
            prev = i;
        }
    }
    plat_log("drums: %d rolls", G.roll_count);
}

static void free_song(void)
{
    free(G.roll);
    G.roll = NULL;
    audio_song_unload();
    vox_free();
    G.vocal = G.sing = false;
    free(G.drum_notes);
    G.drum_notes = NULL;
    G.drums = false;
    G.lanes = LANES;
    if (G.crowd_voice >= 0)
        sound_stop(G.crowd_voice);
    G.crowd_voice = -1;
    chart_free(G.chart);
    free(G.state);
    free(G.got);
    free(G.phrase);
    free(G.phrase_ok);
    free(G.solo);
    free(G.solo_hit);
    G.chart = NULL;
    G.state = G.got = NULL;
    G.phrase = NULL;
    G.phrase_ok = NULL;
    G.solo = NULL;
    G.solo_hit = NULL;
}

static void set_my_stem(float volume);

static void reset_play(void)
{
    const Part * p = G.part;
    memset(G.state, PENDING, p->note_count + 1);
    memset(G.got, 0, p->note_count + 1);
    for (int i = 0; i < p->sp_count; i++)
        G.phrase_ok[i] = true;
    for (int i = 0; i < p->solo_count; i++)
        G.solo_hit[i] = 0;
    memset(G.sus, 0, sizeof(G.sus));
    for (int i = 0; i < MAX_SUS; i++)
        G.sus[i].note = -1;
    G.first = 0;
    G.score = 0;
    G.streak = G.best_streak = G.hits = G.misses = G.overhits = G.sp_hit = 0;
    G.off_sum = G.off_sq = 0;
    G.off_n = 0;
    G.sp = 0;
    G.rock = G.needle = 0.5f;
    G.sp_active = G.sp_ready_told = false;
    G.sp_last_q = -1;
    G.failed = G.done = G.paused = false;
    G.popup[0] = 0;
    G.miss_flash = G.sp_flash = 0;
    G.hit_pulse = G.miss_pulse = G.cheer = 0;
    G.solo_index = -1;
    G.section = -1;
    for (int l = 0; l < 6; l++)
        G.pad_t[l] = G.judged_t[l] = -100;
    G.free_hits = 0;
    G.free_t = -100;
    G.pyro_t = -100;
    G.cam_z = 1.04f;
    for (int i = 0; i < 6; i++)
        G.flame_t[i] = -100;
    G.held = 0;
    G.touch_count = 0;
    double first = p->note_count ? p->notes[0].time / 1e6 : 0;
    G.lead_in = Z_FAR / G.speed + 0.6 - first;
    if (G.lead_in < 1.2)
        G.lead_in = 1.2;
    G.t = -G.lead_in;
    double from = 0;
    if (practice.on) {
        // only the stretch's notes, after a run-up of the highway's length
        practice.notes = 0;
        for (int i = 0; i < p->note_count; i++) {
            double nt = p->notes[i].time / 1e6;
            if (nt < practice.from || nt >= practice.to)
                G.state[i] = HIT;
            else
                practice.notes++;
            if (nt < practice.from)
                G.first = i + 1;
        }
        G.t = practice.from - Z_FAR / G.speed - 0.6;
        from = G.t > 0 ? G.t : 0;
        G.lead_in = from - G.t;
    }
    static const int band_parts[4] = {PART_DRUMS, -1, PART_GUITAR, PART_BASS};
    for (int r = 0; r < 4; r++) {
        memset(&G.band_part[r], 0, sizeof(G.band_part[r]));
        for (int d = 3; r != 1 && d >= 0 && !G.band_part[r].part; d--)
            G.band_part[r].part = chart_part(G.chart, band_parts[r], d);
    }
    set_my_stem(1);
    audio_song_fade(1, 0);
    audio_song_speed(practice.on ? practice.speed : 1);
    audio_song_play(from, G.lead_in);
    if (G.sing) {
        // singing the part: the recorded singer stays as a guide. Over an instrument, they're heard as they are
        if (G.vox_stem >= 0 && G.vocal)
            audio_stem_volume(G.vox_stem, 0.5f);
        G.vox_run = 0;
        vox_start(G.diff, G.autoplay);
        if (practice.on)
            vox_seek(G.t);
    }
    if (G.crowd_voice >= 0)
        sound_stop(G.crowd_voice);
    G.crowd_mood = SND_CROWD_NEUTRAL;
    G.crowd_voice = sound_play(res.snd[G.crowd_mood], cfg.crowd_vol * 0.03f, true);
}

bool game_start(Song * song, int part, int diff)
{
    free_song();
    char path[400];
    res_wait_sounds();
    snprintf(path, sizeof(path), "%s/notes.vhc", song->folder);
    G.chart = chart_load(path);
    if (!G.chart)
        return false;
    static Part vox_part;
    G.vocal = part == PART_VOCALS && vox_load(song->folder);
    G.sing = G.vocal || (cfg.sing_along && vox_load(song->folder));
    if (!G.sing)
        vox_load(song->folder);         // when the singer sings
    if (G.vocal) {
        vox_part.part = part;
        vox_part.diff = diff;
        G.part = &vox_part;
    } else
        G.part = chart_part(G.chart, part, diff);
    if (!G.part || (G.part->note_count == 0 && !G.vocal)) {
        chart_free(G.chart);
        G.chart = NULL;
        return false;
    }
    G.lanes = LANES;
    if (part == PART_DRUMS)
        split_drums();
    G.song = song;
    G.part_id = part;
    G.diff = diff;
    const Part * p = G.part;
    G.state = malloc(p->note_count + 1);
    G.got = malloc(p->note_count + 1);
    G.phrase = malloc((p->note_count + 1) * sizeof(int16_t));
    G.phrase_ok = calloc(p->sp_count + 1, sizeof(bool));
    G.solo = malloc((p->note_count + 1) * sizeof(int16_t));
    G.solo_hit = calloc(p->solo_count + 1, sizeof(int));
    // the phrases and solos each note is in, the score with no multiplier (for the stars), the song's end
    G.base_score = 0;
    G.last_note_end = 0;
    for (int i = 0; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        G.phrase[i] = -1;
        G.solo[i] = -1;
        for (int k = 0; k < p->sp_count; k++)
            if (n->time >= p->sp[k].start && n->time < p->sp[k].end)
                G.phrase[i] = k;
        for (int k = 0; k < p->solo_count; k++)
            if (n->time >= p->solos[k].start && n->time < p->solos[k].end)
                G.solo[i] = k;
        // what a perfect run scores without Star Power, the multiplier building up as it goes (YARG's base score)
        int mult = 1 + i / 10, max = part == PART_BASS ? 6 : 4;
        if (mult > max)
            mult = max;
        G.base_score += (50 * frets_count(n->frets) + 25 * n->sustain_beats / 100.0) * mult;
        double end = (n->time + (double)n->len) / 1e6;
        if (end > G.last_note_end)
            G.last_note_end = end;
    }
    if (G.sing) {
        G.base_score += vox_base_score();
        G.last_note_end = fmax(G.last_note_end, vox_end());
    }
    // the backing and every part's stem play; a miss mutes this part's
    const char * stems[MAX_STEMS];
    for (int i = 0; i < song->stem_count; i++)
        stems[i] = song->stems[i];
    if (!audio_song_load(song->folder, stems, song->stem_count)) {
        free_song();
        return false;
    }
    G.my_stem = song->part_stem[part];
    // drums often come as a few stems: a miss quiets all of them
    G.drum_stem_count = 0;
    for (int i = 0; G.drums && i < song->stem_count; i++)
        if (!strncmp(song->stems[i], "drums", 5))
            G.drum_stems[G.drum_stem_count++] = i;
    G.vox_stem = song->part_stem[PART_VOCALS];
    G.speed = 4.0 + cfg.speed * 0.8;
    G.highway = res_highway(cfg.highway);
    pick_venue(song);
    G.crowd_voice = -1;
    setup_camera();
    plat_set_perf(1);
    reset_play();
    plat_log("game: %s - %s, %s %s, %d notes, %d phrases, lead-in %.2f s", song->artist, song->name,
             part_name(part), diff_name(diff), p->note_count, p->sp_count, G.lead_in);
    return true;
}

void game_practice(double from, double to, float speed)
{
    practice.on = true;
    practice.from = from;
    practice.to = to;
    practice.speed = speed;
}

void game_restart(void)
{
    reset_play();
}

void game_stop(void)
{
    if (G.drums)
        plat_log("drums: %d freestyle hits", G.freestyle);
    practice.on = false;
    audio_song_speed(1);
    memset(G.band, 0, sizeof(G.band));
    res_band_free();
    free_song();
    plat_set_perf(0);
}

void game_pause(bool pause)
{
    G.paused = pause;
    audio_song_pause(pause);
    plat_set_perf(!pause);              // paused: the menu clocks, and the screen may dim
    if (G.crowd_voice >= 0)
        sound_volume(G.crowd_voice, pause ? 0 : cfg.crowd_vol * 0.03f);
}

void game_set_autoplay(bool on)
{
    vox_autoplay(on);
    G.autoplay = on;
}

// YARG's stars: shares of the base score (gold takes Star Power)
static int stars_for(double score)
{
    static const double guitar[6] = {0.06, 0.12, 0.2, 0.47, 0.78, 1.15}, bass[6] = {0.05, 0.1, 0.19, 0.47, 0.78, 1.15};
    const double * cut = G.part_id == PART_BASS ? bass : guitar;
    int s = 0;
    for (int i = 0; i < 6; i++)
        if (G.base_score > 0 && score >= cut[i] * G.base_score)
            s = i + 1;
    return s;
}

Result game_result(void)
{
    Result r;
    memset(&r, 0, sizeof(r));
    r.part = G.part_id;
    r.diff = G.diff;
    r.score = (int)G.score;
    r.stars = stars_for(G.score);
    r.notes_hit = G.hits;
    r.notes_total = G.vocal ? vox_phrases() : G.part ? G.part->note_count : 0;
    r.best_streak = G.best_streak;
    r.overhits = G.overhits;
    if (G.off_n >= 20) {
        double mean = G.off_sum / G.off_n, var = G.off_sq / G.off_n - mean * mean;
        r.timed = true;
        r.off_ms = (int)floor(mean * 1000 + 0.5);
        r.spread_ms = (int)(sqrt(var > 0 ? var : 0) * 1000 + 0.5);
    }
    r.sp_hit = G.sp_hit;
    r.sp_total = G.part ? G.part->sp_count : 0;
    r.failed = G.failed;
    r.fc = !G.failed && r.notes_total > 0 && G.best_streak == r.notes_total;     // no miss and no overhit
    return r;
}

// --------------------------------------------------------------------------------------------------------- judging

static void set_crowd(void)
{
    int mood = G.rock > 0.66f ? SND_CROWD_GOOD : G.rock < 0.33f ? SND_CROWD_BAD : SND_CROWD_NEUTRAL;
    if (mood == G.crowd_mood)
        return;
    G.crowd_mood = mood;
    if (G.crowd_voice >= 0)
        sound_stop(G.crowd_voice);
    G.crowd_voice = sound_play(res.snd[mood], cfg.crowd_vol * 0.03f, true);
}

static void rock_change(float d)
{
    G.rock += d;
    if (G.rock > 1)
        G.rock = 1;
    if (G.rock <= 0) {
        G.rock = 0;
        if (!cfg.nofail && !G.autoplay && !practice.on)
            G.failed = true;
    }
    set_crowd();
}

static void set_my_stem(float volume)
{
    if (G.my_stem >= 0)
        audio_stem_volume(G.my_stem, volume);
    for (int i = 0; i < G.drum_stem_count; i++)
        audio_stem_volume(G.drum_stems[i], volume);
}

static void break_streak(void)
{
    G.streak = 0;
    set_my_stem(0);
}

static void mark_judged(uint8_t frets)
{
    for (int l = 0; l < 6; l++)
        if (frets & (1 << l))
            G.judged_t[l] = G.t;
}

static void miss_note(int i)
{
    G.state[i] = MISSED;
    mark_judged(G.part->notes[i].frets);
    G.misses++;
    break_streak();
    G.miss_flash = 1;
    G.miss_pulse = 1;
    static const float loss[4] = {0.025f, 0.03f, 0.04f, 0.05f};
    // drums have many more gems to the bar
    rock_change(-loss[G.diff] * (G.drums ? 0.5f : 1));
    if (G.phrase[i] >= 0 && G.phrase_ok[G.phrase[i]]) {
        G.phrase_ok[G.phrase[i]] = false;
    }
}

static void hit_note(int i)
{
    const Note * n = &G.part->notes[i];
    G.state[i] = HIT;
    mark_judged(n->frets);
    G.hits++;
    G.streak++;
    if (G.streak > G.best_streak)
        G.best_streak = G.streak;
    G.hit_pulse = 1;
    set_my_stem(1);
    G.score += 50 * frets_count(n->frets) * multiplier();
    rock_change(0.018f);
    for (int l = 0; l < 6; l++)
        if (n->frets & (1 << l))
            G.flame_t[l] = G.t;
    if (G.streak > 0 && G.streak % 50 == 0) {
        char t[32];
        snprintf(t, sizeof(t), "%d NOTE STREAK!", G.streak);
        popup(t);
        G.cheer = 1;
    }
    if (G.solo[i] >= 0)
        G.solo_hit[G.solo[i]]++;
    // a whole Star Power phrase: a quarter of the meter
    int ph = G.phrase[i];
    if (ph >= 0 && (n->flags & NOTE_SP_END) && G.phrase_ok[ph]) {
        bool was_ready = G.sp >= 0.5f;
        G.sp += 0.25f;
        if (G.sp > 1)
            G.sp = 1;
        G.sp_hit++;
        G.sp_flash = 1;
        sound_play(res.snd[SND_SP_AWARD], 0.8f, false);
        if (!was_ready && G.sp >= 0.5f && !G.sp_active) {
            popup("STAR POWER READY!");
            sound_play(res.snd[SND_SP_READY], 0.7f, false);
        }
    }
    if (n->len > 0) {
        for (int k = 0; k < MAX_SUS; k++) {
            if (G.sus[k].note < 0) {
                G.sus[k].note = i;
                G.sus[k].end = (n->time + (double)n->len) / 1e6;
                G.sus[k].frets = n->frets;
                G.sus[k].sp = ph >= 0 && G.phrase_ok[ph];
                G.sus[k].last_q = chart_quarters(G.chart, G.t);
                break;
            }
        }
    }
}

static void overhit(void)
{
    // presses in a long gap don't count against the player
    const Part * p = G.part;
    bool near = false;
    for (int i = G.first; i < p->note_count; i++) {
        double nt = p->notes[i].time / 1e6;
        if (nt > G.t + 0.6)
            break;
        if (nt > G.t - 0.6) {
            near = true;
            break;
        }
    }
    if (!near)
        return;
    G.overhits++;
    break_streak();
    rock_change(-0.02f);
    G.miss_flash = 0.7f;
    sound_play(res.snd[SND_OVERSTRUM1 + rand() % 4], 0.5f, false);
}

// Left and Up for one thumb, Triangle and Circle for the other, the kick under the fingers
// Or the snare under one finger and the kick under the other, so the snare and the hi-hat are on two hands as they are
// on a kit: they're the chord most charts are made of
static uint32_t drum_buttons(int lane)
{
    const uint32_t dpad = BTN_UP | BTN_LEFT | BTN_RIGHT | BTN_DOWN;
    static const uint32_t four[4] = {BTN_LEFT, BTN_UP, BTN_TRIANGLE, BTN_CIRCLE};
    static const uint32_t five[5] = {BTN_LEFT, BTN_UP, BTN_TRIANGLE, BTN_SQUARE, BTN_CIRCLE};
    const uint32_t four_s[4] = {BTN_L, dpad, BTN_TRIANGLE | BTN_SQUARE, BTN_CIRCLE | BTN_CROSS};
    const uint32_t five_s[5] = {BTN_L, dpad, BTN_TRIANGLE, BTN_SQUARE, BTN_CIRCLE | BTN_CROSS};
    if (lane == LANE_OPEN)
        return cfg.drum_layout ? BTN_R : BTN_L | BTN_R | BTN_CROSS | BTN_DOWN;
    if (lane >= G.lanes)
        return 0;
    if (cfg.drum_layout)
        return G.lanes == 4 ? four_s[lane] : five_s[lane];
    return G.lanes == 4 ? four[lane] : five[lane];
}

// a real gap for freestyle: GAP_MIN seconds at least between gems (the rest in a beat isn't one), once the last gem
// has rung and before the next can be hit. Up to the first gem and after the last too
static double gap_left(void)
{
    const Part * p = G.part;
    int lo = 0, hi = p->note_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (p->notes[mid].time / 1e6 > G.t)
            hi = mid;
        else
            lo = mid + 1;
    }
    double prev = lo ? p->notes[lo - 1].time / 1e6 : -1e9;
    double next = lo < p->note_count ? p->notes[lo].time / 1e6 : 1e9;
    return next - prev >= GAP_MIN && G.t > prev + GAP_AFTER && G.t < next - WINDOW ? next - WINDOW - G.t : 0;
}

// a pad's piece of the kit, from what the gem says it is (a chart from before they were told apart, or a hit with no gem:
// yellow's the hi-hat, blue a tom, green the crash)
static bool lane_cymbal(int lane, int flags)
{
    if (G.lanes == 5)
        return lane == 1 || lane == 3;
    return lane >= 1 && lane <= 3 && !(flags & (NOTE_TOM_Y << (lane - 1)));
}

static void pad_sound(int lane, const Note * n, float volume)
{
    static const int four[4] = {SND_DRUM_SNARE, SND_DRUM_HAT, SND_DRUM_TOM_HI, SND_DRUM_CRASH};
    static const int five[5] = {SND_DRUM_SNARE, SND_DRUM_HAT, SND_DRUM_TOM_HI, SND_DRUM_CRASH, SND_DRUM_TOM_LO};
    static const int toms[4] = {SND_DRUM_SNARE, SND_DRUM_TOM_HI, SND_DRUM_TOM_MID, SND_DRUM_TOM_LO};
    static int turn;
    int snd = lane == LANE_OPEN ? SND_DRUM_KICK : G.lanes == 4 ? four[lane & 3] : five[lane % 5];
    if (n && G.lanes == 4 && lane != LANE_OPEN && G.chart->version >= 2) {
        snd = toms[lane & 3];
        if (lane_cymbal(lane, n->flags))
            snd = lane == 1 ? SND_DRUM_HAT : lane == 2 ? SND_DRUM_RIDE : SND_DRUM_CRASH;
    }
    // the takes in turn, so a run isn't one sample over and over
    int takes = snd == SND_DRUM_KICK || snd == SND_DRUM_SNARE || snd == SND_DRUM_HAT ? 3 : 2;
    sound_play(res.snd[snd + turn++ % takes], volume, false);
}

static void press_lane(int lane)
{
    const Part * p = G.part;
    uint8_t bit = lane == LANE_OPEN ? FRET_OPEN : 1 << lane;
    for (int i = G.first; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        double nt = n->time / 1e6;
        if (nt > G.t + WINDOW)
            break;
        if (G.state[i] != PENDING || nt < G.t - WINDOW || !(n->frets & bit) || (G.got[i] & bit))
            continue;
        // on a fast run a hit nearer the next gem on this pad is that gem: the one skipped goes as the miss it was, not
        // one further down the run, and the flame and the sound land on what was played
        if (G.drums && G.t - nt > NEXT_LATE) {
            int k = i + 1;
            while (k < p->note_count && !(p->notes[k].frets & bit))
                k++;
            if (k < p->note_count && p->notes[k].time / 1e6 - G.t < G.t - nt)
                continue;
        }
        // the fingers already down count: a repeated chord needs just one key pressed again
        G.got[i] |= bit | (G.drums ? 0 : G.held & n->frets);
        if (G.drums && cfg.drum_hits)
            pad_sound(lane, n, 0.5f);
        if (G.got[i] == n->frets) {
            hit_note(i);
            double off = G.t - nt;
            G.off_sum += off;
            G.off_sq += off * off;
            G.off_n++;
        }
        return;
    }
    bool gap = G.drums && gap_left() > 0;
    if (G.drums && (gap || cfg.drum_hits))
        pad_sound(lane, NULL, gap ? 0.9f : 0.5f);
    // a finger coming down just after its chord counted, or a press just late for a note that's already cost the miss,
    // isn't a stray one
    if (G.t - G.judged_t[lane] < PRESS_GRACE)
        return;
    // a stray drum hit is just a fill, and heard in a gap of the chart
    if (!G.drums)
        overhit();
    else if (gap) {
        G.freestyle++;
        // a fill, and the crowd's with it. Hits as a drummer could play them: mashing adds nothing
        if (G.t > 0 && G.t - G.free_t >= 0.08) {
            G.free_hits++;
            G.free_t = G.t;
        }
#ifndef __vita__
        plat_log("drums: freestyle lane %d at %.2f", lane, G.t);
#endif
    }
}

static void activate_sp(void)
{
    if (G.sp_active || G.sp < 0.5f)
        return;
    G.sp_active = true;
    G.sp_last_q = chart_quarters(G.chart, G.t);
    G.sp_flash = 1;
    G.pyro_t = G.t;
    popup("STAR POWER!");
    sound_play(res.snd[SND_SP_DEPLOY], 0.9f, false);
    sound_play(res.snd[SND_SP_CROWD], cfg.crowd_vol * 0.06f, false);
#ifndef __vita__
    plat_log("sp: on at %.2f", G.t);
#endif
}

static void judge(uint32_t pressed_lanes)
{
    const Part * p = G.part;
    for (int lane = 0; lane < 6; lane++)
        if (pressed_lanes & (1 << lane))
            press_lane(lane);
    // notes whose window has passed
    while (G.first < p->note_count) {
        int i = G.first;
        double nt = p->notes[i].time / 1e6;
        if (G.state[i] == PENDING) {
            if (G.t <= nt + WINDOW)
                break;
            if (G.roll && G.roll[i] && G.t - G.pad_t[__builtin_ctz(p->notes[i].frets)] < ROLL_HOLD)
                hit_note(i);
            else
                miss_note(i);
        }
        G.first++;
    }
    // a gap's over
    if (G.free_hits && gap_left() <= 0) {
        int bonus = G.free_hits * 20 * multiplier();
        char t[32];
        snprintf(t, sizeof(t), "FREESTYLE +%d", bonus);
        popup(t);
        G.score += bonus;
        if (G.free_hits >= 8)
            G.cheer = 1;
        G.free_hits = 0;
    }
    // a solo's end
    for (int k = 0; k < p->solo_count; k++) {
        double end = p->solos[k].end / 1e6;
        if (G.solo_index < k && G.t >= end) {
            G.solo_index = k;
            int total = 0;
            for (int i = 0; i < p->note_count; i++)
                total += G.solo[i] == k;
            if (total > 0) {
                int pct = G.solo_hit[k] * 100 / total;
                char t[48];
                if (pct == 100)
                    snprintf(t, sizeof(t), "PERFECT SOLO! +%d", G.solo_hit[k] * 100);
                else
                    snprintf(t, sizeof(t), "%d%% SOLO  +%d", pct, pct >= 50 ? G.solo_hit[k] * 50 : 0);
                G.score += pct == 100 ? G.solo_hit[k] * 100 : pct >= 50 ? G.solo_hit[k] * 50 : 0;
                popup(t);
            }
        }
    }
}

static void update_sustains(void)
{
    double q = chart_quarters(G.chart, G.t);
    for (int k = 0; k < MAX_SUS; k++) {
        Sustain * s = &G.sus[k];
        if (s->note < 0)
            continue;
        uint32_t need = s->frets & 0x1F;
        bool held = (G.held & need) == need && (!(s->frets & FRET_OPEN) || (G.held & (1 << LANE_OPEN)));
        double until = G.t < s->end ? q : chart_quarters(G.chart, s->end);
        double dq = until - s->last_q;
        if (dq > 0 && held) {
            G.score += 25 * dq * multiplier();
            if (s->sp && G.whammy > 0.2f) {
                bool was_ready = G.sp >= 0.5f;
                G.sp += (float)dq * SP_PER_QUARTER;
                if (G.sp > 1)
                    G.sp = 1;
                if (!was_ready && G.sp >= 0.5f && !G.sp_active) {
                    popup("STAR POWER READY!");
                    sound_play(res.snd[SND_SP_READY], 0.7f, false);
                }
            }
            for (int l = 0; l < 6; l++)
                if (s->frets & (1 << l))
                    G.flame_t[l] = G.t - 0.06;          // keep the flame going
        }
        s->last_q = until;
        if (!held || G.t >= s->end)
            s->note = -1;
    }
}

// the lanes the touch screen holds: the bottom of the screen, under each lane's cup
static int touch_lane_at(float x, float y)
{
    if (y < SCREEN_H * 0.55f)
        return -1;
    float best = 1e9f;
    int lane = -1;
    for (int l = 0; l < G.lanes; l++) {
        float sx, sy;
        project(lane_x(l), 0, 0, &sx, &sy);
        float d = fabsf(sx - x);
        if (d < best) {
            best = d;
            lane = l;
        }
    }
    float x0, y0, x1, y1;
    project(-1, 0, 0, &x0, &y0);
    project(0, 0, 0, &x1, &y1);
    return best < (x1 - x0) * 0.6f ? lane : -1;
}

// Star Power by tilting: a quick turn of the Vita (about 25 degrees, any way) away from how it's been held. How it's
// been held follows the accelerometer slowly, so drifting into a new grip doesn't count
static void tilt(const float accel[3])
{
    float a[3] = {accel[0], accel[1], accel[2]};
    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (n < 0.6f || n > 1.4f)
        return;                 // shaking: no steady reading of which way is down
    for (int k = 0; k < 3; k++)
        a[k] /= n;
    if (!G.tilt_init) {
        memcpy(G.tilt_base, a, sizeof(a));
        G.tilt_init = true;
    }
    float dot = a[0] * G.tilt_base[0] + a[1] * G.tilt_base[1] + a[2] * G.tilt_base[2];
    float angle = acosf(dot > 1 ? 1 : dot < -1 ? -1 : dot);
    if (G.tilt_armed && angle > 0.42f) {
        activate_sp();
        G.tilt_armed = false;
    } else if (angle < 0.2f)
        G.tilt_armed = true;
    float m = 0;
    for (int k = 0; k < 3; k++) {
        G.tilt_base[k] += (a[k] - G.tilt_base[k]) * 0.015f;
        m += G.tilt_base[k] * G.tilt_base[k];
    }
    m = sqrtf(m);
    for (int k = 0; k < 3; k++)
        G.tilt_base[k] /= m;
}

// both cymbals together: the hi-hat and the crash, unless the chart asks for the two just there
static bool both_cymbals(uint32_t lanes_pressed)
{
    const uint32_t both = 1 << 1 | 1 << 3;
    // together: the second a moment after the first is just two gems played one after the other
    if (!(lanes_pressed & both) || fabs(G.pad_t[1] - G.pad_t[3]) > 0.05)
        return false;
    const Part * p = G.part;
    uint32_t due = 0;
    for (int i = G.first > 8 ? G.first - 8 : 0; i < p->note_count && p->notes[i].time / 1e6 <= G.t + WINDOW; i++)
        if (p->notes[i].time / 1e6 >= G.t - WINDOW)
            due |= p->notes[i].frets;
    return (due & both) != both;
}

// a phrase's grade moves the meter and the run; a run of good ones is the multiplier (x4 at six)
static void update_vocals(void)
{
    float grade, points;
    if (!vox_update(G.t, &grade, &points))
        return;
    static const char * const words[5] = {"AWFUL", "MESSY", "OK", "STRONG", "AWESOME!"};
    int w = grade >= 1 ? 4 : grade >= 0.8f ? 3 : grade >= 0.6f ? 2 : grade >= 0.1f ? 1 : 0;
    int mult = (1 + G.vox_run / 2 > 4 ? 4 : 1 + G.vox_run / 2) * (G.vocal && G.sp_active ? 2 : 1);
    G.score += points * mult;
    G.vox_run = grade >= 0.8f ? G.vox_run + 1 : 0;
    if (!G.vocal) {
        // singing along: the instrument keeps the meter and the streak, the voice only adds its points
        char t[32];
        snprintf(t, sizeof(t), "VOCALS %s", words[w]);
        popup(t);
        return;
    }
    if (grade >= 0.6f)
        G.hits++;
    // the mic has no Star Power notes: strong phrases fill the meter
    if (grade >= 0.8f) {
        bool was_ready = G.sp >= 0.5f;
        G.sp = G.sp > 0.875f ? 1 : G.sp + 0.125f;
        if (!was_ready && G.sp >= 0.5f && !G.sp_active)
            sound_play(res.snd[SND_SP_READY], 0.7f, false);
    }
    G.streak = G.vox_run;
    if (G.streak > G.best_streak)
        G.best_streak = G.streak;
    char t[32];
    if (mult > 1)
        snprintf(t, sizeof(t), "%s  x%d", words[w], mult);
    else
        snprintf(t, sizeof(t), "%s", words[w]);
    popup(t);
    rock_change((grade - 0.6f) * 0.2f);
}

int game_update(const Pad * pad, uint32_t pressed)
{
    if (G.paused || !G.part)
        return GAME_PAUSE;
    G.t = audio_song_time() - cfg.offset_ms / 1000.0;
    if (pressed & BTN_START)
        return GAME_PAUSE;

    // lanes held and newly pressed: buttons, then the touch screen
    uint32_t held = 0, lanes_pressed = 0;
    for (int lane = 0; lane < 6; lane++) {
        int src = lane < G.lanes && cfg.lefty ? G.lanes - 1 - lane : lane;
        uint32_t b = G.drums ? drum_buttons(src) : lane_buttons(src);
        if (pad->buttons & b)
            held |= 1 << lane;
        if (pressed & b)
            lanes_pressed |= 1 << lane;
    }
    int new_ids[MAX_TOUCH], new_count = 0;
    for (int i = 0; i < pad->ntouch; i++) {
        int lane = touch_lane_at(pad->touch_x[i], pad->touch_y[i]);
        if (lane < 0)
            continue;
        held |= 1 << lane;
        bool known = false;
        for (int k = 0; k < G.touch_count; k++)
            if (G.touch_ids[k] == pad->touch_id[i] && G.touch_lane[k] == lane)
                known = true;
        if (!known)
            lanes_pressed |= 1 << lane;
        G.touch_lane[new_count] = lane;
        new_ids[new_count++] = pad->touch_id[i];
    }
    memcpy(G.touch_ids, new_ids, sizeof(new_ids));
    G.touch_count = new_count;

    if (G.autoplay) {
        held = lanes_pressed = 0;
        const Part * p = G.part;
        for (int i = G.first; i < p->note_count && p->notes[i].time / 1e6 <= G.t; i++)
            if (G.state[i] == PENDING)
                lanes_pressed |= p->notes[i].frets & FRET_OPEN ? 1 << LANE_OPEN : p->notes[i].frets;
        held = lanes_pressed;
        for (int k = 0; k < MAX_SUS; k++)
            if (G.sus[k].note >= 0)
                held |= (G.sus[k].frets & 0x1F) | (G.sus[k].frets & FRET_OPEN ? 1 << LANE_OPEN : 0);
        if (G.sp >= 0.5f && !G.sp_active)
            activate_sp();
    }
    G.held = held;
    for (int lane = 0; lane < 6; lane++)
        if (lanes_pressed & (1 << lane))
            G.pad_t[lane] = G.t;

    // whammy: the right stick moving, or a finger rubbing the rear touchpad (a thumb on the frets can't reach the stick).
    // Its way from where it landed is the bar, as far as the stick's from the middle
    float dw = fabsf(pad->rx - G.last_rx) + fabsf(pad->ry - G.last_ry), bar = hypotf(pad->rx, pad->ry);
    G.last_rx = pad->rx;
    G.last_ry = pad->ry;
    if (pad->back_on) {
        if (!G.back_on) {
            G.back_from[0] = G.back_last[0] = pad->back_x;
            G.back_from[1] = G.back_last[1] = pad->back_y;
        }
        dw += fabsf(pad->back_x - G.back_last[0]) + fabsf(pad->back_y - G.back_last[1]);
        G.back_last[0] = pad->back_x;
        G.back_last[1] = pad->back_y;
        float rub = hypotf(pad->back_x - G.back_from[0], pad->back_y - G.back_from[1]) * 3;
        bar = rub > bar ? rub : bar;
    }
    G.back_on = pad->back_on;
    if (G.autoplay) {
        bar = 0.5f + 0.5f * sinf((float)G.t * 30);
        dw = 1;
    }
    G.whammy = G.whammy * 0.9f + (dw > 0.02f ? 0.35f : 0);
    if (G.whammy > 1)
        G.whammy = 1;
    // the part's own stem bends while a sustain rings
    bool ringing = false;
    for (int k = 0; k < MAX_SUS; k++)
        ringing |= G.sus[k].note >= 0;
    G.bend += ((ringing ? (bar > 1 ? 1 : bar) : 0) - G.bend) * 0.3f;
    if (!G.drums)
        audio_stem_bend(G.my_stem, G.bend > 0.003f ? G.bend : 0);

    // Star Power: Select, a quick tilt, or on drums both cymbals at once, as in World Tour
    if ((pressed & BTN_SELECT) || (G.drums && both_cymbals(lanes_pressed)))
        activate_sp();
    if (cfg.tilt && pad->has_motion)
        tilt(pad->accel);

    if (G.sing)
        update_vocals();
    if (!G.vocal) {
        judge(lanes_pressed);
        update_sustains();
    }

    // Star Power drains a meter in 32 quarter notes
    if (G.sp_active) {
        double q = chart_quarters(G.chart, G.t);
        if (q > G.sp_last_q)
            G.sp -= (float)(q - G.sp_last_q) * SP_PER_QUARTER;
        G.sp_last_q = q;
        if (G.sp <= 0) {
            G.sp = 0;
            G.sp_active = false;
            sound_play(res.snd[SND_SP_RELEASE], 0.7f, false);
        }
    }
    G.miss_flash *= 0.9f;
    G.sp_flash *= 0.93f;
    G.hit_pulse *= 0.8f;
    G.miss_pulse *= 0.9f;
    G.cheer *= 0.985f;

    // fps
    G.fps_frames++;
    double now = plat_now_us() / 1e6;
    if (now - G.fps_t >= 1) {
        G.fps = G.fps_frames;
        G.fps_frames = 0;
        G.fps_t = now;
    }

    if (G.failed) {
        sound_play(res.snd[SND_BOO1 + rand() % 3], cfg.crowd_vol * 0.1f, false);
        return GAME_FAILED;
    }
    if (practice.on) {
        // the end of the stretch: how it went, and again
        if (G.t > practice.to + 0.6 || audio_song_finished()) {
            char t[48] = "";
            if (practice.notes)
                snprintf(t, sizeof(t), "%d%%  (%d / %d)", G.hits * 100 / practice.notes, G.hits, practice.notes);
            reset_play();
            popup(t);
        }
        return GAME_PLAYING;
    }
    double end = G.last_note_end + 1.5;
    if ((G.t > end && (audio_song_finished() || G.t > G.song->length_ms / 1000.0 + 0.5)) ||
        (audio_song_finished() && G.t > G.last_note_end)) {
        G.done = true;
        return GAME_DONE;
    }
    return GAME_PLAYING;
}

// ---------------------------------------------------------------------------------------------------------- drawing

static uint32_t board_col(float z, float lift)
{
    float d = z < 0 ? 0 : z / Z_FAR, v = 190 - 70 * d + lift;
    if (G.sp_active)
        return RGBA((int)(v * 0.64f), (int)(v * 0.85f), (int)(v * 1.15f > 255 ? 255 : v * 1.15f), 255);
    return RGBA((int)v, (int)v, (int)v, 255);
}

static void draw_highway(void)
{
    const Sprite hw = sprite_whole(G.highway);
    float t = (float)G.t;
    // the board, scrolling; 10.6 units per repeat keeps the picture's shape on a 5.24-wide board
    float rep = 10.6f, off = t * (float)G.speed / rep;
    // the art sits back so the notes read: brighter near the strikeline, deeper in the distance, a soft lift on each beat
    double beat = G.t > 0 ? chart_quarters(G.chart, G.t) : 0;
    float lift = 22 * expf(-(float)(beat - floor(beat)) * 5);
    gfx_blend(BLEND_ALPHA);
    const int seg = 6;
    for (int i = 0; i < seg; i++) {
        float z0 = Z_NEAR + (Z_FAR - Z_NEAR) * i / seg, z1 = Z_NEAR + (Z_FAR - Z_NEAR) * (i + 1) / seg;
        float v0 = -(z0 / rep + off), v1 = -(z1 / rep + off);
        gfx_tex(G.highway);
        const float p[4][3] = {{-HALF_W, 0, z0}, {HALF_W, 0, z0}, {HALF_W, 0, z1}, {-HALF_W, 0, z1}};
        const float uv[4][2] = {{1, v0}, {0, v0}, {0, v1}, {1, v1}};
        uint32_t b0 = board_col(z0, lift), b1 = board_col(z1, lift);
        const uint32_t c[4] = {with_alpha(b0, fade(z0)), with_alpha(b0, fade(z0)), with_alpha(b1, fade(z1)),
                               with_alpha(b1, fade(z1))};
        gfx_quad(p, uv, c);
    }
    (void)hw;
    // beat lines
    const Sprite * bar_l = atlas_get(res.game, "bar_large"), * bar_m = atlas_get(res.game, "bar_med");
    const Chart * c = G.chart;
    int lo = 0, hi = c->beat_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (c->beats[mid].time / 1e6 < t - 0.3)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (int i = lo; i < c->beat_count; i++) {
        float z = (float)((c->beats[i].time / 1e6 - t) * G.speed);
        if (z > Z_FAR)
            break;
        bool measure = c->beats[i].kind == 0;
        float d = measure ? 0.13f : 0.07f;
        uint32_t col = with_alpha(measure ? RGBA(235, 235, 245, 230) : RGBA(190, 190, 200, 150), fade(z));
        flat(-HALF_W + 0.05f, HALF_W - 0.05f, z - d / 2, z + d / 2, 0.005f, measure ? bar_l : bar_m, 0, 1, col, col);
    }
    // the strings
    const Sprite * line = atlas_get(res.game, "line");
    for (int l = 0; l < G.lanes; l++) {
        float x = lane_x(l);
        uint32_t near_c = RGBA(200, 200, 210, 120);
        double since = G.t - G.flame_t[l];
        if (since >= 0 && since < 0.2)
            near_c = with_alpha(lane_colour[lane_art(l)], 0.9f);
        strip(x - 0.025f, x + 0.025f, 0.008f, line, near_c);
    }
    // the rails: silver, glowing blue with Star Power ready or on, red on a miss
    const Sprite * rail = atlas_get(res.game, "rail");
    uint32_t rc = RGBA(225, 225, 235, 255);
    if (G.sp_active)
        rc = RGBA(120, 210, 255, 255);
    else if (G.sp >= 0.5f)
        rc = RGBA(170, 220, 255, 255);
    if (G.miss_flash > 0.05f) {
        int k = (int)(G.miss_flash * 200);
        rc = RGBA(225, 225 - k, 235 - k, 255);
    }
    for (int side = -1; side <= 1; side += 2) {
        float x = side * HALF_W;
        strip(x - 0.09f, x + 0.09f, 0.01f, rail, rc);
    }
}

static void draw_sustains(void)
{
    const Part * p = G.part;
    const Sprite * sus = atlas_get(res.game, "sustain");
    float t = (float)G.t;
    for (int i = G.first > 8 ? G.first - 8 : 0; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        float z_start = (float)((n->time / 1e6 - t) * G.speed);
        if (z_start > Z_FAR)
            break;
        if (!n->len)
            continue;
        float z_end = (float)(((n->time + (double)n->len) / 1e6 - t) * G.speed);
        if (z_end < 0)
            continue;
        bool active = false;
        for (int k = 0; k < MAX_SUS; k++)
            if (G.sus[k].note == i)
                active = true;
        if (G.state[i] == HIT && !active)
            continue;           // let go: the rest of it is gone
        float z0 = z_start < 0 && active ? 0 : z_start;
        if (z_end > Z_FAR)
            z_end = Z_FAR;
        bool dead = G.state[i] == MISSED;
        for (int l = 0; l < 6; l++) {
            if (!(n->frets & (1 << l)))
                continue;
            bool open = l == LANE_OPEN;
            float x = open ? 0 : lane_x(l), w = open ? 1.6f : 0.13f;
            uint32_t col = open ? lane_colour[6] : lane_colour[l];
            if (G.sp_active || (G.phrase[i] >= 0 && G.phrase_ok[G.phrase[i]]))
                col = lane_colour[5];
            if (dead)
                col = RGBA(110, 110, 120, 200);
            else if (active)
                col = RGBA(((col & 0xFF) + 255) / 2, (((col >> 8) & 0xFF) + 255) / 2, (((col >> 16) & 0xFF) + 255) / 2,
                           255);
            // whammy shakes it
            int pieces = active ? 10 : 1;
            float amp = active ? 0.03f + G.whammy * 0.12f : 0;
            for (int s = 0; s < pieces; s++) {
                float za = z0 + (z_end - z0) * s / pieces, zb = z0 + (z_end - z0) * (s + 1) / pieces;
                float wob = amp * sinf(za * 2.6f + t * 25);
                flat(x - w + wob, x + w + wob, za, zb, 0.012f, sus, 0, 1, with_alpha(col, fade(za)),
                     with_alpha(col, fade(zb)));
            }
        }
    }
}

// the drums' marks on the board: a roll glows down its lane, a gap for freestyle right across
static void draw_zones(void)
{
    float half = fabsf(lane_x(1) - lane_x(0)) * 0.46f;
    for (int i = 0; i < G.gap_count + G.roll_count; i++) {
        const Zone * s = i < G.gap_count ? &G.gaps[i] : &G.rolls[i - G.gap_count];
        float z0 = (float)((s->from - G.t) * G.speed), z1 = (float)((s->to - G.t) * G.speed);
        z0 = z0 < 0 ? 0 : z0;
        z1 = z1 > Z_FAR ? Z_FAR : z1;
        if (z1 <= z0)
            continue;
        bool wide = s->lane < 0 || s->lane == LANE_OPEN;
        float x = wide ? 0 : lane_x(s->lane), w = wide ? HALF_W : half;
        uint32_t c = s->lane < 0 ? RGBA(70, 255, 150, 54) : ALPHA(lane_colour[wide ? 6 : lane_art(s->lane)], 84);
        flat(x - w, x + w, z0, z1, 0.008f, NULL, 0, 1, with_alpha(c, fade(z0)), with_alpha(c, fade(z1)));
    }
}

static void draw_cups(bool caps)
{
    for (int l = 0; l < G.lanes; l++) {
        char name[16];
        float x = lane_x(l);
        if (!caps) {
            snprintf(name, sizeof(name), "cup_%d", lane_art(l));
            board(x, -0.02f, 0, 1.1f, 0.55f, atlas_get(res.game, name), 0, 0, 1, 1, WHITE);
        } else if (G.held & (1 << l)) {
            snprintf(name, sizeof(name), "cap_%d", lane_art(l));
            board(x, 0.1f, -0.01f, 0.8f, 0.4f, atlas_get(res.game, name), 0, 0, 1, 1, WHITE);
        }
    }
    // the open note's bar lights across the cups
    if (caps && (G.held & (1 << LANE_OPEN))) {
        gfx_blend(BLEND_ADD);
        board(0, 0.05f, -0.02f, 5.2f, 0.4f, atlas_get(res.game, "glow"), 0, 0, 1, 1, with_alpha(lane_colour[6], 0.5f));
        gfx_blend(BLEND_ALPHA);
    }
}

static void draw_gems(void)
{
    const Part * p = G.part;
    float t = (float)G.t;
    static const char * kinds[3] = {"normal", "hopo", "tap"};
    // the furthest visible note, then back towards the strikeline (far ones first)
    int last = G.first;
    while (last < p->note_count && (p->notes[last].time / 1e6 - t) * G.speed <= Z_FAR)
        last++;
    int start = G.first > 16 ? G.first - 16 : 0;
    int star_frame = (int)(G.t * 10) & 7;
    for (int i = last - 1; i >= start; i--) {
        const Note * n = &p->notes[i];
        if (G.state[i] == HIT)
            continue;
        float z = (float)((n->time / 1e6 - t) * G.speed);
        if (z < Z_NEAR + 0.3f)
            continue;
        float a = fade(z);
        if (G.state[i] == MISSED)
            a *= 0.55f;
        int kind = n->flags & NOTE_TAP ? 2 : n->flags & NOTE_HOPO ? 1 : 0;
        bool star = G.phrase[i] >= 0 && G.phrase_ok[G.phrase[i]];
        uint32_t col = with_alpha(WHITE, a);
        char name[32];
        if (n->frets & FRET_OPEN) {
            snprintf(name, sizeof(name), "%s_%s", G.sp_active || star ? "opensp" : "open", kinds[kind]);
            board(0, 0, z, 4.9f, 0.62f, atlas_get(res.game, name), 0, 0, 1, 1, col);
            if (n->frets == FRET_OPEN)
                continue;       // on drums the kick comes with pads
        }
        for (int l = 0; l < G.lanes; l++) {
            if (!(n->frets & (1 << l)))
                continue;
            int c = G.sp_active ? 5 : lane_art(l);
            // a cymbal lies wide and flat, its dome lit, so it can be told from a drum at a glance
            bool cym = G.drums && G.chart->version >= 2 && lane_cymbal(l, n->flags);
            const char * look = cym ? kinds[1] : kinds[kind];
            float gw = cym ? 1.2f : 1.04f, gh = cym ? 0.4f : 0.52f;
            if (star)
                snprintf(name, sizeof(name), "star_%s_%d_%d", look, c, star_frame);
            else
                snprintf(name, sizeof(name), "gem_%s_%d", look, c);
            // a soft shadow lifts the gem off the board art; a touch larger than before
            const Sprite * gem = atlas_get(res.game, name);
            board(lane_x(l), 0, z - 0.07f, gw * 1.08f, gh * 1.08f, gem, 0, 0, 1, 1,
                  with_alpha(RGBA(0, 0, 0, 255), a * 0.45f));
            board(lane_x(l), 0, z, gw, gh, gem, 0, 0, 1, 1, col);
        }
    }
}

static void draw_flames(void)
{
    gfx_blend(BLEND_ADD);
    const Sprite * flame = atlas_get(res.game, "flame");
    const Sprite * glow = atlas_get(res.game, "cup_glow");
    uint32_t fc = G.sp_active ? RGBA(120, 210, 255, 255) : RGBA(255, 190, 90, 255);
    for (int l = 0; l < 6; l++) {
        double since = G.t - G.flame_t[l];
        if (since < 0 || since > 0.32)
            continue;
        int frame = (int)(since / 0.32 * 16);
        if (frame > 15)
            frame = 15;
        float u0 = (frame % 4) * 0.25f, v0 = (frame / 4) * 0.25f;
        if (l == LANE_OPEN) {
            for (int k = 0; k < G.lanes; k++)
                board(lane_x(k), 0.05f, 0, 1.0f, 1.6f, flame, u0, v0, u0 + 0.25f, v0 + 0.25f,
                      with_alpha(lane_colour[6], 0.8f));
            continue;
        }
        float x = lane_x(l);
        board(x, 0.08f, 0, 1.1f, 2.1f, flame, u0, v0, u0 + 0.25f, v0 + 0.25f, fc);
        board(x, -0.05f, 0, 1.2f, 0.6f, glow, 0, 0, 1, 1, with_alpha(lane_colour[lane_art(l)], 1 - (float)since / 0.32f));
    }
    // Star Power: lightning down both rails
    if (G.sp_active) {
        const Sprite * bolt = atlas_get(res.game, "lightning");
        for (int side = -1; side <= 1; side += 2) {
            float x = side * HALF_W;
            int seed = (int)(G.t * 20) * 7 + side * 3;
            for (int k = 0; k < 4; k++) {
                float z0 = ((seed * (k + 3)) % 9) * 0.9f, len = 1.6f;
                uint32_t c = with_alpha(RGBA(140, 210, 255, 255), fade(z0) * 0.9f);
                flat(x - 0.22f, x + 0.22f, z0, z0 + len, 0.02f, bolt, 0, 1, c, c);
            }
        }
    }
    gfx_blend(BLEND_ALPHA);
}

// a sprite turned about its own point (px, py) in its pixels, drawn with that point at (x, y)
static void sprite_rot(const Sprite * s, float x, float y, float w, float h, float px, float py, float angle,
                       uint32_t col)
{
    float c = cosf(angle), sn = sinf(angle);
    float ox = -px * w / s->w, oy = -py * h / s->h;
    float cx[4] = {ox, ox + w, ox + w, ox}, cy[4] = {oy, oy, oy + h, oy + h};
    float p[4][3];
    for (int i = 0; i < 4; i++) {
        p[i][0] = x + cx[i] * c - cy[i] * sn;
        p[i][1] = y + cx[i] * sn + cy[i] * c;
        p[i][2] = 0;
    }
    const float uv[4][2] = {{s->u0, s->v0}, {s->u1, s->v0}, {s->u1, s->v1}, {s->u0, s->v1}};
    const uint32_t cc[4] = {col, col, col, col};
    gfx_tex(s->tex);
    gfx_quad(p, uv, cc);
}

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

// part of a HUD texture laid out at scale (kx, ky) with its top-left at (x0, y0); x, y, w, h in the texture's pixels
static void hud_at(const char * name, float x0, float y0, float kx, float ky, float x, float y, float w, float h,
                   uint32_t c)
{
    gfx_sprite(atlas_get(res.hud, name), x0 + x * kx, y0 + y * ky, w * kx, h * ky, c);
}

// a number on an LCD, right-aligned in `cells` cells; the empty cells show their unlit segments
static void lcd(const char * which, long value, int cells, float right, float y, float h)
{
    const Sprite * s = atlas_get(res.game, which);
    char buf[24];
    snprintf(buf, sizeof(buf), "%*ld", cells, value);
    int n = (int)strlen(buf);
    float cell = 1.0f / 11, w = h * 64.0f / 112, x = right - n * w;
    for (int i = 0; i < n; i++) {
        float u0 = (buf[i] == ' ' ? 10 : buf[i] - '0') * cell;
        gfx_sprite_uv(s, x + i * w, y, w, h, u0, 0, u0 + cell, 1, buf[i] == ' ' ? RGBA(255, 255, 255, 90) : WHITE);
    }
}

// GHWT's solo HUD: left of the highway the ROCK dome, the Star Power tubes round its top and the score and streak
// counters under it; on the right rail the multiplier over its seat of streak lamps
// the buttons under the lanes, as a song starts
static void draw_keys(void)
{
    static const struct {
        uint32_t button;
        const char * name;
    } keys[] = {{BTN_L, "btn_l"}, {BTN_R, "btn_r"}, {BTN_LEFT, "btn_left"}, {BTN_UP, "btn_up"},
                {BTN_RIGHT, "btn_right"}, {BTN_DOWN, "btn_down"}, {BTN_TRIANGLE, "btn_triangle"},
                {BTN_SQUARE, "btn_square"}, {BTN_CIRCLE, "btn_circle"}, {BTN_CROSS, "btn_cross"}};
    float a = G.t < 3.5 ? 1 : (float)(4.5 - G.t), size = 34;
    for (int lane = 0; lane < G.lanes; lane++) {
        int src = cfg.lefty ? G.lanes - 1 - lane : lane;
        uint32_t b = G.drums ? drum_buttons(src) : lane_buttons(src);
        float x, y;
        project(lane_x(lane), 0, 0, &x, &y);
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
            if (b & keys[k].button) {
                gfx_sprite(atlas_get(res.hud, keys[k].name), x - size / 2, y + 28, size, size, with_alpha(WHITE, a));
                break;
            }
    }
}

static void draw_hud(void)
{
    gfx_2d();
    gfx_blend(BLEND_ALPHA);
    float t = (float)G.t;
    const float k = 0.72f, dx = 14, dy = 250;          // the dome's 256x256 texture
    const float cx = dx + 129 * k, cy = dy + 124 * k;   // the centre of the dome's arc
    const float pi = 3.14159265f;
    // six tubes stand round the dome, lit as the meter fills: orange, blue once Star Power can be used
    bool blue = G.sp_active || G.sp >= 0.5f;
    float pulse = G.sp_active ? 0.85f + 0.15f * sinf(t * 6) : 1;
    if (blue) {
        float a = G.sp_active ? 0.6f * pulse : 0.3f + 0.15f * sinf(t * 4);
        gfx_blend(BLEND_ADD);
        gfx_sprite(atlas_get(res.hud, "hud_star_power_ready_glow"), cx - 125, cy - 135, 250, 210,
                   fade_alpha(RGBA(128, 204, 255, 255), a));
        gfx_blend(BLEND_ALPHA);
    }
    const Sprite * tube = atlas_get(res.hud, "hud_rock_tube");
    const Sprite * glow = atlas_get(res.hud, blue ? "hud_rock_tube_glow_full_b" : "hud_rock_tube_glow_full");
    for (int i = 0; i < 6; i++) {
        float th = (150 - i * 24) * pi / 180, r = 66;
        float x = cx + r * cosf(th), y = cy - r * sinf(th), rot = pi / 2 - th;
        sprite_rot(tube, x, y, 21, 42, tube->w / 2.0f, (float)tube->h, rot, WHITE);
        float fill = clamp01(G.sp * 6 - i) * pulse;
        if (fill > 0) {
            gfx_blend(BLEND_ADD);
            sprite_rot(glow, x, y, 21, 42, glow->w / 2.0f, (float)glow->h, rot, fade_alpha(WHITE, fill));
            gfx_blend(BLEND_ALPHA);
        }
    }
    // the score counter the dome stands on, the streak plate under it
    const float sk = 0.72f, sx = dx, sy = dy + 199 * k - 67 * sk;
    hud_at("hud_rm_solo_bottom", sx, sy, sk, sk, 0, 0, 256, 128, WHITE);
    lcd("lcd_green", (long)G.score, 7, sx + 232 * sk, sy + 96 * sk - 14, 28);
    const float kx = 0.66f, ky = 0.46f, px = cx - 128 * kx, py = sy + 124 * sk - 21 * ky;
    hud_at("hud_counter_body", px, py, kx, ky, 0, 0, 256, 128, WHITE);
    lcd("lcd_orange", G.streak, 4, px + 205 * kx, py + 62 * ky - 10, 20);
    // the dome: its window red, yellow or green, the needle swinging from red to green
    float h = G.rock;
    hud_at("hud_rock_body", dx, dy, k, k, 0, 0, 256, 256, WHITE);
    float w0 = clamp01((0.40f - h) / 0.1f), w2 = clamp01((h - 0.60f) / 0.1f), w1 = clamp01(1 - w0 - w2);
    if (h < 0.25f)
        w0 *= 0.65f + 0.35f * fabsf(sinf(t * 6));        // failing: the red throbs
    if (w0 > 0)
        hud_at("hud_rock_bg_red", dx, dy, k, k, 0, 0, 256, 256, fade_alpha(WHITE, w0));
    if (w1 > 0)
        hud_at("hud_rock_bg_yellow", dx, dy, k, k, 0, 0, 256, 256, fade_alpha(WHITE, w1));
    if (w2 > 0)
        hud_at("hud_rock_bg_green", dx, dy, k, k, 0, 0, 256, 256, fade_alpha(WHITE, w2));
    hud_at("hud_rock_lights_all", dx, dy, k, k, 0, 0, 256, 128, WHITE);
    const Sprite * needle = atlas_get(res.hud, "hud_rock_needle");
    // shorter than the sprite's own length and a narrower swing, so it stays inside the dome, and weighted: it eases to the
    // value instead of snapping with every note
    G.needle += (G.rock - G.needle) * 0.1f;
    float swing = (G.needle - 0.5f) * 2 * 0.85f;
    sprite_rot(needle, dx + 128 * k, dy + 172 * k, 13 * k, 84 * k, needle->w / 2.0f, (float)needle->h, swing, WHITE);
    if (G.vocal)
        goto after_rail;
    // the multiplier on the right rail, its five streak lamps on a gold seat along the rail (two notes a lamp)
    float ax, ay, bx, by;
    project(-HALF_W - 0.22f, 0, 3.3f, &ax, &ay);
    project(-HALF_W - 0.22f, 0, 1.0f, &bx, &by);
    const Sprite * seat = atlas_get(res.hud, "multi_light_seat");
    float seat_len = hypotf(bx - ax, by - ay), rot = atan2f(-(bx - ax), by - ay);
    sprite_rot(seat, (ax + bx) / 2, (ay + by) / 2, 22, seat_len * 128 / 98, seat->w / 2.0f, 61, rot, WHITE);
    int m = multiplier(), base_m = 1 + G.streak / 10, max_m = G.part_id == PART_BASS ? 6 : 4;
    int lit = base_m >= max_m ? 5 : (G.streak % 10) / 2;
    float size = seat_len / 5 * 1.25f;
    for (int i = 0; i < 5; i++) {
        float f = (i + 0.5f) / 5;
        float lx = bx + (ax - bx) * f, ly = by + (ay - by) * f;
        const char * name = i < lit ? (G.sp_active ? "hud_score_light_2_blue" : "hud_score_light_2")
                                    : (G.sp_active ? "hud_score_light_0_blue" : "hud_score_light_0");
        gfx_sprite(atlas_get(res.hud, name), lx - size * 0.2f, ly - size / 2, size, size, WHITE);
    }
    if (m > 1) {
        char name[16];
        snprintf(name, sizeof(name), "mult_x%d", m);
        gfx_sprite(atlas_get(res.game, name), ax - 14, ay - 44, 84, 42, G.sp_active ? RGBA(170, 225, 255, 255) : WHITE);
    }
after_rail:;
    if (G.drums && G.t < 4.5) {
        // which thumb goes where
        char hint[80];
        snprintf(hint, sizeof(hint), "Kick: %s   Star Power: %s + %s, or Select", cfg.drum_layout ? "R" : "L, R, X or Down",
                 cfg.drum_layout ? "D-pad" : "Up", G.lanes == 4 ? "Circle" : "Square");
        font_draw_shadow(res.ui, hint, SCREEN_W / 2, 164, 22, RGBA(210, 170, 255, 255), ALIGN_CENTER);
    }
    if (G.t < 4.5 && !G.vocal)
        draw_keys();
    if (G.drums && G.t >= 4.5) {
        double left = gap_left();
        if (left > 0.4 && left < 1e6)
            font_draw_shadow(res.ui, "FREESTYLE", SCREEN_W / 2, 124, 30,
                             with_alpha(RGBA(120, 255, 170, 255), left < 0.8 ? (float)(left - 0.4) / 0.4f : 1), ALIGN_CENTER);
    }
    // song progress
    double len = G.song->length_ms / 1000.0;
    if (len > 0) {
        float f = (float)(G.t / len);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        gfx_rect(0, 0, SCREEN_W, 4, RGBA(0, 0, 0, 120));
        gfx_rect(0, 0, SCREEN_W * f, 4, RGBA(255, 200, 60, 200));
    }
    // the song's card at the start
    if (G.t < 4.5 && !G.sing) {
        float a = G.t < 3.5 ? 1 : (float)(4.5 - G.t);
        if (a > 1)
            a = 1;
        gfx_rect_grad(0, 40, 520, 110, RGBA(0, 0, 0, (int)(200 * a)), RGBA(0, 0, 0, (int)(120 * a)));
        font_draw_fit(res.title, G.song->name, 24, 44, 52, 480, with_alpha(RGBA(255, 210, 90, 255), a), ALIGN_LEFT);
        font_draw_fit(res.ui, G.song->artist, 26, 100, 28, 480, with_alpha(WHITE, a), ALIGN_LEFT);
        char t[96];
        if (G.venue_title)
            snprintf(t, sizeof(t), "%s  %s   at %s", part_name(G.part_id), diff_name(G.diff), G.venue_title);
        else
            snprintf(t, sizeof(t), "%s  %s", part_name(G.part_id), diff_name(G.diff));
        font_draw(res.ui, t, 26, 126, 20, with_alpha(RGBA(200, 200, 200, 255), a), ALIGN_LEFT);
    }
    // messages
    if (G.popup[0] && G.t - G.popup_t < 2.0) {
        float a = (float)(G.t - G.popup_t);
        float alpha = a < 1.5f ? 1 : (2.0f - a) / 0.5f;
        float sz = 44 + (a < 0.15f ? (0.15f - a) * 80 : 0);
        font_draw_shadow(res.title, G.popup, SCREEN_W / 2, 150, sz, with_alpha(RGBA(255, 225, 120, 255), alpha),
                         ALIGN_CENTER);
    }
    // a solo in progress
    const Part * p = G.part;
    for (int k = 0; k < p->solo_count; k++) {
        if (G.t >= p->solos[k].start / 1e6 && G.t < p->solos[k].end / 1e6) {
            int total = 0, before = 0;
            for (int i = 0; i < p->note_count; i++)
                if (G.solo[i] == k) {
                    total++;
                    if (p->notes[i].time / 1e6 < G.t - WINDOW)
                        before++;
                }
            char t[32];
            snprintf(t, sizeof(t), "SOLO! %d%%", before ? G.solo_hit[k] * 100 / before : 100);
            font_draw_shadow(res.title, t, SCREEN_W / 2 + 230, 210, 40, RGBA(255, 120, 60, 255), ALIGN_CENTER);
        }
    }
    if (G.autoplay)
        font_draw_shadow(res.ui, "BOT", SCREEN_W - 70, 12, 24, RGBA(255, 255, 255, 200), ALIGN_LEFT);
    if (practice.on) {
        char t[32];
        snprintf(t, sizeof(t), "PRACTICE  %d%%", (int)(practice.speed * 100 + 0.5f));
        font_draw_shadow(res.ui, t, SCREEN_W - 16, 44, 22, RGBA(255, 225, 150, 255), ALIGN_RIGHT);
    }
    if (cfg.show_fps) {
        char t[32];
        snprintf(t, sizeof(t), "%d fps", G.fps);
        font_draw_shadow(res.ui, t, SCREEN_W - 10, SCREEN_H - 30, 20, WHITE, ALIGN_RIGHT);
    }
}

void game_draw(void)
{
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_stage();
    if (G.sp_flash > 0.02f)
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(80, 160, 255, (int)(G.sp_flash * 70)));
    if (!G.vocal) {
        gfx_3d(G.proj, G.view);
        draw_highway();
        if (G.drums)
            draw_zones();
        draw_sustains();
        draw_cups(false);
        draw_gems();
        draw_cups(true);
        draw_flames();
    }
    draw_hud();
    if (G.sing)
        vox_draw(G.t);
    gfx_end();
}
