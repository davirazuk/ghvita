// Song playback (a decode thread streams the song's Vorbis stems into rings; the audio thread mixes them with the
// sound effects) and the song clock the notes follow
#pragma once
#include <stdbool.h>

#define MAX_STEMS 16

void audio_init(void);

// backing first, then the parts' stems: "<dir>/<name>" (pack.py's names have no extension: .ogg), any format
// decoder.c reads
bool audio_song_load(const char * dir, const char * const * stems, int count);
void audio_song_unload(void);
// starts at from seconds into the audio, after lead_in seconds of silence (the song's time runs from from - lead_in)
void audio_song_play(double from, double lead_in);
// a song list preview: the decode thread opens the stems and starts them at from seconds, fading in to volume
// (audio_song_time() is far below zero until it has); audio_song_unload() or a load drops one not started yet
void audio_preview(const char * dir, const char * const * stems, int count, double from, float volume, float fade_in);
void audio_song_pause(bool pause);
// 0.5..1: slower and lower, like a tape; the song clock runs slower with it
void audio_song_speed(float speed);
double audio_song_time(void);
float audio_level(void);
bool audio_song_finished(void);
// a song is loaded for playing
bool audio_song_active(void);
// a stem's volume, 0..1 (eased over a few milliseconds)
void audio_stem_volume(int stem, float volume);
// the whammy: how far the bar is down on a stem, 0..1; the stem stays that much lower, up to a semitone, until it comes up
void audio_stem_bend(int stem, float amount);
// how much of what's been heard so far came out of these stems, 0..1
float audio_stems_share(const int * stems, int count);
// the whole song's volume, eased over seconds (previews fade in and out)
void audio_song_fade(float volume, float seconds);
void audio_set_volumes(float music, float effects);
// underruns so far (the decoder fell behind)
int audio_underruns(void);

typedef struct Sound Sound;
Sound * sound_load(const char * path);
// returns a voice id (or -1)
int sound_play(Sound * s, float volume, bool loop);
void sound_stop(int voice);
void sound_volume(int voice, float volume);
