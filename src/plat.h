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
bool plat_poll(Pad * pad);
void plat_swap(void);
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
void plat_log(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
// the network, started on first use: false (and why) when there's no connection
bool plat_net_up(char * err, int err_size);
// while downloading: the Vita mustn't go to sleep (the screen may still turn off)
void plat_keep_awake(void);
// typing: the Vita's keyboard dialog over the game (which keeps drawing), the desktop's keyboard. poll: 0 while
// typing (text has it so far, for the desktop build to show), 1 entered (text has it), -1 cancelled
void plat_text_begin(const char * title, const char * initial);
int plat_text_poll(char * text, int size);
// a test build's scripted input and screenshots (no-ops on the Vita)
void plat_screenshot(const char * name);
