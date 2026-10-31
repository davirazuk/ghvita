// World Tour: forty gigs at the venues in eight legs. A setlist comes from the player's own songs, the easy ones
// first, and stays the gig's once picked; three gigs played open the next leg
#pragma once
#include <stdbool.h>

#define TOUR_LEGS 8
#define TOUR_GIGS 5                 // to a leg
#define TOUR_SET 4                  // to a gig: three songs, then the encore
#define TOUR_ENCORE (TOUR_SET - 1)
#define TOUR_TO_OPEN 3              // gigs of a leg that open the next

void tour_load(void);
const char * tour_leg_name(int leg);
const char * tour_zone(int leg, int gig);
bool tour_leg_open(int leg);
int tour_gigs_done(int leg);
bool tour_gig_done(int leg, int gig);
int tour_gig_stars(int leg, int gig);
int tour_songs_done(int leg, int gig);
int tour_stars(void);
const char * tour_rank(void);
// what the gig plays (picked now if it's the first look): each slot's library index, -1 for a song no longer there
void tour_setlist(int leg, int gig, int part, int ids[TOUR_SET]);
int tour_slot_stars(int leg, int gig, int slot);
// a song played through: stars kept if they're better
void tour_record(int leg, int gig, int slot, int stars);
