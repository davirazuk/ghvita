#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "game.h"
#include "player.h"
#include "achieve.h"
#include "vocals.h"
#include "res.h"
#include "audio.h"
#include "whammybar.h"
#include "cues.h"

// seconds early or late a note can still be hit: Normal is Warriors of Rock-ish lenient; Options can widen or narrow it
// the hit window either way: Normal 85 ms, Strict 70, Lenient 110, Pro 55
#define WINDOW (cfg.hit_window == 1 ? 0.070 : cfg.hit_window == 2 ? 0.110 : cfg.hit_window == 3 ? 0.055 : cfg.hit_window == 4 ? 0.100 : 0.085)
// Guitar Hero 3's own (read from GH3.exe): 116 ms early, 100 late, hammer-ons and pull-offs 83 either way. Every window
// ends late at half the way to the next note, as in GH: a late press on a fast run is that run's next note, not this one
#define WIN_EARLY (cfg.hit_window == 4 ? 0.116 : WINDOW)
#define HOPO_WIN (cfg.hit_window == 4 ? 0.083 : WINDOW)
#define CHORD_SPREAD 0.08               // a chord's keys pressed this close together count as one hit
#define RATE_PERFECT 0.025              // a hit this close to its note is Perfect, then Great, then Good
#define RATE_GREAT 0.050
static float z_far = 11.0f;
#define Z_FAR z_far                     // the highway's far end (the strikeline is at 0): Options > Highway length
#define Z_NEAR -3.4f
#define HALF_W 2.62f            // half the highway's width (lanes are 1 wide)
#define MAX_SUS 8
#define MAX_ZONES 64
#define MAX_ROLLS 512
#define SP_PER_QUARTER (1.0f / 32)
#define SUS_DROP_GRACE 0.12
#define SUS_SLACK 0.09
#define HIT_LOG 16
#define SPARKS 96
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

// the looks of a chart's own light show (Rock Band's lighting cues)
enum { CUE_NONE, CUE_VERSE, CUE_CHORUS, CUE_COOL, CUE_WARM, CUE_DISCHORD, CUE_STOMP, CUE_HARMONY, CUE_FRENZY,
       CUE_SILHOUETTE, CUE_SILHOUETTE_SPOT, CUE_SEARCH, CUE_STROBE_SLOW, CUE_STROBE_FAST, CUE_BLACK_SLOW, CUE_BLACK_FAST,
       CUE_BLACK_SPOT, CUE_BRE, CUE_INTRO };

static struct {
    Song * song;
    Chart * chart;
    const Part * part;
    int part_id, diff;
    uint8_t * state;
    uint8_t * got;              // lanes already pressed for a chord
    float * got_t;              // when the first of them was pressed
    int perfects, greats, goods;        // the hits by how close they were
    int perfect_run;                    // Perfects in a row
    double rate_t;                      // the last hit's rating, shown over the strikeline for a moment
    int rate_kind;                      // 0 perfect 1 great 2 good 3 miss
    float rate_off;
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
    int early, on_time, late;           // hits more than 30 ms early, within 30 ms, more than 30 ms late
    float sp, rock;
    bool sp_active, sp_ready_told;
    double sp_last_q;           // where Star Power's drain has got to, in quarter notes
    double base_score;
    int beat_best;                  // the best score to beat on this chart, 0 when none or already beaten
    double vox_points;              // singing along: what the voice has added
    int vox_good, vox_graded;       // its phrases, strong ones and all
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
    double miss_t[6];                   // a lane's last miss: its cup flashes red
    double wave_t;
    double strobe_t;                    // a chorus came in: the stage strobes
    int cue_i, cue_mode, cue_step;      // the chart's light show: the next cue, the look it's in, its keyframe
    double cue_t, flare_t, flare_len;   // when the look began; a flare's flash
    float kick;
    double shown_score;                 // what the counter shows, rolling up to the score
    char ach_text[48];                  // an achievement just earned, shown along the bottom
    double ach_t;                         // the camera's jolt: Star Power, a big moment (decays)                      // a streak milestone: a wave of light up the neck
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
    // the camera's shots of the venue (tools/venues.py SHOTS): 0 the wide one, then stage left, stage right, low front
    Tex * shot_tex[VENUE_SHOTS], * shot_glow[VENUE_SHOTS];
    const float (*shot_marks[VENUE_SHOTS])[3];
    int shots, shot;
    double shot_t;                      // the beat the camera cut to this shot on
    int focus_solo;                     // the solo (index + 1) the camera last cut for
    int shot_section;                   // the section the director last cut for
    bool shot_sp;                       // Star Power was on at the last look
    bool cam_flash;                     // the camera is Flash's own
    float fstep;                        // 60ths of a second this drawn frame stands for (2 at 30 fps)
    unsigned updates, updates_drawn;    // game_update calls, and how many there had been at the last frame drawn
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
    double sp_on_t;                     // when Star Power last went on: the band spins round with it
    double intro_end;                   // the camera's on the drummer for the count-in, and pulls back from here
    bool cam_snap;                      // the next frame puts the camera straight where it's going
    float fountain;                     // the Star Power sparklers along the stage front: 0 off .. 1 full
    float laser;                        // the lasers over the crowd: 0 off .. 1 full
    double crash_t;                     // the drummer's last crash cymbal (the chart's): the lights flash with it
    double start_t;                     // the song time the run began at (a practice run starts partway in)
    int metro_next;                     // practice's metronome: the chart's next beat line
    float song_speed;                   // the music's speed: practice's, or the Options' Song speed
} G;

// Guitar Flash's own rules, as its HTML5 engine has them (guitarflash.com's mainGame.js, read 2026-11-13). Its clock
// runs in 24ths of a second. A note comes on screen Y seconds early at the top and falls faster every tick: its speed
// grows by Z each 24th (y += speed; speed += Z, y kept to 0.05). Past y 270 it can be taken, past 350 it's missed: for
// Easy/Medium that's 125 ms early to 83 late, Hard 125/125, Expert 167/125. A key press arms its fret for two frames of
// a 24 fps animation (83 ms): a note in the zone meanwhile is taken, and a press that takes nothing is an error (the
// combo goes, the meter drops as for a miss). Each lane of a chord is its own note. A single note takes the frets held
// below it with it (anchoring); none above. The meter: 60 steps from 29, +1 a hit, -2 a miss or error, out at 0. The
// multiplier steps every 10 hits to x4 (x8 in Star Power), and Star Power lasts 7.4 s on half a bar, 11.5 on three
// quarters, 15.6 full, whatever the tempo
#define GF_TICK (1.0 / 24)
#define GF_ZONE 270.0f
#define GF_GONE 350.0f
#define GF_YN 200
static struct {
    bool on;                            // its rules (Note style Flash and Flash rules on, a fretted part)
    bool look;                          // its note motion (Note style Flash, a fretted part)
    bool step;                          // drawn in its 24 steps a second
    int diff;
    float Y, Z, y[GF_YN], y_hit;        // seconds on screen, speed added a tick, y after n ticks, y at the note's time
    double arm[6];                      // when each fret was pressed, while it's armed
    bool armed[6];
    float sp_rate;                      // Star Power used a second
    double last_t;
} GF;
static void gf_setup(void);
static void gf_flags(void);

static struct {
    bool on;
    double from, to;
    float speed;
    int notes;                          // in the stretch
    int ramp, target;                   // speed up by ramp % (0 off) after each clear, up to target %
    int runs, best;                     // times through, best % of notes
} practice;

// a battle: a computer rival plays the same chart, each side's own rock meter; Star Power phrases earn attacks
// Guitar Hero III's own set joins them: a broken string (a fret dead till it's tapped back to life), double notes,
// the whammy (the highway reels till you whammy it off) and amp overload (the highway shakes, the gems blink)
enum { ATK_LEFTY, ATK_BLACKOUT, ATK_DRAIN, ATK_SPEED, ATK_BROKEN, ATK_DOUBLE, ATK_WHAMMY, ATK_AMP, ATK_COUNT };
static const char * const atk_names[ATK_COUNT] = {"LEFTY FLIP", "BLACKOUT", "AMP DRAIN", "SPEED UP", "BROKEN STRING",
                                                  "DOUBLE NOTES", "WHAMMY", "AMP OVERLOAD"};
enum { BROKEN_TAPS = 5, DOUBLED_MAX = 160 };
static struct {
    bool armed, on;
    float skill;                        // the rival's chance of hitting an easy note
    float form;                         // its mood: up after a run of hits, down after misses (-0.12 .. +0.04)
    int next;                           // the rival's next note
    float rock;
    long score;
    int streak;
    char name[24];
    int mine[3], mine_n;                // attacks ready to throw
    int theirs[3], theirs_n;            // the rival's, held for the moment they hurt most
    double theirs_ready;                // the rival won't throw before this
    double warn_t;                      // a rival attack lands at warn_t (warned a second before)
    int warn_atk;
    int thrown, taken;                  // attacks the player threw, and took
    int hits, played;                   // the rival's notes hit, of those it played
    double lefty_until, blackout_until, speed_until;
    int broken_lane, broken_left;       // the dead fret (-1 none) and the taps it still needs
    float whammy_left;                  // how much whammying still shakes the Whammy attack off (1 .. 0)
    double whammy_until, amp_until;
    int doubled[DOUBLED_MAX], doubled_was[DOUBLED_MAX], doubled_n;  // notes Double Notes added a fret to, as they were
    double rival_hit_until;             // the player's attack still shaking the rival
    float rival_hit;                    // how much it's shaken (off its hit chance)
    float base_speed;
    int level;                          // the rival's, 0 easy .. 3 brutal
    double sp_until;                    // the rival's own Star Power: its points doubled till then
    int result;                         // 1 won, 2 lost
    unsigned seed;
} battle;
static void battle_fire(void);
static void battle_land(int a);
static void battle_undouble(void);
static int battle_pick(unsigned * seed);
static bool battle_proj(float out[16]);
static bool lefty_now(void);
static void battle_reset(void);
static void battle_update(void);
static void draw_battle(void);
static void popup_always(const char * text);
static void ghost_start(const Song * song, int part, int diff);
static void ghost_track(void);
static void ghost_finish(bool failed);
static void draw_ghost(void);
static void draw_flashes(void);

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
    return lefty_now() && lane < G.lanes ? G.lanes - 1 - lane : lane;
}

// the camera looks down +z, so world +x is on the screen's left: green (lane 0) gets the largest x
static float lane_x(int lane)
{
    return ((G.lanes - 1) / 2.0f - display_lane(lane)) * 5.0f / G.lanes;
}

static int multiplier(void)
{
    int max = G.part_id == PART_BASS && !GF.on ? 6 : 4;
    int m = 1 + G.streak / 10;
    if (m > max)
        m = max;
    return G.sp_active ? m * 2 : m;
}

// ------------------------------------------------------------------------------------------------------ the venue

// the song's band: one of each role's members, the same ones every time you play the song (YargLook's Member)
// a per-frame easing step k (at 60 fps) for this drawn frame, which may stand for more than one
static float ease(float k)
{
    return G.fstep <= 1 ? k : 1 - powf(1 - k, G.fstep);
}

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
    G.shots = 0;
    G.shot = 0;
    if (res_venue(zone, &G.venue, &G.venue_lights)) {
        G.venue_title = res_venue_title(zone);
        G.marks = res_venue_marks(zone);
        G.shot_tex[0] = G.venue;
        G.shot_glow[0] = G.venue_lights;
        G.shot_marks[0] = G.marks;
        G.shots = 1;
        // the other angles, all loaded now so a cut never waits on the card
        int want = cfg.still_camera ? 1 : res_venue_shot_count(zone);
        int64_t t0 = plat_now_us();
        for (int k = 1; k < want; k++) {
            if (!res_venue_shot(zone, k, &G.shot_tex[G.shots], &G.shot_glow[G.shots]))
                break;
            G.shot_marks[G.shots] = res_venue_shot_marks(zone, k);
            if (!G.shot_marks[G.shots])
                break;
            G.shots++;
        }
        if (G.shots > 1)
            plat_log("loaded: %d venue shots %d ms", G.shots - 1, (int)((plat_now_us() - t0) / 1000));
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
    {
        // they keep their spots clear of the board in every shot; a shot only makes them nearer or further
        const float (*wide)[3] = G.shot_marks[0] ? G.shot_marks[0] : G.marks;
        float ratio = wide[r][2] > 1 ? G.marks[r][2] / wide[r][2] : 1;
        ratio = ratio < 0.85f ? 0.85f : ratio > 1.3f ? 1.3f : ratio;
        m[2] = fminf(wide[r][2], 118) * 1.1f * ratio;
    }
    const BandPart * b = &G.band_part[r];
    m[0] += b->walk * m[2];
    m[1] += b->front * 26;
    m[2] *= 1 + b->front * 0.2f;
    // wherever they wander, out of the way of the board and the rock meter
    m[0] = r == 3 ? fminf(fmaxf(m[0], 222), 262) : fminf(fmaxf(m[0], 738), 858);
    if (r == 3 && G.band[1]) {
        // and never into the singer, who stands just right of them: half a body's width apart at least
        float v[3];
        member_spot(1, v);
        m[0] = fminf(m[0], v[0] - (v[2] + m[2]) * 0.42f);
    }
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
        float speed = 0.018f * G.fstep;
        b->walk += step > speed ? speed : step < -speed ? -speed : step;
        float out = band_soloing(r) ? 1 : 0;
        b->front += (out - b->front) * ease(0.06f);
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
            G.strobe_t = G.t;
            G.cheer = 1;            // the band leaps as the chorus hits
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
    if (since >= 0 && since < 3.5 && G.marks && !cfg.light_fx) {
        // after the jets, smoke rolls out across the front of the stage and thins away
        const Sprite * haze = atlas_get(res.stage, "haze");
        float k = (float)since / 3.5f;
        for (int j = 0; j < 5; j++) {
            float x = -120 + j * 260 + sinf((float)since * 0.6f + j) * 40 + (j & 1 ? 1 : -1) * k * 90;
            float y = 330 - k * 60 - (j % 3) * 18, w = 420 + k * 260, h = 120 + k * 70;
            uint32_t c = col3(G.stage_col, 1);
            gfx_sprite(haze, x - w / 2, y - h / 2, w, h, with_alpha(c, 0.5f * (1 - k) * (k < 0.1f ? k / 0.1f : 1)));
        }
    }
    // Star Power: fountains of sparks either side of the guitarists, as long as it lasts. Each spark is worked out from the
    // time alone (when it left the fountain, how fast, which way), so nothing is kept between frames
    G.fountain += ((G.sp_active ? 1 : 0) - G.fountain) * ease(0.08f);
    if (G.fountain > 0.02f && G.marks && !cfg.light_fx) {
        const Sprite * dot = atlas_get(res.stage, "dot");
        enum { SPARKS_EACH = 40 };
        const float life = 0.9f, g = 9.8f;
        gfx_blend(BLEND_ADD);
        for (int r = 2; r <= 3; r++)
            for (int side = -1; side <= 1; side += 2) {
                if (!G.band[r])
                    continue;
                float m[3];
                member_spot(r, m);
                float bx = m[0] + side * m[2] * 0.95f, by = m[1] + m[2] * 0.06f, ppm = m[2];
                for (int i = 0; i < SPARKS_EACH; i++) {
                    uint32_t h = (uint32_t)(i * 2 + 1) * 2654435761u ^ (uint32_t)(r * 4 + side + 7) * 40503u;
                    float phase = (float)fmod(G.t / life + (h >> 8 & 1023) / 1023.0, 1.0), age = phase * life;
                    // each time round it comes out another way
                    uint32_t k = h ^ (uint32_t)floor(G.t / life + (h >> 8 & 1023) / 1023.0) * 2246822519u;
                    float vy = 4.4f + (k >> 4 & 255) / 255.0f * 1.4f, vx = ((k >> 12 & 255) / 255.0f - 0.5f) * 0.7f;
                    float x = bx + vx * age * ppm, y = by - (vy * age - 0.5f * g * age * age) * ppm;
                    if (y > by)
                        continue;           // back on the stage
                    stage_point(&x, &y);
                    // bright as they leave, dimming as they fall
                    float a = (1 - phase * phase) * G.fountain, sz = (6 + (k >> 20 & 3)) * G.cam_z * ppm / 110;
                    uint32_t c = k & 1 ? RGBA(255, 226, 160, 255) : RGBA(190, 220, 255, 255);
                    gfx_sprite(dot, x - sz, y - sz, sz * 2, sz * 2, with_alpha(c, a));
                    gfx_sprite(dot, x - sz * 0.5f, y - sz * 0.5f, sz, sz, with_alpha(WHITE, a));
                }
                // the mouth of the fountain burns white
                float x = bx, y = by, w = 30 * G.cam_z * ppm / 110;
                stage_point(&x, &y);
                float fl = 0.85f + 0.15f * sinf((float)G.t * 37 + r + side);
                gfx_sprite(dot, x - w, y - w * 0.9f, w * 2, w * 1.4f, with_alpha(RGBA(200, 225, 255, 255), fl * G.fountain));
                gfx_sprite(dot, x - w * 0.4f, y - w * 0.5f, w * 0.8f, w * 0.6f, with_alpha(WHITE, fl * G.fountain));
            }
        gfx_blend(BLEND_ALPHA);
    }
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
    // a crash cymbal in the drummer's part hits the lights white for a moment
    float crash = G.t >= G.crash_t && G.band_loud > 0.03f ? expf((float)-(G.t - G.crash_t) * 8) : 0;
    level += 0.3f * crash;
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
        // where the beam meets the stage it leaves a pool of its light, wider and fainter the further it's come
        float fx = 480, fy = 305;
        stage_point(&fx, &fy);
        float len = 450 * G.cam_z, c = cosf(angle), along = c > 0.2f ? (fy - lamp_y[i]) / c : -1;
        if (along > 0 && along < len * 0.9f && !cfg.light_fx) {
            float px = lamp_x[i] - sinf(angle) * along, k = along / len, pw = (90 + 90 * k) * G.cam_z / c;
            gfx_sprite(atlas_get(res.stage, "dot"), px - pw / 2, fy - pw * 0.13f, pw, pw * 0.26f,
                       col3(G.key_col, fminf(a * 1.2f * (1 - 0.5f * k), 0.8f)));
        }
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
    if (crash > 0.03f) {
        float cs = (70 + 50 * crash) * G.cam_z;
        for (int i = 0; i < LAMPS; i++)
            gfx_sprite(dot, lamp_x[i] - cs / 2, lamp_y[i] - cs / 2 + 8, cs, cs, with_alpha(WHITE, 0.75f * crash));
    }
    gfx_blend(BLEND_ALPHA);
}

