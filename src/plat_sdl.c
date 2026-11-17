// Desktop test build: an SDL2 window with a legacy OpenGL context, the keyboard as the Vita's buttons, and for the
// tests a script of button presses, screenshots and a fixed 60 Hz clock.
//   keys: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q = L, W = R, Enter = Start,
//         Backspace = Select; the mouse is the touch screen
//   GHV_SCRIPT=file   lines "<frame> press|release <button...>", "<frame> touch <x> <y>", "<frame> untouch",
//                     "<frame> shot <name>", "<frame> quit"; buttons: up down left right cross circle square
//                     triangle l r start select; "<frame> suspend <ms>" jumps the fixed clock (like the Vita
//                     coming back from sleep), "<frame> overlay on|off" is the Vita's quick menu, "<frame> text
//                     <words...>" types into the keyboard (and enters it), "<frame> waitdl" holds the script (and
//                     the fixed clock) until the downloader's search and downloads are done, "<frame> sleep <ms>"
//                     waits that long in real time (a cover coming in)
//                     "<frame> bar <0..1>" holds the whammy (the right stick) that far down
//   GHV_PACE=1        with GHV_FIXED, frames take their real 1/60 s, so what SDL's disk driver writes is the game's mix
//   GHV_FIXED=1       the clock runs at exactly 1/60 s per frame (and so does the song)
//   GHV_SHOTS=dir     where shots go (PPM files)
//   GHV_DATA=dir      the data folder (default ./data/), GHV_ASSETS=dir the assets (default ./assets/)
#ifdef __ANDROID__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#if defined(__ANDROID__) || defined(GHV_GLES)
// a phone (or GHV_GLES on the desktop, to test the same path): OpenGL ES 1.1, the screen whatever shape it is, fingers
#include <GLES/gl.h>
#define GHV_ES 1
#else
#include <GL/gl.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>
#include <stdarg.h>
#include "audio.h"
#include "plat.h"
#include "update_apply.h"

static SDL_Window * window;
static SDL_GLContext context;
static bool fixed_clock;
static int64_t frame_number;
static int64_t skip_us;                 // script "suspend"
static int64_t start_us;
static char data_dir[512] = "data/";
static char asset_dir[512] = "assets/";
static char shot_dir[512] = ".";

// The picture is 960x544 whatever the window is: as big as fits, in the middle (plat_view), and a finger's place is
// brought back into those 960x544
static int win_w = SCREEN_W, win_h = SCREEN_H;
static bool portrait_song;      // plat_portrait_song
static float menu_x0, menu_w = SCREEN_W;    // plat_menu_focus
#define MENU_PAD 0.66f          // a menu held upright: the thumb pad under the picture, this much of the width tall

void plat_view(int * x, int * y, int * w, int * h, int * full_w, int * full_h)
{
    int vw = win_w, vh = win_w * SCREEN_H / SCREEN_W;
    if (win_h > win_w && !portrait_song) {
        // a menu held upright: the part of it that matters (its list) across the screen, bigger than the whole picture
        // would be, over the thumb pad
        float pad_h = win_w * MENU_PAD;
        vw = (int)(win_w * SCREEN_W / menu_w);
        vh = vw * SCREEN_H / SCREEN_W;
        if (vh > win_h - pad_h) {
            vh = (int)(win_h - pad_h);
            vw = vh * SCREEN_W / SCREEN_H;
        }
        float shown = menu_w * vw / SCREEN_W;           // the focus's width on the screen
        *x = (int)((win_w - shown) / 2 - menu_x0 * vw / SCREEN_W);
        *y = (int)(pad_h + (win_h - pad_h - vh) / 2);
        *w = vw;
        *h = vh;
        *full_w = win_w;
        *full_h = win_h;
        return;
    }
    if (win_h > win_w && portrait_song) {
        // held upright in a song: the picture's middle (PORTRAIT_CROP off each side) as wide as the screen, bigger
        vw = win_w * SCREEN_W / (SCREEN_W - 2 * PORTRAIT_CROP);
        vh = vw * SCREEN_H / SCREEN_W;
    } else if (vh > win_h) {
        vh = win_h;
        vw = win_h * SCREEN_W / SCREEN_H;
    }
    *x = (win_w - vw) / 2;
    // held upright (a phone in portrait): the picture across the top, the rest of the screen under it for the frets
    // (plat_below); on its side, in the middle
    if (win_h > win_w && portrait_song) {
        // the pads and the song's name under it, the picture over them in the middle of what's left
        int under = PORTRAIT_UNDER * vw / SCREEN_W;
        *y = under + (win_h - under - vh) / 2;
        if (*y + vh > win_h)
            *y = win_h - vh;
    } else
        *y = (win_h - vh) / 2;
    *w = vw;
    *h = vh;
    *full_w = win_w;
    *full_h = win_h;
}

