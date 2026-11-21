// Playing together and sharing songs between two devices (over p2p.h): the lobby (the song the host picks, each one's
// part and difficulty, ready), the start at the same moment on both, each one's score live to the other, and songs
// copied from one to the other (the one a guest is missing, or any picked from the other's list).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "songs.h"

typedef struct {
    int score, streak, mult, pct;       // pct: of the notes so far
    float rock;                         // 0..1
    bool sp, done, failed;
    double at;                          // when it came (seconds, plat_now_us's)
} MpState;

typedef struct {
    char key[96];                       // the song's folder's own name on the device that has it
    char name[96], artist[96];
    int size_kb;
    bool have;                          // this device has one by the same artist and name
} MpSong;

void mp_poll(void);                     // every frame, whatever screen: the network, what came in, songs going out
bool mp_active(void);                   // connected to another device
// the lobby
void mp_choose(Song * s);               // the host's pick (sent over)
const char * mp_song_name(void);        // what's been picked ("" none yet)
const char * mp_song_artist(void);
Song * mp_song(void);                   // this device's copy of it, NULL while it hasn't one
void mp_pick(int part, int diff);       // this one's part and difficulty
void mp_their_pick(int * part, int * diff);
void mp_ready(bool on);
bool mp_am_ready(void);
bool mp_they_ready(void);
bool mp_they_have(void);
// > 0: seconds (this device's plat_now_us clock) when the song starts on both; 0 none set
double mp_start_at(void);
void mp_started(void);                  // the song's begun here (the start time is used up)
// the song
void mp_send_state(const MpState * s);
const MpState * mp_their_state(void);   // NULL nothing heard this song
void mp_song_over(void);                // back to the lobby: the next pick
// sharing songs
void mp_ask_list(void);
int mp_list(MpSong * out, int max);     // the other one's songs as far as they've come
bool mp_list_done(void);
void mp_get(const char * key);          // a song of theirs, copied here (queued)
// what's being copied, either way: in: the song coming here, out: the one going there; % of its bytes
bool mp_transfer(char * in, int in_size, int * in_pct, char * out, int out_size, int * out_pct, int * queued);
int mp_got(void);                       // songs that have come in since the connection opened
