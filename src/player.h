// Players: each keeps their own scores, favourites, settings, career, tour and daily streak. The first player's files
// are the data folder's own (as before there were players); the others' are under players/<name>/.
#pragma once
#include <stdbool.h>

#define PLAYER_MAX 8
#define PLAYER_NAME 24

void player_init(void);
// the folder this player's files go in, ending in '/'
const char * player_dir(void);
int player_count(void);
int player_current(void);
const char * player_name(int i);
// makes i the player (the caller reloads what's per player)
void player_select(int i);
// a new player: their index, or -1 (full, empty or taken name)
int player_add(const char * name);
// each player's card colour, an index into the picker's palette
#define PLAYER_COLORS 8
int player_color(int i);
void player_set_color(int i, int color);
