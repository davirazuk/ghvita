// The song library (ux0:data/ghvita/songs/*/: ghvita.ini from the Vita's own converter, or song.ini from
// tools/pack.py), best scores and the settings
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "chart.h"
#include "audio.h"

typedef struct {
    int score;
    uint8_t stars;          // 0-6 (6: gold)
    uint8_t percent;
    uint8_t fc;
    uint16_t acc;           // the best accuracy, in tenths of a percent (0: none kept: from before build 227)
} Score;

typedef struct {
    char folder[256];
    char name[96], artist[96], album[96], genre[48], year[12], charter[64];
    char sort_name[96], sort_artist[96];
    int length_ms, preview_ms;
    uint8_t parts;                          // bit per PART_*
    int8_t chart_ver;                       // the converter that made it (ghvita.ini chart; 1 when absent)
    int8_t diff[PART_COUNT];                // song.ini's difficulty tiers (-1 unknown)
    uint8_t guessed;                        // bit per part: its tier was estimated from the chart; 0x80 done
    char stems[MAX_STEMS][24];
    int stem_count;
    int8_t part_stem[PART_COUNT];           // index into stems, -1 none
    char source[48];                        // where the downloader got it ("chorus:<md5>"), else empty
    Score best[PART_COUNT][DIFF_COUNT];
    long added;                             // when it came onto the card
    long stamp;                             // its folder's time when read: the library file's test for a change
    int plays;
    long last_played;                       // when it was last finished (0 never, or only before times were kept)
    bool fav;
} Song;

enum { SORT_TITLE, SORT_ARTIST, SORT_ALBUM, SORT_YEAR, SORT_GENRE, SORT_CHARTER, SORT_HARD, SORT_LENGTH, SORT_NEW, SORT_PLAYS,
       SORT_RECENT, SORT_COUNT };

// progress (or NULL) every 25 songs found, and before each new Clone Hero song (a folder or a .sng dropped in songs/)
// is converted, with its name
int songs_scan(void (*progress)(int found, const char * converting));
// forget what the library file remembers: the next scan opens every folder
void songs_forget(void);
// a song just installed (the downloader's): its index after sorting, or -1
int songs_add(const char * folder);
int songs_find(const char * folder);
// 2: a song from this source ("chorus:<md5>"), 1: one by the same artist and name (any case and punctuation), 0: no
int songs_have(const char * source, const char * artist, const char * name);
int songs_count(void);
// off the card and out of the list
void songs_delete(int index);
Song * songs_get(int index);
// tiers the song's ini didn't give, estimated from its Expert chart's busyness (once; kept in the library file)
void songs_guess_tiers(Song * s);
// writes the library file if estimates have changed it
void songs_flush(void);
void songs_sort(int mode);
const char * sort_name(int mode);
// what a song is listed under in a sort: its first letter, decade, genre...; L and R jump between these
void songs_group(const Song * s, int mode, char * out, size_t size);
void songs_set_fav(Song * s, bool fav);
void songs_save_score(Song * s, int part, int diff, Score score);
// the song's finished plays on a part and difficulty, oldest first, the newest max of them; when: unix time, 0 unknown
typedef struct {
    Score score;
    long when;
} Played;
// another player: their scores, plays and favourites in place of the last one's
void songs_reload_player(void);
int songs_history(const Song * s, int part, int diff, Played * out, int max);
const char * part_name(int part);
const char * diff_name(int diff);

// lanes: green red yellow blue orange, then the open note
#define LANES 5
#define LANE_OPEN 5