// held upright, a song's screen is laid out for it (the picture's middle bigger, the pads under it); a menu shows the
// whole picture in the middle of the screen
void plat_portrait_song(bool on)
{
    portrait_song = on;
}

void plat_menu_focus(float x0, float w)
{
    menu_w = w < 300 ? 300 : w > SCREEN_W ? SCREEN_W : w;
    menu_x0 = x0 < 0 ? 0 : x0 > SCREEN_W - menu_w ? SCREEN_W - menu_w : x0;
}

float plat_menu_band(float * x0, float * w)
{
    *x0 = 0;
    *w = SCREEN_W;
    if (win_h <= win_w || win_w <= 0 || portrait_song)
        return 0;
    int x, y, vw, vh, fw, fh;
    plat_view(&x, &y, &vw, &vh, &fw, &fh);
    *x0 = -(float)x * SCREEN_W / vw;
    *w = (float)win_w * SCREEN_W / vw;
    return (float)y * SCREEN_W / vw;
}

// the screen under the picture in the game's units (its 960 across): 0 on its side, or in a menu
float plat_below(void)
{
    if (win_h <= win_w || win_w <= 0 || !portrait_song)
        return 0;
    int x, y, w, h, fw, fh;
    plat_view(&x, &y, &w, &h, &fw, &fh);
    return (float)y * SCREEN_W / w;      // the strip under it, in the picture's own units
}

// a place in the window (0..1 across and down) as the game's pixels
static void to_screen(float nx, float ny, float * sx, float * sy)
{
    int x, y, w, h, fw, fh;
    plat_view(&x, &y, &w, &h, &fw, &fh);
    // (plat_view's y counts up from the bottom, as GL's viewport does; a finger's from the top)
    *sx = (nx * fw - x) * SCREEN_W / (w > 0 ? w : 1);
    *sy = (ny * fh - (fh - y - h)) * SCREEN_H / (h > 0 ? h : 1);
}

// the fingers on the screen, by SDL's ids
static struct { SDL_FingerID id; float x, y; bool down; } fingers[MAX_TOUCH];
static bool back_key;           // a phone's back button: Circle for a frame

typedef struct {
    int64_t frame;
    char what[16];
    uint32_t buttons;
    float x, y;
    char name[128];
} ScriptLine;
static ScriptLine * script;
static int script_count, script_pos;
static uint32_t script_buttons;
static char pending_shot[128];          // taken from the next frame, before it's shown
static bool script_touch;
static float script_bar;
static int64_t audio_frames;
static float script_tx, script_ty;
static bool script_overlay;
static char script_text[256];
static bool script_text_ready;
static bool typing;                     // plat_text_begin: the keyboard types, not presses
static char typed[256];
static int text_result;                 // 1 entered, -1 cancelled

static uint32_t button_named(const char * s)
{
    static const struct { const char * name; uint32_t bit; } names[] = {
        {"up", BTN_UP}, {"down", BTN_DOWN}, {"left", BTN_LEFT}, {"right", BTN_RIGHT}, {"cross", BTN_CROSS},
        {"circle", BTN_CIRCLE}, {"square", BTN_SQUARE}, {"triangle", BTN_TRIANGLE}, {"l", BTN_L}, {"r", BTN_R},
        {"start", BTN_START}, {"select", BTN_SELECT},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(s, names[i].name) == 0)
            return names[i].bit;
    fprintf(stderr, "script: unknown button %s\n", s);
    return 0;
}

