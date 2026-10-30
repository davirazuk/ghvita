// Song stems as Clone Hero and YARG ship them (Ogg Vorbis, Opus, MP3, WAV), read as 48 kHz 16-bit stereo: other
// rates are resampled on the way out
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Decoder Decoder;

Decoder * dec_open(const char * path);
void dec_close(Decoder * d);
// up to frames of 48 kHz stereo (interleaved); 0 at the end, below 0 on an error
int dec_read(Decoder * d, int16_t * out, int frames);
// to a 48 kHz frame from the start
bool dec_seek(Decoder * d, int64_t frame);
// the length in 48 kHz frames (0 when unknown)
int64_t dec_length(const Decoder * d);
int dec_file_rate(const Decoder * d);
// the length of a file in seconds (0 when it can't be read)
double dec_file_seconds(const char * path);
