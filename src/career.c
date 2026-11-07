#include "player.h"
#include "career.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Career career;

static const struct {
    int stars;
    const char * name;
} ranks[] = {
    {0, "Bedroom Player"}, {15, "Garage Band"}, {50, "Bar Regular"}, {120, "Local Hero"}, {250, "Opening Act"},
    {500, "Headliner"}, {900, "Arena Act"}, {1500, "Stadium Legend"}, {2500, "Rock God"},
};

const char * career_rank(int * next_at)
{
    int n = sizeof(ranks) / sizeof(ranks[0]), k = 0;
    while (k + 1 < n && career.stars >= ranks[k + 1].stars)
        k++;
    if (next_at)
        *next_at = k + 1 < n ? ranks[k + 1].stars : 0;
    return ranks[k].name;
}

static void path_of(char * path, size_t n)
{
    snprintf(path, n, "%scareer.txt", player_dir());
}

void career_load(void)
{
    memset(&career, 0, sizeof(career));
    char path[400], line[120];
    path_of(path, sizeof(path));
    FILE * f = fopen(path, "r");
    if (!f)
        return;
    Career * c = &career;
    while (fgets(line, sizeof(line), f)) {
        char k[40];
        long long v;
        int part;
        if (sscanf(line, "%39s = %lld", k, &v) != 2 || v < 0)
            continue;
        if (!strcmp(k, "plays")) c->plays = (int)v;
        else if (!strcmp(k, "finished")) c->finished = (int)v;
        else if (!strcmp(k, "failed")) c->failed = (int)v;
        else if (!strcmp(k, "full_combos")) c->full_combos = (int)v;
        else if (!strcmp(k, "notes_hit")) c->notes_hit = v;
        else if (!strcmp(k, "notes_total")) c->notes_total = v;
        else if (!strcmp(k, "overhits")) c->overhits = v;
        else if (!strcmp(k, "best_streak")) c->best_streak = (int)v;
        else if (!strcmp(k, "best_score")) c->best_score = (int)v;
        else if (!strcmp(k, "stars")) c->stars = (int)v;
        else if (!strcmp(k, "seconds")) c->seconds = v;
        else if (sscanf(k, "part%d", &part) == 1 && part >= 0 && part < PART_COUNT)
            c->part_plays[part] = (int)v;
    }
    fclose(f);
}

static void save(void)
{
    char path[400], tmp[410];
    path_of(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (!f)
        return;
    const Career * c = &career;
    fprintf(f, "plays = %d\nfinished = %d\nfailed = %d\nfull_combos = %d\nnotes_hit = %lld\nnotes_total = %lld\n"
               "overhits = %lld\nbest_streak = %d\nbest_score = %d\nstars = %d\nseconds = %lld\n",
            c->plays, c->finished, c->failed, c->full_combos, c->notes_hit, c->notes_total, c->overhits, c->best_streak,
            c->best_score, c->stars, c->seconds);
    for (int i = 0; i < PART_COUNT; i++)
        fprintf(f, "part%d = %d\n", i, c->part_plays[i]);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

void career_add(int part, int score, int stars, int hit, int total, int streak, int overhits, int seconds, bool failed,
                bool fc)
{
    Career * c = &career;
    c->plays++;
    c->finished += !failed;
    c->failed += failed;
    c->full_combos += fc;
    c->notes_hit += hit;
    c->notes_total += total;
    c->overhits += overhits;
    c->seconds += seconds;
    if (streak > c->best_streak)
        c->best_streak = streak;
    if (!failed) {
        c->stars += stars;
        if (score > c->best_score)
            c->best_score = score;
    }
    if (part >= 0 && part < PART_COUNT)
        c->part_plays[part]++;
    save();
}