// lasers from behind the kit, fanned out over the crowd and sweeping with the beat: a chorus with the crowd behind you,
// and Star Power. They swing in from nothing and fade out the same way
static void draw_lasers(double beat, float pulse)
{
    bool cued = G.chart->cue_count && G.cue_mode != CUE_NONE;
    bool on = cued ? G.cue_mode == CUE_SEARCH || G.cue_mode == CUE_FRENZY || G.cue_mode == CUE_BRE || G.sp_active
                   : (G.big_section && G.rock >= 0.5f) || G.sp_active;
    G.laser += ((on ? 1 : 0) - G.laser) * ease(0.05f);
    if (G.laser < 0.02f || cfg.light_fx || !G.marks)
        return;
    enum { BEAMS = 5 };
    float sweep = sinf((float)beat * 3.14159265f / 4), spread = 0.26f + 0.06f * sinf((float)beat * 3.14159265f / 8);
    float col[3];
    for (int i = 0; i < 3; i++)                 // a laser's colour is pure: the key colour, its weakest channel taken out
        col[i] = G.key_col[i];
    float lo = fminf(col[0], fminf(col[1], col[2])), hi = fmaxf(col[0], fmaxf(col[1], col[2]));
    for (int i = 0; i < 3; i++)
        col[i] = hi > lo + 0.05f ? (col[i] - lo) / (hi - lo) : 1;
    if (G.sp_active)
        col[0] = 0.45f, col[1] = 0.75f, col[2] = 1;
    const Sprite * dot = atlas_get(res.stage, "dot");
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    gfx_blend(BLEND_ADD);
    // two heads on the truss, either side of the kit, fanned out over the band and swinging against each other so the
    // beams cross
    for (int side = -1; side <= 1; side += 2) {
        float ox = 480 + side * 170, oy = 58;
        stage_point(&ox, &oy);
        gfx_tex(NULL);
        for (int b = 0; b < BEAMS; b++) {
            float ang = (b - (BEAMS - 1) / 2.0f) * spread + side * (0.45f + 0.4f * sweep), len = 900 * G.cam_z;
            float dx = sinf(ang), dy = cosf(ang), nx = dy, ny = -dx;
            float w0 = 1.0f * G.cam_z, w1 = 6 * G.cam_z;
            float ex = ox + dx * len, ey = oy + dy * len;
            float a = G.laser * (0.3f + 0.25f * pulse);
            const float p[4][3] = {{ox - nx * w0, oy - ny * w0, 0}, {ox + nx * w0, oy + ny * w0, 0},
                                   {ex + nx * w1, ey + ny * w1, 0}, {ex - nx * w1, ey - ny * w1, 0}};
            uint32_t c0 = col3(col, a), c1 = col3(col, a * 0.1f);
            const uint32_t c[4] = {c0, c0, c1, c1};
            gfx_quad(p, uv, c);
        }
        // where they come from burns
        float sz = (26 + 16 * pulse) * G.cam_z;
        gfx_sprite(dot, ox - sz / 2, oy - sz / 2, sz, sz, col3(col, 0.7f * G.laser));
    }
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
    mood += G.cheer * 0.35f;            // a streak, a chorus: the crowd surges up
    gfx_tex(t);
    gfx_blend(BLEND_ALPHA);
    for (int row = 0; row < 3; row++) {
        float k = 0.4f + row * 0.3f;       // nearer rows are darker: the light's behind them
        for (int i = 0; i < 3; i++)
            lit[i] = (0.03f + G.stage_col[i] * 0.05f + G.key_col[i] * 0.14f) * (1.1f - k);
        uint32_t col = col3(lit, 1);
        float bounce = fabsf(sinf(3.14159265f * ((float)beat + row * 0.3f)));
        float h = 112 + row * 22, top = 544 - h * (0.62f + 0.38f * mood) + 14 * row - bounce * 14 * mood;
        int sheet = ((int)floor(beat * 0.5) + row) & 3;
        float x = -40 + sinf((float)G.t * (0.3f + row * 0.1f) + row) * 16 + row * 90.0f;
        const float wid = 1100;
        float v0 = sheet * 0.25f + 0.01f, v1 = v0 + 0.23f;
        const float uv[4][2] = {{0, v0}, {1, v0}, {1, v1}, {0, v1}};
        const float p[4][3] = {{x, top, 0}, {x + wid, top, 0}, {x + wid, top + h, 0}, {x, top + h, 0}};
        const uint32_t c[4] = {col, col, col, col};
        gfx_quad(p, uv, c);
    }
}

