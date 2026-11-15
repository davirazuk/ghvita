// The singing kept with a replay, mixed to sit in the song (voicefx.c)
#pragma once
#include <stdint.h>
#include "replay.h"

// x: 16 kHz mono from song time t0, worked on in place (the vocals' chart loaded, for its notes; fr: the replay's
// frames, for the pitch heard). Back: the mix, 16 kHz stereo, from *out_t0 (malloc'd; NULL: left as it was)
enum { VOICE_STYLES = 4 };
int16_t * voicefx_mix(int16_t * x, int n, float t0, const RFrame * fr, int nf, int style, float * out_t0);
const char * voicefx_name(int style);         // Natural, Studio, Big hall, Hard tune
