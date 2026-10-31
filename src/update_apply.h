// Puts a staged update in place: shared by updater.bin, which runs with the game closed, and the game itself for updater.bin
#pragma once
#include <stdbool.h>
#include <stddef.h>

// staged holds the new files and manifest.txt ("<size> <crc32 hex> <path>" lines), app the installed game, old where the
// files they replace go. A file already gone from staged was moved by a run that was cut short, so this can be run again.
// If a move fails, everything is put back. Returns the files moved, or -1 with err set.
int update_apply(const char * staged, const char * app, const char * old, char * err, size_t err_size);
// just this one file, the moment it's verified: updater.bin can't replace itself
bool update_apply_one(const char * staged, const char * app, const char * old, const char * path);
// a path a manifest may name: the program and its assets, nothing above or outside them
bool update_path_ok(const char * path);
void update_mkdirs(const char * dir);
