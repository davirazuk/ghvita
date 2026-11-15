// The app's own art and sounds (assets/), loaded once
#pragma once

enum { VENUE_SHOTS = 4 };   // a venue's camera shots: the wide one and three more
#include <stdbool.h>
#include "gfx.h"
#include "font.h"
#include "audio.h"

typedef struct Atlas Atlas;
Atlas * atlas_load(const char * png, const char * txt, int flags);
// a missing sprite comes back as a plain white one (and is logged)
const Sprite * atlas_get(Atlas * a, const char * name);

enum {
    SND_MISS, SND_OVERSTRUM1, SND_OVERSTRUM2, SND_OVERSTRUM3, SND_OVERSTRUM4, SND_SP_AWARD, SND_SP_DEPLOY,
    SND_SP_READY, SND_SP_RELEASE, SND_STAR, SND_STAR_GOLD, SND_SP_CROWD, SND_CROWD_GOOD, SND_CROWD_NEUTRAL,
    SND_CROWD_BAD, SND_CHEER1, SND_CHEER2, SND_CHEER3, SND_BOO1, SND_BOO2, SND_BOO3, SND_APPLAUSE, SND_UI_MOVE,
    SND_UI_SELECT, SND_UI_BACK,
    // the kit: three takes of what's hit most, two of the rest, one after another here
    SND_DRUM_KICK, SND_DRUM_SNARE = SND_DRUM_KICK + 3, SND_DRUM_HAT = SND_DRUM_SNARE + 3,
    SND_DRUM_TOM_HI = SND_DRUM_HAT + 3, SND_DRUM_TOM_MID = SND_DRUM_TOM_HI + 2, SND_DRUM_TOM_LO = SND_DRUM_TOM_MID + 2,
    SND_DRUM_RIDE = SND_DRUM_TOM_LO + 2, SND_DRUM_CRASH = SND_DRUM_RIDE + 2, SND_COUNT = SND_DRUM_CRASH + 2
};

typedef struct {
    Atlas * game;
    Atlas * hud;
    Atlas * stage;                      // what the venue adds to its picture (tools/mkstage.py)
    Font * ui;
    Font * title;
    Tex * bg_title, * bg_menu, * bg_results, * bg_stage, * logo, * vignette, * hands;
    Tex * crowd[6];                     // GHWT's crowd from behind, a row per mood (tools/crowd.py)
    Sound * snd[SND_COUNT];
    int highway_count;
    char highway_names[24][32];
    // the GHWT venues (assets/venues: <zone>.jpg, <zone>_glow.jpg and venues.txt, made by tools/venues.py)
    int venue_count;
    char venue_zones[48][16];
    char venue_titles[48][32];
    float venue_marks[48][4][3];        // drummer, vocalist, guitarist, bassist: x, y of the feet, pixels per metre
    // the camera's other shots (<zone>_s1.jpg ..): how many each venue has, and where the band's marks land in them
    int venue_shots[48];
    float venue_shot_marks[48][VENUE_SHOTS - 1][4][3];
    // the band's sprite sheets (assets/band, made by tools/band.py)
    int band_count;
    char band_names[24][16];
    float band_rise[24][16];            // a sheet's Star Power jump: height off the floor per frame (band/rise.txt)
    bool band_has_rise[24];
} Res;

extern Res res;

void res_load(void);
// the loading screen (the title picture, text and a bar 0..1); res_load draws it as it goes
void res_title_art(void);
// a hint to show while something loads; next: move on to another
const char * res_tip(bool next);
void res_loading(float done, const char * text);
// the sound effects load on a thread after res_load: a song waits for them
void res_wait_sounds(void);
// the highway picture by name ("classic"...): loaded on demand, one at a time
Tex * res_highway(const char * name);
Tex * res_highway_preview(const char * name);
Tex * res_venue_preview(const char * zone);
// a venue's two pictures, the stage and its lights alone (loaded on demand, one venue at a time); false: no such venue
bool res_venue(const char * zone, Tex ** stage, Tex ** lights);
// shot 1.. of a venue (0 is res_venue's); kept loaded with the venue
bool res_venue_shot(const char * zone, int shot, Tex ** stage, Tex ** lights);
int res_venue_shot_count(const char * zone);
const float (*res_venue_shot_marks(const char * zone, int shot))[3];
// "House of Blues" for "hob" (the zone itself when unknown)
const char * res_venue_title(const char * zone);
// where the band stands in this venue (NULL when unknown)
const float (*res_venue_marks(const char * zone))[3];
// a band member's sheet by name ("guitarist_3"), one kept per role (0 drummer, 1 vocalist, 2 guitarist, 3 bassist)
Tex * res_band_sheet(int role, const char * name);
Tex * res_band_alt(int role);                   // that member's wilder loop (NULL: none), loaded with the sheet
void res_band_free(void);
void ui_sound(int which);
// a background picture that fills the screen (they sit in the top-left 960x544 of a 1024x1024 texture)
void draw_background(Tex * t, uint32_t color);