static void load_script(const char * path)
{
    FILE * f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "script: can't open %s\n", path);
        return;
    }
    char line[512];
    int cap = 0;
    while (fgets(line, sizeof(line), f)) {
        ScriptLine l;
        memset(&l, 0, sizeof(l));
        char * tok = strtok(line, " \t\r\n");
        if (!tok || tok[0] == '#')
            continue;
        l.frame = atoll(tok);
        tok = strtok(NULL, " \t\r\n");
        if (!tok)
            continue;
        snprintf(l.what, sizeof(l.what), "%s", tok);
        if (strcmp(l.what, "text") == 0) {
            tok = strtok(NULL, "\r\n");
            snprintf(l.name, sizeof(l.name), "%s", tok ? tok : "");
        }
        while (strcmp(l.what, "text") != 0 && (tok = strtok(NULL, " \t\r\n"))) {
            if (strcmp(l.what, "shot") == 0 || strcmp(l.what, "overlay") == 0)
                snprintf(l.name, sizeof(l.name), "%s", tok);
            else if (strcmp(l.what, "suspend") == 0 || strcmp(l.what, "sleep") == 0 || strcmp(l.what, "bar") == 0)
                l.x = atof(tok);
            else if (strcmp(l.what, "touch") == 0 || strcmp(l.what, "finger") == 0) {
                l.x = atof(tok);
                tok = strtok(NULL, " \t\r\n");
                l.y = tok ? atof(tok) : 0;
            } else
                l.buttons |= button_named(tok);
        }
        if (script_count == cap) {
            cap = cap ? cap * 2 : 64;
            script = realloc(script, cap * sizeof(ScriptLine));
        }
        script[script_count++] = l;
    }
    fclose(f);
}

void plat_init(void)
{
    const char * e;
    if ((e = getenv("GHV_DATA")))
        snprintf(data_dir, sizeof(data_dir), "%s/", e);
    if ((e = getenv("GHV_ASSETS")))
        snprintf(asset_dir, sizeof(asset_dir), "%s/", e);
    if ((e = getenv("GHV_SHOTS")))
        snprintf(shot_dir, sizeof(shot_dir), "%s", e);
    fixed_clock = getenv("GHV_FIXED") && atoi(getenv("GHV_FIXED"));
    if ((e = getenv("GHV_SCRIPT")))
        load_script(e);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");          // fingers are fingers, not a mouse as well
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
#ifdef __ANDROID__
    // the art is unpacked into the app's own storage by GHVitaActivity before this runs; songs and saves go where a
    // file manager can reach them (Android/data/<package>/files)
    snprintf(asset_dir, sizeof(asset_dir), "%s/assets/", SDL_AndroidGetInternalStoragePath());
    const char * ext = SDL_AndroidGetExternalStoragePath();
    snprintf(data_dir, sizeof(data_dir), "%s/", ext ? ext : SDL_AndroidGetInternalStoragePath());
#endif
#ifdef GHV_ES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);     // the 3D band
    Uint32 wflags = SDL_WINDOW_OPENGL;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE;
#endif
    int ww = SCREEN_W, wh = SCREEN_H;
    if ((e = getenv("GHV_WINDOW")))                 // tests: a window of another shape ("1280x600")
        sscanf(e, "%dx%d", &ww, &wh);
    window = SDL_CreateWindow("GH Vita (test)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, wflags);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        exit(1);
    }
    context = SDL_GL_CreateContext(window);
    if (!context) {
        plat_log("SDL_GL_CreateContext: %s", SDL_GetError());
        exit(1);
    }
    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
    SDL_GL_SetSwapInterval(fixed_clock ? 0 : 1);
    start_us = (int64_t)(SDL_GetPerformanceCounter() * 1000000.0 / SDL_GetPerformanceFrequency());
}

