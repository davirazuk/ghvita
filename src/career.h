// What's been played on this Vita, over all songs: the CAREER screen
#ifndef CAREER_H
#define CAREER_H
#include "chart.h"
#include <stdbool.h>

typedef struct {
    int plays, finished, failed, full_combos;
    long long notes_hit, notes_total, overhits;
    int best_streak, best_score, stars;
    long long seconds;
    int part_plays[PART_COUNT];
    int battles_won, battles_lost;
} Career;

extern Career career;
void career_load(void);
void career_add(int part, int score, int stars, int hit, int total, int streak, int overhits, int seconds, bool failed,
                bool fc);
// the title that goes with the stars earned, and how many the next one takes (0: the top)
const char * career_rank(int * next_at);
// a battle's outcome, for the record
void career_battle(bool won);
#endif
