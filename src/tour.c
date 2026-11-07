#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "player.h"
#include "plat.h"
#include "songs.h"
#include "tour.h"

static const struct {
    const char * name;
    const char * zones[TOUR_GIGS];
} legs[TOUR_LEGS] = {
    {"The Local Scene", {"frathouse", "goth", "recordstore", "hob", "subway"}},
    {"On the Road", {"fairgrounds", "bayou", "hotel", "nashville", "neworleans"}},
    {"Making Noise", {"harbor", "cathedral", "military", "starwood", "studio"}},
    {"Big in America", {"ballpark", "newyork", "forum", "meadowlands", "dragrace"}},
    {"Europe", {"london", "paris", "dublin", "norway", "donnington"}},
    {"Around the World", {"tushino", "cairo", "carhenge", "stone", "s_stage"}},
    {"Festival Season", {"metalfest", "justice", "mop", "studio2", "freakshow"}},
    {"Out of This World", {"castle", "icecave", "lhc", "scifi", "space"}},
};

static const struct {
    int stars;
    const char * name;
} ranks[] = {
    {0, "Garage Band"}, {20, "Bar Band"}, {60, "Club Headliners"}, {120, "Opening Act"}, {200, "Arena Band"},
    {320, "Stadium Rockers"}, {480, "Rock Gods"}, {640, "Legends of Rock"},
};

typedef struct {
    char song[TOUR_SET][128];       // folder names
    uint8_t stars[TOUR_SET];
} Gig;

static Gig gigs[TOUR_LEGS * TOUR_GIGS];

static Gig * gig_at(int leg, int gig)
{
    return &gigs[leg * TOUR_GIGS + gig];
}

static const char * base_of(const char * folder)
{
    const char * b = strrchr(folder, '/');
    return b ? b + 1 : folder;
}

void tour_load(void)
{
    char path[400], line[400];
    memset(gigs, 0, sizeof(gigs));
    snprintf(path, sizeof(path), "%stour.txt", player_dir());
    FILE * f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        int g, slot, stars, used;
        if (sscanf(line, "%d %d %d %n", &g, &slot, &stars, &used) != 3 || g < 0 || g >= TOUR_LEGS * TOUR_GIGS ||
            slot < 0 || slot >= TOUR_SET)
            continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(gigs[g].song[slot], sizeof(gigs[g].song[slot]), "%s", line + used);
        gigs[g].stars[slot] = (uint8_t)stars;
    }
    fclose(f);
}

static void tour_save(void)
{
    char path[400], fresh[400];
    snprintf(path, sizeof(path), "%stour.txt", player_dir());
    snprintf(fresh, sizeof(fresh), "%stour.new", player_dir());
    FILE * f = fopen(fresh, "w");
    if (!f)
        return;
    for (int g = 0; g < TOUR_LEGS * TOUR_GIGS; g++)
        for (int s = 0; s < TOUR_SET; s++)
            if (gigs[g].song[s][0])
                fprintf(f, "%d %d %d %s\n", g, s, gigs[g].stars[s], gigs[g].song[s]);
    fclose(f);
    remove(path);
    rename(fresh, path);
}

const char * tour_leg_name(int leg)
{
    return legs[leg].name;
}

const char * tour_zone(int leg, int gig)
{
    return legs[leg].zones[gig];
}

int tour_songs_done(int leg, int gig)
{
    int n = 0;
    for (int s = 0; s < TOUR_SET; s++)
        n += gig_at(leg, gig)->stars[s] > 0;
    return n;
}

bool tour_gig_done(int leg, int gig)
{
    return tour_songs_done(leg, gig) == TOUR_SET;
}

int tour_gigs_done(int leg)
{
    int n = 0;
    for (int g = 0; g < TOUR_GIGS; g++)
        n += tour_gig_done(leg, g);
    return n;
}

bool tour_leg_open(int leg)
{
    return leg == 0 || (tour_leg_open(leg - 1) && tour_gigs_done(leg - 1) >= TOUR_TO_OPEN);
}

int tour_gig_stars(int leg, int gig)
{
    int n = 0;
    for (int s = 0; s < TOUR_SET; s++) {
        int st = gig_at(leg, gig)->stars[s];
        n += st > 5 ? 5 : st;
    }
    return n;
}

int tour_stars(void)
{
    int n = 0;
    for (int l = 0; l < TOUR_LEGS; l++)
        for (int g = 0; g < TOUR_GIGS; g++)
            n += tour_gig_stars(l, g);
    return n;
}

const char * tour_rank(void)
{
    const char * r = ranks[0].name;
    int have = tour_stars();
    for (size_t i = 0; i < sizeof(ranks) / sizeof(ranks[0]); i++)
        if (have >= ranks[i].stars)
            r = ranks[i].name;
    return r;
}

