// Players: each keeps their own scores, favourites, settings, career, tour, daily streak, achievements, ghosts and
// picture. Each has a folder of their own by a number that never changes: 0 is the data folder itself (the files from
// before there were players), the others players/<n>/.
#pragma once
#include <stdbool.h>

#define PLAYER_MAX 8
#define PLAYER_NAME 24
#define PLAYER_COLORS 8

void player_init(void);
// the folder this player's files go in, ending in '/'
const char * player_dir(void);
// player i's folder, whoever is playing
void player_dir_of(int i, char * out, int size);
int player_count(void);
int player_current(void);
const char * player_name(int i);
// makes i the player (the caller reloads what's per player)
void player_select(int i);
// a new player: their index, or -1 (full, empty or taken name)
int player_add(const char * name);
// false: empty or taken
bool player_rename(int i, const char * name);
// removes the player and all their files (not the last one left)
bool player_delete(int i);
// each player's card colour, an index into the picker's palette
int player_color(int i);
void player_set_color(int i, int color);
// the player's picture file, or "" when they have none
void player_picture(int i, char * out, int size);
// a picture for player i from bytes (PNG or JPEG); true when it's set
bool player_set_picture(int i, const unsigned char * data, int size);
void player_clear_picture(int i);
// changes every time a picture changes, so a screen knows to load it again
int player_picture_version(void);
