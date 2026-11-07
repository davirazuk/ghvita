// Playing a song: judging the buttons against the chart, GH scoring, Star Power, the rock meter, and drawing the
// highway and the HUD
#pragma once
#include <stdbool.h>
#include "songs.h"
#include "plat.h"

#define RESULT_STRIP 64

typedef struct {
    int part, diff;
    int score, stars;
    int notes_hit, notes_total, best_streak, overhits;
    int sp_hit, sp_total;
    bool failed, fc;
    bool timed;                 // enough hits to tell: on average off_ms late (early if negative), spread_ms either way
    int off_ms, spread_ms;
    uint8_t strip[RESULT_STRIP];    // start to end, the percent hit in each slice (255: nothing played there)
    char worst[44];                 // the named section that went worst, and how it went
    int worst_pct;
    int played_s;               // how long the song ran
    int misses;
    const char * weak_pad;      // drums: the pad hit least, NULL if none
    int weak_pct;
    int vox_points;             // singing along: the voice's share of the score
    int vox_good, vox_graded;   // phrases sung strong (80%+) of those graded, 0 when not singing along
    int battle;                 // 0 not a battle, 1 won, 2 lost
    int rival_score;
} Result;

enum { GAME_PLAYING, GAME_DONE, GAME_FAILED, GAME_PAUSE };

bool game_start(Song * song, int part, int diff);
// the venue the next songs play in (the tour's), NULL to leave it to the options
void game_force_venue(const char * zone);
// the next game_start is practice: from..to seconds over and over at speed (0.5..1), and nothing counts
void game_practice(double from, double to, float speed);
// the next game_start is a battle against a computer rival: Star Power phrases earn attacks to throw at them
void game_battle(void);
// after game_practice: each run through with 90% of the notes speeds the next up by step_pct, up to target_pct
void game_practice_ramp(int step_pct, int target_pct);
// while practising: the speed now, runs through so far, the best run's % of notes
bool game_practice_info(int * speed_pct, int * runs, int * best_pct);
// one frame; pressed = buttons that went down this frame
int game_update(const Pad * pad, uint32_t pressed);
void game_draw(void);
void game_pause(bool pause);
void game_log_whammy(void);
// how far through the song, and the percent hit so far (-1: nothing yet), for the pause screen
void game_status(int * song_pct, int * notes_pct);
void game_restart(void);
void game_stop(void);
Result game_result(void);
// test builds: the bot plays every note on time
void game_set_autoplay(bool on);
