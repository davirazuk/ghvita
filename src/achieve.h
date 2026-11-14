// Achievements: badges for things done, each player's own (achievements.txt in the player's folder)
#pragma once
#include <stdbool.h>

typedef struct {
    const char * id, * name, * how;
} Achievement;

// what a finished (or failed) song gives the checks
typedef struct {
    int part, diff, stars, score, best_streak, overhits, sp_hit, sp_total, pct;
    bool failed, fc, practice, autoplay;
    int daily_streak, library;
    int battle, rival;          // battle: 0 none, 1 won, 2 lost; the rival's level (0 easy .. 3 brutal)
    int speed;                  // the song speed, % (100 as written)
    int accuracy;               // tenths of a percent (game.h Result.accuracy_x10), 0 when not rated
    int perfects;
    bool sudden_death, hard_rock;
    int window;                 // cfg.hit_window: 3 is Pro
} AchPlay;

int achieve_count(void);
const Achievement * achieve_get(int i);
bool achieve_has(int i);
// by id: whether it's unlocked, and its name (NULL when there's no such)
bool achieve_has_id(const char * id);
const char * achieve_name(const char * id);
int achieve_unlocked(void);
void achieve_load(void);
// checks a play: how many came new; their indices into out (up to max)
int achieve_check(const AchPlay * p, int * out, int max);