void plat_quit(void)
{
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

int64_t plat_now_us(void)
{
    if (fixed_clock)
        return frame_number * 1000000 / 60 + skip_us;
    return (int64_t)(SDL_GetPerformanceCounter() * 1000000.0 / SDL_GetPerformanceFrequency()) - start_us;
}

bool plat_poll(Pad * pad)
{
    memset(pad, 0, sizeof(*pad));
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            return false;
        if (ev.type == SDL_WINDOWEVENT && (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED))
            SDL_GL_GetDrawableSize(window, &win_w, &win_h);
        if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERUP) {
            int at = -1, spare = -1;
            for (int i = 0; i < MAX_TOUCH; i++) {
                if (fingers[i].down && fingers[i].id == ev.tfinger.fingerId)
                    at = i;
                else if (!fingers[i].down && spare < 0)
                    spare = i;
            }
            if (at < 0 && ev.type != SDL_FINGERUP)
                at = spare;
            if (at >= 0) {
                fingers[at].id = ev.tfinger.fingerId;
                fingers[at].down = ev.type != SDL_FINGERUP;
                to_screen(ev.tfinger.x, ev.tfinger.y, &fingers[at].x, &fingers[at].y);
            }
        }
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_AC_BACK)
            back_key = true;
        if (!typing)
            continue;
        if (ev.type == SDL_TEXTINPUT && strlen(typed) + strlen(ev.text.text) < sizeof(typed))
            strcat(typed, ev.text.text);
        else if (ev.type == SDL_KEYDOWN) {
            size_t n = strlen(typed);
            if (ev.key.keysym.sym == SDLK_BACKSPACE && n) {
                while (n > 0 && ((unsigned char)typed[n - 1] & 0xC0) == 0x80)
                    n--;
                typed[n ? n - 1 : 0] = 0;
            } else if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER)
                text_result = 1;
            else if (ev.key.keysym.sym == SDLK_ESCAPE)
                text_result = -1;
        }
    }
    const Uint8 * k = SDL_GetKeyboardState(NULL);
    static const struct { int sc; uint32_t bit; } keys[] = {
        {SDL_SCANCODE_UP, BTN_UP}, {SDL_SCANCODE_DOWN, BTN_DOWN}, {SDL_SCANCODE_LEFT, BTN_LEFT},
        {SDL_SCANCODE_RIGHT, BTN_RIGHT}, {SDL_SCANCODE_Z, BTN_CROSS}, {SDL_SCANCODE_X, BTN_CIRCLE},
        {SDL_SCANCODE_A, BTN_SQUARE}, {SDL_SCANCODE_S, BTN_TRIANGLE}, {SDL_SCANCODE_Q, BTN_L},
        {SDL_SCANCODE_W, BTN_R}, {SDL_SCANCODE_RETURN, BTN_START}, {SDL_SCANCODE_BACKSPACE, BTN_SELECT},
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && !typing; i++)
        if (k[keys[i].sc])
            pad->buttons |= keys[i].bit;
    if (back_key) {
        pad->buttons |= BTN_CIRCLE;
        back_key = false;
    }
    for (int i = 0; i < MAX_TOUCH; i++)
        if (fingers[i].down && pad->ntouch < MAX_TOUCH) {
            pad->touch_x[pad->ntouch] = fingers[i].x;
            pad->touch_y[pad->ntouch] = fingers[i].y;
            pad->touch_id[pad->ntouch++] = i + 1;
        }
    int mx, my;
    if (!pad->ntouch && (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT))) {
        int lw, lh;
        SDL_GetWindowSize(window, &lw, &lh);
        pad->ntouch = 1;
        to_screen((float)mx / (lw > 0 ? lw : 1), (float)my / (lh > 0 ? lh : 1), &pad->touch_x[0], &pad->touch_y[0]);
    }
    // the script
    while (script_pos < script_count && script[script_pos].frame <= frame_number) {
        ScriptLine * l = &script[script_pos++];
        if (strcmp(l->what, "press") == 0)
            script_buttons |= l->buttons;
        else if (strcmp(l->what, "release") == 0)
            script_buttons &= ~l->buttons;
        else if (strcmp(l->what, "touch") == 0) {
            script_touch = true;
            script_tx = l->x;
            script_ty = l->y;
        } else if (strcmp(l->what, "untouch") == 0)
            script_touch = false;
        else if (strcmp(l->what, "finger") == 0 || strcmp(l->what, "unfinger") == 0) {
            // a finger as the screen gives one: a place in the window (0..1 across and down), turned into the game's
            // pixels as a real touch is, by the picture's place now
            fingers[0].id = 777;
            fingers[0].down = strcmp(l->what, "finger") == 0;
            if (fingers[0].down)
                to_screen((float)l->x, (float)l->y, &fingers[0].x, &fingers[0].y);
        }
        else if (strcmp(l->what, "bar") == 0)
            script_bar = (float)l->x;
        else if (strcmp(l->what, "shot") == 0)
            snprintf(pending_shot, sizeof(pending_shot), "%s", l->name);
        else if (strcmp(l->what, "suspend") == 0)
            skip_us += (int64_t)(l->x * 1000);
        else if (strcmp(l->what, "sleep") == 0)
            SDL_Delay((Uint32)l->x);
        else if (strcmp(l->what, "overlay") == 0)
            script_overlay = strcmp(l->name, "on") == 0;
        else if (strcmp(l->what, "text") == 0) {
            snprintf(script_text, sizeof(script_text), "%s", l->name);
            script_text_ready = true;
        } else if (strcmp(l->what, "waitdl") == 0) {
            // the downloads take real time: the fixed clock stands still until they're done
            int dl_jobs_active(void);
            bool dl_searching(void);
            if (dl_jobs_active() || dl_searching()) {
                script_pos--;
                frame_number--;
                SDL_Delay(50);
                break;
            }
        }
        else if (strcmp(l->what, "quit") == 0)
            return false;
    }
    pad->overlay = script_overlay;
    pad->buttons |= script_buttons;
    if (script_bar > 0)
        pad->rx = 0.12f + 0.7f * script_bar;
    if (script_touch) {
        pad->ntouch = 1;
        pad->touch_x[0] = script_tx;
        pad->touch_y[0] = script_ty;
    }
    return true;
}

