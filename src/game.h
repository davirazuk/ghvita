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
    int early, on_time, late;   // hits more than 30 ms early, within 30 ms, more than 30 ms late
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
    int attacks_thrown, attacks_taken, rival_pct, rival_level;
    char rival_name[24];
    int perfects, greats, goods;    // the hits by timing: within 25 ms, 50 ms, the rest of the window
    int accuracy_x10;               // Perfect 100%, Great 70%, Good 40%, a miss 0, over every note: in tenths of a percent
    char rank[3];                   // S+ S A B C D, F when failed
} Result;

enum { GAME_PLAYING, GAME_DONE, GAME_FAILED, GAME_PAUSE };

bool game_start(Song * song, int part, int diff);
// the venue the next songs play in (the tour's), NULL to leave it to the options
void game_force_venue(const char * zone);
// the next game_start is practice: from..to seconds over and over at speed (0.5..1), and nothing counts
void game_practice(double from, double to, float speed);
float game_song_speed(void);    // the music's speed this run: 1 normal
// the next game_start is a battle against a computer rival: Star Power phrases earn attacks to throw at them.
// level: the rival's, 0 easy .. 3 brutal, or -1 for the Options' choice
void game_battle(int level);
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
// the chart's name for the section the song is in now, "" when it names none
void game_section_now(char * out, int size);
// the part and difficulty being played
void game_playing(int * part, int * diff);
// 0 no battle, 1 won, 2 lost, 3 still on
int game_battle_state(void);
void game_restart(void);
void game_stop(void);
Result game_result(void);
// test builds: the bot plays every note on time
void game_set_autoplay(bool on);
bool game_start_replay(Song * song);            // the loaded replay (replay_load) from the top
bool game_replay_song_vocals(int level);        // 2 full, 1 low, 0 off; false: not a track of its own
float game_my_voice_volume(void);        // cfg.voice_level as a volume
void game_replay_my_voice(float volume);
void game_replay_voice_style(int style, float volume);   // mixed again (a moment) in voicefx's style
void game_set_view(int view);                   // 0 all, 1 stage + lyrics, 2 stage only
