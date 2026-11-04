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
#include "whammybar.h"

#define WINDOW 0.095            // seconds early or late a note can still be hit (Warriors of Rock-ish lenient)
#define ANCHOR_GRACE 0.15       // held frets play a note only for this long after a finger last moved (the anchor
                                // is for fingers arriving early or late, not for keys held through the song)
#define Z_FAR 11.0f             // the highway's far end (the strikeline is at 0)
#define Z_NEAR -3.4f
#define HALF_W 2.62f            // half the highway's width (lanes are 1 wide)
#define MAX_SUS 8
#define MAX_ZONES 64
#define MAX_ROLLS 512
#define SP_PER_QUARTER (1.0f / 32)
#define SUS_DROP_GRACE 0.12
#define SUS_SLACK 0.09
#define HIT_LOG 16
#define SPARKS 40
#define FAIL_FRAMES 75

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
    double lost_t;              // when the frets came up, -1 while they're down
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
    float walk, front;          // metres along the stage they've wandered, 1 out front for a solo
    float turn;                 // walking, a standee shows its edge a little
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
    uint8_t * roll;                     // gems the hits before them carry: drum rolls, and tremolos
    Zone rolls[MAX_ROLLS], gaps[MAX_ZONES];     // where the highway marks them
    int roll_count, gap_count;
    double pad_t[6];                    // when each pad was last hit
    int8_t * roll_mate;                 // a trade roll's other pad, which keeps it going as well
    double slip_t;                      // the last stray press
    bool run_told;                      // the first tremolo says what to do
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
    float whammy, sp_from_bar;
    Bar bar;
    float bend;                         // how far the whammy bends the part's stem
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
    struct { float x, y, z, vx, vy, vz, life; uint32_t col; } spark[SPARKS];
    int spark_next;
    int count_done;
    int fail_frames;                    // the power-down after a failed song
    int sus_dropped, camera, mult_seen;
    bool big_section;                   // a chorus or a solo: the band goes harder
    float mult_pop;                     // the multiplier jumps as it goes up
    double press_t[6];                  // when each lane last went down
    double act_t;                       // when any finger last moved: held frets anchor only for a while after this
    float hit_log[HIT_LOG];             // the last hits, seconds off the note
    int hit_log_n;
    int section, section_pal;           // the chart's section now, and its colours
    double pyro_t;                      // when the jets along the stage last went off
    float cam_x, cam_y, cam_z;          // the camera's drift
    float cam_tx, cam_ty, cam_tz;       // where it's gliding, before the drift
    int focus;                          // the member it's closing in on (-1: the whole stage)
    float key_col[3];                   // the lights' colour at full strength
    Tex * art;                          // the song's cover, for the stage's screens
    double party_t;                     // when the confetti last came down
    bool end_party;
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

static const char * forced_venue;

void game_force_venue(const char * zone)
{
    forced_venue = zone;
}

