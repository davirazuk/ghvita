// Replays: a run as the game saw it, frame by frame (the time, the lanes held and struck, the whammy, Star Power, what
// the microphone heard), played back through the same judging so it scores what it scored. With the singing kept
// (sing along or the vocals), heard back over the song, the song's own vocals off if wanted.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "songs.h"

typedef struct {
    double t;                   // the song's time that frame (G.t)
    uint8_t held, pressed;      // lanes (bits as game.c's), after the touch screen, lefty and the bot
    uint8_t flags;              // RF_*
    uint8_t pad_;
    float bar, dw;              // the whammy's depth and how much it moved
    float pitch, level;         // what the microphone heard (MIDI, -1 nothing; loudness), when singing
} RFrame;

enum { RF_SP = 1, RF_SING = 2 };

typedef struct {
    char song[256];             // the song's folder
    char name[96], artist[96];
    int part, diff, score, stars, notes_hit, notes_total, failed;
    int64_t when;               // seconds since 1970
    int frames, voice_n;        // voice: 16 kHz samples
    float voice_t0;             // the song's time of the first
    int build;
} ReplayInfo;

#define VOICE_RATE 16000

// recording (game.c): from the top of a run; void when it can't be played back (the bot, practice, a battle)
void replay_rec_begin(void);
void replay_rec_frame(const RFrame * f);
void replay_rec_void(void);
bool replay_rec_ok(void);
// kept: as <player>/replays/<song>_<part><diff>_last.ghr, and _best too when it's the best one kept. The singing
// (vox_record_get) goes in with it
bool replay_save(const Song * song, const ReplayInfo * info);

// the replays kept for a song (newest first): their files and what they were
int replay_list(const Song * song, char paths[][400], ReplayInfo * infos, int max);
bool replay_read_info(const char * path, ReplayInfo * info);
bool replay_last_path(const Song * song, int part, int diff, char * out, int size);

// playing one back: its settings stand in for yours while it plays (config_save keeps yours)
bool replay_load(const char * path);
void replay_unload(void);
bool replay_playing(void);
const ReplayInfo * replay_info(void);
const Config * replay_config(void);
// the next frame due by song time t (the frames in order, each once); NULL: none yet
const RFrame * replay_next(double t);
void replay_rewind(void);
// the singing, decoded: 16 kHz from t0, n frames of 1 or 2 channels (NULL: none)
const int16_t * replay_voice(int * n, float * t0, int * channels);
// mixed to sit in the song (voicefx.c; the vocals' chart must be loaded): stereo from then on
bool replay_mix_voice(int style);           // voicefx's styles; the mixer must not be playing the old mix
