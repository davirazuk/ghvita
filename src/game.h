// Playing a song: judging the buttons against the chart, GH scoring, Star Power, the rock meter, and drawing the
// highway and the HUD
#pragma once
#include <stdbool.h>
#include "songs.h"
#include "plat.h"

typedef struct {
    int part, diff;
    int score, stars;
    int notes_hit, notes_total, best_streak, overhits;
    int sp_hit, sp_total;
    bool failed, fc;
} Result;

enum { GAME_PLAYING, GAME_DONE, GAME_FAILED, GAME_PAUSE };

bool game_start(Song * song, int part, int diff);
// the next game_start is practice: from..to seconds over and over at speed (0.5..1), and nothing counts
void game_practice(double from, double to, float speed);
// one frame; pressed = buttons that went down this frame
int game_update(const Pad * pad, uint32_t pressed);
void game_draw(void);
void game_pause(bool pause);
void game_restart(void);
void game_stop(void);
Result game_result(void);
// test builds: the bot plays every note on time
void game_set_autoplay(bool on);