typedef struct {
    int speed;              // note speed 1-10
    char highway[32];
    char venue[16];         // "song" (each song its own), "random", "classic" (no venue) or a venue's zone
    int offset_ms;          // audio offset: notes are judged and drawn this much later
    int layout;             // 0 default, 1 alternate
    int drum_layout;        // 0 thumbs on the pads, 1 the shoulders, 2 the guitar's own buttons per colour
    bool drum_hits;         // every pad hit sounds, not just those in the gaps
    bool tilt;              // Star Power by tilting
    bool nofail;
    int music_vol, sfx_vol, crowd_vol;      // 0-10
    bool lefty;
    bool show_fps;
    bool no_band;           // no band on the venue's stage (saves its four sheets and draws)
    char band_pick[4][16];  // drummer, singer, guitarist, bassist chosen in the options ("": a different one each song)
    int camera;             // 0 World Tour, 1 lower and nearer, 2 high and long
    int highway_alpha;      // 0-10, how solid the highway is
    int venue_light;        // 0-10, how bright the stage behind it is
    int flashes;            // 0 full, 1 soft, 2 none
    bool no_popups;         // no "50 NOTE STREAK!" and the like
    bool timing_bar;        // where the last hits fell in the window
    bool live_stats;        // percent hit and a full-combo mark in the song
    bool no_ghost;          // no racing the chart's best run
    bool metronome;         // practice ticks along with the beat
    int song_speed;         // the song's speed in %, outside practice (below 100 the run isn't saved)
    bool light_fx;          // the lighter look: no ring, glitter, smoke, backlight or crowd flashes (for a steady frame rate)
    int hit_window;         // 0 normal (85 ms), 1 strict (70), 2 lenient (110), 3 pro (55), 4 GH3 (116 early, 100 late)
    int rival;              // battle: the rival's skill, 0 easy 1 normal 2 hard 3 brutal
    bool vanish;            // notes vanish before they reach the strikeline
    bool no_miss_sound;
    bool no_rear_whammy;    // the rear touchpad isn't a whammy bar
    bool no_bend;           // the whammy earns Star Power but leaves the sound alone
    bool no_countdown;      // resume at once
    bool stage_screens;     // the song's cover on two screens over the stage
    bool no_lyrics;         // no words along the top
    bool auto_repress;      // a note on a fret still down plays by itself, whatever's above it
    bool flat_notes;        // notes and cups lie on the neck
    bool no_wobble;         // held notes stay straight
    int fire;               // hit flames: 0 full, 1 small, 2 none
    int note_size;          // 0 small, 1 normal, 2 big
    bool no_crowd;          // no hands at the foot of the picture
    bool no_vignette;       // no darkened edges
    bool no_menu_music;     // silent menus
    int cup_skin;           // 0 World Tour's buttons, 1 the deep cups
    bool no_progress;       // no song progress line along the top
    bool no_count_in;       // no sticks before the first note
    bool easy_sustains;     // one fret of a chord keeps its sustain ringing
    bool double_kick;       // Expert+: the kicks charted for a second foot
    bool five_pads;         // 4-pad charts laid out on World Tour's five
    bool sing_along;        // the mic and the lyrics on top of any instrument, when the song has vocals
    bool sing_sp;           // singing in tune starts Star Power, like YARG's voice-activated option
    int sing_diff;          // the singing along is judged at: the instrument's would be Expert (MEDIUM as set)
    bool still_camera;      // the venue's wide shot only: no cuts to its other camera angles (and 7.5 MB less loaded)
    int frame_rate;         // in songs: 60, or 30 (every other frame drawn; buttons still read 60 times a second)
    int backdrop;           // 0 the venue and band, 1 the song's cover art (Flash's band photo), 2 dark
    int hit_ratings;        // 0 off, 1 PERFECT / GREAT / GOOD over the strikeline, 2 the same with the milliseconds
    int hw_length;          // the highway's far end: 0 short, 1 normal, 2 long
    bool sudden_death;      // one missed note fails the song
    bool hard_rock;         // the rock meter falls twice as fast
    int engine;             // 0 World Tour's rules, 1 Guitar Flash's (its timing window, misses, meter, Star Power)
    int note_steps;         // 1: the notes move in 24ths of a second (Guitar Flash's clock), 0 smooth
    int last_part, last_diff;
    int sort;
    char last_song[256];
    bool keep_singing;      // the mic's kept with the replay (sing along, the vocals) to hear back
} Config;

extern Config cfg;
void config_load(void);
void config_save(void);
extern bool cfg_borrowed;   // a replay's settings are in cfg for now: config_save leaves the file alone
void config_defaults(void);
// the buttons that play each lane (and the open note) in the current layout
uint32_t lane_buttons(int lane);