static void frame_passed(void);

static bool half_rate;
static double half_t;                   // when the last frame was drawn

static double now_ms(void)
{
    return SDL_GetPerformanceCounter() * 1000.0 / SDL_GetPerformanceFrequency();
}

// wait till ms after the last drawn frame
static void wait_until(double ms)
{
    double left = half_t + ms - now_ms();
    if (left > 0 && left < 50)
        SDL_Delay((Uint32)left);
}

void plat_half_rate(bool on)
{
    if (on && !half_rate)
        half_t = now_ms();
    half_rate = on;
}

void plat_skip_frame(void)
{
    // a shot asked for now waits for the next frame drawn
    if (half_rate && !fixed_clock)
        wait_until(1000.0 / 60);
    else if (!fixed_clock)
        SDL_Delay(16);
    frame_passed();
}

void plat_swap(void)
{
    if (pending_shot[0]) {
        if (getenv("GHV_HANDLOG"))
            fprintf(stderr, "shot %s\n", pending_shot);
        plat_screenshot(pending_shot);
        pending_shot[0] = 0;
    }
    if (half_rate && !fixed_clock) {
        wait_until(1000.0 / 30);
        half_t = now_ms();
    }
    SDL_GL_SwapWindow(window);
    frame_passed();
}

static void frame_passed(void)
{
    frame_number++;
    if (fixed_clock && getenv("GHV_PACE")) {
        // the sound card sets the speed: the fixed clock and the frames captured can't drift apart
        static int64_t first_frame = -1, first_audio;
        if (first_frame < 0) {
            first_frame = frame_number;
            first_audio = audio_frames;
        }
        while ((audio_frames - first_audio) * 60 < (frame_number - first_frame) * AUDIO_RATE)
            SDL_Delay(1);
    }
}

