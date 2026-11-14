#include <stdio.h>
#include <string.h>
#include <time.h>
#include "achieve.h"
#include "career.h"
#include "chart.h"
#include "player.h"
#include "tour.h"

static const Achievement list[] = {
    {"first", "Opening Act", "Finish a song"},
    {"plays10", "Regular", "Finish 10 songs"},
    {"plays50", "Road Warrior", "Finish 50 songs"},
    {"plays250", "Living Legend", "Finish 250 songs"},
    {"fc", "Flawless", "Full combo a song"},
    {"fc_hard", "Steady Hands", "Full combo a song on Hard"},
    {"fc_expert", "Untouchable", "Full combo a song on Expert"},
    {"five_expert", "Rock Star", "5 stars on Expert"},
    {"gold", "Golden", "Gold stars on a song"},
    {"streak100", "Centurion", "A 100-note streak"},
    {"streak500", "Unstoppable", "A 500-note streak"},
    {"streak1000", "Machine", "A 1000-note streak"},
    {"sp_all", "Power Hungry", "Every Star Power phrase in a song (3 or more)"},
    {"clean", "Clean Hands", "90% on Expert with no overhits"},
    {"band", "Whole Band", "Finish songs on guitar, bass, drums and vocals"},
    {"keys", "Ivory Tickler", "Finish a song on keys"},
    {"drums_expert", "Thunder Feet", "Finish a song on Expert drums"},
    {"vocals_five", "Golden Voice", "5 stars singing"},
    {"comeback", "Close Call", "Finish a song with under 60% of the notes"},
    {"daily3", "Habit", "Daily song 3 days running"},
    {"daily7", "Week Long", "Daily song 7 days running"},
    {"daily30", "Devoted", "Daily song 30 days running"},
    {"hours1", "Warmed Up", "An hour of playing"},
    {"hours10", "Dedicated", "10 hours of playing"},
    {"tour_leg", "On the Road", "Finish a World Tour leg"},
    {"tour_all", "Headliner", "Finish the whole World Tour"},
    {"library100", "Collector", "100 songs on the card"},
    {"library500", "Archivist", "500 songs on the card"},
    {"battle1", "Rival Slayer", "Win a battle"},
    {"battle10", "Duelist", "Win 10 battles"},
    {"brutal", "Giant Killer", "Beat a Brutal rival on Expert"},
    {"speed125", "Speed Demon", "Finish a song at 125% song speed or faster"},
    {"speed150", "Ludicrous", "4 stars at 150% song speed"},
    {"rank_a", "Sharp", "An A rank (90% accuracy) on Hard or Expert"},
    {"rank_s", "Surgical", "An S rank (95% accuracy) on Expert"},
    {"rank_splus", "Perfectionist", "S+ on Expert: 98% accuracy and a full combo"},
    {"perfect300", "Laser Focus", "300 Perfect hits in one song"},
    {"sudden", "No Second Chances", "Finish a song on Hard or Expert with Sudden death on"},
    {"pro_window", "Pro Timing", "4 stars on Expert with the Pro timing window"},
    {"hard_rock", "Tough Crowd", "Finish an Expert song with the Hard rock meter"},
};
#define N (int)(sizeof(list) / sizeof(list[0]))
static bool have[N];

int achieve_count(void)
{
    return N;
}

const Achievement * achieve_get(int i)
{
    return i >= 0 && i < N ? &list[i] : NULL;
}

bool achieve_has(int i)
{
    return i >= 0 && i < N && have[i];
}

bool achieve_has_id(const char * id)
{
    for (int i = 0; i < N; i++)
        if (!strcmp(list[i].id, id))
            return have[i];
    return false;
}

const char * achieve_name(const char * id)
{
    for (int i = 0; i < N; i++)
        if (!strcmp(list[i].id, id))
            return list[i].name;
    return NULL;
}

int achieve_unlocked(void)
{
    int n = 0;
    for (int i = 0; i < N; i++)
        n += have[i];
    return n;
}

static void path_of(char * path, size_t n)
{
    snprintf(path, n, "%sachievements.txt", player_dir());
}

void achieve_load(void)
{
    memset(have, 0, sizeof(have));
    char path[400], line[96];
    path_of(path, sizeof(path));
    FILE * f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, " \t\r\n")] = 0;
        for (int i = 0; i < N; i++)
            if (!strcmp(line, list[i].id))
                have[i] = true;
    }
    fclose(f);
}

static bool tour_leg_finished(bool all)
{
    int legs = 0;
    for (int l = 0; l < TOUR_LEGS; l++)
        legs += tour_gigs_done(l) >= TOUR_GIGS;
    return all ? legs == TOUR_LEGS : legs > 0;
}

