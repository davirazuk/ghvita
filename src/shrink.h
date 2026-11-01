// downloads made small: a song's audio to Opus, as it lands
#pragma once
#include <stdbool.h>

// song.ogg of a Rock Band package (12+ tracks, song.mix beside it) to a stem per instrument, Opus. False leaves it as it was
bool shrink_mogg(const char * folder);
// every stem of a song folder that isn't Opus yet. What fails stays as it was
void shrink_song(const char * folder);
// every stem of a song folder is Opus already
bool shrink_done(const char * folder);