void plat_present(void)
{
    // GHV_LOADSHOTS=1: each loading screen frame as a shot (loading_0, loading_1...)
    static int n;
    if (getenv("GHV_LOADSHOTS")) {
        char name[32];
        snprintf(name, sizeof(name), "loading_%d", n++);
        plat_screenshot(name);
    }
    SDL_GL_SwapWindow(window);
}

const char * plat_data_dir(void)
{
    return data_dir;
}

const char * plat_asset_dir(void)
{
    return asset_dir;
}

const char * plat_app_dir(void)
{
    return getenv("GHV_APP") ? getenv("GHV_APP") : "./app";
}

// what updater.bin does on a Vita, then a restart is the next run
void plat_run_updater(const char * staged, const char * app, const char * old)
{
    char err[200] = "", path[512];
    int n = update_apply(staged, app, old, err, sizeof(err));
    snprintf(path, sizeof(path), "%supdate/result.txt", data_dir);
    FILE * f = fopen(path, "w");
    if (f) {
        if (n >= 0)
            fprintf(f, "ok %d\n", n);
        else
            fprintf(f, "failed %s\n", err);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%supdate/pending", data_dir);
    remove(path);
    exit(n >= 0 ? 0 : 1);
}

static void (*audio_mix)(int16_t * out, int frames);
static void (*audio_started)(int64_t frame, int64_t when_us);

// GHV_MIC_ECHO=<gain>: the test mic also hears the game's own output, like the Vita's speakers: a small speaker's
// sound (no bass, rounded peaks, a reflection), GHV_MIC_ECHO_MS (default 60) after it was played
static void audio_callback(void * user, Uint8 * stream, int len)
{
    (void)user;
    int frames = len / 4;
    audio_mix((int16_t *)stream, frames);
    // what's mixed now is heard after the buffer SDL is already playing
    if (audio_started && !fixed_clock)
        audio_started(audio_frames, plat_now_us() + (int64_t)frames * 1000000 / AUDIO_RATE);
    audio_frames += frames;
}

void plat_audio_start(void (*mix)(int16_t * out, int frames), void (*started)(int64_t frame, int64_t when_us))
{
    audio_mix = mix;
    audio_started = started;
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        fprintf(stderr, "no audio: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(dev, 0);
}

void plat_thread(const char * name, int (*fn)(void * arg), void * arg, int core)
{
    SDL_Thread * t = SDL_CreateThread(fn, name, arg);
    (void)core;
    SDL_DetachThread(t);
}

// the tests sing through vocals.c's autoplay voice; GHV_MIC=1: a test microphone, a tone gliding up and down an
// octave at the mic's real pace (with GHV_PACE, so the song keeps time with it)
static int mic_grain, mic_rate;
static double mic_phase, mic_due, mic_late;
static short * mic_file;           // GHV_MIC=<raw file>: that's what's sung
static long mic_file_n;

// a real microphone through SDL (a phone's; GHV_MIC=sdl on the desktop): what it hears goes into a ring the reader
// takes grains from
static SDL_AudioDeviceID cap_dev;
static int16_t cap_ring[48000];
static volatile int cap_w, cap_r;
static void cap_feed(void * ud, Uint8 * stream, int len)
{
    (void)ud;
    const int16_t * s = (const int16_t *)stream;
    int n = len / 2, cap = (int)(sizeof(cap_ring) / sizeof(cap_ring[0]));
    for (int i = 0; i < n; i++) {
        cap_ring[cap_w] = s[i];
        cap_w = (cap_w + 1) % cap;
        if (cap_w == cap_r)
            cap_r = (cap_r + 1) % cap;          // full: the oldest goes
    }
}

int plat_mic_open(int grain, int rate)
{
#ifdef __ANDROID__
    bool real = true;
#else
    bool real = getenv("GHV_MIC") && strcmp(getenv("GHV_MIC"), "sdl") == 0;
#endif
    if (real) {
        SDL_AudioSpec want = {0}, have;
        want.freq = rate;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = (Uint16)(grain < 256 ? 256 : grain);
        want.callback = cap_feed;
        cap_w = cap_r = 0;
        cap_dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
        if (!cap_dev) {
            plat_log("mic: %s", SDL_GetError());
            return -1;
        }
        if (have.freq != rate)
            plat_log("mic: %d Hz, not %d", have.freq, rate);
        mic_grain = grain;
        mic_rate = rate;
        SDL_PauseAudioDevice(cap_dev, 0);
        return 1;
    }
    if (!getenv("GHV_MIC"))
        return -1;
    mic_grain = grain;
    mic_rate = rate;
    mic_due = 0;
    const char * path = getenv("GHV_MIC");
    mic_late = getenv("GHV_MIC_LATE") ? atof(getenv("GHV_MIC_LATE")) : 0;
    FILE * f = strcmp(path, "1") && !mic_file ? fopen(path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        mic_file_n = ftell(f) / 2;
        fseek(f, 0, SEEK_SET);
        mic_file = malloc(mic_file_n * 2);
        mic_file_n = fread(mic_file, 2, mic_file_n, f);
        fclose(f);
    }
    return 0;
}

void plat_mic_read(int port, short * out)
{
    if (!mic_grain)
        return;
    if (port == 1 && cap_dev) {
        // a grain from the ring, waiting for it as a real mic's read does
        int cap = (int)(sizeof(cap_ring) / sizeof(cap_ring[0]));
        for (int tries = 0; tries < 200 && (cap_w - cap_r + cap) % cap < mic_grain; tries++)
            SDL_Delay(2);
        SDL_LockAudioDevice(cap_dev);
        for (int i = 0; i < mic_grain; i++) {
            if (cap_r != cap_w) {
                out[i] = cap_ring[cap_r];
                cap_r = (cap_r + 1) % cap;
            } else
                out[i] = 0;
        }
        SDL_UnlockAudioDevice(cap_dev);
        return;
    }
    double now = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    if (mic_due < now - 0.2)
        mic_due = now;
    mic_due += (double)mic_grain / mic_rate;
    if (mic_due > now)
        SDL_Delay((Uint32)((mic_due - now) * 1000));
    if (mic_file) {
        // a recording (raw 16-bit mono at the mic's rate, song time 0 at its start), heard GHV_MIC_LATE s late
        // a steady stream like a real mic: one grain after the other, back in step only if 100 ms out (the test
        // clock jitters; the Vita's mic and speakers run off steady clocks)
        static long pos = -1000000000;
        double t = audio_song_clock() - mic_late;
        long want = (long)(t * mic_rate) - mic_grain;
        if (labs(want - pos) > mic_rate / 10)
            pos = want;
        long at = pos;
        pos += mic_grain;
        t = (double)(at + mic_grain) / mic_rate;
        float eg = getenv("GHV_MIC_ECHO") ? (float)atof(getenv("GHV_MIC_ECHO")) : 0;
        double ed = (getenv("GHV_MIC_ECHO_MS") ? atof(getenv("GHV_MIC_ECHO_MS")) : 60) / 1000;
        int16_t played[4096 + 90];
        if (eg > 0)
            audio_test_heard(t - ed - (double)(mic_grain + 90) / mic_rate, played, mic_grain + 90);
        static float hp_x, hp_y;
        for (int i = 0; i < mic_grain; i++) {
            float v = at + i >= 0 && at + i < mic_file_n ? mic_file[at + i] : 0;
            if (eg > 0) {
                float x = played[90 + i] + 0.3f * played[i], y = 0.86f * (hp_y + x - hp_x);    // high pass ~350 Hz
                hp_x = x, hp_y = y;
                v += eg * 12000 * tanhf(y / 12000);
            }
            out[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        return;
    }
    double hz = 196 * pow(2, (sin(now * 0.7) + 1) / 2);
    for (int i = 0; i < mic_grain; i++) {
        mic_phase += 2 * M_PI * hz / mic_rate;
        out[i] = (short)(9000 * sin(mic_phase) + 3000 * sin(mic_phase * 2));
    }
}

void plat_mic_close(int port)
{
    if (port == 1 && cap_dev) {
        SDL_CloseAudioDevice(cap_dev);
        cap_dev = 0;
    }
}

void plat_sleep_us(int us)
{
    SDL_Delay(us / 1000 > 0 ? us / 1000 : 1);
}

void plat_perf_trouble(void)
{
}

void plat_set_perf(int level)
{
    (void)level;
}

void plat_log(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, ap);      // logcat
    va_end(ap);
#else
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
#endif
}

bool plat_net_up(char * err, int err_size)
{
    (void)err;
    (void)err_size;
    return true;
}

void plat_keep_awake(void)
{
}

void plat_text_begin(const char * title, const char * initial)
{
    plat_log("text: %s", title);
    snprintf(typed, sizeof(typed), "%s", initial);
    typing = true;
    text_result = 0;
    SDL_StartTextInput();
}

void plat_text_set(const char * text)
{
    if (typing)
        snprintf(typed, sizeof(typed), "%s", text);
}

void plat_text_end(void)
{
    if (typing) {
        typing = false;
        SDL_StopTextInput();
    }
}

int plat_text_poll(char * text, int size)
{
    if (!typing)
        return -1;
    if (script_text_ready) {
        snprintf(typed, sizeof(typed), "%s", script_text);
        script_text_ready = false;
        text_result = 1;
    }
    snprintf(text, size, "%s", typed);
    if (!text_result)
        return 0;
    typing = false;
    SDL_StopTextInput();
    return text_result;
}

void plat_screenshot(const char * name)
{
    static unsigned char px[SCREEN_W * SCREEN_H * 3];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (getenv("GHV_SHOTWIN")) {
        // tests: the whole window, whatever its shape
        int ww = win_w, wh = win_h;
        unsigned char * all = malloc((size_t)ww * wh * 4);
        glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, all);
        char p2[700];
        snprintf(p2, sizeof(p2), "%s/%s.ppm", shot_dir, name);
        FILE * o = fopen(p2, "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", ww, wh);
            for (int y = wh - 1; y >= 0; y--)
                for (int x = 0; x < ww; x++)
                    fwrite(all + ((size_t)y * ww + x) * 4, 1, 3, o);
            fclose(o);
        }
        free(all);
        return;
    }
    if (win_w != SCREEN_W || win_h != SCREEN_H)
        return;                 // shots are of the picture at its own size
#ifdef GHV_ES
    static unsigned char rgba[SCREEN_W * SCREEN_H * 4];
    glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        memcpy(px + i * 3, rgba + i * 4, 3);
#else
    glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGB, GL_UNSIGNED_BYTE, px);
#endif
    char path[700];
    snprintf(path, sizeof(path), "%s/%s.ppm", shot_dir, name);
    FILE * f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int y = SCREEN_H - 1; y >= 0; y--)
        fwrite(px + y * SCREEN_W * 3, 1, SCREEN_W * 3, f);
    fclose(f);
    plat_log("shot: %s", path);
}

long long plat_free_bytes(void)
{
    struct statvfs v;
    return statvfs(".", &v) ? -1 : (long long)v.f_bavail * (long long)v.f_frsize;
}

void plat_local_time(struct tm * out)
{
    time_t now = time(NULL);
    struct tm * tm = localtime(&now);
    if (tm)
        *out = *tm;
    else
        memset(out, 0, sizeof(*out));
}

// a laptop's battery; GHV_BATTERY=<percent> stands one in for the tests
int plat_battery(bool * charging)
{
    const char * e = getenv("GHV_BATTERY");
    if (charging)
        *charging = false;
    if (e)
        return atoi(e);
    int secs, pct;
    SDL_PowerState st = SDL_GetPowerInfo(&secs, &pct);
    if (charging)
        *charging = st == SDL_POWERSTATE_CHARGING;
    return (st == SDL_POWERSTATE_ON_BATTERY || st == SDL_POWERSTATE_CHARGING) && pct >= 0 ? pct : -1;
}
