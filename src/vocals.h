// Singing: the song's PART VOCALS (vocals.vhv, written when a song is converted) against the microphone. A worker
// thread reads the mic and finds the pitch; each frame the game asks how the note under the now line is going
#pragma once
#include <stdbool.h>

bool vox_load(const char * folder);         // false: no vocals.vhv (or nothing in it)
void vox_free(void);                        // stops the mic too
bool vox_exists(const char * folder);
void vox_start(int diff, bool autoplay);    // from the top; autoplay sings by itself, through the same detector
void vox_seek(double t);
void vox_lyrics(double t);                    // the phrases before t are over, unsung and ungraded
void vox_autoplay(bool on);
bool vox_singing(double t);                 // in a phrase at time t (no vocals: true)
double vox_end(void);                       // when the last note ends, seconds
double vox_base_score(void);                // the score with no multiplier
int vox_phrases(void);
// advances to time t. A phrase that ended is graded: its share sung (0..1) in *grade, its points in *points, true returned
bool vox_update(double t, float * grade, float * points);
void vox_draw(double t);
// the worst the mic has been: false when it couldn't be opened
bool vox_mic_ok(void);
// the pitch detector alone (for the tests): MIDI note of 1024 samples at 16 kHz, -1 none
float vox_pitch(const short * in, int n);
