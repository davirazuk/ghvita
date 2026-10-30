// Rock Band customs: an Xbox 360 "CON" package (STFS) holds songs.dta, a .mid and a .mogg per song
#pragma once
#include <stdbool.h>
#include <stddef.h>

// unpacks each song of the package into a folder of dest (notes.mid, song.ogg, song.ini) that the converter takes.
// Returns how many; 0 and err when none. Audio that's encrypted is left alone: that's err
int rbcon_unpack(const char * path, const char * dest, char * err, size_t err_size);