int tour_slot_stars(int leg, int gig, int slot)
{
    return gig_at(leg, gig)->stars[slot];
}

void tour_record(int leg, int gig, int slot, int stars)
{
    Gig * g = gig_at(leg, gig);
    if (stars > g->stars[slot]) {
        g->stars[slot] = (uint8_t)stars;
        tour_save();
    }
}

// ---------------------------------------------------------------------------------------------------- the setlists

static int hard_part;

static uint32_t name_hash(const char * s, uint32_t h)
{
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

// songs without a difficulty read as the middle one
static int hardness(const Song * s)
{
    int d = s->diff[hard_part];
    return d < 0 ? 3 : d;
}

static int by_hardness(const void * a, const void * b)
{
    const Song * x = songs_get(*(const int *)a), * y = songs_get(*(const int *)b);
    int c = hardness(x) - hardness(y);
    if (c)
        return c;
    uint32_t hx = name_hash(x->folder, 2166136261u), hy = name_hash(y->folder, 2166136261u);
    return hx < hy ? -1 : hx > hy;
}

static int times_used(const char * base)
{
    int n = 0;
    for (int g = 0; g < TOUR_LEGS * TOUR_GIGS; g++)
        for (int s = 0; s < TOUR_SET; s++)
            n += strcmp(gigs[g].song[s], base) == 0;
    return n;
}

static int same_band(const Gig * g, const Song * s)
{
    int n = 0, count = songs_count();
    for (int o = 0; o < TOUR_SET; o++)
        for (int i = 0; g->song[o][0] && i < count; i++)
            if (strcmp(base_of(songs_get(i)->folder), g->song[o]) == 0) {
                n += strcasecmp(songs_get(i)->artist, s->artist) == 0;
                break;
            }
    return n;
}

// the leg's stretch of the library, easy to hard: what's been played least goes first, the encore the hardest of those
static void pick_songs(int leg, int gig, int part)
{
    Gig * g = gig_at(leg, gig);
    int n = songs_count(), have = 0;
    int * list = malloc((n + 1) * sizeof(int));
    if (!list)
        return;
    for (int i = 0; i < n; i++)
        if (songs_get(i)->parts & (1 << part))
            list[have++] = i;
    if (have < TOUR_SET) {                  // too few with this part: any song will do
        have = n;
        for (int i = 0; i < n; i++)
            list[i] = i;
    }
    hard_part = part;
    qsort(list, have, sizeof(int), by_hardness);
    int from = have * leg / TOUR_LEGS, to = have * (leg + 2) / TOUR_LEGS;
    if (to > have)
        to = have;
    while (to - from < TOUR_SET * 2 && (from > 0 || to < have)) {
        if (from > 0)
            from--;
        if (to < have)
            to++;
    }
    for (int s = 0; s < TOUR_SET; s++) {
        if (g->song[s][0] && g->stars[s])
            continue;                       // played: it stays, even if the song's left the card
        int best = -1, best_used = 0;
        uint32_t best_h = 0;
        for (int i = from; i < to; i++) {
            const char * base = base_of(songs_get(list[i])->folder);
            bool taken = false;
            for (int o = 0; o < TOUR_SET; o++)
                taken |= strcmp(g->song[o], base) == 0;
            if (taken)
                continue;
            // and the same band twice on one bill only when there's no one else
            int used = times_used(base) + same_band(g, songs_get(list[i])) * 100;
            uint32_t h = name_hash(base, 2166136261u + leg * TOUR_GIGS + gig);
            // the list runs easy to hard: on a tie the encore takes the later one
            if (best < 0 || used < best_used || (used == best_used && (s == TOUR_ENCORE || h < best_h))) {
                best = i;
                best_used = used;
                best_h = h;
            }
        }
        if (best >= 0)
            snprintf(g->song[s], sizeof(g->song[s]), "%s", base_of(songs_get(list[best])->folder));
    }
    free(list);
    tour_save();
}

void tour_setlist(int leg, int gig, int part, int ids[TOUR_SET])
{
    Gig * g = gig_at(leg, gig);
    int n = songs_count();
    for (int pass = 0; pass < 2; pass++) {
        bool missing = false;
        for (int s = 0; s < TOUR_SET; s++) {
            ids[s] = -1;
            for (int i = 0; g->song[s][0] && i < n && ids[s] < 0; i++)
                if (strcmp(base_of(songs_get(i)->folder), g->song[s]) == 0)
                    ids[s] = i;
            missing |= ids[s] < 0 && !g->stars[s];
        }
        if (!missing || !n)
            return;
        pick_songs(leg, gig, part);
    }
}