int achieve_check(const AchPlay * p, int * out, int max)
{
    if (p->autoplay)
        return 0;
    bool done = !p->failed && !p->practice;
    bool expert = p->diff == DIFF_EXPERT, vocal = p->part == PART_VOCALS;
    bool now[N] = {0};
    for (int i = 0; i < N; i++) {
        const char * id = list[i].id;
        bool v = false;
        if (!strcmp(id, "first")) v = career.finished >= 1;
        else if (!strcmp(id, "plays10")) v = career.finished >= 10;
        else if (!strcmp(id, "plays50")) v = career.finished >= 50;
        else if (!strcmp(id, "plays250")) v = career.finished >= 250;
        else if (!strcmp(id, "fc")) v = done && p->fc;
        else if (!strcmp(id, "fc_hard")) v = done && p->fc && p->diff >= DIFF_HARD;
        else if (!strcmp(id, "fc_expert")) v = done && p->fc && expert;
        else if (!strcmp(id, "five_expert")) v = done && expert && p->stars >= 5;
        else if (!strcmp(id, "gold")) v = done && p->stars >= 6;
        else if (!strcmp(id, "speed125")) v = done && p->speed >= 125;
        else if (!strcmp(id, "speed150")) v = done && p->speed >= 150 && p->stars >= 4;
        else if (!strcmp(id, "rank_a")) v = done && !vocal && p->diff >= DIFF_HARD && p->accuracy >= 900;
        else if (!strcmp(id, "rank_s")) v = done && !vocal && expert && p->accuracy >= 950;
        else if (!strcmp(id, "rank_splus")) v = done && !vocal && expert && p->accuracy >= 980 && p->fc;
        else if (!strcmp(id, "perfect300")) v = done && p->perfects >= 300;
        else if (!strcmp(id, "sudden")) v = done && !vocal && p->sudden_death && p->diff >= DIFF_HARD;
        else if (!strcmp(id, "pro_window")) v = done && expert && p->window == 3 && p->stars >= 4;
        else if (!strcmp(id, "hard_rock")) v = done && expert && p->hard_rock;
        else if (!strcmp(id, "streak100")) v = !p->practice && p->best_streak >= 100;
        else if (!strcmp(id, "streak500")) v = !p->practice && p->best_streak >= 500;
        else if (!strcmp(id, "streak1000")) v = !p->practice && p->best_streak >= 1000;
        else if (!strcmp(id, "sp_all")) v = done && p->sp_total >= 3 && p->sp_hit == p->sp_total;
        else if (!strcmp(id, "clean")) v = done && expert && !vocal && p->pct >= 90 && !p->overhits;
        else if (!strcmp(id, "band"))
            v = career.part_plays[PART_GUITAR] && career.part_plays[PART_BASS] && career.part_plays[PART_DRUMS] &&
                career.part_plays[PART_VOCALS];
        else if (!strcmp(id, "keys")) v = done && p->part == PART_KEYS;
        else if (!strcmp(id, "drums_expert")) v = done && p->part == PART_DRUMS && expert;
        else if (!strcmp(id, "vocals_five")) v = done && vocal && p->stars >= 5;
        else if (!strcmp(id, "comeback")) v = done && p->pct < 60;
        else if (!strcmp(id, "daily3")) v = p->daily_streak >= 3;
        else if (!strcmp(id, "daily7")) v = p->daily_streak >= 7;
        else if (!strcmp(id, "daily30")) v = p->daily_streak >= 30;
        else if (!strcmp(id, "hours1")) v = career.seconds >= 3600;
        else if (!strcmp(id, "hours10")) v = career.seconds >= 36000;
        else if (!strcmp(id, "tour_leg")) v = tour_leg_finished(false);
        else if (!strcmp(id, "tour_all")) v = tour_leg_finished(true);
        else if (!strcmp(id, "library100")) v = p->library >= 100;
        else if (!strcmp(id, "library500")) v = p->library >= 500;
        else if (!strcmp(id, "battle1")) v = p->battle == 1;
        else if (!strcmp(id, "battle10")) v = career.battles_won >= 10;
        else if (!strcmp(id, "brutal")) v = p->battle == 1 && p->rival == 3 && expert;
        now[i] = v && !have[i];
    }
    int n = 0;
    char path[400];
    path_of(path, sizeof(path));
    FILE * f = NULL;
    for (int i = 0; i < N; i++) {
        if (!now[i])
            continue;
        have[i] = true;
        if (!f)
            f = fopen(path, "a");
        if (f)
            fprintf(f, "%s %ld\n", list[i].id, (long)time(NULL));
        if (n < max)
            out[n] = i;
        n++;
    }
    if (f)
        fclose(f);
    return n;
}
