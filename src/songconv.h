// Clone Hero / YARG songs into GH Vita's own, on the Vita itself (tools/pack.py does the same on a PC, and also
// remixes the audio): a song folder (song.ini + notes.chart or notes.mid + audio) or a .sng package becomes a folder
// with notes.vhc (the 5-fret parts, timed), ghvita.ini (what the song list shows), album.jpg (256x256) and the song's
// own audio files, played as they are
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char * source_id;         // remembered in ghvita.ini ("chorus:<md5>"), so the downloader knows it's there
    bool in_place;                  // write next to the song's own files (a raw Clone Hero folder in songs/)
    bool move;                      // a folder's audio files are moved into the new folder rather than copied
} ConvOptions;

// src: a song folder or a .sng file; dest_root: where the new "<Artist - Title>" folder goes (unused in place).
// true: out has the song's folder; false: err says why not
bool songconv_install(const char * src, const char * dest_root, const ConvOptions * opt, char * out, size_t out_size,
                      char * err, size_t err_size);
// a Clone Hero song folder (a chart and no ghvita.ini or pack.py song.ini yet)
bool songconv_is_raw(const char * folder);
// a picture (PNG or JPEG) as a 256x256 album.jpg
bool songconv_cover(const unsigned char * data, size_t size, const char * path);
// "Artist - Title" as a folder name (no characters exFAT refuses)
void songconv_safe_name(char * out, size_t size, const char * in);
