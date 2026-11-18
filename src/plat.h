// The platform under the game: the Vita (plat_vita.c) or a desktop test build on SDL2 (plat_sdl.c).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define AUDIO_RATE 48000

enum {
    BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_CROSS = 1 << 4, BTN_CIRCLE = 1 << 5, BTN_SQUARE = 1 << 6, BTN_TRIANGLE = 1 << 7,
    BTN_L = 1 << 8, BTN_R = 1 << 9, BTN_START = 1 << 10, BTN_SELECT = 1 << 11,
};

#define MAX_TOUCH 6

typedef struct {
    uint32_t buttons;               // held
    uint8_t key_strum;              // a PC keyboard's strum keys held in a song (the arrows up and down)
    uint8_t key_frets;              // a PC keyboard's frets held in a song: bit per lane (0-4), bit 5 the open note
    float lx, ly, rx, ry;           // sticks, -1..1
    int ntouch;                     // front touch screen, in screen pixels
    float touch_x[MAX_TOUCH], touch_y[MAX_TOUCH];
    int back_n;                     // the rear touchpad: the fingers on it, and where (0..1)
    int back_id[4];
    float back_xs[4], back_ys[4];
    int touch_id[MAX_TOUCH];
    float accel[3];                 // the accelerometer (g): which way is down, in the Vita's own frame
    bool has_motion;
    bool overlay;                   // the system's quick menu is over the app (Vita)
} Pad;

void plat_init(void);
void plat_quit(void);
// reads the pad; false when the app should close (the desktop window was closed)
// in a song a PC keyboard's letters are frets (A S D F G, J K L, Space): not the Vita buttons they stand for in menus
void plat_song_keys(bool on);
void plat_key_frets(int layout);    // 0: A S D F G, 1: A S + J K L
bool plat_poll(Pad * pad);
void plat_swap(void);
// where the 960x544 picture sits in the window (pixels, from the bottom left), and the window's size (not the Vita's)
void plat_view(int * x, int * y, int * w, int * h, int * full_w, int * full_h);
// a phone held upright: the screen under the picture, in the game's units (960 across); 0 otherwise. A finger there
// comes in with y past SCREEN_H
float plat_below(void);
void plat_portrait_song(bool on);    // a song's own portrait layout (on) or a menu's (the whole picture)
// a menu held upright: the stretch of the picture worth seeing (x0 and width, in its 960), shown as big as fits over
// the thumb pad; and where that leaves things: the picture's units shown across, and the strip under it (0: not upright)
void plat_menu_focus(float x0, float w);
float plat_menu_band(float * x0, float * w);
float plat_menu_pad(void);           // the pad's band at the bottom, in those units
// the rest of a menu's picture (its panel) under the list, drawn as a second pass of the screen; pad_extra: the menu's
// own buttons' rows over the pad, in parts of the width
void plat_menu_focus2(float x0, float w, float pad_extra);
void plat_menu_pass(int pass);
bool plat_menu_second(void);
bool plat_pass_scissor(int sc[4]);
#define PORTRAIT_CROP 300       // held upright, this much of each side of the picture is off the screen (a song: the highway fills the width, the picture tall)
#define PORTRAIT_UNDER 560      // and this much under it (in its units) for the song's name and the pads
// a frame not drawn (30 fps): its time passes as if it were shown, nothing new goes on the screen
void plat_skip_frame(void);
// 30 fps in songs: a drawn frame every second refresh, a frame only read (plat_skip_frame) between. The swap doesn't
// wait for the screen (it's queued), so the pacing has to be kept by the refresh count, both kinds of frame
void plat_half_rate(bool on);
// in songs the Vita's clocks follow the load (plat_swap watches the frame times): trouble elsewhere (the audio
// decoder falling behind) asks for full speed
void plat_perf_trouble(void);
// shows a frame drawn outside the main loop (the loading screen); the tests' fixed clock doesn't count it
void plat_present(void);
int64_t plat_now_us(void);
const char * plat_data_dir(void);       // saves, settings, songs/ (ends in '/')
const char * plat_asset_dir(void);      // the app's own files (ends in '/')
const char * plat_app_dir(void);        // where the app's installed, to write to
void plat_run_updater(const char * staged, const char * app, const char * old);     // returns only if it couldn't start
// starts the audio output: mix() fills frames of 48 kHz interleaved stereo and is called from the audio thread;
// started() is told when that buffer starts playing, as (first frame's number since start, plat_now_us())
void plat_audio_start(void (*mix)(int16_t * out, int frames), void (*started)(int64_t frame, int64_t when_us));
// the microphone: a port (<0: none), read blocks until a grain of mono samples is in
int plat_mic_open(int grain, int rate);
void plat_mic_read(int port, short * out);
void plat_mic_close(int port);
// a worker thread (on the Vita: on core 1 or 2, below the audio thread's priority; core 0 runs the game), or with
// PLAT_BACKGROUND a thread below all of those (the downloader)
#define PLAT_BACKGROUND 0
void plat_thread(const char * name, int (*fn)(void * arg), void * arg, int core);
void plat_sleep_us(int us);
// 0: light (menus), 1: full speed (a song)
void plat_set_perf(int level);
struct tm;
void plat_local_time(struct tm * out);  // the date and time as the console's clock is set (its own time zone)
int plat_battery(bool * charging);      // % of charge left, -1 where there's no battery to tell
void plat_log(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
// the network, started on first use: false (and why) when there's no connection
bool plat_net_up(char * err, int err_size);
// while downloading: the Vita mustn't go to sleep (the screen may still turn off)
void plat_keep_awake(void);
// typing: the Vita's keyboard dialog over the game (which keeps drawing), the desktop's keyboard. poll: 0 while
// typing (text has it so far, for the desktop build to show), 1 entered (text has it), -1 cancelled
void plat_text_begin(const char * title, const char * initial);
int plat_text_poll(char * text, int size);
// a phone's own keyboard kept up beside the game's: what the game's keys typed handed to it, and it put away
void plat_text_set(const char * text);
void plat_text_end(void);
// a test build's scripted input and screenshots (no-ops on the Vita)
void plat_screenshot(const char * name);// bytes free on the card, -1 if it can't be told
long long plat_free_bytes(void);