static void draw_flashes(void)
{
    // a slow song going well: lighters up in the crowd, swaying with the beat
    double bpm = G.t > 0 ? (chart_quarters(G.chart, G.t + 0.5) - chart_quarters(G.chart, G.t)) * 120 : 120;
    if (bpm > 1 && bpm < 96 && G.rock > 0.5f && G.t > 2) {
        const Sprite * dot = atlas_get(res.stage, "dot");
        double q = chart_quarters(G.chart, G.t);
        float sway = sinf((float)q * 3.14159265f / 2);
        gfx_blend(BLEND_ADD);
        for (int j = 0; j < 22; j++) {
            uint32_t h = (uint32_t)j * 2654435761u;
            float x = (h >> 12 & 1023) / 1023.0f * 280, y = 440 + (h >> 22 & 63) * 1.4f;
            if (h & 1)
                x = SCREEN_W - x;
            x += sway * (8 + (h >> 4 & 7));
            float fl = 0.75f + 0.25f * sinf((float)G.t * (9 + (h >> 8 & 7)) + j);
            gfx_sprite(dot, x - 9, y - 12, 18, 22, RGBA(255, 190, 90, (int)(200 * fl)));
            gfx_sprite(dot, x - 3, y - 6, 6, 9, RGBA(255, 250, 220, (int)(230 * fl)));
        }
        gfx_blend(BLEND_ALPHA);
    }
    // phones and cameras going off in the crowd: more the better it's going, a storm under Star Power
    {
        const Sprite * dot = atlas_get(res.stage, "dot");
        float rate = G.sp_active ? 1 : G.cheer > 0.25f ? 0.8f : G.rock > 0.66f ? 0.35f : G.rock > 0.33f ? 0.12f : 0;
        rate *= 0.4f + 0.6f * G.band_loud;
        int slot = (int)(G.t * 20);
        gfx_blend(BLEND_ADD);
        for (int k = 0; k < 4; k++) {
            int sl = slot - k;          // a flash lasts four twentieths of a second, fading
            for (int j = 0; j < 6; j++) {
                uint32_t h = (uint32_t)sl * 2654435761u + (uint32_t)j * 40503u;
                if ((h >> 8 & 1023) / 1023.0f > rate * 0.8f)
                    continue;
                // either side of the highway, where the crowd shows
                float x = (h >> 12 & 1023) / 1023.0f * 300, y = 400 + (h >> 22 & 127) * 1.05f, a = 1 - k / 4.0f;
                if (h & 1)
                    x = SCREEN_W - x;
                float sz = 10 + (h >> 4 & 15);
                gfx_sprite(dot, x - sz, y - sz, sz * 2, sz * 2, RGBA(255, 255, 255, (int)(230 * a)));
                gfx_sprite(dot, x - sz * 3, y - sz * 3, sz * 6, sz * 6, RGBA(200, 220, 255, (int)(60 * a)));
            }
        }
        gfx_blend(BLEND_ALPHA);
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
static float clamp01(float v);
// the chart's light show up to now: a look, a keyframe step, a flare or the jets
static void run_cues(void)
{
    const Chart * c = G.chart;
    if (!c->cue_count)
        return;
    if (G.cue_i > 0 && c->cues[G.cue_i - 1].time / 1e6 > G.t + 0.05) {
        // back in the song (practice, a restart): from the top
        G.cue_i = G.cue_step = 0;
        G.cue_mode = CUE_NONE;
    }
    static const struct { const char * name; int mode; } looks[] = {
        {"verse", CUE_VERSE}, {"chorus", CUE_CHORUS}, {"manual cool", CUE_COOL}, {"loop cool", CUE_COOL},
        {"manual warm", CUE_WARM}, {"loop warm", CUE_WARM}, {"dischord", CUE_DISCHORD}, {"stomp", CUE_STOMP},
        {"harmony", CUE_HARMONY}, {"frenzy", CUE_FRENZY}, {"silhouettes", CUE_SILHOUETTE},
        {"silhouettes spot", CUE_SILHOUETTE_SPOT}, {"searchlights", CUE_SEARCH}, {"sweep", CUE_SEARCH},
        {"strobe slow", CUE_STROBE_SLOW}, {"strobe fast", CUE_STROBE_FAST}, {"blackout slow", CUE_BLACK_SLOW},
        {"blackout fast", CUE_BLACK_FAST}, {"blackout spot", CUE_BLACK_SPOT}, {"bre", CUE_BRE}, {"intro", CUE_INTRO},
        {"default", CUE_NONE}};
    while (G.cue_i < c->cue_count && c->cues[G.cue_i].time / 1e6 <= G.t) {
        const char * n = c->cues[G.cue_i++].name;
        bool late = G.t - c->cues[G.cue_i - 1].time / 1e6 > 0.3;    // caught up after a seek: no flashes
        if (!strcmp(n, "next"))
            G.cue_step++;
        else if (!strcmp(n, "prev"))
            G.cue_step--;
        else if (!strcmp(n, "first"))
            G.cue_step = 0;
        else if (!strcmp(n, "bonusfx")) {
            if (!late && G.rock >= 0.4f) {
                G.pyro_t = G.t;
                party();
            }
        } else if (!strncmp(n, "flare", 5)) {
            if (!late) {
                G.flare_t = G.t;
                G.flare_len = strstr(n, "slow") ? 0.9 : 0.3;
            }
        } else
            for (size_t k = 0; k < sizeof(looks) / sizeof(looks[0]); k++)
                if (!strcmp(n, looks[k].name)) {
                    if (looks[k].mode != G.cue_mode)
                        G.cue_t = G.t;
                    G.cue_mode = looks[k].mode;
                    G.cue_step = 0;
                    break;
                }
    }
}

static void draw_stage(void)
{
    if (!G.venue && cfg.backdrop == 1 && G.art) {
        const Sprite cs = sprite_whole(G.art);
        gfx_sprite(&cs, 0, (SCREEN_H - SCREEN_W) / 2.0f - 60, SCREEN_W, SCREEN_W, RGBA(150, 150, 150, 255));
        return;
    }
    if (!G.venue || cfg.backdrop == 2) {
        if (cfg.backdrop == 2)
            gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(8, 8, 12, 255));
        else
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
    run_cues();
    if (G.chart->cue_count && G.cue_mode != CUE_NONE) {
        // the chart's own show: the look its lighting cue asks for, stepped on by its keyframes
        int st = ((G.cue_step % 3) + 3) % 3;
        float since = (float)(G.t - G.cue_t);
        switch (G.cue_mode) {
        case CUE_VERSE: case CUE_INTRO:
            pal = cool[st];
            inten = 0.8f;
            gl = 0.3f + 0.4f * G.band_loud + 0.25f * pulse;
            lvl = G.cue_mode == CUE_INTRO ? 0.38f : 0.5f;
            break;
        case CUE_CHORUS:
            pal = warm[st];
            gl = 0.55f + 0.4f * G.band_loud + 0.5f * pulse;
            lvl = 0.64f + 0.08f * pulse;
            break;
        case CUE_COOL: case CUE_WARM:
            pal = (G.cue_mode == CUE_COOL ? cool : warm)[st];
            break;
        case CUE_HARMONY:
            pal = cool[st];
            for (int i = 0; i < 3; i++) {
                key[i] = palettes[pal][0][i] * 0.6f + 0.4f;
                wash[i] = palettes[pal][1][i] * 0.6f + 0.35f;
            }
            gl = 0.4f + 0.15f * sinf(t * 1.3f);
            lvl = 0.52f;
            break;
        case CUE_DISCHORD:
            pal = ((int)floor(beat * 2) & 1) ? 0 : 3;      // red against green, clashing on the off-beats
            gl = 0.45f + 0.4f * (0.5f + 0.5f * sinf(t * 23));
            break;
        case CUE_STOMP:
            gl = 0.12f + 1.0f * pulse * pulse;
            lvl = 0.4f + 0.25f * pulse * pulse;
            break;
        case CUE_FRENZY: case CUE_BRE:
            pal = ((int)floor(beat * 2) % 6 + 6) % 6;
            gl = 0.7f + 0.4f * pulse;
            lvl = 0.62f + 0.08f * pulse;
            break;
        case CUE_SILHOUETTE: case CUE_SILHOUETTE_SPOT:
            // the stage in the dark, the lights blazing behind: the band in black against them
            pal = G.cue_mode == CUE_SILHOUETTE_SPOT ? warm[st] : cool[st];
            gl = 1.1f;
            lvl = 0.24f;
            break;
        case CUE_SEARCH:
            pal = 2;
            gl = 0.5f + 0.3f * pulse;
            lvl = 0.38f;
            break;
        case CUE_STROBE_SLOW: case CUE_STROBE_FAST:
            gl = 0.3f;
            lvl = 0.3f;
            break;
        case CUE_BLACK_SLOW: case CUE_BLACK_FAST: case CUE_BLACK_SPOT: {
            float d = clamp01(since / (G.cue_mode == CUE_BLACK_SLOW ? 1.2f : 0.08f));
            gl *= 1 - 0.9f * d;
            lvl *= 1 - (G.cue_mode == CUE_BLACK_SPOT ? 0.5f : 0.62f) * d;
            break;
        }
        default:
            break;
        }
        if (G.cue_mode != CUE_HARMONY) {
            memcpy(key, palettes[pal][0], sizeof(key));
            memcpy(wash, palettes[pal][1], sizeof(wash));
        }
    }
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
    // the director's cuts between the venue's shots (wide, stage left, stage right, low front): the count-in and the
    // last chord wide; Star Power from low down; a solo from the soloist's side; each new section its own angle, the big
    // ones low. A shot holds two bars at least, and between cuts the camera creeps in on it
    if (G.shots > 1) {
        int want = G.shot;
        bool force = false;
        int me = member_me();
        if (G.t < G.intro_end || G.t > G.last_note_end) {
            want = 0;
            force = G.t < G.intro_end;
        } else if (G.sp_active && !G.shot_sp) {
            want = 3;
            force = true;
        } else if (solo && G.focus_solo != G.solo_index + 1) {
            G.focus_solo = G.solo_index + 1;
            want = me == 2 ? 2 : me == 3 ? 1 : me == 0 ? 3 : 0;
            force = true;
        } else if (named && G.section != G.shot_section) {
            G.shot_section = G.section;
            if (G.big_section)
                want = 3;
            else {
                static const int pick[] = {1, 2, 0, 2, 1, 0};
                want = pick[(G.section * 7 + 3) % 6];
                if (want == G.shot)
                    want = (want + 1) % 3;
            }
        } else if (!named && G.t > 0) {
            static const int cycle[] = {0, 1, 0, 2, 0, 3};
            want = cycle[((int)floor(beat / 16) % 6 + 6) % 6];
        }
        G.shot_sp = G.sp_active;
        if (want >= G.shots)
            want = 0;
        if (want != G.shot && (force || beat - G.shot_t >= 8)) {
            G.shot = want;
            G.shot_t = beat;
            G.venue = G.shot_tex[want];
            G.venue_lights = G.shot_glow[want];
            G.marks = G.shot_marks[want];
            G.cam_snap = true;
        }
    }
    // the camera: the whole stage, and every other four bars closing in on one of the band as a director would, whoever
    // has the solo first. They stay where they are on the screen and grow, so nobody slides behind the highway
    static const int turns[4] = {2, 1, 3, 0};
    int slot = (int)floor(beat / 16);
    G.focus = -1;
    bool intro = G.t < G.intro_end;
    if (intro)
        ;                               // the whole band, close: the camera pulls back from it as the song comes in
    else if (solo)
        G.focus = member_me();
    else if (!G.sp_active && G.t > 0 && (slot & 1))
        G.focus = turns[(slot / 2) & 3];
    if (G.focus >= 0 && !G.band[G.focus])
        G.focus = -1;
    float zoom = 1.04f + 0.012f * G.band_loud, aim_x = 0, aim_y = 0;
    if (intro) {
        // held about the middle of the band, at chest height, so nobody slides off the edges as it widens
        zoom = 1.22f;
        aim_y = (272 - 205) * (zoom - 1);
    } else if (G.focus >= 0) {
        float m[3];
        member_spot(G.focus, m);
        zoom = 1.2f;
        aim_x = (480 - m[0]) * (zoom - 1);
        aim_y = (272 - (m[1] - m[2])) * (zoom - 1);
        float rx = 480 * (zoom - 1) - 14, ry = 272 * (zoom - 1) - 8;
        aim_x = aim_x > rx ? rx : aim_x < -rx ? -rx : aim_x;
        aim_y = aim_y > ry ? ry : aim_y < -ry ? -ry : aim_y;
    }
    if (G.shots > 1 && G.t >= G.intro_end) {
        // the slow push in on whatever shot it is (8 bars to its closest)
        float in = (float)((beat - G.shot_t) / 32);
        zoom *= 1 + 0.045f * (in < 0 ? 0 : in > 1 ? 1 : in);
    }
    // pulling back from the count-in is slower: the stage is revealed rather than cut to
    float glide = 1 - expf((float)-dt * (G.t < G.intro_end + 2.5 ? 0.9f : 1.4f));
    if (G.cam_snap) {
        glide = 1;
        G.cam_snap = false;
    }
    G.cam_tz += (zoom - G.cam_tz) * glide;
    G.cam_tx += (aim_x - G.cam_tx) * glide;
    G.cam_ty += (aim_y - G.cam_ty) * glide;
    // a jolt punches the camera in and shakes it, settling in half a second
    float kick = cfg.flashes < 2 ? G.kick : 0;
    G.cam_z = G.cam_tz * (1 + 0.05f * kick);
    G.cam_x = G.cam_tx + sinf((float)G.t * 0.23f) * 13 + sinf((float)G.t * 61) * 9 * kick;
    G.cam_y = G.cam_ty + sinf((float)G.t * 0.17f + 1) * 6 + sinf((float)G.t * 47 + 2) * 6 * kick;
    G.kick *= powf(0.9f, G.fstep);
    if (G.t > G.last_note_end && G.t > 0 && !G.end_party) {
        G.end_party = true;
        party();
        if (G.rock >= 0.5f && !G.failed) {
            // a song played well goes out with the jets and the lights hit white
            G.pyro_t = G.t;
            G.strobe_t = G.t;
            G.kick = 0.6f;
            G.cheer = 1;
        }
        sound_play(res.snd[SND_APPLAUSE], cfg.crowd_vol * 0.1f, false);
    }
    if (cfg.backdrop == 2 || (cfg.backdrop == 1 && !G.art)) {
        // dark: a black stage, the show's key colour glowing faintly from above on the beat
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(8, 8, 12, 255));
        gfx_tex(NULL);
        const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        float g = 0.18f + 0.12f * pulse * (0.5f + 0.5f * energy);
        uint32_t top = col3(G.key_col, g), clear = col3(G.key_col, 0);
        const float p[4][3] = {{0, 0, 0}, {SCREEN_W, 0, 0}, {SCREEN_W, SCREEN_H * 0.6f, 0}, {0, SCREEN_H * 0.6f, 0}};
        const uint32_t c[4] = {top, top, clear, clear};
        gfx_quad(p, uv, c);
        return;
    }
    if (cfg.backdrop == 1) {
        // the song's cover filling the screen behind a dark highway, the way Flash shows the band's photo: drifting
        // slowly, a touch brighter on each beat, lit by the show's wash
        const Sprite cs = sprite_whole(G.art);
        float z = 1.08f + 0.03f * sinf(t * 0.11f) + 0.012f * pulse, w = SCREEN_W * z;
        float x = (SCREEN_W - w) / 2 + sinf(t * 0.07f) * 18, y = (SCREEN_H - w) / 2 - 60 + cosf(t * 0.05f) * 14;
        float lit = (G.sp_active ? 0.78f : 0.55f) + 0.1f * pulse * energy;
        float c[3];
        for (int i = 0; i < 3; i++)
            c[i] = fminf(1, lit * (0.75f + 0.25f * G.stage_col[i]));
        gfx_sprite(&cs, x, y, w, w, col3(c, 1));
        if (!cfg.no_vignette)
            vignette();
        return;
    }
    stage_layer(G.venue, col3(G.stage_col, 1));
    // a whole screen of blending: not when it would add nothing
    if (G.venue_lights && fmaxf(G.light_col[0], fmaxf(G.light_col[1], G.light_col[2])) > 0.02f) {
        gfx_blend(BLEND_ADD);
        float crash = G.t >= G.crash_t && G.band_loud > 0.03f && !cfg.light_fx ? expf((float)-(G.t - G.crash_t) * 8) : 0;
        stage_layer(G.venue_lights, col3(G.light_col, fminf(0.72f + 0.28f * crash, 1)));
        gfx_blend(BLEND_ALPHA);
    }
    if (cfg.stage_screens)
        draw_screens(pulse);
    draw_haze();
    draw_lasers(beat, pulse);
    draw_band(beat);
    draw_rig(beat, pulse);
    draw_pyro();
    draw_crowd(beat);
    draw_party();
    if (!cfg.no_vignette)
        vignette();
    if (!cfg.no_crowd && !cfg.light_fx)
        draw_flashes();
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
// guitars and bass (tools/band.py GROOVE, STRUMS): the body grooves through its 4-beat loop in 16 steps, locked to
// the song's beat; each step has 3 frames of a strum (ready, struck, through) the chart drives. Between their parts
// they keep grooving, hand ready, as GH's band does
static int band_groove_cell(const BandPart * b, double phase, bool playing)
{
    int step = (int)(phase * 16) & 15, strum = 0;
    if (b->part && playing) {
        double since = G.t - b->last_hit;
        if (since >= 0 && since < 0.07)
            strum = 1;
        else if (since >= 0 && since < 0.2 && !(G.t > b->hit_t - 0.05))
            strum = 2;
    } else if (!b->part && playing) {
        // no chart for them: strumming eighths
        double e = fmod(phase * 8, 1);
        strum = e < 0.25 ? 1 : e < 0.6 ? 2 : 0;
    }
    return step * 3 + strum;
}

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
        if (r == 0 && (n->frets & 8) && !(n->flags & NOTE_TOM_G))
            G.crash_t = n->time / 1e6;          // the green cymbal: a crash
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
    G.band_loud += (loud - G.band_loud) * ease(0.12f);
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
        int r = order[i & 3];
        Tex * t = G.band[r];
        if (!t)
            continue;
        float m[3];
        member_spot(r, m);
        // cell size, pixels per metre and the cell's bottom edge in metres, as in tools/band.py
        float cw = r == 0 ? 266 : 170, ch = r == 0 ? 232 : 256, ppm = r == 0 ? 88 : 120, y0 = r == 0 ? -0.62f : -0.08f;
        // each plays when their part does (no music, no playing), and between parts holds still
        BandPart * bp = &G.band_part[r & 3];
        bool playing = band_playing(r, beat_len) && G.band_loud > 0.03f;
#ifndef __vita__
        if (playing != bp->was)
            plat_log("band: member %d %s at %.2f", r, playing ? "plays" : "stops", G.t);
        bp->was = playing;
#endif
        if (r >= 2)
            bp->cell = band_groove_cell(bp, phase, playing);
        else if (playing && r != 1)
            bp->cell = band_cell(r, bp, beat);
        int f = bp->cell;
        stage_point(&m[0], &m[1]);
        m[2] *= G.cam_z;
        float s = m[2] / ppm, w = cw * s * (1 - 0.16f * bp->turn), h = ch * s;
        // the kit stays where it stands; what's on their feet moves from the knees up, and a jump leaves the stage
        float bottom = m[1] - y0 * m[2] - (r ? hop : 0) * m[2];
        // Star Power goes on and the front line spins round in the air, one after the other: a card turning about its
        // middle, edge-on halfway, its back (the picture mirrored) at the half turn
        float spun = (float)(G.t - G.sp_on_t) - (r == 1 ? 0.12f : r == 3 ? 0.24f : 0);
        if (r && spun > 0 && spun < 0.6f) {
            float k = spun / 0.6f;
            k = k * k * (3 - 2 * k);
            w *= cosf(k * 2 * pi);
            bottom -= sinf(k * pi) * 0.22f * m[2];
        }
        float dip = (r ? bob * (playing ? 1 : 0.5f) + bp->pulse * 0.05f : bob * 0.3f + bp->pulse * 0.035f) * m[2] +
                    slump * m[2];
        // the guitars' bodies move in their pictures now: the card's own sway only adds a little
        float lean = (r ? (i & 1 ? sway : -sway) * (playing ? 1 : 0.4f) * (r >= 2 ? 0.45f : 1) : 0) * m[2];
        if (r >= 2) {
            // walking: the weight goes foot to foot. In a solo they rock back with it
            float going = fabsf(bp->walk - (((((uint32_t)(beat / 16) + 1) * 2654435761u * (r * 2 + 1)) >> 16 & 255) / 255.0f - 0.5f) * 1.3f) > 0.03f ? 1 : 0;
            bp->turn += (going - bp->turn) * ease(0.12f);
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
        bp->pulse *= powf(0.82f, G.fstep);
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
            // backlit: their own shape a little larger, added in the stage's key colour, swelling on the beat (whoever the
            // camera's on, or everyone under Star Power: a second draw of a member costs the Vita's fill rate)
            if ((G.focus == r || G.sp_active) && !cfg.light_fx) {
            gfx_blend(BLEND_ADD);
            band_frame(t, f, cw, ch, m[0], bottom + h * 0.01f, w * 1.07f, h * 1.035f, lean, dip,
                       col3(G.key_col, (0.3f + 0.45f * on_beat * G.band_loud) * (G.focus == r ? 1.4f : 1)));
            gfx_blend(BLEND_ALPHA);
            }
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

static void popup_always(const char * text)
{
    snprintf(G.popup, sizeof(G.popup), "%s", text);
    G.popup_t = G.t;
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
static const float * proj_now;          // the projection the highway is drawn with this frame (a battle may shake it)

// Flash is flat: its highway a trapezoid on the screen, narrow and tall up the middle, the background either side.
// The board's z runs straight up it (the far end at the top); a point's height lifts it, scaled like the lanes
#define F2_CX (SCREEN_W / 2.0f)
#define F2_TOP 118.0f                   // the far end's height on the screen
#define F2_LINE 466.0f                  // the strikeline's
#define F2_HW_TOP 60.0f                 // the highway's half-width there
#define F2_HW_LINE 252.0f

static float f2_hw(float y)
{
    return F2_HW_TOP + (y - F2_TOP) / (F2_LINE - F2_TOP) * (F2_HW_LINE - F2_HW_TOP);
}

static void project(float x, float y, float z, float * sx, float * sy)
{
    if (GF.look) {
        float ys = F2_LINE - (F2_LINE - F2_TOP) * z / Z_FAR, hw = f2_hw(ys);
        *sx = F2_CX - x / HALF_W * hw;     // the board's x runs right to left, as the camera sees it
        *sy = ys - y / HALF_W * hw;
        return;
    }
    const float * v = G.view, * p = proj_now ? proj_now : G.proj;
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
    G.cam_flash = cfg.gem_style == 1 && cfg.camera == 0;
    const float * eye = eyes[cfg.camera], at[3] = {0, 0, ats[cfg.camera]}, up[3] = {0, 1, 0};
    // Flash's own: low and close, the highway wide across the screen and the targets near its foot
    // its highway narrows to a fifth across its length (lanes 18 px apart at the top, 102 at the targets): low and close
    static const float flash_eye[3] = {0, 2.3f, -2.4f};
    float fat[3] = {0, 0, 4.2f};
    float fov = 40;
    if (G.cam_flash) {
        eye = flash_eye;
        memcpy((float *)at, fat, sizeof(fat));
        fov = 64;
    }
    mat_perspective(G.proj, fov, (float)SCREEN_W / SCREEN_H, 0.5f, 60);
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
    free(G.got_t);
    G.got_t = NULL;
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
    if (G.got_t)
        memset(G.got_t, 0, (p->note_count + 1) * sizeof(float));
    G.perfects = G.greats = G.goods = G.perfect_run = 0;
    G.rate_t = -100;    for (int i = 0; i < p->sp_count; i++)
        G.phrase_ok[i] = true;
    for (int i = 0; i < p->solo_count; i++)
        G.solo_hit[i] = 0;
    memset(G.sus, 0, sizeof(G.sus));
    for (int i = 0; i < MAX_SUS; i++)
        G.sus[i].note = -1;
    G.first = 0;
    G.score = 0;
    G.vox_points = 0;
    G.vox_good = G.vox_graded = 0;
    G.streak = G.best_streak = G.hits = G.misses = G.overhits = G.sp_hit = 0;
    G.off_sum = G.off_sq = 0;
    G.off_n = 0;
    G.early = G.on_time = G.late = 0;
    G.hit_log_n = 0;
    for (int i = 0; i < 6; i++)
        G.press_t[i] = -100;
    G.act_t = -100;
    G.sp = 0;
    gf_setup();
    gf_flags();
    G.rock = G.needle = GF.on ? 29 / 60.0f : 0.5f;     // Flash's meter starts at 29 of its 60
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
    // back to the wide shot
    if (G.shots > 0) {
        G.shot = 0;
        G.venue = G.shot_tex[0];
        G.venue_lights = G.shot_glow[0];
        G.marks = G.shot_marks[0];
    }
    G.shot_t = -1e9;
    G.shot_section = -1;
    G.shot_sp = false;
    G.focus_solo = 0;
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
    G.sp_on_t = -100;
    G.cam_snap = true;
    G.fountain = G.laser = 0;
    G.crash_t = -100;
    for (int i = 0; i < 6; i++) {
        G.flame_t[i] = -100;
        G.miss_t[i] = -100;
    }
    G.wave_t = -100;
    G.kick = 0;
    G.shown_score = 0;
    G.ach_t = -100;
    G.strobe_t = -100;
    G.cue_i = 0;
    G.cue_mode = CUE_NONE;
    G.cue_step = 0;
    G.cue_t = G.flare_t = -100;
    G.held = 0;
    G.touch_count = 0;
    double first = p->note_count ? p->notes[0].time / 1e6 : 0;
    G.lead_in = (GF.look ? GF.Y : Z_FAR / G.speed) + 0.6 - first;
    if (G.lead_in < 1.2)
        G.lead_in = 1.2;
    G.t = -G.lead_in;
    // the count-in is on the drummer, and as the song comes in the camera pulls back to the whole stage
    G.intro_end = first - 1.2 < G.t + 2.6 ? first - 1.2 : G.t + 2.6;
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
        G.intro_end = -100;             // straight to the notes: no camera show
    }
    G.start_t = G.t;
    G.metro_next = 0;
    while (G.metro_next < G.chart->beat_count && G.chart->beats[G.metro_next].time / 1e6 < G.t)
        G.metro_next++;
    static const int band_parts[4] = {PART_DRUMS, -1, PART_GUITAR, PART_BASS};
    for (int r = 0; r < 4; r++) {
        memset(&G.band_part[r], 0, sizeof(G.band_part[r]));
        for (int d = 3; r != 1 && d >= 0 && !G.band_part[r].part; d--)
            G.band_part[r].part = chart_part(G.chart, band_parts[r], d);
    }
    set_my_stem(1);
    audio_song_fade(1, 0);
    G.song_speed = practice.on ? practice.speed : (cfg.song_speed >= 50 && cfg.song_speed <= 150 ? cfg.song_speed : 100) / 100.0f;
    audio_song_speed(G.song_speed);
    battle_reset();
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
    battle_undouble();         // the last song's doubled notes, back before its chart goes
    free_song();
    char path[400];
    res_wait_sounds();
    snprintf(path, sizeof(path), "%s/notes.vhc", song->folder);
    G.chart = chart_load(path);
    if (!G.chart)
        return false;
    if (!G.chart->cue_count)            // converted before the charts kept their light shows: from the song's own chart
        G.chart->cue_count = cues_load(song->folder, G.chart, &G.chart->cues);
#ifndef __vita__
    plat_log("lighting: %d cues", G.chart->cue_count);
    for (int i = 0; getenv("GHV_CUELOG") && i < G.chart->cue_count; i++)
        plat_log("cue %.3f %s", G.chart->cues[i].time / 1e6, G.chart->cues[i].name);
#endif
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
    G.beat_best = song->best[part][diff].score;
    battle.on = battle.armed && !practice.on;
    battle.armed = false;
    battle.base_speed = 0;      // the highway speed is read afresh
    ghost_start(song, part, diff);
    const Part * p = G.part;
    G.state = malloc(p->note_count + 1);
    G.got = malloc(p->note_count + 1);
    G.got_t = calloc(p->note_count + 1, sizeof(float));
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
    practice.ramp = 0;
    practice.target = 100;
    practice.runs = practice.best = 0;
}

static int battle_level = -1;          // the next battle's rival, -1 the Options' choice

void game_battle(int level)
{
    battle.armed = true;
    battle_level = level;
}

void game_practice_ramp(int step_pct, int target_pct)
{
    practice.ramp = step_pct;
    practice.target = target_pct;
}

bool game_practice_info(int * speed_pct, int * runs, int * best_pct)
{
    if (speed_pct)
        *speed_pct = (int)(practice.speed * 100 + 0.5f);
    if (runs)
        *runs = practice.runs;
    if (best_pct)
        *best_pct = practice.best;
    return practice.on;
}

void game_restart(void)
{
    reset_play();
}

void game_stop(void)
{
    if (battle.on && battle.base_speed > 0)
        G.speed = battle.base_speed;
    battle.on = false;
    battle.base_speed = 0;
    if (G.drums)
        plat_log("drums: %d freestyle hits", G.freestyle);
    practice.on = false;
    audio_song_speed(1);
    memset(G.band, 0, sizeof(G.band));
    res_band_free();
    battle_undouble();
    // the venue's other shots go with the song (the wide one stays cached for the next)
    if (G.shots > 1) {
        Tex * a, * b;
        res_venue_shot(NULL, 1, &a, &b);
        G.venue = G.shot_tex[0];
        G.venue_lights = G.shot_glow[0];
        G.marks = G.shot_marks[0];
    }
    G.shots = 0;
    free_song();
    tex_free(G.art);
    G.art = NULL;
    plat_set_perf(0);
    game_log_whammy();
}

int game_battle_state(void)
{
    return battle.on ? (battle.result ? battle.result : 3) : 0;
}

void game_playing(int * part, int * diff)
{
    *part = G.part_id;
    *diff = G.diff;
}

// the section the song is in now (the chart's name for it), "" when the chart names none
void game_section_now(char * out, int size)
{
    const Chart * c = G.chart;
    out[0] = 0;
    if (!c || !c->section_count)
        return;
    int k = -1;
    for (int i = 0; i < c->section_count && c->sections[i].time / 1e6 <= G.t; i++)
        k = i;
    if (k >= 0)
        snprintf(out, size, "%s", c->sections[k].name);
}

void game_status(int * song_pct, int * notes_pct)
{
    double start = 0, end = G.last_note_end > 0 ? G.last_note_end : 1;
    if (practice.on)
        start = practice.from, end = practice.to;   // how far through the stretch, not the song
    double t = G.t - start, len = end - start > 0.01 ? end - start : 0.01;
    *song_pct = t <= 0 ? 0 : t >= len ? 100 : (int)(t * 100 / len);
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

float game_song_speed(void)
{
    return G.song_speed > 0 ? G.song_speed : 1;
}

Result game_result(void)
{
    Result r;
    memset(&r, 0, sizeof(r));
    ghost_finish(G.failed);
    if (G.sing && !G.vocal) {
        r.vox_points = (int)G.vox_points;
        r.vox_good = G.vox_good;
        r.vox_graded = G.vox_graded;
    }
    if (battle.on) {
        if (!battle.result)
            battle.result = (long)G.score >= battle.score ? 1 : 2;
        r.battle = battle.result;
        r.rival_score = (int)battle.score;
        r.attacks_thrown = battle.thrown;
        r.attacks_taken = battle.taken;
        r.rival_pct = battle.played ? battle.hits * 100 / battle.played : 0;
        r.rival_level = battle.level;
        snprintf(r.rival_name, sizeof(r.rival_name), "%s", battle.name);
    }
    r.part = G.part_id;
    r.diff = G.diff;
    r.score = (int)G.score;
    r.stars = stars_for(G.score);
    r.notes_hit = G.hits;
    r.notes_total = G.vocal ? vox_phrases() : G.part ? G.part->note_count : 0;
    r.best_streak = G.best_streak;
    r.overhits = G.overhits;
    r.misses = G.misses;
    r.perfects = G.perfects;
    r.greats = G.greats;
    r.goods = G.goods;
    if (!G.vocal && r.notes_total > 0) {
        double pts = G.perfects + 0.7 * G.greats + 0.4 * G.goods;
        r.accuracy_x10 = (int)(pts * 1000 / r.notes_total + 0.5);
        int a = r.accuracy_x10;
        const char * rk = G.failed ? "F" : a >= 980 && G.hits == r.notes_total ? "S+" : a >= 950 ? "S" : a >= 900 ? "A" :
                          a >= 800 ? "B" : a >= 700 ? "C" : "D";
        snprintf(r.rank, sizeof(r.rank), "%s", rk);
    }
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
    r.early = G.early;
    r.on_time = G.on_time;
    r.late = G.late;
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

// Flash's numbers for a difficulty (its levels d c b a are Easy to Expert)
static void gf_setup(void)
{
    static const float Ys[4] = {1.666667f, 1.666667f, 1.9583f, 2.25f}, Zs[4] = {0.36f, 0.36f, 0.26f, 0.2f};
    int d = G.diff < 0 ? 0 : G.diff > 3 ? 3 : G.diff;
    GF.diff = d;
    GF.Y = Ys[d];
    GF.Z = Zs[d];
    float speed = GF.Z, y = 0;
    for (int n = 0; n < GF_YN; n++) {
        speed += GF.Z;
        y += speed;
        float j = floorf(y * 10) / 10;  // its fixFlash: y kept to 0.05
        y = y - j > 0.05f ? j + 0.05f : j;
        GF.y[n] = y;
    }
    GF.y_hit = GF.y[(int)lrintf(GF.Y * 24)];
    for (int l = 0; l < 6; l++)
        GF.armed[l] = false;
    GF.sp_rate = 0;
    GF.last_t = G.t;
}

static void gf_flags(void)
{
    bool frets = !G.drums && !G.vocal;
    GF.look = cfg.gem_style == 1 && frets;
    GF.on = GF.look && cfg.flash_rules && !battle.on;
    GF.step = !cfg.flash_motion;
}

// ticks since a note at time nt came on screen, at time t: on its clock (24ths, rounded) or smooth
static double gf_ticks(double nt, double t, bool step)
{
    double start = nt - GF.Y;
    if (step)
        return floor((floor(t * 24 + 0.5) / 24 - start) * 24 + 0.5);
    return (t - start) * 24;
}

static float gf_y(double n)
{
    if (n < 0)
        return -1;
    int i = (int)n;
    if (i >= GF_YN - 1)
        return GF.y[GF_YN - 1];
    return GF.y[i] + (GF.y[i + 1] - GF.y[i]) * (float)(n - i);
}

// where on the highway something due at time `when` is drawn now. Flash's notes come down its curve: the board's z
// runs straight up its flat highway, so its y (0 at the top, y_hit on the targets) sets z directly
static float time_z(double when)
{
    if (!GF.look)
        return (float)((when - G.t) * G.speed);
    double n = gf_ticks(when, G.t, GF.step);
    if (n < 0)
        return Z_FAR + 1 + (float)(-n) * 0.1f;  // not on screen yet
    return Z_FAR * (1 - gf_y(n) / GF.y_hit);
}

// is a note at time nt on the highway yet
static bool on_highway(double nt)
{
    if (GF.look)
        return nt - G.t <= GF.Y + GF_TICK;
    return (nt - G.t) * G.speed <= Z_FAR;
}

static void rock_change(float d)
{
    if (d < 0 && cfg.hard_rock)
        d *= 2;                         // Hard Rock: the crowd turns twice as fast
    G.rock += d;
    if (G.rock > 1)
        G.rock = 1;
    if (G.rock <= 0) {
        G.rock = 0;
        if ((!cfg.nofail || battle.on) && !G.autoplay && !practice.on) {
            G.failed = true;
            if (battle.on)
                battle.result = 2;
        }
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
    G.rate_kind = 3;
    G.rate_t = G.t;
    G.perfect_run = 0;
    if (cfg.sudden_death && !G.autoplay && !practice.on && !G.vocal)
        rock_change(-2);                // Sudden Death: one note missed and the song's over
#ifndef __vita__
    {
        const Note * n = &G.part->notes[i];
        if (getenv("GHV_MISSLOG"))
            plat_log("MISS t=%.3f frets=%02x flags=%02x prev=%.3f prevf=%02x prevstate=%d held=%02x", n->time / 1e6, n->frets, n->flags,
                     i ? G.part->notes[i - 1].time / 1e6 : 0, i ? G.part->notes[i - 1].frets : 0, i ? G.state[i - 1] : -1, G.held);
    }
#endif
    mark_judged(G.part->notes[i].frets);
    for (int l = 0; l < 6; l++)
        if (G.part->notes[i].frets & (1 << l))
            G.miss_t[l] = G.t;
    G.misses++;
    break_streak();
    G.miss_flash = 1;
    G.miss_pulse = 1;
    static const float loss[4] = {0.025f, 0.03f, 0.04f, 0.05f};
    // drums have many more gems to the bar, and a tremolo a dozen to the second
    if (GF.on)
        rock_change(-2 / 60.0f);
    else
        // in Star Power the crowd's with you: a miss costs half, as in GH
        rock_change(-loss[G.diff] * (G.roll && G.roll[i] ? 0.25f : G.drums ? 0.5f : 1) * (G.sp_active ? 0.5f : 1));
    if (G.phrase[i] >= 0 && G.phrase_ok[G.phrase[i]]) {
        G.phrase_ok[G.phrase[i]] = false;
    }
}

static void hit_note(int i)
{
    const Note * n = &G.part->notes[i];
    G.state[i] = HIT;
    {
        // how close it was: Perfect, Great or Good (a run's gems going by on their own are on time)
        // buttons are read once a frame: a press was made half a frame, on average, before it's seen
        float off = (float)(G.t - n->time / 1e6) - 1 / 120.0f, a = fabsf(off);
        int kind = a <= RATE_PERFECT ? 0 : a <= RATE_GREAT ? 1 : 2;
        G.perfect_run = kind == 0 ? G.perfect_run + 1 : 0;
        if (kind == 0)
            G.perfects++;
        else if (kind == 1)
            G.greats++;
        else
            G.goods++;
        G.rate_kind = kind;
        G.rate_off = off;
        G.rate_t = G.t;
    }
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
        for (int k = 0; k < 7; k++) {
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
    if (G.beat_best && G.score > G.beat_best && !G.autoplay) {
        // past your best on this chart, mid-song
        G.beat_best = 0;
        popup("NEW HIGH SCORE!");
        G.cheer = 1;
        sound_play(res.snd[SND_CHEER1 + rand() % 3], cfg.crowd_vol * 0.08f, false);
    }
    rock_change(GF.on ? 1 / 60.0f : G.sp_active ? 0.036f : 0.018f);     // Star Power fills it twice as fast
    for (int l = 0; l < 6 && struck; l++)
        if (n->frets & (1 << l))
            G.flame_t[l] = G.t;
    if (G.streak > 0 && G.streak % 50 == 0) {
        char t[32];
        snprintf(t, sizeof(t), "%d NOTE STREAK!", G.streak);
        popup(t);
        G.wave_t = G.t;
        // a streak achievement earned right now: said now, not only at the results
        const char * id = G.streak == 100 ? "streak100" : G.streak == 500 ? "streak500" : G.streak == 1000 ? "streak1000" : NULL;
        if (id && !G.autoplay && !practice.on && !achieve_has_id(id)) {
            snprintf(G.ach_text, sizeof(G.ach_text), "ACHIEVEMENT: %s", achieve_name(id));
            G.ach_t = G.t;
        }
        if (G.streak % 100 == 0)
            G.kick = 0.6f;
        G.cheer = 1;
        sound_play(res.snd[SND_CHEER1 + rand() % 3], cfg.crowd_vol * 0.05f, false);
        if (G.streak % 100 == 0)
            party();
    }
    if (G.solo[i] >= 0)
        G.solo_hit[G.solo[i]]++;
    // a whole Star Power phrase: a quarter of the meter
    int ph = G.phrase[i];
    if (battle.on && ph >= 0 && (n->flags & NOTE_SP_END) && G.phrase_ok[ph]) {
        // a battle: the phrase is an attack, not Star Power
        if (battle.mine_n < 3)
            battle.mine[battle.mine_n++] = battle_pick(&battle.seed);
        G.sp_hit++;
        popup("ATTACK READY!");
        sound_play(res.snd[SND_SP_READY], 0.7f, false);
    } else if (ph >= 0 && (n->flags & NOTE_SP_END) && G.phrase_ok[ph]) {
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

// Thumbs: the snare and the hats split the D-pad, the toms the buttons, the kick under the fingers.
// Shoulders: the snare on L or D-pad down, the kick on R, and the other pads in pairs — the hats on the
// D-pad's top and left (right too on four pads), the low pad on the right, blue up-left on the buttons
// and green down-right. Every pad has two buttons but the low one.
// Guitar: each pad where that colour sits on the guitar, so the fingers already know the way: the snare
// on Left, the hats on Up or Triangle, blue on Circle, green on L, orange on R — and the kick wherever
// it fits, R too when there's no orange
static uint32_t drum_buttons(int lane)
{
    static const uint32_t four[4] = {BTN_LEFT | BTN_DOWN, BTN_UP | BTN_TRIANGLE, BTN_SQUARE | BTN_CIRCLE, BTN_L};
    static const uint32_t five[5] = {BTN_LEFT | BTN_DOWN, BTN_UP | BTN_TRIANGLE, BTN_SQUARE | BTN_CIRCLE, BTN_RIGHT,
                                     BTN_L};
    const uint32_t four_s[4] = {BTN_L | BTN_DOWN, BTN_UP | BTN_LEFT | BTN_RIGHT, BTN_TRIANGLE | BTN_SQUARE,
                                BTN_CIRCLE | BTN_CROSS};
    const uint32_t five_s[5] = {BTN_L | BTN_DOWN, BTN_UP | BTN_LEFT, BTN_TRIANGLE | BTN_SQUARE, BTN_RIGHT,
                                BTN_CIRCLE | BTN_CROSS};
    static const uint32_t four_g[4] = {BTN_LEFT, BTN_UP | BTN_TRIANGLE, BTN_CIRCLE, BTN_L};
    static const uint32_t five_g[5] = {BTN_LEFT, BTN_UP | BTN_TRIANGLE, BTN_CIRCLE, BTN_R, BTN_L};
    if (lane == LANE_OPEN)
        return cfg.drum_layout == 1 ? BTN_R
                                    : cfg.drum_layout == 2 ? (G.lanes == 4 ? BTN_R | BTN_CROSS | BTN_DOWN
                                                                           : BTN_CROSS | BTN_DOWN)
                                                           : BTN_R | BTN_CROSS;
    if (lane >= G.lanes)
        return 0;
    if (cfg.drum_layout == 1)
        return G.lanes == 4 ? four_s[lane] : five_s[lane];
    if (cfg.drum_layout == 2)
        return G.lanes == 4 ? four_g[lane] : five_g[lane];
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

// how late note i can still be hit: the window, but no further than halfway to the next note (a chord's notes are one;
// a tremolo's and drums' gems keep the whole window, their own rules see to them)
static double win_late(int i)
{
    const Part * p = G.part;
    double w = WINDOW, nt = p->notes[i].time;
    if (G.drums || (G.roll && G.roll[i]))
        return w;
    for (int k = i + 1; k < p->note_count; k++)
        if (p->notes[k].time > nt) {
            double half = (p->notes[k].time - nt) / 2e6;
            return half < 0.035 ? (w < 0.035 ? w : 0.035) : half < w ? half : w;
        }
    return w;
}

// the note a press plays, if there's one in reach
static bool press_note(int lane)
{
    const Part * p = G.part;
    uint8_t bit = lane == LANE_OPEN ? FRET_OPEN : 1 << lane;
    for (int i = G.first; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        double nt = n->time / 1e6;
        if (nt > G.t + WIN_EARLY)
            break;
        if (G.state[i] != PENDING || nt < G.t - win_late(i) || !(n->frets & bit) || (G.got[i] & bit))
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
        // a chord's keys come down together: a key pressed long after the others starts it again from the frets
        // still held (a slow roll across the keys isn't a chord)
        if (G.got[i] && G.got_t && G.t - G.got_t[i] > CHORD_SPREAD)
            G.got[i] = G.drums ? 0 : (uint8_t)(G.held & n->frets & ~bit);
        if (!G.got[i] && G.got_t)
            G.got_t[i] = (float)G.t;
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
            if (off < -0.03) G.early++; else if (off > 0.03) G.late++; else G.on_time++;
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
        if (nt > G.t + HOPO_WIN)
            return;
        if (G.state[i] != PENDING || nt < G.t - fmin(HOPO_WIN, win_late(i)))
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
        if (off < -0.03) G.early++; else if (off > 0.03) G.late++; else G.on_time++;
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
    if (battle.on) {
        battle_fire();
        return;
    }
    if (G.sp_active || G.sp < 0.5f)
        return;
    G.sp_active = true;
    if (GF.on) {
        // Flash's Star Power lasts by the clock: 7.4 s on half a bar, 11.5 on three quarters, 15.6 on a full one
        float s = G.sp, secs = s <= 0.75f ? 7.375f + (s - 0.5f) / 0.25f * 4.0833f : 11.4583f + (s - 0.75f) / 0.25f * 4.125f;
        GF.sp_rate = s / secs;
    }
    if (!GF.on)
        rock_change(0.1f);              // GH: Star Power lifts the meter, the way to save a song from the red
    G.kick = 1;
    G.sp_on_t = G.t;
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
    const Part * p = G.part;
    int i = G.first;
    while (i < p->note_count && G.state[i] != PENDING)
        i++;
    if (i == 0 || i >= p->note_count)
        return;
    const Note * n = &p->notes[i], * prev = &p->notes[i - 1];
    double nt = n->time / 1e6;
    if (G.t < nt || G.t > nt + fmin(HOPO_WIN, win_late(i)) || (n->frets & FRET_OPEN) || G.state[i - 1] != HIT)
        return;
    // a finger must have moved since the note before (pressed for it, or lifted after it): frets held through
    // the song never chain a run on their own, however early the hand let go of the last chord
    if (G.act_t < prev->time / 1e6 - WINDOW)
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

static void solo_ends(void)
{
    const Part * p = G.part;
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

// a press that took nothing: Flash's error, a miss of its own
static void gf_error(void)
{
    G.overhits++;
    break_streak();
    rock_change(-2 / 60.0f);
    G.miss_flash = 0.7f;
    if (!cfg.no_miss_sound)
        sound_play(res.snd[SND_OVERSTRUM1 + rand() % 4], 0.5f, false);
}

// Guitar Flash's judging (see GF): presses arm frets, notes in the zone take them, each tick of its clock
static void gf_judge(uint32_t pressed_lanes)
{
    const Part * p = G.part;
    if (pressed_lanes)
        G.act_t = G.t;
    for (int l = 0; l < 6; l++)
        if (pressed_lanes & (1 << l)) {
            G.press_t[l] = G.t;
            GF.arm[l] = G.t;
            GF.armed[l] = true;
        }
    for (int i = G.first; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        double nt = n->time / 1e6;
        float y = gf_y(gf_ticks(nt, G.t, true));
        if (y <= GF_ZONE)
            break;                      // this one and all after it are still above the zone
        if (G.state[i] != PENDING)
            continue;
        bool single = __builtin_popcount(n->frets) == 1;
        for (int l = 0; l < 6; l++) {
            uint8_t bit = l == LANE_OPEN ? FRET_OPEN : 1 << l;
            if (!(n->frets & bit) || (G.got[i] & bit) || !GF.armed[l])
                continue;
            G.got[i] |= bit;
            GF.armed[l] = false;
            // a single note takes the frets held below it along (anchoring), without an error
            for (int k = 0; single && l != LANE_OPEN && k < l; k++)
                GF.armed[k] = false;
        }
        if (G.got[i] == n->frets) {
            hit_note(i);
            double off = G.t - nt;
            G.hit_log[G.hit_log_n++ % HIT_LOG] = (float)off;
            G.off_sum += off;
            G.off_sq += off * off;
            if (off < -0.03) G.early++; else if (off > 0.03) G.late++; else G.on_time++;
            G.off_n++;
        }
    }
    // a press whose two frames ran out with nothing taken
    for (int l = 0; l < 6; l++)
        if (GF.armed[l] && G.t - GF.arm[l] >= 2 * GF_TICK - 1e-6) {
            GF.armed[l] = false;
#ifndef __vita__
            if (getenv("GHV_MISSLOG"))
                plat_log("GFERR t=%.3f lane=%d armed_at=%.3f held=%02x", G.t, l, GF.arm[l], G.held);
#endif
            if (!G.autoplay)
                gf_error();
        }
    // past the zone: missed
    while (G.first < p->note_count) {
        int i = G.first;
        if (G.state[i] == PENDING) {
            if (gf_y(gf_ticks(p->notes[i].time / 1e6, G.t, true)) <= GF_GONE)
                break;
            miss_note(i);
        }
        G.first++;
    }
    solo_ends();
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
            if (G.t <= nt + win_late(i))
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
    solo_ends();
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
    G.vox_points += points * mult;
    G.vox_graded++;
    G.vox_good += grade >= 0.8f;
    G.vox_run = grade >= 0.8f ? G.vox_run + 1 : 0;
    if (!G.vocal) {
        // singing along: the instrument keeps the meter and the streak, the voice only adds its points
        // the singer's word shows even with popups off: with two playing, it's the singer's only feedback
        char t[32];
        snprintf(t, sizeof(t), "VOCALS %s", words[w]);
        popup_always(t);
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
    if (cfg.no_count_in || G.first >= p->note_count || G.count_done >= 4)
        return;
    // the first note of the run: a practice run starts at its stretch, not the song's first note
    double first = p->notes[G.first].time / 1e6;
    double beat = 60.0 / ((chart_quarters(G.chart, first + 0.5) - chart_quarters(G.chart, first)) * 120 + 1e-6);
    double at = first - (4 - G.count_done) * beat;
    if (first - G.start_t > 60 || (G.count_done == 0 && at < G.start_t)) {
        G.count_done = 4;
        return;
    }
    if (G.t >= at) {
        G.count_done++;
        sound_play(res.snd[SND_DRUM_HAT], 0.55f, false);
    }
}

// practice's metronome: a tick on every beat the chart marks, the bar's first louder
static void metronome(void)
{
    if (!practice.on || !cfg.metronome || G.failed)
        return;
    const Chart * c = G.chart;
    bool ticked = false;
    while (G.metro_next < c->beat_count && c->beats[G.metro_next].time / 1e6 <= G.t) {
        const BeatLine * b = &c->beats[G.metro_next++];
        // a frame late is fine; a hitch that skipped several isn't worth a burst of them
        if (!ticked && G.t - b->time / 1e6 < 0.1) {
            sound_play(res.snd[SND_DRUM_HAT], b->kind == 0 ? 0.7f : 0.38f, false);
            ticked = true;
        }
    }
}

int game_update(const Pad * pad, uint32_t pressed)
{
    if (G.paused || !G.part)
        return GAME_PAUSE;
    G.t = audio_song_time() - cfg.offset_ms / 1000.0;
#ifndef __vita__
    if (getenv("GHV_CLOCKLOG")) {
        static double lt, lw;
        double w = plat_now_us() / 1e6;
        if (lw > 0 && G.t > 0)
            plat_log("CLK %.6f %.6f", G.t - lt, w - lw);
        lt = G.t;
        lw = w;
    }
#endif
    ghost_track();
#ifndef __vita__
    {
        // tests: GHV_ATTACK=n lands attack n on the player four seconds in (a battle; GHV_BATTLE=1)
        static const Part * tested;
        const char * want = getenv("GHV_ATTACK");
        if (want && battle.on && tested != G.part && G.t > 4) {
            tested = G.part;
            bool bot = G.autoplay;
            G.autoplay = false;             // the broken string sticks even under the bot
            battle_land(atoi(want) % ATK_COUNT);
            G.autoplay = bot;
        }
    }
#endif
    if (battle.on && !G.failed && !battle.result) {
        battle_update();
        if (battle.rock <= 0) {
            // the rival's out: the song's won
            battle.result = 1;
            popup_always("YOU WIN!");
            G.done = true;
            return GAME_DONE;
        }
    }
    // failed: the song winds down like a tape, the board goes red, then the results
    if (G.failed) {
        if (G.fail_frames == 0) {
            sound_play(res.snd[SND_BOO1 + rand() % 3], cfg.crowd_vol * 0.1f, false);
            audio_song_fade(0, FAIL_FRAMES / 60.0f);
        }
        if (++G.fail_frames >= FAIL_FRAMES)
            return GAME_FAILED;
        audio_song_speed(G.song_speed * (1 - 0.7f * G.fail_frames / FAIL_FRAMES));
        return GAME_PLAYING;
    }
    if (pressed & BTN_START)
        return GAME_PAUSE;
    count_in();
    metronome();

    // lanes held and newly pressed: buttons, then the touch screen
    uint32_t held = 0, lanes_pressed = 0;
    for (int lane = 0; lane < 6; lane++) {
        int src = lane < G.lanes && lefty_now() ? G.lanes - 1 - lane : lane;
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
        if (battle.on && battle.mine_n)
            battle_fire();
    }
    if (battle.on && battle.broken_lane >= 0) {
        // the broken string: its fret does nothing till it's tapped back to life
        uint32_t bit = 1u << battle.broken_lane;
        if ((lanes_pressed & bit) && --battle.broken_left <= 0) {
            battle.broken_lane = -1;
            popup_always("STRING FIXED!");
            sound_play(res.snd[SND_SP_READY], 0.5f, false);
        } else {
            held &= ~bit;
            lanes_pressed &= ~bit;
        }
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
    gf_flags();
    if (!G.vocal) {
        if (GF.on)
            gf_judge(lanes_pressed);
        else
            judge(lanes_pressed, let_go);
        update_sustains();
    }

    // Star Power drains a meter in 32 quarter notes (Flash's by the clock)
    if (G.sp_active && GF.on) {
        G.sp -= GF.sp_rate * (float)fmax(0, G.t - GF.last_t);
        G.sp_last_q = chart_quarters(G.chart, G.t);
        if (G.sp <= 0) {
            G.sp = 0;
            G.sp_active = false;
            sound_play(res.snd[SND_SP_RELEASE], 0.7f, false);
        }
    } else if (G.sp_active) {
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
    GF.last_t = G.t;
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

    G.updates++;

    if (practice.on) {
        // the end of the stretch: how it went, and again
        if (G.t > practice.to + 0.6 || audio_song_finished()) {
            // a clear (90% of the notes) speeds the next time through up, when that's asked for
            char t[64] = "";
            int pct = practice.notes ? G.hits * 100 / practice.notes : 100;
            practice.runs++;
            if (pct > practice.best)
                practice.best = pct;
            int speed = (int)(practice.speed * 100 + 0.5f);
            if (practice.ramp && pct >= 90 && speed < practice.target) {
                speed = speed + practice.ramp > practice.target ? practice.target : speed + practice.ramp;
                practice.speed = speed / 100.0f;
                snprintf(t, sizeof(t), "%d%%  SPEED UP %d%%", pct, speed);
            } else if (practice.notes)
                snprintf(t, sizeof(t), "%d%%  (%d / %d)", pct, G.hits, practice.notes);
            plat_log("practice: run %d, %d%% of %d notes, next at %d%%, %.1f s of song", practice.runs, pct, practice.notes,
                     speed, practice.to - practice.from);
            reset_play();
            // the result of a run is the point of practice: it shows even with popups off
            popup_always(t);
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
    for (int i = 0; i < seg && cfg.gem_style == 1; i++) {
        // Flash: a plain dark board, a thin line between the lanes
        float z0 = Z_NEAR + (Z_FAR - Z_NEAR) * i / seg, z1 = Z_NEAR + (Z_FAR - Z_NEAR) * (i + 1) / seg;
        gfx_tex(NULL);
        const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        float f0 = fade(z0) * 0.92f * (0.6f + 0.4f * solid), f1 = fade(z1) * 0.92f * (0.6f + 0.4f * solid);
        // Flash's board is near black, a touch lighter toward you
        uint32_t b0 = with_alpha(RGBA(22, 22, 24, 255), f0), b1 = with_alpha(RGBA(10, 10, 11, 255), f1);
        const float p[4][3] = {{-HALF_W, 0, z0}, {HALF_W, 0, z0}, {HALF_W, 0, z1}, {-HALF_W, 0, z1}};
        const uint32_t c[4] = {b0, b0, b1, b1};
        gfx_quad(p, uv, c);
        for (int l = 1; l < G.lanes; l++) {
            float x = (lane_x(l - 1) + lane_x(l)) / 2;
            // Flash's lanes part on thin grey lines
            const float q[4][3] = {{x - 0.018f, 0.004f, z0}, {x + 0.018f, 0.004f, z0}, {x + 0.018f, 0.004f, z1}, {x - 0.018f, 0.004f, z1}};
            const uint32_t lc[4] = {with_alpha(RGBA(96, 96, 104, 255), f0), with_alpha(RGBA(96, 96, 104, 255), f0),
                                    with_alpha(RGBA(70, 70, 76, 255), f1), with_alpha(RGBA(70, 70, 76, 255), f1)};
            gfx_quad(q, uv, lc);
        }
    }
    for (int i = 0; i < seg && cfg.gem_style != 1; i++) {
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
    for (int i = cfg.gem_style == 1 ? c->beat_count : lo; i < c->beat_count; i++) {     // Flash: no beat lines
        float z = time_z(c->beats[i].time / 1e6);
        if (z > Z_FAR)
            break;
        bool measure = c->beats[i].kind == 0;
        // silver bars stopping short of the rails, the measure's the stronger
        float d = measure ? 0.26f : 0.13f;
        uint32_t col = with_alpha(measure ? RGBA(235, 235, 245, 255) : RGBA(190, 185, 200, 170), fade(z) * (cfg.gem_style == 1 ? 0.3f : 1));
        flat(-HALF_W + 0.13f, HALF_W - 0.13f, z - d / 2, z + d / 2, 0.005f, measure ? bar_l : bar_m, 0, 1, col, col);
    }
    // the strings: coiled wire hooked on the back of each cup, fading off up the neck; a struck one rings in its colour
    const Sprite * wire = atlas_get(res.game, "string");
    for (int l = 0; l < G.lanes && cfg.gem_style != 1; l++) {
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
        if (cfg.gem_style == 1) {
            // Flash: a white edge with a grey outer lip, all the way up
            flat(x - 0.09f, x + 0.09f, Z_NEAR, Z_FAR, 0.012f, NULL, 0, 1, rc, with_alpha(rc, 0.35f));
            flat(x + side * 0.09f - 0.03f, x + side * 0.09f + 0.03f, Z_NEAR, Z_FAR, 0.013f, NULL, 0, 1, RGBA(120, 120, 128, 255),
                 RGBA(120, 120, 128, 90));
            continue;
        }
        strip(x - 0.07f, x + 0.07f, 0.01f, rail, rc);
    }
    if (cfg.gem_style == 1)
        return;
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
        float z_start = time_z(n->time / 1e6);
        if (z_start > Z_FAR)
            break;
        if (!n->len)
            continue;
        float z_end = time_z((n->time + (double)n->len) / 1e6);
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
            if (cfg.gem_style == 1 && !G.drums) {
                // Flash's tail: a straight solid bar in the lane's colour, rimmed black, a lighter stripe down it
                uint32_t fc = dead ? RGBA(90, 90, 96, 255) : glow_c, dark = RGBA(6, 6, 8, 255);
                uint32_t lite = RGBA(((fc & 0xFF) + 255) / 2, (((fc >> 8) & 0xFF) + 255) / 2, (((fc >> 16) & 0xFF) + 255) / 2, 255);
                float fw = open ? 1.6f : 0.12f;
                int pieces = 1;
                float amp = 0;
                for (int s = 0; s < pieces; s++) {
                    float za = z0 + (z_end - z0) * s / pieces, zb = z0 + (z_end - z0) * (s + 1) / pieces;
                    float wob = amp * sinf(za * 2.6f + t * 25);
                    flat(x - fw * 1.3f + wob, x + fw * 1.3f + wob, za, zb, 0.011f, NULL, 0, 1, dark, dark);
                    flat(x - fw + wob, x + fw + wob, za, zb, 0.012f, NULL, 0, 1, active ? lite : fc, active ? lite : fc);
                    flat(x - fw * 0.3f + wob, x + fw * 0.3f + wob, za, zb, 0.013f, NULL, 0, 1, with_alpha(WHITE, active ? 0.8f : 0.4f),
                         with_alpha(WHITE, active ? 0.8f : 0.4f));
                }
                continue;
            }
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
static void lay_cup(float x, float y, float z, float w, float d, const Sprite * s, uint32_t c);

static void draw_cups(bool caps)
{
    if (cfg.gem_style == 1 && !G.drums)
        return;
    const float cw = 1.02f, cup_d = 0.84f;        // 128x64 art lying on the neck: 2.9 to 1 on the screen, as World Tour's
    for (int l = 0; l < G.lanes; l++) {
        char name[16];
        float x = lane_x(l);
        if (!caps) {
            snprintf(name, sizeof(name), "%s_%d", cfg.cup_skin ? "cupdeep" : "cup", lane_art(l));
            lay_cup(x, 0.015f, 0, cw, cup_d, atlas_get(res.game, name), WHITE);
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
    const float rope0 = -HALF_W + 0.07f, rope1 = -HALF_W - 0.15f, z0 = 0.55f, z1 = 3.0f;    // flush on the rail
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
        const float lz = z0 + 0.52f, lg = (z1 - 0.42f - lz) / 5, fx = rope1 + 0.08f;
        const float a3[3] = {fx, 0.016f, lz + lg * (i - 0.02f)}, b3[3] = {fx, 0.016f, lz + lg * (i + 1.02f)};
        float a[2], b[2];
        project(a3[0], a3[1], a3[2], &a[0], &a[1]);
        project(b3[0], b3[1], b3[2], &b[0], &b[1]);
        // a stiff lamp pointing out to the side and tipped up, its dome 65 degrees off upright (the user's pick off the
        // footage), not square to the slanted rope
        const float tilt = -0.44f, ux = cosf(tilt), uy = sinf(tilt), wide = hypotf(b[0] - a[0], b[1] - a[1]) * 0.5f;
        const float len = wide * 2.0f, mx0 = (a[0] + b[0]) / 2, my0 = (a[1] + b[1]) / 2, vx = -uy * wide, vy = ux * wide;
        const float p[4][3] = {{mx0 + ux * len - vx, my0 + uy * len - vy, 0}, {mx0 - vx, my0 - vy, 0}, {mx0 + vx, my0 + vy, 0},
                               {mx0 + ux * len + vx, my0 + uy * len + vy, 0}};
        const float uv[4][2] = {{s->u0, s->v0}, {s->u1, s->v0}, {s->u1, s->v1}, {s->u0, s->v1}};
        // unlit, the footage's domes are a dark red glass, not gold
        const uint32_t lc = state == 0 ? RGBA(235, 120, 95, 255) : WHITE, c[4] = {lc, lc, lc, lc};
        gfx_tex(s->tex);
        gfx_quad(p, uv, c);
        if (state == 2) {
            // lit, it throws light round itself
            const uint32_t light = G.sp_active ? RGBA(95, 225, 255, 255) : base_m >= 3 ? RGBA(255, 130, 225, 255)
                                 : base_m == 2 ? RGBA(90, 245, 110, 255) : RGBA(255, 196, 70, 255);
            float mx = mx0 + ux * len * 0.8f, my = my0 + uy * len * 0.8f, r = wide * 2.0f;
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
        if (G.mult_pop > 0.05f) {
            // a burst of light behind the plate as it goes up, in the multiplier's colour
            uint32_t bc = G.sp_active ? RGBA(95, 225, 255, 255) : m >= 3 ? RGBA(255, 130, 225, 255)
                        : RGBA(90, 245, 110, 255);
            gfx_blend(BLEND_ADD);
            board(-HALF_W - 0.30f, 0.0f, z1 + 0.05f - 0.4f * G.mult_pop, 2.6f * G.mult_pop, 2.0f * G.mult_pop,
                  atlas_get(res.game, "glow"), 0, 0, 1, 1, with_alpha(bc, G.mult_pop));
            gfx_blend(BLEND_ALPHA);
        }
        board(-HALF_W - 0.30f, 0.02f, z1 + 0.05f, 0.8f * pop, 0.4f * pop, atlas_get(res.game, name), 0, 0, 1, 1,
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

// a strikeline cup without the arch (World Tour's string hook) its art has across the top: lying on the neck or
// standing, that arch never sat square over the cup. The art's top 27% goes and the cup keeps its own size
static void lay_cup(float x, float y, float z, float w, float d, const Sprite * s, uint32_t c)
{
    const float cut = 0.27f;
    if (!cfg.flat_notes) {
        board(x, y, z - d * 0.25f, w, d * 0.5f * (1 - cut), s, 0, cut, 1, 1, c);
        return;
    }
    flat(x + w / 2, x - w / 2, z - d / 2, z - d / 2 + d * (1 - cut), y, s, 1, cut, c, c);
}

static void lay_gem(float x, float y, float z, float w, float d, const Sprite * s, uint32_t c)
{
    // square in its lane, like the cups: the old depth spread pulled each gem's far edge (the button's white cap)
    // towards the middle of the screen, so only the middle lane's cap sat centered on its gem
    lay(x, y, z, w, d, s, c);
}

// ---- Flash, the other note style: round flat discs on a plain dark board, drawn as shapes (no art)
static const uint8_t flash_cols[6][3] = {{38, 200, 78}, {232, 52, 46}, {246, 214, 40}, {48, 128, 240}, {246, 140, 30},
                                         {110, 220, 255}};

static uint32_t flash_col(int c, float k, float a)
{
    int r = (int)(flash_cols[c][0] * k), g = (int)(flash_cols[c][1] * k), b = (int)(flash_cols[c][2] * k);
    return with_alpha(RGBA(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, 255), a);
}

// 2D shapes for Flash: an ellipse filled from its middle colour to its edge colour, and a ring between two ellipses
static void flash_ell(float cx, float cy, float rx, float ry, uint32_t cin, uint32_t cout)
{
    enum { SEG = 20 };
    static float cs[SEG + 1], sn[SEG + 1];
    if (cs[0] == 0)
        for (int i = 0; i <= SEG; i++) {
            cs[i] = cosf(i * 6.2831853f / SEG);
            sn[i] = sinf(i * 6.2831853f / SEG);
        }
    gfx_tex(NULL);
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32_t c[4] = {cin, cout, cout, cin};
    for (int i = 0; i < SEG; i += 1) {
        const float p[4][3] = {{cx, cy, 0}, {cx + cs[i] * rx, cy + sn[i] * ry, 0}, {cx + cs[i + 1] * rx, cy + sn[i + 1] * ry, 0},
                               {cx, cy, 0}};
        gfx_quad(p, uv, c);
    }
}

static void flash_annulus(float cx, float cy, float rx0, float ry0, float rx1, float ry1, uint32_t c0, uint32_t c1)
{
    enum { SEG = 20 };
    gfx_tex(NULL);
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32_t c[4] = {c0, c1, c1, c0};
    for (int i = 0; i < SEG; i++) {
        float a0 = i * 6.2831853f / SEG, a1 = (i + 1) * 6.2831853f / SEG;
        const float p[4][3] = {{cx + cosf(a0) * rx0, cy + sinf(a0) * ry0, 0}, {cx + cosf(a0) * rx1, cy + sinf(a0) * ry1, 0},
                               {cx + cosf(a1) * rx1, cy + sinf(a1) * ry1, 0}, {cx + cosf(a1) * rx0, cy + sinf(a1) * ry0, 0}};
        gfx_quad(p, uv, c);
    }
}

// a lane's spot on the screen at depth z: its middle, and the half-sizes of a disc of world radius r lying there
static void flash_spot(int l, float z, float r, float * sx, float * sy, float * rx, float * ry)
{
    float x = lane_x(l), ax, ay, bx, by;
    project(x, 0, z, sx, sy);
    project(x + r, 0, z, &ax, &ay);
    project(x, 0, z + r, &bx, &by);
    *rx = fabsf(ax - *sx);
    *ry = GF.look ? *rx * 0.42f : fmaxf(fabsf(by - *sy), *rx * 0.28f);
}

// a four-pointed twinkle: two thin diamonds crossed
static void f2_twinkle(float x, float y, float r, uint32_t c)
{
    gfx_tex(NULL);
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    uint32_t e = c & 0x00FFFFFF;
    const float v[4][3] = {{x, y - r, 0}, {x + r * 0.22f, y, 0}, {x, y + r, 0}, {x - r * 0.22f, y, 0}};
    const float h[4][3] = {{x - r, y, 0}, {x, y - r * 0.22f, 0}, {x + r, y, 0}, {x, y + r * 0.22f, 0}};
    const uint32_t cv[4] = {e, c, e, c}, ch[4] = {e, c, e, c};
    gfx_quad(v, uv, cv);
    gfx_quad(h, uv, ch);
}

// one Flash note: a chunky round candy, outlined bold in black, its side a deep shade of the lane's colour, its top
// bright with a darker inner ring and a twinkle of light. A hammer-on's top is pale (World Tour rules only), a tap's
// dark with a coloured ring; Star Power notes are cyan
static void flash_gem(int l, float z, int c, int kind, float a, float size)
{
    float sx, sy, rx, ry;
    flash_spot(l, z, 0.43f * size, &sx, &sy, &rx, &ry);
    float h = rx * 0.27f, ol = fmaxf(2, rx * 0.09f);
    uint32_t black = with_alpha(RGBA(12, 10, 18, 255), a);
    flash_ell(sx, sy + ry * 0.35f, rx * 1.1f, ry * 1.1f, with_alpha(RGBA(0, 0, 0, 255), a * 0.45f), with_alpha(RGBA(0, 0, 0, 255), 0));
    // the outline: the whole puck's shape, a size bigger, in black
    for (int k = 0; k <= 3; k++)
        flash_ell(sx, sy - h * k / 3, rx + ol, ry + ol, black, black);
    // the side
    for (int k = 0; k <= 3; k++)
        flash_ell(sx, sy - h * k / 3, rx, ry, kind == 2 ? with_alpha(RGBA(40, 38, 50, 255), a) : flash_col(c, 0.5f + 0.06f * k, a),
                  kind == 2 ? with_alpha(RGBA(24, 22, 30, 255), a) : flash_col(c, 0.38f + 0.06f * k, a));
    float ty = sy - h;
    // the top
    if (kind == 2) {
        flash_ell(sx, ty, rx * 0.94f, ry * 0.94f, with_alpha(RGBA(60, 58, 72, 255), a), with_alpha(RGBA(30, 28, 38, 255), a));
        flash_annulus(sx, ty, rx * 0.62f, ry * 0.62f, rx * 0.86f, ry * 0.86f, flash_col(c, 1.1f, a), flash_col(c, 0.9f, a));
    } else if (kind == 1) {
        flash_ell(sx, ty, rx * 0.94f, ry * 0.94f, flash_col(c, 1.0f, a), flash_col(c, 0.8f, a));
        flash_ell(sx, ty, rx * 0.66f, ry * 0.66f, with_alpha(RGBA(255, 255, 255, 255), a), with_alpha(RGBA(225, 225, 235, 255), a));
    } else {
        flash_ell(sx, ty, rx * 0.94f, ry * 0.94f, flash_col(c, 1.3f, a), flash_col(c, 0.95f, a));
        flash_annulus(sx, ty, rx * 0.56f, ry * 0.56f, rx * 0.68f, ry * 0.68f, flash_col(c, 0.7f, a * 0.8f), flash_col(c, 0.7f, a * 0.8f));
    }
    // the top's rim catching the light along its far edge
    flash_annulus(sx, ty, rx * 0.84f, ry * 0.84f, rx * 0.96f, ry * 0.96f, with_alpha(WHITE, 0), with_alpha(WHITE, a * (kind == 2 ? 0.2f : 0.45f)));
    // light on it: a soft glint up and to the left, a twinkle in it
    flash_ell(sx - rx * 0.3f, ty - ry * 0.2f, rx * 0.36f, ry * 0.3f, with_alpha(WHITE, a * 0.5f), with_alpha(WHITE, 0));
    f2_twinkle(sx - rx * 0.34f, ty - ry * 0.26f, rx * 0.3f, with_alpha(WHITE, a * 0.95f));
}

// the targets on the strikeline: a bold black-outlined ring in the lane's colour round a dark well with a little lens
// in it. Pressed, the ring pushes down and the lens lights up; a hit throws a white ring off it, a miss flashes it red
static void flash_targets(void)
{
    for (int l = 0; l < G.lanes; l++) {
        float sx, sy, rx, ry;
        flash_spot(l, 0, 0.48f, &sx, &sy, &rx, &ry);
        int c = lane_art(l);
        float since = (float)(G.t - G.press_t[l]);
        bool held = G.held & (1 << l);
        float press = held ? 1 : since >= 0 && since < 0.083f ? 1 - since / 0.083f : 0;
        float ol = fmaxf(2.5f, rx * 0.08f), drop = press * ry * 0.12f;
        // its base, then the ring on it
        flash_ell(sx, sy + ry * 0.16f, rx + ol, ry + ol, RGBA(10, 8, 16, 255), RGBA(10, 8, 16, 255));
        flash_ell(sx, sy + ry * 0.16f, rx, ry, flash_col(c, 0.4f, 1), flash_col(c, 0.3f, 1));
        float cy = sy + drop;
        flash_ell(sx, cy, rx + ol, ry + ol, RGBA(10, 8, 16, 255), RGBA(10, 8, 16, 255));
        flash_annulus(sx, cy, rx * 0.66f, ry * 0.66f, rx, ry, flash_col(c, 0.9f + 0.3f * press, 1), flash_col(c, 1.15f + 0.2f * press, 1));
        flash_annulus(sx, cy, rx * 0.6f, ry * 0.6f, rx * 0.68f, ry * 0.68f, RGBA(10, 8, 16, 255), RGBA(10, 8, 16, 255));
        flash_ell(sx, cy, rx * 0.6f, ry * 0.6f, RGBA(40, 36, 56, 255), RGBA(18, 16, 28, 255));
        // the lens
        flash_ell(sx, cy, rx * 0.3f, ry * 0.3f, flash_col(c, 0.35f + 0.95f * press, 1), flash_col(c, 0.25f + 0.6f * press, 1));
        if (press > 0) {
            gfx_blend(BLEND_ADD);
            flash_ell(sx, cy, rx * 0.62f, ry * 0.62f, flash_col(c, 0.6f, 0.6f * press), flash_col(c, 0.4f, 0));
            gfx_blend(BLEND_ALPHA);
        }
        // its top edge catching the light
        flash_annulus(sx, cy - ry * 0.05f, rx * 0.8f, ry * 0.8f, rx * 0.93f, ry * 0.93f, with_alpha(WHITE, 0), with_alpha(WHITE, 0.3f));
        float hit = clamp01(1 - (float)(G.t - G.flame_t[l]) * 6);
        if (hit > 0) {
            gfx_blend(BLEND_ADD);
            float g = 1 + (1 - hit) * 0.5f;
            flash_annulus(sx, cy, rx * (g - 0.1f), ry * (g - 0.1f), rx * g, ry * g, with_alpha(WHITE, hit * 0.8f), with_alpha(WHITE, hit * 0.25f));
            gfx_blend(BLEND_ALPHA);
        }
        float miss = clamp01(1 - (float)(G.t - G.miss_t[l]) * 5);
        if (miss > 0 && cfg.flashes < 2)
            flash_annulus(sx, cy, rx * 0.66f, ry * 0.66f, rx * 1.05f, ry * 1.05f, RGBA(255, 40, 40, (int)(200 * miss)),
                          RGBA(255, 40, 40, (int)(120 * miss)));
    }
}

// Flash's fire: a solid cartoon flame on each hit, an orange outline round yellow round pale yellow, its tongues
// licking up. It flares up in a few frames, then shrinks back into the ring
static void flash_flame_shape(float sx, float sy, float w, float h, float t, int seed, uint32_t cin, uint32_t cout)
{
    enum { N = 24 };
    gfx_tex(NULL);
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    float px[N + 1], py[N + 1];
    for (int i = 0; i <= N; i++) {
        // half a turn over the top, from the right foot round to the left
        float ang = 3.14159265f * i / N, cx = cosf(ang), sn = sinf(ang);
        // tongues: three peaks (the middle tallest), wavering with time
        float tongue = 0.55f + 0.45f * powf(fabsf(cosf(ang * 2.5f + sinf(t * 23 + seed) * 0.35f)), 3.0f);
        float top = sn * (0.45f + 0.55f * powf(sn, 4)) * tongue;
        px[i] = sx + cx * w * (0.8f + 0.2f * sn);
        py[i] = sy - h * top - h * 0.08f;
    }
    float baseh = h * 0.12f;
    for (int i = 0; i < N; i++) {
        const float p[4][3] = {{sx, sy - baseh, 0}, {px[i], py[i], 0}, {px[i + 1], py[i + 1], 0}, {sx, sy - baseh, 0}};
        const uint32_t c[4] = {cin, cout, cout, cin};
        gfx_quad(p, uv, c);
    }
    // the round foot, in the ring
    flash_ell(sx, sy - baseh * 0.5f, w * 0.82f, w * 0.3f, cin, cout);
}

static void flash_flames(void)
{
    gfx_2d();
    gfx_blend(BLEND_ALPHA);
    for (int l = 0; l < G.lanes; l++) {
        double since = G.t - G.flame_t[l];
        if (since < 0 || since > 0.3 || cfg.fire == 2)
            continue;
        float sx, sy, rx, ry;
        flash_spot(l, 0, 0.5f, &sx, &sy, &rx, &ry);
        // up in two frames, held, down by the end
        float k = since < 0.06 ? (float)since / 0.06f : since > 0.18 ? 1 - (float)(since - 0.18) / 0.12f : 1;
        float big = cfg.fire ? 0.75f : 1;
        float w = rx * 0.82f * big * (0.7f + 0.3f * k), h = rx * 2.1f * big * k;
        float t = (float)G.t;
        bool sp = G.sp_active;
        uint32_t o0 = sp ? RGBA(40, 120, 230, 255) : RGBA(236, 84, 14, 255), o1 = sp ? RGBA(20, 80, 200, 230) : RGBA(210, 50, 10, 230);
        uint32_t m0 = sp ? RGBA(120, 200, 255, 255) : RGBA(255, 196, 30, 255), m1 = sp ? RGBA(80, 170, 255, 255) : RGBA(255, 150, 20, 255);
        uint32_t c0 = sp ? RGBA(235, 250, 255, 255) : RGBA(255, 250, 190, 255), c1 = sp ? RGBA(170, 225, 255, 255) : RGBA(255, 226, 90, 255);
        // a bold outline round it all
        flash_flame_shape(sx, sy + 2, w * 1.12f, h * 1.08f + 3, t, l * 7, RGBA(14, 8, 10, 235), RGBA(14, 8, 10, 235));
        flash_flame_shape(sx, sy, w, h, t, l * 7, o0, o1);
        flash_flame_shape(sx, sy, w * 0.78f, h * 0.8f, t + 0.05f, l * 7 + 3, m0, m1);
        flash_flame_shape(sx, sy, w * 0.5f, h * 0.52f, t + 0.1f, l * 7 + 5, c0, c1);
    }
}

// all of Flash's notes, far ones first, over the targets
static void draw_flash_notes(void)
{
    const Part * p = G.part;
    gfx_2d();
    gfx_blend(BLEND_ALPHA);
    flash_targets();
    int last = G.first;
    while (last < p->note_count && on_highway(p->notes[last].time / 1e6))
        last++;
    int start = G.first > 16 ? G.first - 16 : 0;
    static const float fs[3] = {0.9f, 1, 1.14f};
    for (int i = last - 1; i >= start; i--) {
        const Note * n = &p->notes[i];
        if (G.state[i] == HIT)
            continue;
        float z = time_z(n->time / 1e6);
        if (z < Z_NEAR + 0.3f || z > Z_FAR)
            continue;
        float a = GF.look ? 1 : fade(z);       // Flash's notes come in whole at the top
        if (G.state[i] == MISSED)
            a *= 0.5f;
        else if (cfg.vanish || (battle.on && G.t < battle.blackout_until))
            a *= clamp01((z - 2.2f) / 1.6f);
        else if (battle.on && G.t < battle.amp_until && fmod(G.t + z * 0.05, 0.22) < 0.08)
            a *= 0.15f;
        if (a <= 0.01f)
            continue;
        // Flash has no hammer-ons: under its rules every note looks (and plays) the same
        int kind = GF.on ? 0 : n->flags & NOTE_TAP ? 2 : n->flags & NOTE_HOPO ? 1 : 0;
        bool star = G.phrase[i] >= 0 && G.phrase_ok[G.phrase[i]];
        if (n->frets & FRET_OPEN) {
            // an open note: a bar across the board, white-rimmed purple
            float d1, d2, rxo, ryo, x0, y0, x1, y1;
            flash_spot(0, z, 0.3f, &d1, &d2, &rxo, &ryo);
            project(lane_x(0), 0, z, &x0, &y0);
            project(lane_x(G.lanes - 1), 0, z, &x1, &y1);
            float left = fminf(x0, x1) - rxo, w = fabsf(x1 - x0) + rxo * 2, hh = fmaxf(ryo, 3);
            gfx_rect(left, y0 - hh - 2, w, hh * 2 + 4, with_alpha(WHITE, a));
            gfx_rect(left + 3, y0 - hh, w - 6, hh * 2, G.sp_active || star ? flash_col(5, 1, a) : with_alpha(RGBA(190, 70, 230, 255), a));
            continue;
        }
        for (int l = 0; l < G.lanes; l++)
            if (n->frets & (1 << l))
                flash_gem(l, z, G.sp_active || star ? 5 : lane_art(l), kind, a, fs[cfg.note_size]);
    }
}

// Flash's heads-up display: the rock meter a bar up the left of the highway, Star Power up the right, the multiplier
// in a badge under the strikeline, the score bottom left
// a quad between four points on the board (x across, z up it), lifted y, in screen space
static void flash_board_quad(float x0, float x1, float z0, float z1, uint32_t c00, uint32_t c10, uint32_t c11, uint32_t c01)
{
    float ax, ay, bx, by, cx, cy, dx, dy;
    project(x0, 0, z0, &ax, &ay);
    project(x1, 0, z0, &bx, &by);
    project(x1, 0, z1, &cx, &cy);
    project(x0, 0, z1, &dx, &dy);
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const float p[4][3] = {{ax, ay, 0}, {bx, by, 0}, {cx, cy, 0}, {dx, dy, 0}};
    const uint32_t c[4] = {c00, c10, c11, c01};
    gfx_quad(p, uv, c);
}

// Flash's flat board: deep night blue, a few stars drifting down it, thin lines between the lanes, bold white rails
// outlined in black; the held notes' tails on it. Star Power washes it blue, a miss flashes the rails red
static void draw_flash_board(void)
{
    gfx_2d();
    gfx_blend(BLEND_ALPHA);
    gfx_tex(NULL);
    const float hw = HALF_W, zb = Z_NEAR, zt = Z_FAR;
    float spk = G.sp_active ? 0.6f + 0.15f * sinf((float)G.t * 6) : 0;
    uint32_t ink = RGBA(10, 8, 16, 255);
    // the outline, then the board
    flash_board_quad(-hw - 0.32f, hw + 0.32f, zb, zt + 0.25f, ink, ink, ink, ink);
    uint8_t r0 = (uint8_t)(36 + 10 * spk), g0 = (uint8_t)(32 + 40 * spk), b0 = (uint8_t)(62 + 80 * spk);
    uint32_t near = RGBA(r0, g0, b0, 250), far = RGBA(14, 12, 28, 250);
    flash_board_quad(-hw, hw, zb, zt, near, near, far, far);
    // stars drifting down: the board scrolls under the notes
    for (int k = 0; k < 26; k++) {
        float x = (float)((k * 7919) % 1000) / 1000 * 2 - 1;
        float zz = fmodf((float)((k * 104729) % 1000) / 1000 * (zt - zb) - (float)G.t * (float)G.speed * 0.5f, zt - zb);
        if (zz < 0)
            zz += zt - zb;
        float z = zb + zz, sx, sy;
        project(x * (hw - 0.3f), 0, z, &sx, &sy);
        float sz = f2_hw(sy) / F2_HW_LINE * (k % 3 == 0 ? 4.5f : 2.5f);
        float tw = 0.5f + 0.5f * sinf((float)G.t * 3 + k);
        f2_twinkle(sx, sy, sz, RGBA(200, 210, 255, (int)(60 + 80 * tw)));
    }
    // the lines between the lanes
    for (int l = 1; l < G.lanes; l++) {
        float x = (lane_x(l - 1) + lane_x(l)) / 2;
        uint32_t c0 = RGBA(110, 104, 150, 200), c1 = RGBA(60, 56, 90, 140);
        flash_board_quad(x - 0.025f, x + 0.025f, zb, zt, c0, c0, c1, c1);
    }
    // the tails of held notes
    const Part * p = G.part;
    for (int i = G.first > 8 ? G.first - 8 : 0; i < p->note_count; i++) {
        const Note * n = &p->notes[i];
        float z0 = time_z(n->time / 1e6);
        if (z0 > Z_FAR)
            break;
        if (!n->len)
            continue;
        float z1 = time_z((n->time + (double)n->len) / 1e6);
        if (z1 < 0)
            continue;
        bool active = false;
        for (int k = 0; k < MAX_SUS; k++)
            active |= G.sus[k].note == i;
        if (G.state[i] == HIT && !active)
            continue;
        if (z0 < 0)
            z0 = 0;
        if (z1 > Z_FAR)
            z1 = Z_FAR;
        bool dead = G.state[i] == MISSED, star = G.sp_active || (G.phrase[i] >= 0 && G.phrase_ok[G.phrase[i]]);
        for (int l = 0; l < 6; l++) {
            if (!(n->frets & (1 << l)))
                continue;
            bool open = l == LANE_OPEN;
            float x = open ? 0 : lane_x(l), w = open ? hw - 0.3f : 0.13f;
            int c = star ? 5 : lane_art(l);
            uint32_t fc = dead ? RGBA(80, 78, 96, 255) : open ? RGBA(190, 70, 230, 255) : flash_col(c, active ? 1.25f : 1, 1);
            uint32_t fc2 = dead ? RGBA(60, 58, 74, 255) : open ? RGBA(140, 50, 180, 255) : flash_col(c, active ? 0.95f : 0.7f, 1);
            float wob = active && !cfg.no_wobble ? (0.02f + G.whammy * 0.06f) * sinf((float)G.t * 30) : 0;
            flash_board_quad(x - w - 0.07f + wob, x + w + 0.07f + wob, z0, z1, ink, ink, ink, ink);
            flash_board_quad(x - w + wob, x + w + wob, z0, z1, fc2, fc, fc, fc2);
            uint32_t hl = with_alpha(WHITE, active ? 0.75f : 0.35f);
            flash_board_quad(x - w * 0.25f + wob, x + w * 0.25f + wob, z0, z1, hl, hl, with_alpha(WHITE, 0.1f), with_alpha(WHITE, 0.1f));
        }
    }
    // the rails
    uint32_t rc = G.sp_active ? RGBA(120, 220, 255, 255) : G.sp >= 0.5f ? RGBA(200, 235, 255, 255) : RGBA(245, 245, 250, 255);
    if (G.miss_flash > 0.05f && cfg.flashes < 2) {
        int k = (int)(G.miss_flash * (cfg.flashes ? 90 : 220));
        rc = RGBA(255, 245 - k, 250 - k, 255);
    }
    for (int side = -1; side <= 1; side += 2) {
        float x = side * hw;
        flash_board_quad(x - 0.17f, x + 0.17f, zb, zt + 0.2f, ink, ink, ink, ink);
        flash_board_quad(x - 0.1f, x + 0.1f, zb, zt, rc, rc, with_alpha(rc, 0.7f), with_alpha(rc, 0.7f));
    }
    // the strikeline: a faint band behind the targets
    uint32_t sl = RGBA(255, 255, 255, 40);
    flash_board_quad(-hw, hw, -0.25f, 0.25f, sl, sl, sl, sl);
}

// a capsule standing up off to the side of the highway: a bold outline, dark glass, filled from the bottom
static void f2_capsule(float x, float y0, float y1, float w, float level, uint32_t fill, uint32_t fill2, bool marks)
{
    uint32_t ink = RGBA(10, 8, 16, 255);
    float r = w / 2, ol = 3;
    flash_ell(x, y0, r + ol, r + ol, ink, ink);
    flash_ell(x, y1, r + ol, r + ol, ink, ink);
    gfx_rect(x - r - ol, y0, w + ol * 2, y1 - y0, ink);
    flash_ell(x, y0, r, r, RGBA(30, 28, 44, 240), RGBA(30, 28, 44, 240));
    flash_ell(x, y1, r, r, RGBA(30, 28, 44, 240), RGBA(30, 28, 44, 240));
    gfx_rect(x - r, y0, w, y1 - y0, RGBA(30, 28, 44, 240));
    level = clamp01(level);
    if (level > 0.003f) {
        float top = y1 - (y1 - y0) * level;
        flash_ell(x, y1, r, r, fill2, fill2);
        gfx_tex(NULL);
        const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        // shaded round: dark sides, bright down the middle
        const float pl[4][3] = {{x - r, top, 0}, {x, top, 0}, {x, y1, 0}, {x - r, y1, 0}};
        const float pr[4][3] = {{x, top, 0}, {x + r, top, 0}, {x + r, y1, 0}, {x, y1, 0}};
        const uint32_t cl[4] = {fill2, fill, fill, fill2}, cr[4] = {fill, fill2, fill2, fill};
        gfx_quad(pl, uv, cl);
        gfx_quad(pr, uv, cr);
        flash_ell(x, top, r, r * 0.4f, fill, fill2);
    }
    for (int i = 1; i < 4 && marks; i++)
        gfx_rect(x - r, y1 - (y1 - y0) * i / 4 - 1, w, 2, RGBA(10, 8, 16, 200));
    // the glass shine
    gfx_rect(x - r * 0.55f, y0 + 2, r * 0.3f, y1 - y0 - 4, RGBA(255, 255, 255, 60));
}

// Flash's meters: the rock meter a capsule left of the highway (red, yellow or green by where it stands), Star Power
// one on the right (blue, quartered; it pulses once there's enough to use)
static void flash_bar(float side, float level, bool rock)
{
    float x = F2_CX + side * (F2_HW_LINE + 92), y0 = 150, y1 = 420;
    uint32_t f, f2;
    if (rock) {
        float n = clamp01(level) * 60;
        if (n < 19) {
            float k = 0.75f + 0.25f * fabsf(sinf((float)G.t * 8));
            f = RGBA((int)(255 * k), 60, 50, 255);
            f2 = RGBA((int)(150 * k), 20, 20, 255);
        } else if (n < 41) {
            f = RGBA(255, 222, 60, 255);
            f2 = RGBA(170, 120, 10, 255);
        } else {
            f = RGBA(110, 240, 100, 255);
            f2 = RGBA(30, 130, 40, 255);
        }
    } else {
        bool ready = G.sp >= 0.5f || G.sp_active;
        float k = ready ? 0.85f + 0.15f * sinf((float)G.t * 7) : 1;
        f = RGBA((int)(110 * k), (int)(210 * k), 255, 255);
        f2 = RGBA(20, (int)(80 * k), (int)(190 * k), 255);
    }
    f2_capsule(x, y0, y1, 24, level, f, f2, !rock);
    // a little icon over each
    float iy = y0 - 30;
    if (rock) {
        flash_ell(x, iy, 13, 13, RGBA(10, 8, 16, 255), RGBA(10, 8, 16, 255));
        flash_ell(x, iy, 10, 10, f, f2);
    } else {
        flash_ell(x, iy, 13, 13, RGBA(10, 8, 16, 255), RGBA(10, 8, 16, 255));
        f2_twinkle(x, iy, 12, RGBA(150, 225, 255, 255));
    }
}

static void draw_flash_hud(void)
{
    G.needle += (G.rock - G.needle) * ease(0.1f);
    flash_bar(-1, G.needle, true);
    flash_bar(1, G.sp, false);
    float sx, sy;
    project(0, 0, -0.9f, &sx, &sy);
    if (GF.look)
        sy = F2_LINE + 48;              // under the targets, clear of them
    sy = fminf(sy, SCREEN_H - 30);
    int m = multiplier();
    // Flash's badge: a light grey glossy disc, its number dark; blue in Star Power
    float pop = 1 + 0.25f * G.mult_pop;
    uint32_t b0 = G.sp_active ? RGBA(170, 225, 255, 255) : RGBA(236, 236, 240, 255), b1 = G.sp_active ? RGBA(60, 140, 230, 255)
                                                                                                    : RGBA(150, 150, 158, 255);
    flash_ell(sx, sy + 3, 31 * pop, 29 * pop, RGBA(0, 0, 0, 120), RGBA(0, 0, 0, 0));
    flash_ell(sx, sy, 28 * pop, 26 * pop, b0, b1);
    flash_ell(sx - 7, sy - 10, 14 * pop, 8 * pop, with_alpha(WHITE, 0.7f), with_alpha(WHITE, 0));
    char t[24];
    snprintf(t, sizeof(t), "x%d", m);
    font_draw(res.title, t, sx, sy - 15, 28, RGBA(40, 40, 46, 255), ALIGN_CENTER);
    double gap = G.score - G.shown_score;
    G.shown_score = gap < 1 ? G.score : G.shown_score + gap * 0.25 + 1;
    snprintf(t, sizeof(t), "%ld", (long)G.shown_score);
    font_draw_shadow(res.title, t, 18, SCREEN_H - 92, 40, WHITE, ALIGN_LEFT);
    snprintf(t, sizeof(t), "%d", G.streak);
    font_draw_shadow(res.title, t, 18, SCREEN_H - 50, 30, RGBA(255, 210, 90, 255), ALIGN_LEFT);
}

static void draw_gems(void)
{
    if (cfg.gem_style == 1 && !G.drums)
        return;                         // drawn flat on the screen afterwards (draw_flash_notes)
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
        else if (cfg.vanish || (battle.on && G.t < battle.blackout_until))
            a *= clamp01((z - 2.2f) / 1.6f);    // gone before the strikeline: play it from memory
        else if (battle.on && G.t < battle.amp_until && fmod(G.t + z * 0.05, 0.22) < 0.08)
            a *= 0.15f;                         // the amp's overloaded: the gems flicker
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
            if (star && !G.drums && z < 7 && !cfg.light_fx) {
                // a Star Power note glitters: a halo and a few twinkles trailing behind it up the neck
                const Sprite * dot = atlas_get(res.game, "glow");
                gfx_blend(BLEND_ADD);
                board(lane_x(l), 0.01f, z, gw * 1.6f, gh * 2.2f, dot, 0, 0, 1, 1,
                      with_alpha(RGBA(120, 210, 255, 255), a * (0.35f + 0.15f * sinf((float)G.t * 9 + i))));
                for (int k2 = 0; k2 < 3; k2++) {
                    uint32_t h = (uint32_t)(i * 7 + l) * 2654435761u + (uint32_t)k2 * 40503u;
                    float ph = fmodf((float)G.t * 2.5f + (h >> 8 & 255) / 255.0f, 1.0f);
                    float tx = lane_x(l) + ((h >> 16 & 255) / 255.0f - 0.5f) * 0.6f, tz = z + 0.2f + ph * 0.9f;
                    board(tx, 0.15f + ph * 0.25f, tz, 0.16f, 0.16f, dot, 0, 0, 1, 1, with_alpha(WHITE, a * (1 - ph) * 0.9f));
                }
                gfx_blend(BLEND_ALPHA);
            }
        }
    }
}

static void draw_held_lanes(void)
{
    gfx_blend(BLEND_ADD);
    // a fret held: its lane lights faintly up the neck, under the notes, so the fingers show without tinting them. It
    // comes out of the cup, from halfway up it (the cup's drawn over the start), not from the board's edge below
    for (int l = 0; l < G.lanes && !G.autoplay; l++)
        if (G.held & (1 << l)) {
            uint32_t lc = G.sp_active ? RGBA(150, 220, 255, 255) : lane_colour[lane_art(l)];
            flat(lane_x(l) - 0.45f, lane_x(l) + 0.45f, 0.0f, 4.5f, 0.014f, NULL, 0, 1, with_alpha(lc, 0.24f),
                 with_alpha(lc, 0));
        }
    gfx_blend(BLEND_ALPHA);
}

static void draw_flash_hud(void);
static void draw_flash_board(void);
static void draw_flames(void)
{
    if (GF.look)
        return;                         // Flash: its fire and misses are drawn flat with the notes
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
        if (cfg.gem_style == 1 && frame > 8)
            continue;                   // Flash's fire is all flame: the sprite's last frames are smoke
        float u0 = (frame % 4) * 0.25f, v0 = (frame / 4) * 0.25f;
        if (l == LANE_OPEN) {
            // the kick lights its line across: fire on every cup read as notes hit that weren't
            board(0, 0, -0.1f, 5.0f, 0.5f, glow, 0, 0, 1, 1, with_alpha(lane_colour[6], 0.8f * (1 - (float)since / 0.32f)));
            continue;
        }
        float x = lane_x(l);
        float big = (cfg.fire ? 0.6f : 1) * (cfg.gem_style == 1 ? 1.3f : 1);
        if (cfg.gem_style == 1) {
            // Flash's fire: big and bright yellow, laid on twice
            uint32_t ff = G.sp_active ? RGBA(150, 220, 255, 255) : RGBA(255, 215, 110, 255);
            board(x, 0.03f, -0.42f * big, 1.1f * big, 2.1f * big, flame, u0, v0, u0 + 0.25f, v0 + 0.25f, ff);
            board(x, 0.04f, -0.3f * big, 0.8f * big, 1.6f * big, flame, u0, v0, u0 + 0.25f, v0 + 0.25f, ff);
        } else
            board(x, 0.03f, -0.42f * big, 1.1f * big, 2.1f * big, flame, u0, v0, u0 + 0.25f, v0 + 0.25f, fc);
        lay(x, 0.05f, 0, 1.2f, 1.0f, glow, with_alpha(lane_colour[lane_art(l)], 1 - (float)since / 0.32f));
    }
    const Sprite * dot = atlas_get(res.game, "glow");
    const Sprite * flare = atlas_get(res.game, "flare");
    for (int l = 0; l < G.lanes && cfg.fire != 2; l++) {
        double since = G.t - G.flame_t[l];
        if (since < 0 || since > 0.3)
            continue;
        float x = lane_x(l), k = (float)since;
        uint32_t lc = G.sp_active ? RGBA(150, 220, 255, 255) : lane_colour[lane_art(l)];
        // the hit: a white-hot flash over the cup for a tenth of a second
        if (k < 0.12f) {
            float a = 1 - k / 0.12f, sz = 1.3f + k * 4;
            lay(x, 0.08f, 0, sz, sz * 0.8f, flare, with_alpha(WHITE, 0.85f * a));
            lay(x, 0.07f, 0, sz * 1.5f, sz * 1.2f, dot, with_alpha(lc, 0.9f * a));
        }
        // the gem itself, caught at the strikeline: it swells and fades for a moment as it's taken
        if (k < 0.1f && cfg.gem_style != 1) {
            char gname[24];
            snprintf(gname, sizeof(gname), "gem_normal_%d", G.sp_active ? 5 : lane_art(l));
            float g = 1 + k * 5, ga = 1 - k / 0.1f;
            lay(x, 0.09f, 0, 1.04f * g, 0.48f * 2.1f * g, atlas_get(res.game, gname), with_alpha(WHITE, 0.8f * ga));
        }
        // and a ring of light running out across the neck
        float r = 0.35f + k * 3.2f, ra = 1 - k / 0.3f;
        for (int j = 0; j < (cfg.light_fx || cfg.gem_style == 1 ? 0 : 10); j++) {
            float ang = j * 6.2831853f / 10;
            float px = x + cosf(ang) * r * 0.9f, pz = sinf(ang) * r * 0.55f;
            board(px, 0.04f, pz, 0.2f, 0.2f, dot, 0, 0, 1, 1, with_alpha(lc, 0.7f * ra * ra));
        }
    }
    // a miss: the cup flashes red
    for (int l = 0; l < G.lanes; l++) {
        double since = G.t - G.miss_t[l];
        if (since >= 0 && since < 0.25)
            lay(lane_x(l), 0.07f, 0, 1.6f, 1.3f, dot, with_alpha(RGBA(255, 40, 30, 255), 0.9f * (1 - (float)since / 0.25f)));
    }
    // a streak milestone: a band of light sweeps from the strikeline to the far end
    {
        double since = G.t - G.wave_t;
        if (since >= 0 && since < 0.7) {
            float z = (float)since / 0.7f * Z_FAR, a = 1 - (float)since / 0.7f;
            uint32_t c = G.sp_active ? RGBA(120, 200, 255, 255) : RGBA(255, 210, 120, 255);
            flat(-HALF_W, HALF_W, z - 1.4f, z + 0.3f, 0.025f, NULL, 0, 1, with_alpha(c, 0), with_alpha(c, 0.55f * a));
        }
    }
    // a held sustain: its cup burns steady, a flicker on top
    for (int k = 0; k < MAX_SUS; k++) {
        if (G.sus[k].note < 0 || G.sus[k].lost_t >= 0)
            continue;
        for (int l = 0; l < G.lanes; l++)
            if (G.sus[k].frets & (1 << l)) {
                float fl = 0.75f + 0.25f * sinf((float)G.t * 40 + l * 1.7f);
                uint32_t lc = G.sp_active ? RGBA(150, 220, 255, 255) : lane_colour[lane_art(l)];
                lay(lane_x(l), 0.06f, 0, 1.9f, 1.5f, dot, with_alpha(lc, 0.55f * fl));
            }
    }
    for (int i = 0; i < SPARKS && cfg.gem_style != 1; i++)
        if (G.spark[i].life > 0)
            board(G.spark[i].x, G.spark[i].y, G.spark[i].z, 0.24f, 0.24f, dot, 0, 0, 1, 1,
                  with_alpha(G.spark[i].col, G.spark[i].life));
    // Star Power: the neck glows blue, energy runs up the rails and embers lift off them
    if (G.sp_active) {
        // the whole neck washes blue, swelling on each beat
        double q = chart_quarters(G.chart, G.t);
        float beat = expf(-(float)(q - floor(q)) * 4);
        int wa = (int)(28 + 40 * beat);
        flat(-HALF_W, HALF_W, Z_NEAR, Z_FAR * 0.85f, 0.017f, NULL, 0, 1, RGBA(60, 140, 255, wa), RGBA(60, 140, 255, 0));
        for (int side = -1; side <= 1; side += 2)
            flat(side * HALF_W - side * 0.25f, side * HALF_W + side * 0.9f, Z_NEAR, Z_FAR * 0.75f, 0.018f, dot, 0, 1,
                 RGBA(90, 170, 255, 55), RGBA(90, 170, 255, 0));
        // energy running up both rails: bright pulses travelling away from the player, and blue embers lifting off
        for (int side = -1; side <= 1; side += 2) {
            float x = side * HALF_W;
            for (int k = 0; k < 6; k++) {
                float z = fmodf((float)G.t * 9 + k * 2.2f, Z_FAR * 0.8f);
                float a = fade(z) * (0.55f + 0.45f * beat);
                // a comet: a wide soft glow, a bright core, a tail fading behind it
                for (int g = 0; g < 2; g++)
                    flat(x - 0.6f, x + 0.6f, z - 1.2f, z + 1.2f, 0.03f, dot, 0, 1, with_alpha(RGBA(70, 160, 255, 255), a),
                         with_alpha(RGBA(70, 160, 255, 255), a));
                flat(x - 0.09f, x + 0.09f, z - 1.6f, z + 0.25f, 0.034f, NULL, 0, 1, with_alpha(RGBA(150, 220, 255, 255), 0),
                     with_alpha(WHITE, a));
            }
            for (int k = 0; k < 8; k++) {
                uint32_t h = (uint32_t)(k * 2654435761u + (side + 2) * 97u);
                float life = fmodf((float)G.t * 0.9f + (h >> 8 & 255) / 255.0f, 1.0f);
                float z = 0.5f + (h >> 16 & 255) / 255.0f * Z_FAR * 0.6f;
                float y = 0.05f + life * 0.9f, xo = side * (0.1f + life * 0.35f);
                board(x + xo, y, z, 0.16f, 0.16f, dot, 0, 0, 1, 1, with_alpha(RGBA(140, 210, 255, 255), (1 - life) * fade(z)));
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
        int src = lefty_now() ? G.lanes - 1 - lane : lane;
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

// ------------------------------------------------------------------------------------------------------------ ghost
// The best run on a chart, its score every half second: the HUD races it ("GHOST +1200"). A new best replaces it.
// ghosts/<song folder>.<part>.<diff> in the player's folder.
#define GHOST_MAX 2400                  // 20 minutes
static struct {
    int ref[GHOST_MAX], ref_n;          // the best run's
    int rec[GHOST_MAX], rec_n;          // this run's
    char path[600];
} ghost;

static void ghost_start(const Song * song, int part, int diff)
{
    ghost.ref_n = ghost.rec_n = 0;
    ghost.path[0] = 0;
    if (practice.on)
        return;
    const char * base = strrchr(song->folder, '/');
    base = base ? base + 1 : song->folder;
    char dir[420];
    snprintf(dir, sizeof(dir), "%sghosts", player_dir());
    mkdir(dir, 0777);
    snprintf(ghost.path, sizeof(ghost.path), "%s/%s.%d.%d", dir, base, part, diff);
    FILE * f = fopen(ghost.path, "r");
    if (!f)
        return;
    while (ghost.ref_n < GHOST_MAX && fscanf(f, "%d", &ghost.ref[ghost.ref_n]) == 1)
        ghost.ref_n++;
    fclose(f);
}

static void ghost_track(void)
{
    if (!ghost.path[0] || G.t < 0)
        return;
    int k = (int)(G.t * 2);
    while (ghost.rec_n <= k && ghost.rec_n < GHOST_MAX)
        ghost.rec[ghost.rec_n++] = (int)G.score;
}

// the run's over: kept when it beat the one there was
static void ghost_finish(bool failed)
{
    if (!ghost.path[0] || failed || G.autoplay || practice.on || G.song_speed < 0.995f || !ghost.rec_n)
        return;
    int had = ghost.ref_n ? ghost.ref[ghost.ref_n - 1] : 0;
    ghost.rec[ghost.rec_n - 1] = (int)G.score;
    if ((int)G.score <= had)
        return;
    FILE * f = fopen(ghost.path, "w");
    if (!f)
        return;
    for (int i = 0; i < ghost.rec_n; i++)
        fprintf(f, "%d\n", ghost.rec[i]);
    fclose(f);
}

static void draw_ghost(void)
{
    if (!ghost.ref_n || G.t < 0 || cfg.no_ghost)
        return;
    int k = (int)(G.t * 2);
    int then = ghost.ref[k < ghost.ref_n ? k : ghost.ref_n - 1], d = (int)G.score - then;
    char t[40];
    snprintf(t, sizeof(t), "GHOST %+d", d);
    font_draw_shadow(res.ui, t, SCREEN_W - 16, 214, 22, d >= 0 ? RGBA(130, 230, 150, 255) : RGBA(255, 140, 120, 255),
                     ALIGN_RIGHT);
    // the race: how far through the song each run has scored, out of the best run's final score
    int best = ghost.ref[ghost.ref_n - 1] > 0 ? ghost.ref[ghost.ref_n - 1] : 1;
    const float w = 120, x = SCREEN_W - 16 - w, y = 240;
    float me = (float)G.score / best, them = (float)then / best;
    me = me > 1 ? 1 : me;
    them = them > 1 ? 1 : them;
    gfx_rect(x - 2, y - 2, w + 4, 14, RGBA(0, 0, 0, 170));
    gfx_rect(x, y, w * them, 4, RGBA(200, 200, 210, 200));
    gfx_rect(x, y + 5, w * me, 5, d >= 0 ? RGBA(130, 230, 150, 255) : RGBA(255, 140, 120, 255));
}

// ----------------------------------------------------------------------------------------------------------- battle

static bool lefty_now(void)
{
    return cfg.lefty != (battle.on && G.t < battle.lefty_until);
}

static float battle_rand(void)
{
    battle.seed = battle.seed * 1103515245u + 12345u;
    return (battle.seed >> 8 & 0xffff) / 65536.0f;
}

// the notes Double Notes changed, back as charted
static void battle_undouble(void)
{
    Note * notes = G.part ? (Note *)G.part->notes : NULL;
    for (int k = 0; k < battle.doubled_n && notes; k++)
        if (battle.doubled[k] < G.part->note_count)
            notes[battle.doubled[k]].frets = (uint8_t)battle.doubled_was[k];
    battle.doubled_n = 0;
}

// an attack that suits the part: drums get a dead pad for a broken string but no whammy or doubles; the mic gets only
// the attacks on the eyes
static int battle_pick(unsigned * seed)
{
    for (;;) {
        *seed = *seed * 1103515245u + 12345u;
        int a = (int)(*seed >> 8) % ATK_COUNT;
        if (G.vocal && (a == ATK_BROKEN || a == ATK_DOUBLE || a == ATK_WHAMMY || a == ATK_LEFTY))
            continue;
        if (G.drums && (a == ATK_DOUBLE || a == ATK_WHAMMY))
            continue;
        return a;
    }
}

// the GH3 attacks on the highway itself: the Whammy reels it, Amp Overload shakes it. False: it's as it is
static bool battle_proj(float out[16])
{
    if (!battle.on)
        return false;
    float roll = 0, dx = 0, dy = 0;
    if (battle.whammy_left > 0) {
        float k = battle.whammy_left;
        roll = sinf((float)G.t * 5.5f) * 0.15f * (0.4f + 0.6f * k);
        dx = sinf((float)G.t * 3.1f) * 0.05f * k;
    }
    if (G.t < battle.amp_until) {
        float left = (float)(battle.amp_until - G.t), k = left > 1 ? 1 : left;
        dx += (sinf((float)G.t * 71) * 0.018f + sinf((float)G.t * 23) * 0.012f) * k;
        dy += sinf((float)G.t * 57 + 1) * 0.02f * k;
    }
    if (roll == 0 && dx == 0 && dy == 0)
        return false;
    memcpy(out, G.proj, sizeof(float) * 16);
    float c = cosf(roll), sn = sinf(roll);
    for (int col = 0; col < 4; col++) {
        float x = out[col * 4], y = out[col * 4 + 1], w = out[col * 4 + 3];
        out[col * 4] = c * x - sn * y + dx * w;
        out[col * 4 + 1] = sn * x + c * y + dy * w;
    }
    return true;
}

static void battle_reset(void)
{
    battle_undouble();
    static const float skill[DIFF_COUNT] = {0.9f, 0.93f, 0.95f, 0.97f};
    // the rival's own level on top of the difficulty's: easy, normal, hard, brutal
    static const float level[4] = {-0.1f, 0, 0.02f, 0.035f};
    static const char * const names[] = {"Lou", "Grim Ripper", "Lars Umlaut", "The Vandals", "Casey Lynch", "Izzy Sparks",
                                         "Midori", "Johnny Napalm", "Axel Steel", "Judy Nails", "Pandora", "Clive Winston"};
    unsigned seed = battle.seed ? battle.seed : 7;
    bool on = battle.on;
    float base = battle.base_speed > 0 ? battle.base_speed : (float)G.speed;
    memset(&battle, 0, sizeof(battle));
    battle.on = on;
    battle.seed = seed ^ (unsigned)(G.part ? G.part->note_count * 2654435761u : 0);
    battle.level = battle_level >= 0 ? battle_level & 3 : cfg.rival & 3;
    battle.skill = skill[G.diff >= 0 && G.diff < DIFF_COUNT ? G.diff : 3] + level[battle.level];
    battle.rock = 0.5f;
    battle.next = G.first;
    battle.base_speed = base;
    G.speed = base;
    battle.theirs_ready = 8;            // nothing thrown in the first seconds
    battle.broken_lane = -1;
    snprintf(battle.name, sizeof(battle.name), "%s", names[(int)(battle_rand() * 12) % 12]);
}

// how hard a note is for the rival's fingers: chords, fast runs, long streams and taps cost it
static float note_cost(int i)
{
    const Part * p = G.part;
    const Note * n = &p->notes[i];
    float cost = 0;
    int frets = frets_count(n->frets);
    if (frets >= 2)
        cost += 0.012f * (frets - 1);
    double gap = i > 0 ? (n->time - p->notes[i - 1].time) / 1e6 : 1;
    if (gap < 0.09)
        cost += 0.03f;
    else if (gap < 0.14)
        cost += 0.015f;
    if (n->flags & NOTE_TAP)
        cost += 0.01f;
    // a stream: many notes in the next second
    int dense = 0;
    for (int k = i + 1; k < p->note_count && p->notes[k].time - n->time < 1000000; k++)
        dense++;
    if (dense > 10)
        cost += 0.004f * (dense - 10);
    return cost > 0.15f ? 0.15f : cost;
}

// how much is coming at the player in the next few seconds: notes a second
static float notes_ahead(double secs)
{
    const Part * p = G.part;
    int n = 0;
    for (int k = G.first; k < p->note_count && p->notes[k].time / 1e6 < G.t + secs; k++)
        if (p->notes[k].time / 1e6 >= G.t)
            n++;
    return n / (float)secs;
}

// an attack from the player, the oldest first: each shakes the rival its own way
static void battle_fire(void)
{
    if (!battle.mine_n || G.t < 0)
        return;
    int a = battle.mine[0];
    memmove(battle.mine, battle.mine + 1, sizeof(int) * --battle.mine_n);
    static const float shake[ATK_COUNT] = {0.12f, 0.16f, 0.06f, 0.1f, 0.14f, 0.12f, 0.1f, 0.1f};
    static const float secs[ATK_COUNT] = {6, 5, 3, 7, 6, 7, 5, 6};
    battle.rival_hit = shake[a];
    battle.rival_hit_until = G.t + secs[a];
    if (a == ATK_DRAIN)
        battle.rock -= 0.15f;
    battle.form -= 0.02f;
    battle.thrown++;
    char t[48];
    snprintf(t, sizeof(t), "%s!", atk_names[a]);
    popup_always(t);
    sound_play(res.snd[SND_SP_DEPLOY], 0.9f, false);
}

static void battle_land(int a)
{
    if (a == ATK_LEFTY)
        battle.lefty_until = G.t + 7;
    else if (a == ATK_BLACKOUT)
        battle.blackout_until = G.t + 6;
    else if (a == ATK_SPEED)
        battle.speed_until = G.t + 7;
    else if (a == ATK_BROKEN) {
        // the fret the next six seconds lean on most
        const Part * p = G.part;
        int uses[LANES] = {0}, best = 0;
        for (int k = G.first; k < p->note_count && p->notes[k].time / 1e6 < G.t + 6; k++)
            for (int l = 0; l < LANES; l++)
                uses[l] += (p->notes[k].frets >> l) & 1;
        for (int l = 1; l < LANES && l < G.lanes; l++)
            if (uses[l] > uses[best])
                best = l;
        battle.broken_lane = best;
        battle.broken_left = G.autoplay ? 0 : BROKEN_TAPS;
        if (G.autoplay)
            battle.broken_lane = -1;
    } else if (a == ATK_DOUBLE) {
        // eight seconds of single notes become two-fret chords: the fret beside each joins it
        Note * notes = (Note *)G.part->notes;
        for (int k = G.first; k < G.part->note_count && notes[k].time / 1e6 < G.t + 8 && battle.doubled_n < DOUBLED_MAX; k++) {
            uint8_t f = notes[k].frets;
            if (G.state[k] != PENDING || notes[k].time / 1e6 < G.t + 0.4 || (f & FRET_OPEN) || frets_count(f) != 1 ||
                G.lanes < 5)
                continue;
            int l = __builtin_ctz(f & 0x1f);
            battle.doubled[battle.doubled_n] = k;
            battle.doubled_was[battle.doubled_n++] = f;
            notes[k].frets = (uint8_t)(f | (1 << (l == 4 ? 3 : l + 1)));
        }
    } else if (a == ATK_WHAMMY) {
        battle.whammy_left = 1;
        battle.whammy_until = G.t + 10;
    } else if (a == ATK_AMP)
        battle.amp_until = G.t + 6;
    else
        rock_change(-0.15f);
    battle.taken++;
    char t[48];
    snprintf(t, sizeof(t), "%s!", atk_names[a]);
    popup_always(t);
    sound_play(res.snd[SND_BOO1], cfg.crowd_vol * 0.06f, false);
}

// the rival plays up to now, and throws what it earned when it hurts most
static void battle_update(void)
{
    const Part * p = G.part;
    while (battle.next < p->note_count && p->notes[battle.next].time / 1e6 <= G.t) {
        int i = battle.next++;
        const Note * n = &p->notes[i];
        float chance = battle.skill - note_cost(i) + battle.form;
        if (G.t < battle.rival_hit_until)
            chance -= battle.rival_hit;
        // with its back to the wall it tightens up
        if (battle.rock < 0.25f)
            chance += 0.04f;
        battle.played++;
        if (battle_rand() < chance) {
            battle.hits++;
            battle.streak++;
            int mult = (1 + (battle.streak / 10 < 3 ? battle.streak / 10 : 3)) * (G.t < battle.sp_until ? 2 : 1);
            // a note's points, and its sustain's as the player gets them (25 a beat, about)
            battle.score += (50 * frets_count(n->frets) + (long)(n->len / 1e6 * 50)) * mult;
            battle.rock = battle.rock + 0.015f > 1 ? 1 : battle.rock + 0.015f;
            battle.form = battle.form + 0.002f > 0.04f ? 0.04f : battle.form + 0.002f;
            if (n->flags & NOTE_SP_END) {
                // a phrase: an attack if it has room for one, else its own Star Power
                if (battle.theirs_n < 2)
                    battle.theirs[battle.theirs_n++] = battle_pick(&battle.seed);
                else
                    battle.sp_until = G.t + 8;
            }
        } else {
            // a miss shakes it: the next ones are more likely to go too
            battle.streak = 0;
            battle.rock -= 0.035f;
            battle.form = battle.form - 0.015f < -0.08f ? -0.08f : battle.form - 0.015f;
        }
    }
    battle.form *= 0.9995f;             // it settles back towards its level
    if (battle.whammy_left > 0) {
        battle.whammy_left -= G.whammy * 0.012f;
        if (battle.whammy_left <= 0 || G.t > battle.whammy_until) {
            battle.whammy_left = 0;
            popup_always("SHOOK IT OFF!");
        }
    }
    // an attack on its way lands
    if (battle.warn_t > 0 && G.t >= battle.warn_t) {
        battle.warn_t = 0;
        battle_land(battle.warn_atk);
    }
    // the rival's throw: when the player is low, when a run is coming, or when it's sat on one too long
    if (battle.theirs_n && !battle.warn_t && G.t >= battle.theirs_ready) {
        bool finish = G.rock < 0.4f, run = notes_ahead(4) >= 6, bored = G.t >= battle.theirs_ready + 12;
        int pick = -1;
        for (int k = 0; k < battle.theirs_n && pick < 0; k++) {
            int a = battle.theirs[k];
            if ((a == ATK_DRAIN && finish) || (a != ATK_DRAIN && run))
                pick = k;
        }
        if (pick < 0 && (bored || finish))
            pick = 0;
        if (pick >= 0) {
            battle.warn_atk = battle.theirs[pick];
            memmove(battle.theirs + pick, battle.theirs + pick + 1, sizeof(int) * (--battle.theirs_n - pick));
            battle.warn_t = G.t + 1.0;
            battle.theirs_ready = G.t + 6;
        }
    }
    // Speed Up: the notes rush down the highway while it lasts
    float want = G.t < battle.speed_until ? battle.base_speed * 1.6f : battle.base_speed;
    G.speed += (want - G.speed) * 0.1f;
}

static void draw_battle(void)
{
    if (!battle.on)
        return;
    // the tug of war across the top: your meter against the rival's, the split where they meet
    const float w = 420, x = (SCREEN_W - w) / 2, y = 12, h = 14;
    float me = G.rock < 0 ? 0 : G.rock, them = battle.rock < 0 ? 0 : battle.rock;
    float split = me + them > 0.001f ? me / (me + them) : 0.5f;
    gfx_rect(x - 3, y - 3, w + 6, h + 6, RGBA(0, 0, 0, 200));
    gfx_rect(x, y, w * split, h, RGBA(70, 190, 255, 255));
    gfx_rect(x + w * split, y, w * (1 - split), h, RGBA(235, 70, 55, 255));
    gfx_rect(x + w * split - 2, y - 4, 4, h + 8, WHITE);
    char t[48];
    font_draw_shadow(res.ui, "YOU", x - 10, y - 2, 18, RGBA(150, 215, 255, 255), ALIGN_RIGHT);
    font_draw_shadow(res.ui, battle.name, x + w + 10, y - 2, 18, RGBA(255, 160, 140, 255), ALIGN_LEFT);
    snprintf(t, sizeof(t), "%ld", battle.score);
    font_draw_shadow(res.ui, t, x + w + 10, y + 18, 16, WHITE, ALIGN_LEFT);
    if (G.t < battle.rival_hit_until)
        font_draw_shadow(res.ui, "HIT!", x + w + 10, y + 36, 16, RGBA(255, 210, 90, 255), ALIGN_LEFT);
    if (battle.warn_t > 0) {
        // the rival's attack on its way: a second's warning
        char w[48];
        snprintf(w, sizeof(w), "INCOMING: %s", atk_names[battle.warn_atk]);
        font_draw_shadow(res.title, w, SCREEN_W / 2, 96, 30, RGBA(255, 110, 90, 255), ALIGN_CENTER);
    }
    // the GH3 attacks on you: the dead fret marked on its cup, the whammy to shake off
    if (battle.broken_lane >= 0 && !G.vocal) {
        float sx, sy;
        project(lane_x(battle.broken_lane), 0, 0, &sx, &sy);
        float k = 0.75f + 0.25f * sinf((float)G.t * 12);
        gfx_rect(sx - 30, sy - 40, 60, 46, RGBA(0, 0, 0, 150));
        font_draw_shadow(res.title, "X", sx, sy - 44, 64, RGBA(255, 60, 40, (int)(255 * k)), ALIGN_CENTER);
        char t[32];
        snprintf(t, sizeof(t), "TAP IT x%d", battle.broken_left);
        font_draw_shadow(res.ui, t, sx, sy + 14, 18, RGBA(255, 200, 190, 255), ALIGN_CENTER);
    }
    if (battle.whammy_left > 0)
        font_draw_shadow(res.title, "WHAMMY TO SHAKE IT OFF!", SCREEN_W / 2, 130, 26,
                         RGBA(255, 210, 90, (int)(170 + 85 * sinf((float)G.t * 10))), ALIGN_CENTER);
    // the attacks in hand, the next one lit
    if (battle.mine_n) {
        gfx_rect(8, 198, 196, 30 + battle.mine_n * 22, RGBA(0, 0, 0, 150));
        font_draw_shadow(res.ui, "ATTACK: Star Power button", 16, 204, 15, RGBA(200, 200, 210, 255), ALIGN_LEFT);
        for (int i = 0; i < battle.mine_n; i++)
            font_draw_shadow(res.ui, atk_names[battle.mine[i]], 16, 224 + i * 22, 18,
                             i ? RGBA(200, 200, 200, 255) : RGBA(255, 210, 90, 255), ALIGN_LEFT);
    }
}

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
    if (cfg.hit_ratings && !G.vocal && G.t - G.rate_t < 0.5 && G.t - G.rate_t >= 0) {
        // the last hit's rating over the strikeline, rising and fading
        static const char * const rn[4] = {"PERFECT", "GREAT", "GOOD", "MISS"};
        static const uint32_t rc[4] = {RGBA(255, 214, 70, 255), RGBA(110, 235, 130, 255), RGBA(225, 225, 235, 255),
                                       RGBA(255, 80, 60, 255)};
        float age = (float)(G.t - G.rate_t), sx, sy;
        project(0, 0, 2.2f, &sx, &sy);
        float pop = age < 0.06f ? 1.25f - age * 4 : 1;
        char t2[32];
        if (cfg.hit_ratings == 2 && G.rate_kind < 3)
            snprintf(t2, sizeof(t2), "%s %+d", rn[G.rate_kind], (int)lroundf(G.rate_off * 1000));
        else if (G.rate_kind == 0 && G.perfect_run >= 3)
            snprintf(t2, sizeof(t2), "PERFECT x%d", G.perfect_run);
        else
            snprintf(t2, sizeof(t2), "%s", rn[G.rate_kind]);
        font_draw_shadow(res.title, t2, sx, sy - 34 - age * 30, 30 * pop, with_alpha(rc[G.rate_kind], clamp01((0.5f - age) / 0.2f)),
                         ALIGN_CENTER);
    }
    if (cfg.hud_style == 1)
        draw_flash_hud();
    else {
        const float k = 0.6f, dx = 30, dy = 204;           // the dome's 256x256 texture
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
        // World Tour's score amp under the dome: the score glowing green in its window, the multiplier a nixie tube
        // in its ring (orange, blue in Star Power), lit up a moment as it climbs. Its 256 art at ak
        const float ak = 0.62f, ax = cx - 136 * ak, ay = dy + 166 * k;
        int m = multiplier();
        static const char * const nix_a[5] = {NULL, "hud_score_nixie_1a", "hud_score_nixie_2a", "hud_score_nixie_3a", "hud_score_nixie_4a"};
        const char * nix = NULL;
        if (G.sp_active)
            nix = m == 2 ? "hud_score_nixie_2b" : m == 4 ? "hud_score_nixie_4b" : m == 6 ? "hud_score_nixie_6b" : m == 8 ? "hud_score_nixie_8b" : NULL;
        else if (m >= 1 && m <= 4)
            nix = nix_a[m];
        float rcx = ax + 136 * ak, rcy = ay + 150 * ak, rr = 56 * ak;
        gfx_rect(ax + 46 * ak, ay + 30 * ak, 176 * ak, 42 * ak, RGBA(2, 10, 4, 255));
        if (nix)
            hud_at(nix, rcx - rr + 6 * ak, rcy - rr, ak * 112 / 128, ak * 112 / 128, 0, 0, 128, 128, WHITE);
        else {
            // past the tubes the art has (a bass's x5, x6, or x10 and x12 in Star Power): the number lit in the glass
            flash_ell(rcx, rcy, rr, rr, RGBA(30, 20, 14, 255), RGBA(12, 8, 6, 255));
            char mt[8];
            snprintf(mt, sizeof(mt), "%d", m);
            font_draw_shadow(res.title, mt, rcx + 8 * ak, rcy - 24 * ak, 52 * ak,
                             G.sp_active ? RGBA(120, 220, 255, 255) : RGBA(255, 150, 50, 255), ALIGN_CENTER);
        }
        gfx_sprite(atlas_get(res.hud, "hud_score_body"), ax, ay, 256 * ak, 256 * ak, WHITE);
        if (G.mult_pop > 0.02f) {
            gfx_blend(BLEND_ADD);
            float fw = 150 * ak * (1 + 0.3f * (1 - G.mult_pop));
            gfx_sprite(atlas_get(res.hud, "hud_score_flash"), rcx - fw / 2, rcy - fw / 2, fw, fw,
                       fade_alpha(G.sp_active ? RGBA(130, 210, 255, 255) : RGBA(255, 190, 110, 255), G.mult_pop));
            gfx_blend(BLEND_ALPHA);
        }
        // the counter rolls up to the score rather than jumping
        double gap = G.score - G.shown_score;
        G.shown_score = gap < 1 ? G.score : G.shown_score + gap * 0.25 + 1;
        lcd("lcd_green", (long)G.shown_score, 7, ax + 216 * ak, ay + 38 * ak, 26 * ak * 1.15f);
        // the dome: the metal first, its bright window set inside it with ROCK in black, the three sectors pale, the one
        // the needle's in lit up. The art's plinth under the rim is cropped: the dome sits on the score plate
        float h = G.rock;
        float w0 = clamp01((0.40f - h) / 0.1f), w2 = clamp01((h - 0.60f) / 0.1f), w1 = clamp01(1 - w0 - w2);
        if (h < 0.25f)
            w0 *= 0.65f + 0.35f * fabsf(sinf(t * 6));        // failing: the red throbs
        gfx_sprite_uv(atlas_get(res.hud, "hud_rock_body"), dx, dy, 256 * k, 172 * k, 0, 0, 1, 172.0f / 256, RGBA(245, 230, 195, 255));
        (void)m;
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
        G.needle += (G.rock - G.needle) * ease(0.1f);
        float swing = (G.needle - 0.5f) * 2 * 0.85f;
        if (h < 0.25f && !G.failed)
            swing += sinf(t * 47) * 0.04f * (0.25f - h) / 0.25f;   // in the red, the needle trembles
        if (h < 0.25f && !G.failed) {
            // and a red glow swells around the dome
            gfx_blend(BLEND_ADD);
            float gw = 230 * k, ga = (0.25f - h) / 0.25f * (0.4f + 0.3f * fabsf(sinf(t * 6)));
            gfx_sprite(atlas_get(res.game, "glow"), cx - gw / 2, cy - gw / 2, gw, gw, with_alpha(RGBA(255, 40, 20, 255), ga));
            gfx_blend(BLEND_ALPHA);
        } else if (G.rock > 0.95f) {
            // full: a soft green glow
            gfx_blend(BLEND_ADD);
            float gw = 210 * k;
            gfx_sprite(atlas_get(res.game, "glow"), cx - gw / 2, cy - gw / 2, gw, gw,
                       with_alpha(RGBA(80, 255, 120, 255), 0.18f + 0.1f * fabsf(sinf(t * 3))));
            gfx_blend(BLEND_ALPHA);
        }
        sprite_rot(needle, dx + 128 * k, dy + 172 * k, 13 * k, 84 * k, needle->w / 2.0f, (float)needle->h, swing, WHITE);
    }
    if (G.drums && G.t < 4.5) {
        // which thumb goes where
        char hint[80];
        snprintf(hint, sizeof(hint), "Kick: %s   Star Power: both cymbals together, or Select",
                 cfg.drum_layout == 1 ? "R"
                                      : cfg.drum_layout == 2 ? (G.lanes == 4 ? "R, X or Down" : "X or Down") : "R or X");
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
        // the song's card slides in from the left, the cover beside the name, and fades out as the notes arrive
        float a = G.t < 3.5 ? 1 : (float)(4.5 - G.t);
        if (a > 1)
            a = 1;
        float in = (float)(G.t + G.lead_in) / 0.45f;
        in = in < 0 ? 0 : in > 1 ? 1 : in;
        float dx = -560 * (1 - in) * (1 - in) * (1 - in);
        float tx = G.art ? 128 : 24;
        gfx_rect_grad(dx, 40, 560, 110, RGBA(0, 0, 0, (int)(200 * a)), RGBA(0, 0, 0, (int)(120 * a)));
        gfx_rect(dx, 148, 560 * in, 3, with_alpha(RGBA(255, 200, 90, 255), a));
        if (G.art) {
            const Sprite art = sprite_whole(G.art);
            gfx_sprite(&art, dx + 16, 48, 96, 96, with_alpha(WHITE, a));
        }
        font_draw_fit(res.title, G.song->name, dx + tx, 44, 52, 540 - tx, with_alpha(RGBA(255, 210, 90, 255), a), ALIGN_LEFT);
        font_draw_fit(res.ui, G.song->artist, dx + tx + 2, 100, 28, 540 - tx, with_alpha(WHITE, a), ALIGN_LEFT);
        char t[96];
        if (G.venue_title)
            snprintf(t, sizeof(t), "%s  %s   at %s", part_name(G.part_id), diff_name(G.diff), G.venue_title);
        else
            snprintf(t, sizeof(t), "%s  %s", part_name(G.part_id), diff_name(G.diff));
        font_draw_fit(res.ui, t, dx + tx + 2, 126, 20, 540 - tx, with_alpha(RGBA(200, 200, 200, 255), a), ALIGN_LEFT);
    }
    if (G.t - G.ach_t < 3) {
        // a gold banner rising from the bottom
        float k = (float)(G.t - G.ach_t), in = k < 0.3f ? k / 0.3f : k > 2.6f ? (3 - k) / 0.4f : 1;
        float y = SCREEN_H - 30 - 50 * in;
        float w = font_width(res.ui, G.ach_text, 22) + 40;
        gfx_rect(SCREEN_W / 2 - w / 2, y, w, 34, RGBA(40, 25, 0, (int)(220 * in)));
        gfx_rect(SCREEN_W / 2 - w / 2, y, w, 2, RGBA(255, 210, 90, (int)(255 * in)));
        font_draw_shadow(res.ui, G.ach_text, SCREEN_W / 2, y + 6, 22, with_alpha(RGBA(255, 220, 120, 255), in), ALIGN_CENTER);
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
            // of the solo's notes judged so far (hit, early ones too, or missed): never past 100%
            int judged = 0;
            for (int i = G.first > 0 ? G.first - 1 : 0; i >= 0 && i < p->note_count; i--)
                if (G.solo[i] == k && G.state[i] != PENDING)
                    judged++;
                else if (p->notes[i].time / 1e6 < p->solos[k].start / 1e6)
                    break;
            for (int i = G.first; i < p->note_count && p->notes[i].time / 1e6 < G.t + WINDOW; i++)
                if (G.solo[i] == k && G.state[i] != PENDING)
                    judged++;
            int pct = judged ? G.solo_hit[k] * 100 / judged : 100;
            char t[32];
            snprintf(t, sizeof(t), "SOLO! %d%%", pct > 100 ? 100 : pct);
            font_draw_shadow(res.title, t, SCREEN_W / 2 + 230, 210, 40, RGBA(255, 120, 60, 255), ALIGN_CENTER);
        }
    }
    if (G.autoplay)
        font_draw_shadow(res.ui, "BOT", SCREEN_W - 70, 12, 24, RGBA(255, 255, 255, 200), ALIGN_LEFT);
    if (practice.on) {
        char t[32];
        if (practice.ramp && (int)(practice.speed * 100 + 0.5f) < practice.target)
            snprintf(t, sizeof(t), "PRACTICE  %d%% > %d%%", (int)(practice.speed * 100 + 0.5f), practice.target);
        else
            snprintf(t, sizeof(t), "PRACTICE  %d%%", (int)(practice.speed * 100 + 0.5f));
        font_draw_shadow(res.ui, t, SCREEN_W - 16, 44, 22, RGBA(255, 225, 150, 255), ALIGN_RIGHT);
        // and where in the song you are, by the chart's section ("verse_1" reads "Verse 1")
        char sec[48];
        game_section_now(sec, sizeof(sec));
        if (sec[0] && strncasecmp(sec, "ugc", 3)) {
            for (char * c = sec; *c; c++)
                if (*c == '_')
                    *c = ' ';
            if (sec[0] >= 'a' && sec[0] <= 'z')
                sec[0] -= 32;
            font_draw_shadow(res.ui, sec, SCREEN_W - 16, 70, 18, RGBA(220, 215, 205, 230), ALIGN_RIGHT);
        }
    } else if (G.song_speed < 0.995f || G.song_speed > 1.005f) {
        char t[32];
        snprintf(t, sizeof(t), "SPEED  %d%%", (int)(G.song_speed * 100 + 0.5f));
        font_draw_shadow(res.ui, t, SCREEN_W - 16, 44, 22, RGBA(255, 225, 150, 255), ALIGN_RIGHT);
    }
    draw_ghost();
    draw_battle();
    if (G.sing && !G.vocal && G.vox_graded) {
        // the singer's own tally, over the lyrics' side
        char t[48];
        snprintf(t, sizeof(t), "VOCALS %d   %d/%d", (int)G.vox_points, G.vox_good, G.vox_graded);
        gfx_rect(8, 182, font_width(res.ui, t, 20) + 16, 28, RGBA(0, 0, 0, 150));
        font_draw_shadow(res.ui, t, 16, 186, 20, RGBA(150, 210, 255, 255), ALIGN_LEFT);
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
    if (G.camera != cfg.camera || G.cam_flash != (cfg.gem_style == 1 && cfg.camera == 0))
        setup_camera();         // changed from the pause menu
    // the frame rate (the frames drawn), and how many updates this frame covers: what moves by the frame moves this much
    G.fps_frames++;
    double now = plat_now_us() / 1e6;
    if (now - G.fps_t >= 1) {
        G.fps = G.fps_frames;
        G.fps_frames = 0;
        G.fps_t = now;
    }
    z_far = cfg.hw_length == 0 ? 8.5f : cfg.hw_length == 2 ? 14.5f : 11.0f;
    unsigned du = G.updates - G.updates_drawn;
    G.updates_drawn = G.updates;
    G.fstep = du < 1 ? 1 : du > 4 ? 4 : (float)du;
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_stage();
    if (cfg.venue_light < 10)
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, (10 - cfg.venue_light) * 24));
    {
        // the chorus comes in: three white strobes over the stage (behind the highway, so the notes stay clear)
        double since = G.t - G.strobe_t;
        if (since >= 0 && since < 0.36 && fmod(since, 0.12) < 0.05 && cfg.flashes == 0)
            gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(255, 250, 240, 90));
        // the chart's strobes: on the quarters, or the sixteenths; gentler with Flashes reduced, none with them off
        int m = G.cue_mode;
        if (G.chart->cue_count && (m == CUE_STROBE_SLOW || m == CUE_STROBE_FAST || m == CUE_BRE) && cfg.flashes < 2 &&
            G.t > 0 && !G.sp_active) {
            double q = chart_quarters(G.chart, G.t) * (m == CUE_STROBE_SLOW || cfg.flashes ? 1 : 4);
            if (q - floor(q) < 0.22)
                gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(255, 252, 245, cfg.flashes ? 30 : 105));
        }
        double fl = G.t - G.flare_t;
        if (fl >= 0 && fl < G.flare_len * 3 && cfg.flashes < 2)
            gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(255, 245, 225, (int)((cfg.flashes ? 35 : 120) * exp(-fl / G.flare_len))));
    }
    if (G.rock < 0.2f && !G.failed && !G.autoplay && G.t > 0 && cfg.flashes < 2) {
        // about to fail: the edges of the picture pulse red, faster the closer it gets
        float danger = (0.2f - G.rock) / 0.2f, beat = 0.5f + 0.5f * sinf((float)G.t * (6 + 6 * danger));
        int a = (int)((40 + 90 * danger) * beat);
        gfx_tex(NULL);
        const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const float w = SCREEN_W, h = SCREEN_H, in = 140;
        uint32_t e = RGBA(200, 20, 10, a), n = RGBA(200, 20, 10, 0);
        const float l[4][3] = {{0, 0, 0}, {in, 0, 0}, {in, h, 0}, {0, h, 0}};
        const uint32_t lc[4] = {e, n, n, e};
        gfx_quad(l, uv, lc);
        const float r[4][3] = {{w - in, 0, 0}, {w, 0, 0}, {w, h, 0}, {w - in, h, 0}};
        const uint32_t rc[4] = {n, e, e, n};
        gfx_quad(r, uv, rc);
    }
    if (G.sp_flash > 0.02f && cfg.flashes < 2)
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(80, 160, 255, (int)(G.sp_flash * (cfg.flashes ? 25 : 70))));
    if (!G.vocal) {
        static float shook[16];
        proj_now = battle_proj(shook) ? shook : G.proj;
        gfx_3d(proj_now, G.view);
        gf_flags();
        if (GF.look) {
            draw_flash_board();
            gfx_3d(proj_now, G.view);
        } else
            draw_highway();
        if (!GF.look) {
            if (G.roll_count || G.gap_count)
                draw_zones();
            draw_held_lanes();
            draw_sustains();
        }
        if (cfg.gem_style == 1 && !G.drums) {
            draw_flash_notes();
            flash_flames();
            gfx_3d(proj_now, G.view);
        } else {
            draw_cups(false);
            draw_gems();
            draw_cups(true);
        }
        draw_flames();
        if (cfg.hud_style != 1 && !GF.look)
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