// the song's venue: each song always plays the same one (like YargLook's pick), or as the options say
static void pick_venue(const Song * song)
{
    G.venue = G.venue_lights = NULL;
    G.venue_title = NULL;
    G.marks = NULL;
    if (!res.venue_count || (!forced_venue && strcmp(cfg.venue, "classic") == 0))
        return;
    const char * zone = NULL;
    uint32_t h = 2166136261u;
    for (const char * c = song->artist; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    h = (h ^ '|') * 16777619u;
    for (const char * c = song->name; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    if (forced_venue)
        zone = forced_venue;
    else if (strcmp(cfg.venue, "song") == 0) {
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
        G.stage_col[i] = G.light_col[i] = G.key_col[i] = 0.8f;
    G.show_t = -1e9;
}

static void draw_band(double beat);
static uint32_t with_alpha(uint32_t c, float a);
static uint32_t col3(const float c[3], float a);
static void sprite_rot(const Sprite * s, float x, float y, float w, float h, float px, float py, float angle,
                       uint32_t col);

static void party(void)
{
    if (G.t - G.party_t > 1.5)
        G.party_t = G.t;
}

// where a member really stands, in the venue picture's pixels: x, y of the feet, pixels per metre. The venues' own marks
// can be off the screen or behind the rock meter, so they're brought in to where they're seen. The singer's is behind
// the highway: beside the kit they can be seen
static void member_spot(int r, float m[3])
{
    m[0] = G.marks[r][0];
    m[1] = G.marks[r][1];
    m[2] = G.marks[r][2];
    if (r == 1) {
        // the singer stands forward of the kit, clear of the far end of the board
        m[0] = fminf(G.marks[0][0] - G.marks[0][2] * 1.35f, 322);
        m[1] = fmaxf(G.marks[0][1] + 24, 262);
        m[2] = G.marks[0][2] * 1.25f;
    }
    if (r < 2)
        return;
    m[0] = r == 3 ? fmaxf(m[0] + 34, 244) : fminf(m[0], 850);
    m[1] = fminf(m[1], 318);
    m[2] = fminf(m[2], 118) * 1.1f;
    const BandPart * b = &G.band_part[r];
    m[0] += b->walk * m[2];
    m[1] += b->front * 26;
    m[2] *= 1 + b->front * 0.2f;
    // wherever they wander, out of the way of the board and the rock meter
    m[0] = r == 3 ? fminf(fmaxf(m[0], 222), 262) : fminf(fmaxf(m[0], 738), 858);
}

// a guitarist out in a solo of their own part
static bool band_soloing(int r)
{
    const Part * p = G.band_part[r].part;
    for (int k = 0; p && k < p->solo_count; k++)
        if (p->solos[k].start / 1e6 <= G.t && G.t < p->solos[k].end / 1e6)
            return true;
    return false;
}

// they don't stand on one mark: a new spot every four bars, walking there, and out to the front for their solos
static void band_wander(double beat)
{
    for (int r = 2; r < 4; r++) {
        BandPart * b = &G.band_part[r];
        uint32_t h = ((uint32_t)(beat / 16) + 1) * 2654435761u * (r * 2 + 1);
        float want = beat > 0 ? ((h >> 16 & 255) / 255.0f - 0.5f) * 1.3f : 0;
        float step = want - b->walk;
        float speed = 0.018f;
        b->walk += step > speed ? speed : step < -speed ? -speed : step;
        float out = band_soloing(r) ? 1 : 0;
        b->front += (out - b->front) * 0.06f;
    }
}

static int member_me(void)
{
    return G.part_id == PART_DRUMS ? 0 : G.part_id == PART_VOCALS ? 1 : G.part_id == PART_BASS ? 3 : 2;
}

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
        G.big_section = big;
        static const int cool[] = {1, 3, 4}, warm[] = {0, 2, 5};
        G.section_pal = (big ? warm : cool)[h % 3];
        if (strstr(low, "chorus") && G.rock >= 0.5f) {
            G.pyro_t = G.t;
            party();
        }
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
            float m[3];
            member_spot(r, m);
            float x = m[0] + side * m[2] * 0.75f, y = m[1], w = 64 * m[2] / 110;
            stage_point(&x, &y);
            gfx_sprite_uv(flame, x - w / 2, y - w * 2.6f, w, w * 2.6f, u0, v0, u0 + 0.25f, v0 + 0.25f, WHITE);
        }
    gfx_blend(BLEND_ALPHA);
}

// the rig over the stage: a truss of moving lamps, whose beams dance through the song's patterns, one always on whoever
// the camera's on
static void draw_rig(double beat, float pulse)
{
    enum { LAMPS = 6 };
    const Sprite * truss = atlas_get(res.stage, "truss"), * lamp = atlas_get(res.stage, "lamp");
    const Sprite * beam = atlas_get(res.stage, "beam");
    float lamp_x[LAMPS], lamp_y[LAMPS];
    for (int i = -1; i < 5; i++) {
        float x = i * 256.0f, y = 6;
        stage_point(&x, &y);
        gfx_sprite(truss, x, y, 256 * G.cam_z, 30 * G.cam_z, col3(G.stage_col, 1));
    }
    for (int i = 0; i < LAMPS; i++) {
        lamp_x[i] = 90 + i * 156.0f;
        lamp_y[i] = 24;
        stage_point(&lamp_x[i], &lamp_y[i]);
    }
    float tx = 480, ty = 300;
    if (G.focus >= 0) {
        float m[3];
        member_spot(G.focus, m);
        tx = m[0];
        ty = m[1];
        stage_point(&tx, &ty);
    }
    int spot = (int)((tx - 12) / 156 + 0.5f);
    spot = spot < 0 ? 0 : spot >= LAMPS ? LAMPS - 1 : spot;
    int pattern = (int)floor(beat / 16) & 3;
    float swing = sinf((float)beat * 3.14159265f / 4), level = 0.2f + 0.26f * G.band_loud + 0.26f * pulse * G.band_loud;
    if (G.rock < 0.33f && !G.sp_active)
        level *= 0.6f;
    if (G.sp_active)
        level = 0.42f + 0.3f * pulse;
    gfx_blend(BLEND_ADD);
    for (int i = 0; i < LAMPS; i++) {
        float from = (i - 2.5f) * 0.17f, angle;
        if (pattern == 1)
            angle = (i & 1 ? 0.34f : -0.34f) * swing;
        else if (pattern == 2)
            angle = -from * 1.6f + 0.18f * swing;
        else if (pattern == 3)
            angle = atan2f(-(480 - lamp_x[i]), 300) + 0.12f * swing * (i & 1 ? 1 : -1);
        else
            angle = from + 0.3f * swing;
        float a = level;
        if (G.focus >= 0 && i == spot) {
            // the beam hangs straight down at zero, so the angle is from +y, not from +x
            angle = atan2f(-(tx - lamp_x[i]), ty - lamp_y[i]);
            a = 0.5f + 0.3f * pulse;
        }
        sprite_rot(beam, lamp_x[i], lamp_y[i], 200 * G.cam_z, 450 * G.cam_z, beam->w / 2.0f, 0, angle,
                   col3(G.key_col, a));
    }
    gfx_blend(BLEND_ALPHA);
    for (int i = 0; i < LAMPS; i++)
        gfx_sprite(lamp, lamp_x[i] - 15 * G.cam_z, lamp_y[i] - 18 * G.cam_z, 30 * G.cam_z, 36 * G.cam_z,
                   col3(G.key_col, 1));
    // the lamps flare with the kick
    const Sprite * dot = atlas_get(res.stage, "dot");
    float size = (46 + 54 * pulse * G.band_loud) * G.cam_z;
    gfx_blend(BLEND_ADD);
    for (int i = 0; i < LAMPS; i++)
        gfx_sprite(dot, lamp_x[i] - size / 2, lamp_y[i] - size / 2 + 8, size, size, col3(G.key_col, 0.35f + 0.5f * pulse));
    gfx_blend(BLEND_ALPHA);
}

// the air the lights go through
static void draw_haze(void)
{
    const Sprite * haze = atlas_get(res.stage, "haze");
    float t = (float)G.t, a = 0.1f + 0.1f * G.band_loud;
    gfx_blend(BLEND_ADD);
    gfx_sprite(haze, -260 + sinf(t * 0.11f) * 170, 130, 1480, 170, col3(G.key_col, a));
    gfx_sprite(haze, -240 - sinf(t * 0.07f + 2) * 200, 215, 1440, 130, col3(G.key_col, a * 0.8f));
    gfx_blend(BLEND_ALPHA);
}

// two screens hanging from the truss with the song's cover, or the music's bars where it has none
static void draw_screens(float pulse)
{
    const Sprite * dot = atlas_get(res.stage, "dot");
    for (int side = 0; side < 2; side++) {
        // nearer than the stage, so the camera's moves only nudge them and they stay on the screen
        float w = 88, x = (side ? 860 : 12) + G.cam_x * 0.25f, y = 30 + G.cam_y * 0.25f;
        gfx_blend(BLEND_ADD);
        gfx_sprite(dot, x - w * 0.5f, y - w * 0.5f, w * 2, w * 2, col3(G.key_col, 0.14f + 0.14f * pulse));
        gfx_blend(BLEND_ALPHA);
        gfx_rect(x + w * 0.15f, y - 30, 3, 32, RGBA(30, 28, 38, 255));
        gfx_rect(x + w * 0.85f, y - 30, 3, 32, RGBA(30, 28, 38, 255));
        gfx_rect(x - 5, y - 5, w + 10, w + 10, RGBA(14, 9, 24, 255));
        if (G.art) {
            const Sprite art = sprite_whole(G.art);
            int v = 190 + (int)(65 * pulse);
            gfx_sprite(&art, x, y, w, w, RGBA(v, v, v, 255));
            continue;
        }
        for (int i = 0; i < 8; i++) {
            float wob = 0.5f + 0.5f * sinf((float)G.t * (3 + i * 1.3f) + i * 2.1f + side);
            float h = w * (0.12f + 0.84f * wob * (0.25f + 0.75f * G.band_loud));
            gfx_rect(x + w * (i + 0.12f) / 8, y + w - h, w * 0.76f / 8, h, col3(G.key_col, 1));
        }
    }
}

// the crowd down front, seen from behind: everyone keeps their own time, and the better it goes the higher they jump and
// the more hands go up
// the edges of the picture fall away, so the eye stays on the highway and the band, more under Star Power's flood
static void vignette(void)
{
    gfx_tex(NULL);
    gfx_blend(BLEND_ALPHA);
    int a = G.sp_active ? 170 : 125;
    uint32_t edge = RGBA(4, 2, 10, a), none = RGBA(4, 2, 10, 0);
    const float w = SCREEN_W, h = SCREEN_H, in = w * 0.2f;
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const float l[4][3] = {{0, 0, 0}, {in, 0, 0}, {in, h, 0}, {0, h, 0}};
    const uint32_t lc[4] = {edge, none, none, edge};
    gfx_quad(l, uv, lc);
    const float r[4][3] = {{w - in, 0, 0}, {w, 0, 0}, {w, h, 0}, {w - in, h, 0}};
    const uint32_t rc[4] = {none, edge, edge, none};
    gfx_quad(r, uv, rc);
    const float top[4][3] = {{0, 0, 0}, {w, 0, 0}, {w, 100, 0}, {0, 100, 0}};
    const uint32_t tc[4] = {edge, edge, none, none};
    gfx_quad(top, uv, tc);
}

// World Tour's own sheet of raised hands, four rows of it, three depths across the foot of the picture. Dark against
// the lights, more of them and higher the better it's going; they jump on the beat
static void draw_crowd(double beat)
{
    Tex * t = res.hands;
    if (!t || cfg.no_crowd)
        return;
    float lit[3];
    float mood = G.rock < 0.33f ? 0.2f : 0.5f + G.rock * 0.5f;
    if (G.sp_active)
        mood = 1;
    gfx_tex(t);
    gfx_blend(BLEND_ALPHA);
    for (int row = 0; row < 3; row++) {
        float k = 0.4f + row * 0.3f;        // nearer rows are darker: the light's behind them
        for (int i = 0; i < 3; i++)
            lit[i] = (0.03f + G.stage_col[i] * 0.05f + G.key_col[i] * 0.14f) * (1.1f - k);
        uint32_t col = col3(lit, 1);
        float bounce = fabsf(sinf(3.14159265f * ((float)beat + row * 0.3f)));
        float h = 112 + row * 22, top = 544 - h * (0.62f + 0.38f * mood) + 14 * row - bounce * 14 * mood;
        int sheet = ((int)floor(beat * 0.5) + row) & 3;
        float x = -40 + sinf((float)G.t * (0.3f + row * 0.1f) + row) * 16 + row * 90.0f;
        const float wid = 1100;
        float v0 = sheet * 0.25f + 0.01f, v1 = v0 + 0.23f;
        const float p[4][3] = {{x, top, 0}, {x + wid, top, 0}, {x + wid, top + h, 0}, {x, top + h, 0}};
        const float uv[4][2] = {{0, v0}, {1, v0}, {1, v1}, {0, v1}};
        const uint32_t c[4] = {col, col, col, col};
        gfx_quad(p, uv, c);
    }
}

static void draw_party(void)
{
    double since = G.t - G.party_t;
    if (since < 0 || since > 3)
        return;
    static const uint32_t cols[5] = {RGBA(255, 214, 90, 255), RGBA(255, 96, 190, 255), RGBA(100, 225, 255, 255),
                                     RGBA(176, 120, 255, 255), RGBA(255, 255, 255, 255)};
    const Sprite * star = atlas_get(res.stage, "star"), * bit = atlas_get(res.stage, "confetti");
    uint32_t seed = (uint32_t)(G.party_t * 16);
    float fade_out = since > 2.3 ? (float)(3 - since) / 0.7f : 1;
    for (int i = 0; i < 56; i++) {
        uint32_t h = (seed + i) * 2654435761u;
        float tau = (float)since - (float)(h >> 8 & 63) / 100;
        if (tau < 0)
            continue;
        float x = (float)(h >> 14 & 1023) * 0.95f + sinf(tau * 3 + i) * 24;
        float y = -24 + tau * (170 + (h >> 24 & 127));
        float size = i % 3 == 0 ? 22 : 15;
        sprite_rot(i % 3 == 0 ? star : bit, x, y, size, size, (i % 3 == 0 ? star : bit)->w / 2.0f,
                   (i % 3 == 0 ? star : bit)->h / 2.0f, tau * (2.5f + i % 5) + i, with_alpha(cols[i % 5], fade_out));
    }
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
    for (int i = 0; i < 3; i++)
        G.key_col[i] += (key[i] - G.key_col[i]) * k;
    // the camera: the whole stage, and every other four bars closing in on one of the band as a director would, whoever
    // has the solo first. They stay where they are on the screen and grow, so nobody slides behind the highway
    static const int turns[4] = {2, 1, 3, 0};
    int slot = (int)floor(beat / 16);
    G.focus = -1;
    if (solo)
        G.focus = member_me();
    else if (!G.sp_active && G.t > 0 && (slot & 1))
        G.focus = turns[(slot / 2) & 3];
    if (G.focus >= 0 && !G.band[G.focus])
        G.focus = -1;
    float zoom = 1.04f + 0.012f * G.band_loud, aim_x = 0, aim_y = 0;
    if (G.focus >= 0) {
        float m[3];
        member_spot(G.focus, m);
        zoom = 1.2f;
        aim_x = (480 - m[0]) * (zoom - 1);
        aim_y = (272 - (m[1] - m[2])) * (zoom - 1);
        float rx = 480 * (zoom - 1) - 14, ry = 272 * (zoom - 1) - 8;
        aim_x = aim_x > rx ? rx : aim_x < -rx ? -rx : aim_x;
        aim_y = aim_y > ry ? ry : aim_y < -ry ? -ry : aim_y;
    }
    float glide = 1 - expf((float)-dt * 1.4f);
    G.cam_tz += (zoom - G.cam_tz) * glide;
    G.cam_tx += (aim_x - G.cam_tx) * glide;
    G.cam_ty += (aim_y - G.cam_ty) * glide;
    G.cam_z = G.cam_tz;
    G.cam_x = G.cam_tx + sinf((float)G.t * 0.23f) * 13;
    G.cam_y = G.cam_ty + sinf((float)G.t * 0.17f + 1) * 6;
    if (G.t > G.last_note_end && G.t > 0 && !G.end_party) {
        G.end_party = true;
        party();
        sound_play(res.snd[SND_APPLAUSE], cfg.crowd_vol * 0.1f, false);
    }
    stage_layer(G.venue, col3(G.stage_col, 1));
    // a whole screen of blending: not when it would add nothing
    if (G.venue_lights && fmaxf(G.light_col[0], fmaxf(G.light_col[1], G.light_col[2])) > 0.02f) {
        gfx_blend(BLEND_ADD);
        stage_layer(G.venue_lights, col3(G.light_col, 0.72f));
        gfx_blend(BLEND_ALPHA);
    }
    if (cfg.stage_screens)
        draw_screens(pulse);
    draw_haze();
    draw_band(beat);
    draw_rig(beat, pulse);
    draw_pyro();
    draw_crowd(beat);
    draw_party();
    if (!cfg.no_vignette)
        vignette();
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

// A member's cell as a stiff card: the feet stay on their mark (and the kit under the drummer), and from there up
// the body leans by `lean` and dips by `dip` (pixels, at the top of the cell), more the higher up: a spine and a pair of
// knees, for four more quads than a flat picture.
static void band_strip(Tex * t, float x, float bottom, float w, float h, const float uv[4], float lean, float dip,
                       uint32_t tint)
{
    // a stiff card turns about its feet: a true rotation, nothing sheared
    const uint32_t c[4] = {tint, tint, tint, tint};
    float a = h != 0 ? atan2f(lean, fabsf(h)) : 0, cs = cosf(a), sn = sinf(a);
    float top = h - dip, sign = h < 0 ? -1.0f : 1.0f;
    float bl[2] = {-w / 2, 0}, br[2] = {w / 2, 0}, tr[2] = {w / 2, -top}, tl[2] = {-w / 2, -top};
    const float * q[4] = {tl, tr, br, bl};
    float p[4][3];
    for (int i = 0; i < 4; i++) {
        p[i][0] = x + q[i][0] * cs - q[i][1] * sn * sign;
        p[i][1] = bottom + q[i][0] * sn * sign + q[i][1] * cs;
        p[i][2] = 0;
    }
    const float tc[4][2] = {{uv[0], uv[1]}, {uv[2], uv[1]}, {uv[2], uv[3]}, {uv[0], uv[3]}};
    gfx_tex(t);
    gfx_quad(p, tc, c);
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
    float mood = (G.rock < 0.33f ? 0.3f : 0.6f + G.rock * 0.9f) * (G.big_section ? 1.35f : 1);
    float bob = (1 - on_beat) * (1 - on_beat) * (0.04f + 0.14f * G.band_loud) * mood;   // down on the beat
    float sway = sinf((float)(beat * speed) * pi * 0.5f) * (0.05f + 0.13f * G.band_loud) * mood;
    float slump = G.rock < 0.33f ? (0.33f - G.rock) * 0.5f : 0;   // losing the crowd, they sag
    // and the last chord rings with everybody in the air
    float hop = G.sp_active || G.cheer > 0.25f || (G.t > G.last_note_end && G.band_loud > 0.03f) ? on_beat * 0.3f : 0;
    band_wander(beat);
    int me = member_me();
    float lit[3];
    for (int i = 0; i < 3; i++)
        lit[i] = 0.2f + G.stage_col[i] * 1.05f;
    uint32_t col = col3(lit, 1);
    // their shadows on the floor, and a pool of light under whoever the camera's on
    const Sprite * dot = atlas_get(res.stage, "dot");
    for (int r = 0; r < 4; r++) {
        if (!G.band[r])
            continue;
        float m[3];
        member_spot(r, m);
        stage_point(&m[0], &m[1]);
        float w = m[2] * G.cam_z * (r ? 1.25f : 2.6f), h = w * 0.26f;
        if (r == G.focus) {
            gfx_blend(BLEND_ADD);
            gfx_sprite(dot, m[0] - w * 0.9f, m[1] - h * 0.9f, w * 1.8f, h * 1.8f, col3(G.key_col, 0.6f));
            gfx_blend(BLEND_ALPHA);
        }
        gfx_sprite(dot, m[0] - w / 2, m[1] - h / 2, w, h, RGBA(6, 3, 12, 190));
    }
    for (int i = 0; i < 4; i++) {
        int r = order[i];
        Tex * t = G.band[r];
        if (!t)
            continue;
        float m[3];
        member_spot(r, m);
        // cell size, pixels per metre and the cell's bottom edge in metres, as in tools/band.py
        float cw = r == 0 ? 266 : 170, ch = r == 0 ? 232 : 256, ppm = r == 0 ? 88 : 120, y0 = r == 0 ? -0.62f : -0.08f;
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
        float s = m[2] / ppm, w = cw * s * (1 - 0.16f * bp->turn), h = ch * s;
        // the kit stays where it stands; what's on their feet moves from the knees up, and a jump leaves the stage
        float bottom = m[1] - y0 * m[2] - (r ? hop : 0) * m[2];
        float dip = (r ? bob * (playing ? 1 : 0.5f) + bp->pulse * 0.05f : bob * 0.3f + bp->pulse * 0.035f) * m[2] +
                    slump * m[2];
        float lean = (r ? (i & 1 ? sway : -sway) * (playing ? 1 : 0.4f) : 0) * m[2];
        if (r >= 2) {
            // walking: the weight goes foot to foot. In a solo they rock back with it
            float going = fabsf(bp->walk - (((((uint32_t)(beat / 16) + 1) * 2654435761u * (r * 2 + 1)) >> 16 & 255) / 255.0f - 0.5f) * 1.3f) > 0.03f ? 1 : 0;
            bp->turn += (going - bp->turn) * 0.12f;
            dip += going * fabsf(sinf((float)G.t * 9)) * 0.05f * m[2];
            lean += going * sinf((float)G.t * 9) * 0.035f * m[2];
            lean += bp->front * (i & 1 ? 0.14f : -0.14f) * m[2];
        }
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
        } else {
            // a pool of the stage's light at their feet, the one the camera's on brighter, and the cut-out's own shape
            // lying on the floor in front of it
            const Sprite * pool = atlas_get(res.stage, "dot");
            float pw = w * 1.5f, ph = pw * 0.26f;
            gfx_blend(BLEND_ADD);
            gfx_sprite(pool, m[0] - pw / 2, bottom - ph / 2, pw, ph, col3(G.key_col, G.focus == r ? 0.75f : 0.3f + 0.2f * G.band_loud));
            gfx_blend(BLEND_ALPHA);
            band_frame(t, f, cw, ch, m[0], bottom, w, -h * 0.18f, -lean * 0.18f, 0, RGBA(6, 3, 12, 120));
            band_frame(t, f, cw, ch, m[0], bottom, w, h, lean, dip, tint);
        }
        // Star Power: they burn with it
        if (G.sp_active) {
            gfx_blend(BLEND_ADD);
            band_frame(t, r == 1 ? (int)(phase * 32) % 32 : f, cw, ch, m[0], bottom, w, h, lean, dip,
                       RGBA(90, 150, 255, 70 + (int)(90 * on_beat)));
            gfx_blend(BLEND_ALPHA);
        }
    }
}

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static void popup(const char * text)
{
    if (cfg.no_popups)
        return;
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
    // World Tour's, then lower and nearer for bigger gems, then high up to read further ahead
    static const float eyes[3][3] = {{0, 4.7f, -6.5f}, {0, 4.0f, -5.5f}, {0, 5.7f, -6.9f}};
    static const float ats[3] = {4.6f, 3.9f, 5.4f};
    G.camera = cfg.camera;
    const float * eye = eyes[cfg.camera], at[3] = {0, 0, ats[cfg.camera]}, up[3] = {0, 1, 0};
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
#define ROLL_TRADE 6                    // gems trading between two pads this fast are a roll as well
#define SPAM_GAP 0.12                   // the same frets again this soon: no thumb keeps up, it's a tremolo
#define SPAM_RUN 4                      // that many in a row
#define SPAM_HOLD 0.2                   // a tremolo goes on while its frets were hit this recently
#define SPAM_AFTER 0.3                  // hits this soon after a tremolo are still the tremolo
#define EARLY_GRACE 0.18                // a press this soon before its lane's note is that note, mistimed
#define SLIP_RUN 1.0                    // one stray press is a finger slipping, a second this soon is mashing

// songs converted before the kick had the open bit keep it in bit 7, and their charts are gone
static uint8_t drum_frets(const Note * n)
{
    return n->frets & 0x80 ? (n->frets & 0x1f) | FRET_OPEN : n->frets;
}

// as YARG lays four pads on five: cymbals on yellow and orange, the toms on blue and green. A chart that says
// nothing of toms and cymbals (plain) keeps its pads on their own colours, green up to the fifth lane
static uint8_t five_pads(uint8_t frets, uint8_t flags, bool plain)
{
    if (plain)
        return (uint8_t)((frets & ~0x08u) | (frets & 0x08 ? 0x10 : 0));
    uint8_t out = frets & (1 | FRET_OPEN);
    bool yt = (frets & 2) && (flags & NOTE_TOM_Y), bt = (frets & 4) && (flags & NOTE_TOM_B);
    bool gt = (frets & 8) && (flags & NOTE_TOM_G), yc = (frets & 2) && !yt, bc = (frets & 4) && !bt, gc = (frets & 8) && !gt;
    if (yc)
        out |= 2;                       // the yellow cymbal on the yellow
    if (yt)
        out |= out & 4 ? 16 : 4;        // the yellow tom moves to blue's lane, or green's when blue's taken
    if (bc)
        out |= 8;                       // the blue cymbal lives on the orange
    if (bt)
        out |= out & 4 ? 16 : 4;        // the blue tom on blue, or green's lane
    if (gt)
        out |= out & 16 ? 4 : 16;       // the green tom on green, or blue's lane
    if (gc)
        out |= out & 8 ? 16 : 8;        // the green cymbal on the orange, or green's own lane
    return out;
}

static void split_drums(void)
{
    const Part * p = G.part;
    int total = 0, used = 0;
    bool five = false, plain = true;        // plain: no note in the chart says tom or cymbal
    for (int i = 0; i < p->note_count; i++) {
        total += frets_count(drum_frets(&p->notes[i]));
        five |= (drum_frets(&p->notes[i]) & 0x10) != 0;
        plain &= (p->notes[i].flags & (NOTE_TOM_Y | NOTE_TOM_B | NOTE_TOM_G)) == 0;
    }
    // World Tour's kit from a 4-pad chart
    bool to_five = cfg.five_pads && !five;
    G.drum_notes = calloc(total + 1, sizeof(Note));
    if (!G.drum_notes)
        return;
    for (int i = 0; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        uint8_t frets = drum_frets(n), flags = n->flags;
        if ((flags & NOTE_KICK2) && !cfg.double_kick)
            frets &= ~FRET_OPEN;
        if (to_five) {
            frets = five_pads(frets, flags, plain);
            flags = (flags & ~(NOTE_TOM_Y | NOTE_TOM_G)) | NOTE_TOM_B;
        }
        // a phrase's last gem gone with its kick: the one before ends it
        if (!frets) {
            if ((flags & NOTE_SP_END) && used)
                G.drum_notes[used - 1].flags |= NOTE_SP_END;
            continue;
        }
        int left = frets_count(frets);
        for (int b = 0; b < 6; b++) {
            if (!(frets & (1 << b)))
                continue;
            Note * o = &G.drum_notes[used++];
            *o = *n;
            o->flags = flags;
            o->frets = (uint8_t)(1 << b);
            // the phrase ends once, on its last gem
            if (--left)
                o->flags &= ~NOTE_SP_END;
        }
    }
    G.drums = true;
    G.lanes = to_five ? 5 : 4;
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
    G.roll = calloc(used + 1, 1);
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
                        if (G.drum_notes[k].frets & (1 << b))
                            G.roll[k] = 1;
                    if (G.roll_count < MAX_ROLLS)
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
    // two pads traded faster than thumbs trade them: the same roll, kept going by either pad (the highway marks
    // both of its lanes)
    G.roll_mate = calloc(used + 1, 1);
    if (G.roll_mate)
        memset(G.roll_mate, -1, used + 1);
    for (int i = 0; G.roll && G.roll_mate && i < used;) {
        uint8_t fa = G.drum_notes[i].frets;
        if ((fa & FRET_OPEN) || G.roll[i]) {
            i++;
            continue;
        }
        int la = __builtin_ctz(fa), pb = -1, j = i + 1;
        while (j < used) {
            uint8_t fj = G.drum_notes[j].frets;
            if ((fj & FRET_OPEN) || (G.drum_notes[j].time - G.drum_notes[j - 1].time) / 1e6 > ROLL_GAP)
                break;
            int l = __builtin_ctz(fj);
            if (l == la) {
                if (pb < 0)
                    break;              // back on the first pad: a run on one pad, the lane pass already had it
            } else if (pb < 0)
                pb = l;
            else if (l != pb)
                break;                  // a third pad joins: not a trade
            j++;
        }
        if (j - i >= ROLL_TRADE && pb >= 0) {
            double t0 = G.drum_notes[i].time / 1e6, t1 = G.drum_notes[j - 1].time / 1e6;
            for (int k = i + 1; k < j; k++) {
                G.roll[k] = 1;
                G.roll_mate[k] = (int8_t)(__builtin_ctz(G.drum_notes[k].frets) == la ? pb : la);
            }
            if (G.roll_count + 2 <= MAX_ROLLS) {
                G.rolls[G.roll_count++] = (Zone){t0, t1, la};
                G.rolls[G.roll_count++] = (Zone){t0, t1, pb};
            }
            i = j;
        } else
            i++;
    }
}

// What no thumb can play: the same frets over and over, faster than a button goes. As Rock Band's tremolo lanes, the first
// gem is played and the rest pass while the frets keep being hit at a pace a thumb can keep. It goes by what the chart
// asks of the fingers, so nothing else in a song changes, and every gem here can still be played one by one
static void find_tremolos(void)
{
    const Part * p = G.part;
    G.roll = calloc(p->note_count + 1, 1);
    for (int i = 0, last; G.roll && i < p->note_count; i = last + 1) {
        const Note * n = p->notes;
        last = i;
        while (last + 1 < p->note_count && n[last + 1].frets == n[i].frets &&
               (n[last + 1].time - n[last].time) / 1e6 <= SPAM_GAP)
            last++;
        if (last - i + 1 < SPAM_RUN)
            continue;
        memset(G.roll + i + 1, 1, last - i);
        for (int l = 0; l < 6 && G.roll_count < MAX_ROLLS; l++)
            if (n[i].frets & (1 << l))
                G.rolls[G.roll_count++] = (Zone){n[i].time / 1e6, n[last].time / 1e6, l};
    }
    plat_log("tremolos: %d lanes", G.roll_count);
}

static void free_song(void)
{
    free(G.roll);
    G.roll = NULL;
    free(G.roll_mate);
    G.roll_mate = NULL;
    G.roll_count = G.gap_count = 0;
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
static bool stem_is_the_song(void);

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
    G.hit_log_n = 0;
    for (int i = 0; i < 6; i++)
        G.press_t[i] = -100;
    G.act_t = -100;
    G.sp = 0;
    G.rock = G.needle = 0.5f;
    G.sp_active = G.sp_ready_told = false;
    G.sp_last_q = -1;
    G.failed = G.done = G.paused = false;
    G.fail_frames = 0;
    G.count_done = 0;
    for (int i = 0; i < SPARKS; i++)
        G.spark[i].life = 0;
    G.popup[0] = 0;
    G.miss_flash = G.sp_flash = 0;
    G.hit_pulse = G.miss_pulse = G.cheer = 0;
    G.solo_index = -1;
    G.section = -1;
    for (int l = 0; l < 6; l++)
        G.pad_t[l] = G.judged_t[l] = -100;
    G.slip_t = -100;
    G.run_told = false;
    G.free_hits = 0;
    G.free_t = -100;
    G.pyro_t = -100;
    G.party_t = -100;
    G.end_party = false;
    G.cam_z = G.cam_tz = 1.04f;
    G.cam_tx = G.cam_ty = 0;
    G.focus = -1;
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
        // singing over an instrument is judged at the voice's own difficulty, the instrument's would be
        // Expert just because the hands chose Expert
        vox_start(G.vocal ? G.diff : cfg.sing_diff, G.autoplay);
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
    else if (!G.vocal)
        find_tremolos();
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
    // a lone stem is the whole song, whatever it's called: a miss or the whammy would take all of it
    G.my_stem = song->stem_count > 1 ? song->part_stem[part] : -1;
    // drums often come as a few stems: a miss quiets all of them
    G.drum_stem_count = 0;
    for (int i = 0; G.drums && i < song->stem_count; i++)
        if (!strncmp(song->stems[i], "drums", 5))
            G.drum_stems[G.drum_stem_count++] = i;
    if (G.drum_stem_count == song->stem_count)
        G.drum_stem_count = 0;
    G.vox_stem = song->stem_count > 1 ? song->part_stem[PART_VOCALS] : -1;
    G.speed = 4.0 + cfg.speed * 0.8;
    G.highway = res_highway(cfg.highway);
    pick_venue(song);
    tex_free(G.art);
    G.art = NULL;
    if (G.venue) {
        char art[320];
        snprintf(art, sizeof(art), "%s/album.jpg", song->folder);
        FILE * f = fopen(art, "rb");
        if (f) {
            fclose(f);
            G.art = tex_load(art, TEX_MIPMAP);
        }
    }
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
    tex_free(G.art);
    G.art = NULL;
    plat_set_perf(0);
    game_log_whammy();
}

void game_status(int * song_pct, int * notes_pct)
{
    double end = G.last_note_end > 0 ? G.last_note_end : 1;
    *song_pct = G.t <= 0 ? 0 : G.t >= end ? 100 : (int)(G.t * 100 / end);
    int judged = G.hits + G.misses;
    *notes_pct = judged ? G.hits * 100 / judged : -1;
}

void game_log_whammy(void)
{
    if (G.sp_from_bar > 0)
        plat_log("whammy: %.1f%% of the Star Power meter earned on sustains", G.sp_from_bar * 100);
    G.sp_from_bar = 0;
    plat_log("sustains: %d let go early", G.sus_dropped);
    G.sus_dropped = 0;
    audio_log_cost();
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
    r.misses = G.misses;
    // the pad that let you down
    if (G.drums) {
        static const char * names[6] = {"Red pad", "Yellow pad", "Blue pad", "Orange pad", "Green pad", "Kick"};
        int hit[6] = {0}, all[6] = {0}, worst = 101;
        for (int i = 0; i < G.part->note_count; i++)
            for (int l = 0; l < 6; l++)
                if (G.part->notes[i].frets & (1 << l)) {
                    all[l]++;
                    hit[l] += G.state[i] == HIT;
                }
        for (int l = 0; l < 6; l++) {
            int pct = all[l] >= 10 ? hit[l] * 100 / all[l] : 101, art = l == LANE_OPEN ? 5 : G.lanes == 4 ? (l == 3 ? 4 : l) : l;
            if (pct < worst) {
                worst = pct;
                r.weak_pad = names[art];
            }
        }
        r.weak_pct = worst;
    }
    r.played_s = G.t > 0 ? (int)G.t : 0;
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
    memset(r.strip, 255, sizeof(r.strip));
    const Part * p = G.part;
    if (!p || G.vocal || p->note_count < 2 || !G.state)
        return r;
    // where the notes went, start to end (notes the song never reached aren't counted)
    int got[RESULT_STRIP] = {0}, all[RESULT_STRIP] = {0};
    double first = p->notes[0].time, span = p->notes[p->note_count - 1].time - first;
    for (int i = 0; span > 0 && i < p->note_count; i++) {
        if (G.state[i] == PENDING)
            continue;
        int b = (int)((p->notes[i].time - first) / span * (RESULT_STRIP - 1));
        all[b]++;
        got[b] += G.state[i] == HIT;
    }
    for (int b = 0; b < RESULT_STRIP; b++)
        if (all[b])
            r.strip[b] = (uint8_t)(got[b] * 100 / all[b]);
    // and the song's own sections: a tool's "ugc_section_20" says nothing, so those don't count
    const Chart * c = G.chart;
    r.worst_pct = 101;
    for (int k = 0, i = 0; k < c->section_count; k++) {
        double end = k + 1 < c->section_count ? c->sections[k + 1].time : 1e15;
        int n = 0, hit = 0;
        for (; i < p->note_count && p->notes[i].time < end; i++)
            if (G.state[i] != PENDING && p->notes[i].time >= c->sections[k].time) {
                n++;
                hit += G.state[i] == HIT;
            }
        if (n >= 8 && strncasecmp(c->sections[k].name, "ugc", 3) && hit * 100 / n < r.worst_pct) {
            r.worst_pct = hit * 100 / n;
            snprintf(r.worst, sizeof(r.worst), "%s", c->sections[k].name);
        }
    }
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
    G.crowd_voice = sound_play(res.snd[mood], cfg.crowd_vol * 0.04f, true);
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

// Customs often leave the backing near silent and put the band in the part's stem: past the first seconds, a stem with
// nearly everything in it stays up and unbent. Only the player's part is ever muted, so a guitar solo isn't caught
static bool stem_is_the_song(void)
{
    int mine[MAX_STEMS + 1], n = 0;
    if (G.my_stem >= 0)
        mine[n++] = G.my_stem;
    for (int i = 0; i < G.drum_stem_count; i++)
        mine[n++] = G.drum_stems[i];
    return n && G.t > 3 && audio_stems_share(mine, n) > 0.85f;
}

static void set_my_stem(float volume)
{
    if (volume < 1 && stem_is_the_song())
        volume = 1;
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
#ifndef __vita__
    {
        const Note * n = &G.part->notes[i];
        if (getenv("GHV_MISSLOG"))
            plat_log("MISS t=%.3f frets=%02x flags=%02x prev=%.3f prevf=%02x prevstate=%d held=%02x", n->time / 1e6, n->frets, n->flags,
                     i ? G.part->notes[i - 1].time / 1e6 : 0, i ? G.part->notes[i - 1].frets : 0, i ? G.state[i - 1] : -1, G.held);
    }
#endif
    mark_judged(G.part->notes[i].frets);
    G.misses++;
    break_streak();
    G.miss_flash = 1;
    G.miss_pulse = 1;
    static const float loss[4] = {0.025f, 0.03f, 0.04f, 0.05f};
    // drums have many more gems to the bar, and a tremolo a dozen to the second
    rock_change(-loss[G.diff] * (G.roll && G.roll[i] ? 0.25f : G.drums ? 0.5f : 1));
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
    // a burst off each cup that was struck. A tremolo's gems that pass by themselves weren't, and a kick has no cup
    bool struck = !(G.roll && G.roll[i]);
    for (int l = 0; l < G.lanes && struck; l++) {
        if (!(n->frets & (1 << l)))
            continue;
        for (int k = 0; k < 4; k++) {
            typeof(&G.spark[0]) sp = &G.spark[G.spark_next++ % SPARKS];
            float a = (rand() % 628) / 100.0f;
            sp->x = lane_x(l);
            sp->y = 0.1f;
            sp->z = 0;
            sp->vx = cosf(a) * 0.045f;
            sp->vz = sinf(a) * 0.03f;
            sp->vy = 0.05f + (rand() % 100) * 0.0006f;
            sp->life = 1;
            sp->col = G.sp_active ? lane_colour[5] : lane_colour[lane_art(l)];
        }
    }
    G.score += 50 * frets_count(n->frets) * multiplier();
    rock_change(0.018f);
    for (int l = 0; l < 6 && struck; l++)
        if (n->frets & (1 << l))
            G.flame_t[l] = G.t;
    if (G.streak > 0 && G.streak % 50 == 0) {
        char t[32];
        snprintf(t, sizeof(t), "%d NOTE STREAK!", G.streak);
        popup(t);
        G.cheer = 1;
        sound_play(res.snd[SND_CHEER1 + rand() % 3], cfg.crowd_vol * 0.05f, false);
        if (G.streak % 100 == 0)
            party();
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
                G.sus[k].lost_t = -1;
                G.sus[k].end = (n->time + (double)n->len) / 1e6;
                G.sus[k].frets = n->frets;
                G.sus[k].sp = ph >= 0 && G.phrase_ok[ph];
                G.sus[k].last_q = chart_quarters(G.chart, fmax(G.t, n->time / 1e6));
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
    // pressing a fret is the strum here, so a finger on its way to the next fret can land on the wrong one. One slip costs
    // a little and the streak goes on; a second soon after is mashing, and that's an overhit
    bool again = G.t - G.slip_t < SLIP_RUN;
    G.slip_t = G.t;
    if (!cfg.no_miss_sound)
        sound_play(res.snd[SND_OVERSTRUM1 + rand() % 4], again ? 0.5f : 0.25f, false);
    if (!again) {
        rock_change(-0.01f);
        G.miss_flash = 0.3f;
        return;
    }
    G.overhits++;
    G.streak = 0;           // the song keeps your part: only a missed note takes it away, as YARG has it
    rock_change(-0.02f);
    G.miss_flash = 0.7f;
}

// Thumbs: Left and Up for one thumb, Triangle and Circle for the other, the kick under the fingers.
// Shoulders: the snare on L and the kick on R, and the other pads in pairs, one pair per cluster: the
// hi-hat up-left on the D-pad and the low pad down-right, the blue pads up-left on the buttons and the
// green down-right — every pad but the snare has two buttons, like the 4-pad layout
static uint32_t drum_buttons(int lane)
{
    const uint32_t dpad = BTN_UP | BTN_LEFT | BTN_RIGHT | BTN_DOWN;
    static const uint32_t four[4] = {BTN_LEFT, BTN_UP, BTN_TRIANGLE, BTN_CIRCLE};
    static const uint32_t five[5] = {BTN_LEFT, BTN_UP, BTN_TRIANGLE, BTN_SQUARE, BTN_CIRCLE};
    const uint32_t four_s[4] = {BTN_L, dpad, BTN_TRIANGLE | BTN_SQUARE, BTN_CIRCLE | BTN_CROSS};
    const uint32_t five_s[5] = {BTN_L, BTN_UP | BTN_LEFT, BTN_TRIANGLE | BTN_SQUARE, BTN_RIGHT | BTN_DOWN,
                                BTN_CIRCLE | BTN_CROSS};
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

// the note a press plays, if there's one in reach
static bool press_note(int lane)
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
        if (!G.got[i] && G.t - nt > NEXT_LATE) {
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
            G.hit_log[G.hit_log_n++ % HIT_LOG] = (float)off;
            G.off_sum += off;
            G.off_sq += off * off;
            G.off_n++;
        }
        return true;
    }
    return false;
}

// a press just before its note is that note mistimed, and missing it is the cost: not a stray one as well
static bool press_early(int lane)
{
    const Part * p = G.part;
    uint8_t bit = lane == LANE_OPEN ? FRET_OPEN : 1 << lane;
    for (int i = G.first; i < p->note_count && p->notes[i].time / 1e6 <= G.t + EARLY_GRACE; i++)
        if (G.state[i] == PENDING && (p->notes[i].frets & bit))
            return true;
    return false;
}

// still hammering at a tremolo that's just changed to other frets
static bool after_tremolo(int lane)
{
    if (!G.roll)
        return false;
    const Part * p = G.part;
    uint8_t bit = lane == LANE_OPEN ? FRET_OPEN : 1 << lane;
    for (int i = G.first - 1; i >= 0 && G.t - p->notes[i].time / 1e6 < SPAM_AFTER; i--)
        if (G.roll[i] && (p->notes[i].frets & bit))
            return true;
    return false;
}

static void press_lane(int lane)
{
    if (press_note(lane))
        return;
    bool gap = G.drums && gap_left() > 0;
    if (G.drums && (gap || cfg.drum_hits))
        pad_sound(lane, NULL, gap ? 0.9f : 0.5f);
    // a finger coming down just after its chord counted, or a press just late for a note that's already cost the miss,
    // isn't a stray one
    if (G.t - G.judged_t[lane] < PRESS_GRACE)
        return;
    // a stray drum hit is just a fill, and heard in a gap of the chart
    if (!G.drums) {
        if (!press_early(lane) && !after_tremolo(lane))
            overhit();
    } else if (gap) {
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

// Letting go onto a lower fret plays a pull-off, as on a guitar: hold green, tap red, and the greens play themselves.
// Only hammer-ons and tapped notes, and a hammer-on only while the streak's alive, as in the games
static void pull_off(void)
{
    const Part * p = G.part;
    uint32_t down = G.held & 0x1f;
    if (!down)
        return;
    // a sustain ringing above is no finger on the way to this note
    uint32_t ringing = 0;
    for (int k = 0; k < MAX_SUS; k++)
        if (G.sus[k].note >= 0)
            ringing |= G.sus[k].frets & 0x1f;
    for (int i = G.first; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        double nt = n->time / 1e6;
        if (nt > G.t + WINDOW)
            return;
        if (G.state[i] != PENDING || nt < G.t - WINDOW)
            continue;
        if (n->frets & FRET_OPEN)
            return;
        // Pressing a fret is the strum here, so a note on a fret already down has nothing to press: holding green and
        // tapping red, lifting red plays the next green, hammer-on or not. Frets below the note's may stay down, none
        // above it
        uint32_t free = down & ~(ringing & ~(uint32_t)n->frets);
        if ((free & n->frets) != n->frets || __builtin_clz(free) != __builtin_clz((uint32_t)n->frets))
            return;
        hit_note(i);
        double off = G.t - nt;
        G.hit_log[G.hit_log_n++ % HIT_LOG] = (float)off;
        G.off_sum += off;
        G.off_sq += off * off;
        G.off_n++;
        return;
    }
}

// is a roll or a tremolo still being kept going, for this gem? Every fret of it down or just hit, and one just hit
static bool run_going(int i)
{
    int frets = G.part->notes[i].frets;
    if (G.drums) {
        int l = __builtin_ctz(frets);
        if (G.t - G.pad_t[l] < ROLL_HOLD)
            return true;
        int m = G.roll_mate ? G.roll_mate[i] : -1;      // a trade roll: either pad keeps it going
        return m >= 0 && m < 6 && G.t - G.pad_t[m] < ROLL_HOLD;
    }
    bool struck = false;
    for (int l = 0; l < 6; l++) {
        if (!(frets & (1 << l)))
            continue;
        double since = G.t - G.pad_t[l];
        if (since >= SPAM_HOLD && !(G.held & (1 << l)))
            return false;
        struck |= since < SPAM_HOLD;
    }
    return struck;
}

static void activate_sp(void)
{
    if (G.sp_active || G.sp < 0.5f)
        return;
    G.sp_active = true;
    G.sp_last_q = chart_quarters(G.chart, G.t);
    G.sp_flash = 1;
    G.pyro_t = G.t;
    party();
    popup("STAR POWER!");
    sound_play(res.snd[SND_SP_DEPLOY], 0.9f, false);
    sound_play(res.snd[SND_SP_CROWD], cfg.crowd_vol * 0.06f, false);
#ifndef __vita__
    plat_log("sp: on at %.2f", G.t);
#endif
}

// Held green, tapped red, and the next note's green: nothing's left to press, and fingers don't arrive on the frame. The
// green may have stayed down all along, or been lifted and put back a moment early, red up or still coming up. Whichever:
// when the note reaches the line with its frets down and nothing above them, and the last note was hit, it plays. On a
// repeat of the last note's own frets the finger must have come back down since it, so a held fret never plays a run
static void anchored_note(void)
{
    // frets held through the song stay just what they are: the anchor carries a note only while fingers are
    // still moving about (a key held from always was the whole song playing itself without this)
    if (G.t - G.act_t > ANCHOR_GRACE)
        return;
    const Part * p = G.part;
    int i = G.first;
    while (i < p->note_count && G.state[i] != PENDING)
        i++;
    if (i == 0 || i >= p->note_count)
        return;
    const Note * n = &p->notes[i], * prev = &p->notes[i - 1];
    double nt = n->time / 1e6;
    if (G.t < nt || G.t > nt + WINDOW || (n->frets & FRET_OPEN) || G.state[i - 1] != HIT)
        return;
    uint32_t ringing = 0;
    for (int k = 0; k < MAX_SUS; k++)
        if (G.sus[k].note >= 0)
            ringing |= G.sus[k].frets & 0x1f;
    uint32_t free = G.held & 0x1f & ~(ringing & ~(uint32_t)n->frets);
    if ((free & n->frets) != n->frets)
        return;
    // Auto re-press: the green plays even with the red still down on its way up
    if (__builtin_clz(free) != __builtin_clz((uint32_t)n->frets) && !cfg.auto_repress)
        return;
    for (int l = 0; l < 5; l++)
        if ((n->frets & (1 << l)) && prev->frets == n->frets && G.press_t[l] <= prev->time / 1e6 + WINDOW * 0.5)
            return;
    hit_note(i);
}

static void judge(uint32_t pressed_lanes, uint32_t let_go)
{
    const Part * p = G.part;
    if (pressed_lanes || let_go)
        G.act_t = G.t;
    for (int lane = 0; lane < 6; lane++)
        if (pressed_lanes & (1 << lane)) {
            G.press_t[lane] = G.t;
            press_lane(lane);
        }
    if (let_go && !G.drums && !G.autoplay)
        pull_off();
    if (!G.drums && !G.autoplay && !G.vocal)
        anchored_note();
    // rolls and tremolos go by as they're kept going, each gem on the line
    for (int i = G.first; G.roll && i < p->note_count && p->notes[i].time / 1e6 <= G.t; i++) {
        if (G.state[i] != PENDING || !G.roll[i] || (!G.drums && G.state[i - 1] == PENDING))
            continue;
        if (run_going(i))
            hit_note(i);
        if (!G.drums && !G.run_told) {
            G.run_told = true;
            popup("KEEP HITTING IT!");
        }
    }
    // notes whose window has passed
    while (G.first < p->note_count) {
        int i = G.first;
        double nt = p->notes[i].time / 1e6;
        if (G.state[i] == PENDING) {
            if (G.t <= nt + WINDOW)
                break;
            if (G.roll && G.roll[i] && run_going(i))
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
                if (pct >= 90)
                    sound_play(res.snd[SND_CHEER1 + rand() % 3], cfg.crowd_vol * 0.08f, false);
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
        // a chord on two thumbs: one of its frets keeps it ringing
        if (cfg.easy_sustains && (G.held & need))
            held = true;
        // a finger shifting on the button isn't letting go
        if (held)
            s->lost_t = -1;
        else if (s->lost_t < 0)
            s->lost_t = G.t;
        if (!held && G.t - s->lost_t < SUS_SLACK)
            held = true;
        double until = G.t < s->end ? q : chart_quarters(G.chart, s->end);
        double dq = until - s->last_q;
        if (dq > 0 && held) {
            G.score += 25 * dq * multiplier();
            if (s->sp && G.whammy > 0.2f) {
                bool was_ready = G.sp >= 0.5f;
                G.sp += (float)dq * SP_PER_QUARTER;
                G.sp_from_bar += (float)dq * SP_PER_QUARTER;
                if (G.sp > 1)
                    G.sp = 1;
                if (!was_ready && G.sp >= 0.5f && !G.sp_active) {
                    popup("STAR POWER READY!");
                    sound_play(res.snd[SND_SP_READY], 0.7f, false);
                }
            }
            for (int l = 0; l < 6; l++)
                if ((s->frets & (1 << l)) && (G.held & (1 << l)))
                    G.flame_t[l] = G.t - 0.06;          // keep the flame going, on the frets really down
        }
        s->last_q = until;
        // let go with the note still ringing: as in YARG the part goes quiet until the next hit, but it's no miss.
        // The last moments don't count, nobody times a release to the frame
        if (!held && G.t < s->end - SUS_DROP_GRACE) {
            G.sus_dropped++;
            s->note = -1;
            bool others = false;
            for (int j = 0; j < MAX_SUS; j++)
                others |= G.sus[j].note >= 0;
            if (!others)
                set_my_stem(0);
        }
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
    bool phrase = vox_update(G.t, &grade, &points);
    // YARG's voice-activated Star Power: with the meter half full, a note sung in tune is enough
    if (cfg.sing_sp && !G.sp_active && G.sp >= 0.5f && vox_sung_now())
        activate_sp();
    if (!phrase)
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

// the drummer's sticks: four clicks up to the first note, when the lead-in has room
static void count_in(void)
{
    const Part * p = G.part;
    if (cfg.no_count_in || !p->note_count || G.count_done >= 4)
        return;
    double first = p->notes[0].time / 1e6;
    double beat = 60.0 / ((chart_quarters(G.chart, first + 0.5) - chart_quarters(G.chart, first)) * 120 + 1e-6);
    double at = first - (4 - G.count_done) * beat;
    if (first > 60 || (G.count_done == 0 && at < -G.lead_in)) {
        G.count_done = 4;
        return;
    }
    if (G.t >= at) {
        G.count_done++;
        sound_play(res.snd[SND_DRUM_HAT], 0.55f, false);
    }
}

int game_update(const Pad * pad, uint32_t pressed)
{
    if (G.paused || !G.part)
        return GAME_PAUSE;
    G.t = audio_song_time() - cfg.offset_ms / 1000.0;
    // failed: the song winds down like a tape, the board goes red, then the results
    if (G.failed) {
        if (G.fail_frames == 0) {
            sound_play(res.snd[SND_BOO1 + rand() % 3], cfg.crowd_vol * 0.1f, false);
            audio_song_fade(0, FAIL_FRAMES / 60.0f);
        }
        if (++G.fail_frames >= FAIL_FRAMES)
            return GAME_FAILED;
        audio_song_speed(1 - 0.7f * G.fail_frames / FAIL_FRAMES);
        return GAME_PLAYING;
    }
    if (pressed & BTN_START)
        return GAME_PAUSE;
    count_in();

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
    uint32_t let_go = G.held & ~held;
    G.held = held;
    for (int lane = 0; lane < 6; lane++)
        if (lanes_pressed & (1 << lane))
            G.pad_t[lane] = G.t;

    bool ringing = false;
    for (int k = 0; k < MAX_SUS; k++)
        ringing |= G.sus[k].note >= 0;
    // whammy: the right stick, or a finger rubbing the rear touchpad (a thumb on the frets can't reach the stick)
    float dw;
    float bar = bar_read(&G.bar, pad->rx, pad->ry, cfg.no_rear_whammy ? 0 : pad->back_n, pad->back_id, pad->back_xs, pad->back_ys, ringing, &dw);
    if (G.autoplay) {
        bar = 0.5f + 0.5f * sinf((float)G.t * 30);
        dw = 1;
    }
    G.whammy = G.whammy * 0.9f + (dw > 0.02f ? 0.35f : 0);
    if (G.whammy > 1)
        G.whammy = 1;
    // the part's own stem bends while a sustain rings
    G.bend += ((ringing ? (bar > 1 ? 1 : bar) : 0) - G.bend) * 0.5f;
    if (!G.drums)
        audio_stem_bend(G.my_stem, G.bend > 0.003f && !cfg.no_bend && !stem_is_the_song() ? G.bend : 0);

    // Star Power: Select, a quick tilt, or on drums both cymbals at once, as in World Tour
    if ((pressed & BTN_SELECT) || (G.drums && both_cymbals(lanes_pressed)))
        activate_sp();
    if (cfg.tilt && pad->has_motion)
        tilt(pad->accel);

    if (G.sing)
        update_vocals();
    if (!G.vocal) {
        judge(lanes_pressed, let_go);
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
    for (int i = 0; i < SPARKS; i++)
        if (G.spark[i].life > 0) {
            G.spark[i].x += G.spark[i].vx;
            G.spark[i].y += G.spark[i].vy;
            G.spark[i].z += G.spark[i].vz;
            G.spark[i].vy -= 0.004f;
            G.spark[i].life -= 0.045f;
        }
    if (multiplier() > G.mult_seen) {
        G.mult_pop = 1;
        if (G.t > 0)
            sound_play(res.snd[SND_STAR], 0.3f, false);
    }
    G.mult_seen = multiplier();
    G.mult_pop *= 0.88f;
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
    // measured off World Tour's neck: its art at about 80%, 60% at the far end
    float d = z < 0 ? 0 : z / Z_FAR, v = 205 - 60 * d + lift * 0.6f;
    v = v > 255 ? 255 : v;
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
    const float solid = cfg.highway_alpha / 10.0f;
    const int seg = 6;
    for (int i = 0; i < seg; i++) {
        float z0 = Z_NEAR + (Z_FAR - Z_NEAR) * i / seg, z1 = Z_NEAR + (Z_FAR - Z_NEAR) * (i + 1) / seg;
        float v0 = -(z0 / rep + off), v1 = -(z1 / rep + off);
        gfx_tex(G.highway);
        const float p[4][3] = {{-HALF_W, 0, z0}, {HALF_W, 0, z0}, {HALF_W, 0, z1}, {-HALF_W, 0, z1}};
        const float uv[4][2] = {{1, v0}, {0, v0}, {0, v1}, {1, v1}};
        uint32_t b0 = board_col(z0, lift), b1 = board_col(z1, lift);
        const uint32_t c[4] = {with_alpha(b0, fade(z0) * solid), with_alpha(b0, fade(z0) * solid),
                               with_alpha(b1, fade(z1) * solid), with_alpha(b1, fade(z1) * solid)};
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
        // silver bars stopping short of the rails, the measure's the stronger
        float d = measure ? 0.26f : 0.13f;
        uint32_t col = with_alpha(measure ? RGBA(235, 235, 245, 255) : RGBA(190, 185, 200, 170), fade(z));
        flat(-HALF_W + 0.13f, HALF_W - 0.13f, z - d / 2, z + d / 2, 0.005f, measure ? bar_l : bar_m, 0, 1, col, col);
    }
    // the strings: coiled wire hooked on the back of each cup, fading off up the neck; a struck one rings in its colour
    const Sprite * wire = atlas_get(res.game, "string");
    for (int l = 0; l < G.lanes; l++) {
        float x = lane_x(l);
        double since = G.t - G.flame_t[l];
        float ring = since >= 0 && since < 0.25 ? 1 - (float)since / 0.25f : 0;
        uint32_t c0 = ring > 0 ? lane_colour[lane_art(l)] : RGBA(235, 232, 240, 255);
        // thin wound steel as in the footage, the coil tight, gone by the far end of the neck
        const float seg = 0.24f, start = 0.3f, end = 8.4f, half = 0.034f + ring * 0.016f;
        for (float z = start; z < end; z += seg) {
            float a0 = 1 - (z - start) / (end - start), a1 = 1 - (z + seg - start) / (end - start);
            flat(x - half, x + half, z, z + seg, 0.008f, wire, 0, 1, with_alpha(c0, a0), with_alpha(c0, a1));
        }
    }
    // the rails: silver, glowing blue with Star Power ready or on, red on a miss
    const Sprite * rail = atlas_get(res.game, "rail");
    uint32_t rc = RGBA(225, 225, 235, 255);
    if (G.sp_active)
        rc = RGBA(120, 210, 255, 255);
    else if (G.sp >= 0.5f)
        rc = RGBA(170, 220, 255, 255);
    if (G.miss_flash > 0.05f && cfg.flashes < 2) {
        int k = (int)(G.miss_flash * (cfg.flashes ? 80 : 200));
        rc = RGBA(225, 225 - k, 235 - k, 255);
    }
    for (int side = -1; side <= 1; side += 2) {
        float x = side * HALF_W;
        strip(x - 0.07f, x + 0.07f, 0.01f, rail, rc);
    }
    // the silver bar just in front of the cups, rail to rail, whatever the board is doing
    uint32_t bar_c = G.sp_active ? RGBA(150, 220, 255, 255) : RGBA(235, 235, 245, 255);
    flat(-HALF_W + 0.1f, HALF_W - 0.1f, -0.5f, -0.34f, 0.012f, bar_l, 0, 1, bar_c, bar_c);
}

static void draw_sustains(void)
{
    const Part * p = G.part;
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
            uint32_t col = open ? lane_colour[6] : lane_colour[lane_art(l)];
            if (G.sp_active || (G.phrase[i] >= 0 && G.phrase_ok[G.phrase[i]]))
                col = lane_colour[5];
            if (dead)
                col = RGBA(100, 100, 110, 200);
            uint32_t glow_c = col;
            if (!dead && active)
                col = RGBA(((col & 0xFF) + 255) / 2, (((col >> 8) & 0xFF) + 255) / 2, (((col >> 16) & 0xFF) + 255) / 2, 255);
            // whammy shakes it
            if (active) {
                // a held note burns: a wide glow round the beam, breathing with the song's beat, in the note's own colour
                float breath = 0.32f + 0.1f * sinf(t * 14);
                gfx_blend(BLEND_ADD);
                flat(x - w * 3.2f, x + w * 3.2f, z0, z_end, 0.011f, atlas_get(res.game, "glow"), 0, 1,
                     with_alpha(glow_c, breath), with_alpha(glow_c, breath));
                gfx_blend(BLEND_ALPHA);
                w *= 1.3f;
            }
            int pieces = active ? 10 : 1;
            float amp = active && !cfg.no_wobble ? 0.03f + G.whammy * 0.12f : 0;
            for (int s = 0; s < pieces; s++) {
                float za = z0 + (z_end - z0) * s / pieces, zb = z0 + (z_end - z0) * (s + 1) / pieces;
                float wob = amp * sinf(za * 2.6f + t * 25);
                flat(x - w + wob, x + w + wob, za, zb, 0.012f, atlas_get(res.game, "sustain"), 0, 1, with_alpha(col, fade(za)),
                     with_alpha(col, fade(zb)));
            }
        }
    }
}

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

static void lay(float x, float y, float z, float w, float d, const Sprite * s, uint32_t c);

static void draw_cups(bool caps)
{
    const float cw = 1.02f, cup_d = 0.84f;        // 128x64 art lying on the neck: 2.9 to 1 on the screen, as World Tour's
    for (int l = 0; l < G.lanes; l++) {
        char name[16];
        float x = lane_x(l);
        if (!caps) {
            snprintf(name, sizeof(name), "%s_%d", cfg.cup_skin ? "cupdeep" : "cup", lane_art(l));
            lay(x, 0.015f, 0, cw, cup_d, atlas_get(res.game, name), WHITE);
        } else if (G.held & (1 << l)) {
            // held, the cap stands in the cup, centred on it, raised a little
            snprintf(name, sizeof(name), "cap_%d", lane_art(l));
            lay(x, 0.09f, 0, cw * 0.9f, cup_d * 0.9f, atlas_get(res.game, name), WHITE);
        }
    }
    // the open note's bar lights across the cups
    if (caps && (G.held & (1 << LANE_OPEN))) {
        gfx_blend(BLEND_ADD);
        board(0, 0.05f, -0.02f, 5.2f, 0.4f, atlas_get(res.game, "glow"), 0, 0, 1, 1, with_alpha(lane_colour[6], 0.5f));
        gfx_blend(BLEND_ALPHA);
    }
}

// the streak meter, as World Tour has it: a gold strip lying on the board beside the right rail, five lamps on it filling
// from the strikeline up, the multiplier at its top. On the board, so it takes the camera's perspective
static void rail_quad(const Sprite * s, float x0, float x1, float z0, float z1, bool along, bool flip, uint32_t col)
{
    const float y = 0.015f;
    float u0 = flip ? s->u1 : s->u0, u1 = flip ? s->u0 : s->u1;
    gfx_tex(s->tex);
    const float p[4][3] = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
    // along: the art's height runs up the board; otherwise its width runs across it
    float uv[4][2] = {{u0, s->v1}, {u1, s->v1}, {u1, s->v0}, {u0, s->v0}};
    if (!along) {
        uv[0][0] = u0; uv[0][1] = s->v0;
        uv[1][0] = u1; uv[1][1] = s->v0;
        uv[2][0] = u1; uv[2][1] = s->v1;
        uv[3][0] = u0; uv[3][1] = s->v1;
    }
    const uint32_t c[4] = {col, col, col, col};
    gfx_quad(p, uv, c);
}

static void draw_rail_meter(void)
{
    // the gold rope hugs the outside of the rail with five glass domes along it, stood straight out from it and set
    // apart; yellow up to x2, green to x3, purple on to the top, as the footage has them
    const float rope0 = -HALF_W + 0.04f, rope1 = -HALF_W - 0.24f, z0 = 0.55f, z1 = 3.0f;
    int m = multiplier(), base_m = 1 + G.streak / 10, max_m = G.part_id == PART_BASS ? 6 : 4;
    int lit = base_m >= max_m ? 5 : (G.streak % 10) / 2;
    bool half = base_m < max_m && G.streak % 2;
    const char * hue = G.sp_active ? "_blue" : base_m >= 3 ? "_purple" : base_m == 2 ? "_green" : "";
    gfx_blend(BLEND_ALPHA);
    gfx_2d();
    for (int i = 0; i < 5; i++) {
        char name[32];
        int state = i < lit ? 2 : half && i == lit ? 1 : 0;
        snprintf(name, sizeof(name), "lamp_%d%s", state, hue);
        const Sprite * s = atlas_get(res.game, name);
        // where its foot lies on the rope, on the screen; the dome stands square to that, as deep as it's wide
        const float lz = z0 + 0.52f, lg = (z1 - 0.42f - lz) / 5, fx = rope1 + 0.14f;
        const float a3[3] = {fx, 0.016f, lz + lg * (i - 0.02f)}, b3[3] = {fx, 0.016f, lz + lg * (i + 1.02f)};
        float a[2], b[2];
        project(a3[0], a3[1], a3[2], &a[0], &a[1]);
        project(b3[0], b3[1], b3[2], &b[0], &b[1]);
        // a stiff lamp pointing straight out to the side of the screen, as in the footage, not square to the slanted rope
        const float tilt = -0.09f, ux = cosf(tilt), uy = sinf(tilt), wide = hypotf(b[0] - a[0], b[1] - a[1]) * 0.5f;
        const float len = wide * 2.5f, mx0 = (a[0] + b[0]) / 2, my0 = (a[1] + b[1]) / 2, vx = -uy * wide, vy = ux * wide;
        const float p[4][3] = {{mx0 + ux * len - vx, my0 + uy * len - vy, 0}, {mx0 - vx, my0 - vy, 0}, {mx0 + vx, my0 + vy, 0},
                               {mx0 + ux * len + vx, my0 + uy * len + vy, 0}};
        const float uv[4][2] = {{s->u0, s->v0}, {s->u1, s->v0}, {s->u1, s->v1}, {s->u0, s->v1}};
        const uint32_t c[4] = {WHITE, WHITE, WHITE, WHITE};
        gfx_tex(s->tex);
        gfx_quad(p, uv, c);
        if (state == 2) {
            // lit, it throws light round itself
            const uint32_t light = G.sp_active ? RGBA(95, 225, 255, 255) : base_m >= 3 ? RGBA(255, 130, 225, 255)
                                 : base_m == 2 ? RGBA(90, 245, 110, 255) : RGBA(255, 196, 70, 255);
            float mx = mx0 + ux * len * 0.72f, my = my0 + uy * len * 0.72f, r = wide * 2.2f;
            gfx_blend(BLEND_ADD);
            gfx_sprite(atlas_get(res.game, "glow"), mx - r, my - r, r * 2, r * 2, with_alpha(light, 0.6f));
            gfx_blend(BLEND_ALPHA);
        }
    }
    gfx_3d(G.proj, G.view);
    gfx_blend(BLEND_ALPHA);
    rail_quad(atlas_get(res.hud, "hud_rope"), rope0, rope1, z0, z1, true, false, WHITE);
    if (m > 1) {
        char name[16];
        snprintf(name, sizeof(name), "mult_x%d", m);
        float pop = 1 + G.mult_pop * 0.45f;
        board(-HALF_W - 0.42f, 0.02f, z1 + 0.15f, 0.8f * pop, 0.4f * pop, atlas_get(res.game, name), 0, 0, 1, 1,
              G.sp_active ? RGBA(170, 225, 255, 255) : WHITE);
    }
}

// a note or cup lying on the neck: the art's foot nearest you, so the perspective turns it as it does the lanes. Off in
// Options, they stand facing you
static void lay(float x, float y, float z, float w, float d, const Sprite * s, uint32_t c)
{
    if (!cfg.flat_notes) {
        board(x, y, z - d * 0.25f, w, d * 0.5f, s, 0, 0, 1, 1, c);
        return;
    }
    flat(x + w / 2, x - w / 2, z - d / 2, z + d / 2, y, s, 1, 0, c, c);
}

static void lay_gem(float x, float y, float z, float w, float d, const Sprite * s, uint32_t c)
{
    // square in its lane, like the cups: the old depth spread pulled each gem's far edge (the button's white cap)
    // towards the middle of the screen, so only the middle lane's cap sat centered on its gem
    lay(x, y, z, w, d, s, c);
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
        else if (cfg.vanish)
            a *= clamp01((z - 2.2f) / 1.6f);    // gone before the strikeline: play it from memory
        int kind = G.drums ? 0 : n->flags & NOTE_TAP ? 2 : n->flags & NOTE_HOPO ? 1 : 0;
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
            // Star Power's own cyan, for a phrase still in earn and while it burns: the beams below already are
            int c = G.sp_active || star ? 5 : lane_art(l);
            // a cymbal lies wide and flat, its dome lit, so it can be told from a drum at a glance
            bool cym = G.drums && G.chart->version >= 2 && lane_cymbal(l, n->flags);
            const char * look = cym ? kinds[1] : kinds[kind];
            float gw = cym ? 1.2f : 1.04f, gh = cym ? 0.4f : 0.48f;
            // drums: an accent stands out big and lit, a ghost note's small and faint. Any hit plays them: buttons have
            // no velocity
            bool accent = G.drums && (n->flags & NOTE_HOPO), ghost = G.drums && (n->flags & NOTE_TAP);
            if (accent || ghost) {
                gw *= accent ? 1.24f : 0.76f;
                gh *= accent ? 1.24f : 0.76f;
            }
            if (star)
                snprintf(name, sizeof(name), "star_%s_%d_%d", look, c, star_frame);
            else
                snprintf(name, sizeof(name), "gem_%s_%d", look, c);
            // a soft shadow lifts the gem off the board art; a touch larger than before
            const Sprite * gem = atlas_get(res.game, name);
            static const float sizes[3] = {0.9f, 1, 1.14f};
            float k = sizes[cfg.note_size];
            gw *= k;
            gh *= k;
            lay_gem(lane_x(l), 0.012f, z - 0.05f, gw * 1.08f, gh * 2.2f, gem, with_alpha(RGBA(0, 0, 0, 255), a * 0.45f));
            lay_gem(lane_x(l), 0.02f, z, gw, gh * 2.1f, gem, ghost ? with_alpha(WHITE, a * 0.7f) : col);
            if (accent) {
                gfx_blend(BLEND_ADD);
                board(lane_x(l), 0, z, gw * 1.3f, gh * 1.5f, atlas_get(res.game, "glow"), 0, 0, 1, 1, with_alpha(WHITE, a * 0.55f));
                gfx_blend(BLEND_ALPHA);
            }
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
        if (since < 0 || since > 0.32 || cfg.fire == 2)
            continue;
        int frame = (int)(since / 0.32 * 16);
        if (frame > 15)
            frame = 15;
        float u0 = (frame % 4) * 0.25f, v0 = (frame / 4) * 0.25f;
        if (l == LANE_OPEN) {
            // the kick lights its line across: fire on every cup read as notes hit that weren't
            board(0, 0, -0.1f, 5.0f, 0.5f, glow, 0, 0, 1, 1, with_alpha(lane_colour[6], 0.8f * (1 - (float)since / 0.32f)));
            continue;
        }
        float x = lane_x(l);
        float big = cfg.fire ? 0.6f : 1;
        board(x, 0.03f, -0.42f * big, 1.1f * big, 2.1f * big, flame, u0, v0, u0 + 0.25f, v0 + 0.25f, fc);
        lay(x, 0.05f, 0, 1.2f, 1.0f, glow, with_alpha(lane_colour[lane_art(l)], 1 - (float)since / 0.32f));
    }
    const Sprite * dot = atlas_get(res.game, "glow");
    for (int i = 0; i < SPARKS; i++)
        if (G.spark[i].life > 0)
            board(G.spark[i].x, G.spark[i].y, G.spark[i].z, 0.24f, 0.24f, dot, 0, 0, 1, 1,
                  with_alpha(G.spark[i].col, G.spark[i].life));
    // Star Power: the rails burn, and the game's own jagged bolts leap off them out over the stage, a new set a dozen
    // times a second, each with a wider faint one behind it for the glow
    if (G.sp_active) {
        for (int side = -1; side <= 1; side += 2)
            flat(side * HALF_W - side * 0.25f, side * HALF_W + side * 0.9f, Z_NEAR, Z_FAR * 0.75f, 0.018f, dot, 0, 1,
                 RGBA(90, 170, 255, 55), RGBA(90, 170, 255, 0));
        const Sprite * bolt = atlas_get(res.game, "bolt");
        gfx_2d();
        gfx_blend(BLEND_ADD);
        for (int side = -1; side <= 1; side += 2) {
            uint32_t seed = (uint32_t)(G.t * 12) * 2654435761u + (uint32_t)(side + 2) * 40503u;
            for (int k = 0; k < 7; k++) {
                uint32_t h = (seed + (uint32_t)k * 97u) * 2654435761u;
                float z = 0.6f + k * 1.0f + (h >> 8 & 255) / 255.0f * 0.8f, sx, sy;
                project(side * HALF_W, 0, z, &sx, &sy);
                float near = 5.5f / (z + 5.5f), len = (70 + (h >> 16 & 255) / 255.0f * 70) * near;
                float tilt = ((h >> 24 & 255) / 127.5f - 1) * 0.7f;
                // on the screen +x is -side: out from the rail either way
                float rot = (side < 0 ? 0 : 3.14159265f) + tilt - 0.2f * (side < 0 ? 1 : -1);
                float px = bolt->w * 0.02f, py = bolt->h / 2.0f, fa = fade(z);
                sprite_rot(bolt, sx, sy, len * 1.1f, 34 * near, px, py, rot, with_alpha(RGBA(60, 170, 255, 255), 0.55f * fa));
                sprite_rot(bolt, sx, sy, len, 18 * near, px, py, rot, with_alpha(RGBA(215, 245, 255, 255), fa));
            }
        }
        gfx_blend(BLEND_ALPHA);
        gfx_3d(G.proj, G.view);
        gfx_blend(BLEND_ADD);
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
        if (buf[i] == ' ')
            continue;
        float u0 = (buf[i] - '0') * cell;
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

// percent of the notes so far, and a mark while nothing's gone wrong
static void draw_live_stats(void)
{
    int judged = G.hits + G.misses;
    if (!cfg.live_stats || !judged)
        return;
    char t[32];
    snprintf(t, sizeof(t), "%.1f%%", G.hits * 100.0 / judged);
    font_draw_shadow(res.ui, t, SCREEN_W - 16, 134, 26, WHITE, ALIGN_RIGHT);
    const Sprite * star = atlas_get(res.hud, "hud_star_on");
    for (int i = 0, n = stars_for(G.score); i < n; i++)
        gfx_sprite(star, SCREEN_W - 34 - i * 20, 190, 18, 18, n >= 6 ? RGBA(255, 215, 90, 255) : WHITE);
    if (!G.misses && !G.overhits)
        font_draw_shadow(res.ui, "FC", SCREEN_W - 16, 162, 26, RGBA(255, 210, 60, 255), ALIGN_RIGHT);
}

// the hit window under the highway: each hit a tick, left early, right late, the old ones fading out
static void draw_timing_bar(void)
{
    if (!cfg.timing_bar)
        return;
    const float cx = SCREEN_W / 2, y = SCREEN_H - 16, half = 120;
    gfx_rect(cx - half, y, half * 2, 4, RGBA(0, 0, 0, 170));
    gfx_rect(cx - half * 0.32f, y, half * 0.64f, 4, RGBA(70, 200, 90, 200));    // within 30 ms
    gfx_rect(cx - 1, y - 6, 2, 16, WHITE);
    int n = G.hit_log_n < HIT_LOG ? G.hit_log_n : HIT_LOG;
    for (int k = 1; k <= n; k++) {
        float off = G.hit_log[(G.hit_log_n - k) % HIT_LOG], d = fabsf(off);
        uint32_t c = d < 0.03f ? RGBA(120, 240, 140, 255) : d < 0.06f ? RGBA(250, 215, 90, 255) : RGBA(250, 130, 60, 255);
        gfx_rect(cx + off / (float)WINDOW * half - 1.5f, y - 5, 3, 14, fade_alpha(c, k == 1 ? 1 : 0.75f * (1 - (float)k / HIT_LOG)));
    }
}

static void draw_hud(void)
{
    gfx_2d();
    gfx_blend(BLEND_ALPHA);
    float t = (float)G.t;
    const float k = 0.6f, dx = 30, dy = 288;           // the dome's 256x256 texture
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
        float th = (146 - i * 22.4f) * pi / 180, r = 52;
        float x = cx + r * cosf(th), y = cy - r * sinf(th), rot = pi / 2 - th;
        sprite_rot(tube, x, y, 19, 36, tube->w / 2.0f, (float)tube->h, rot, WHITE);
        float fill = clamp01(G.sp * 6 - i) * pulse;
        if (fill > 0) {
            gfx_blend(BLEND_ADD);
            sprite_rot(glow, x, y, 19, 36, glow->w / 2.0f, (float)glow->h, rot, fade_alpha(WHITE, fill));
            gfx_blend(BLEND_ALPHA);
        }
    }
    // the score counter the dome stands on, the streak plate under it
    const float sk = 0.62f, sx = cx - 129 * sk, sy = dy + 172 * k - 66 * sk;
    gfx_rect(sx + 22 * sk, sy + 78 * sk, 212 * sk, 44 * sk, RGBA(2, 12, 4, 255));
    hud_at("hud_rm_solo_bottom", sx, sy, sk, sk, 0, 0, 256, 128, WHITE);
    lcd("lcd_green", (long)G.score, 7, sx + 226 * sk, sy + 100 * sk - 13, 26);
    const float kx = 0.54f, ky = 0.42f, px = cx - 128 * kx, py = sy + 122 * sk - 24 * ky;
    gfx_rect(px + 40 * kx, py + 38 * ky, 176 * kx, 50 * ky, RGBA(3, 3, 3, 255));
    hud_at("hud_counter_body", px, py, kx, ky, 0, 0, 256, 128, WHITE);
    lcd("lcd_orange", G.streak, 4, px + 214 * kx, py + 62 * ky - 9, 19);
    // the dome: the metal first, its bright window set inside it with ROCK in black, the three sectors pale, the one
    // the needle's in lit up. The art's plinth under the rim is cropped: the dome sits on the score plate
    float h = G.rock;
    float w0 = clamp01((0.40f - h) / 0.1f), w2 = clamp01((h - 0.60f) / 0.1f), w1 = clamp01(1 - w0 - w2);
    if (h < 0.25f)
        w0 *= 0.65f + 0.35f * fabsf(sinf(t * 6));        // failing: the red throbs
    gfx_sprite_uv(atlas_get(res.hud, "hud_rock_body"), dx, dy, 256 * k, 172 * k, 0, 0, 1, 172.0f / 256, RGBA(245, 230, 195, 255));
    const float wk = k * 0.9f, wx = cx - 129 * wk, wy = cy - 124 * wk;
    if (w0 > 0)
        hud_at("hud_rock_bg_red", wx, wy, wk, wk, 0, 0, 256, 256, fade_alpha(WHITE, w0));
    if (w1 > 0)
        hud_at("hud_rock_bg_yellow", wx, wy, wk, wk, 0, 0, 256, 256, fade_alpha(WHITE, w1));
    if (w2 > 0)
        hud_at("hud_rock_bg_green", wx, wy, wk, wk, 0, 0, 256, 256, fade_alpha(WHITE, w2));
    hud_at("hud_rock_lights_all", wx, wy, wk, wk, 0, 0, 256, 128, RGBA(235, 225, 210, 235));
    gfx_blend(BLEND_ADD);
    if (w0 > 0)
        hud_at("hud_rock_lights_red", wx, wy, wk, wk, 0, 0, 128, 128, fade_alpha(WHITE, w0));
    if (w1 > 0)
        hud_at("hud_rock_lights_yellow", wx + 64 * wk, wy, wk, wk, 0, 0, 128, 128, fade_alpha(WHITE, w1));
    if (w2 > 0)
        hud_at("hud_rock_lights_green", wx + 128 * wk, wy, wk, wk, 0, 0, 128, 128, fade_alpha(WHITE, w2));
    gfx_blend(BLEND_ALPHA);
    const Sprite * needle = atlas_get(res.hud, "hud_rock_needle");
    // shorter than the sprite's own length and a narrower swing, so it stays inside the dome, and weighted: it eases to the
    // value instead of snapping with every note
    G.needle += (G.rock - G.needle) * 0.1f;
    float swing = (G.needle - 0.5f) * 2 * 0.85f;
    sprite_rot(needle, dx + 128 * k, dy + 172 * k, 13 * k, 84 * k, needle->w / 2.0f, (float)needle->h, swing, WHITE);
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
        if (!cfg.no_progress && !G.sing) {
            gfx_rect(0, 0, SCREEN_W, 2, RGBA(0, 0, 0, 90));
            gfx_rect(0, 0, SCREEN_W * f, 2, RGBA(235, 235, 245, 150));
        }
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
    if (!G.vocal) {
        draw_live_stats();
        draw_timing_bar();
    }
    if (cfg.show_fps) {
        char t[32];
        snprintf(t, sizeof(t), "%d fps", G.fps);
        font_draw_shadow(res.ui, t, SCREEN_W - 10, SCREEN_H - 30, 20, WHITE, ALIGN_RIGHT);
    }
}

void game_draw(void)
{
#ifndef __vita__
    if (getenv("GHV_SPFX"))
        G.sp_active = true;             // to look at Star Power without playing up to it
#endif
    if (G.camera != cfg.camera)
        setup_camera();         // changed from the pause menu
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_stage();
    if (cfg.venue_light < 10)
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, (10 - cfg.venue_light) * 24));
    if (G.sp_flash > 0.02f && cfg.flashes < 2)
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(80, 160, 255, (int)(G.sp_flash * (cfg.flashes ? 25 : 70))));
    if (!G.vocal) {
        gfx_3d(G.proj, G.view);
        draw_highway();
        if (G.roll_count || G.gap_count)
            draw_zones();
        draw_sustains();
        draw_cups(false);
        draw_gems();
        draw_cups(true);
        draw_flames();
        draw_rail_meter();
    }
    draw_hud();
    if (G.fail_frames) {
        float f = (float)G.fail_frames / FAIL_FRAMES;
        gfx_2d();
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(120, 10, 10, (int)(f * 170)));
        font_draw_shadow(res.title, "SONG FAILED", SCREEN_W / 2, 210, 72, with_alpha(RGBA(255, 90, 70, 255), f > 0.3f ? 1 : f / 0.3f),
                         ALIGN_CENTER);
    }
    if (G.sing)
        vox_draw(G.t);
    else if (!cfg.no_lyrics)
        vox_lyrics(G.t);
    gfx_end();
}
