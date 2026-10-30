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
//   GHV_FIXED=1       the clock runs at exactly 1/60 s per frame (and so does the song)
//   GHV_SHOTS=dir     where shots go (PPM files)
//   GHV_DATA=dir      the data folder (default ./data/), GHV_ASSETS=dir the assets (default ./assets/)
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "plat.h"

static SDL_Window * window;
static SDL_GLContext context;
static bool fixed_clock;
static int64_t frame_number;
static int64_t skip_us;                 // script "suspend"
static int64_t start_us;
static char data_dir[512] = "data/";
static char asset_dir[512] = "assets/";
static char shot_dir[512] = ".";

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
            else if (strcmp(l.what, "suspend") == 0 || strcmp(l.what, "sleep") == 0)
                l.x = atof(tok);
            else if (strcmp(l.what, "touch") == 0) {
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
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    window = SDL_CreateWindow("GH Vita (test)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H,
                              SDL_WINDOW_OPENGL);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        exit(1);
    }
    context = SDL_GL_CreateContext(window);
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
    int mx, my;
    if (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) {
        pad->ntouch = 1;
        pad->touch_x[0] = mx;
        pad->touch_y[0] = my;
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
    if (script_touch) {
        pad->ntouch = 1;
        pad->touch_x[0] = script_tx;
        pad->touch_y[0] = script_ty;
    }
    return true;
}

void plat_swap(void)
{
    if (pending_shot[0]) {
        plat_screenshot(pending_shot);
        pending_shot[0] = 0;
    }
    SDL_GL_SwapWindow(window);
    frame_number++;
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

static void (*audio_mix)(int16_t * out, int frames);
static void (*audio_started)(int64_t frame, int64_t when_us);
static int64_t audio_frames;

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

// the tests sing through vocals.c's autoplay voice
int plat_mic_open(int grain, int rate)
{
    (void)grain;
    (void)rate;
    return -1;
}

void plat_mic_read(int port, short * out)
{
    (void)port;
    (void)out;
}

void plat_mic_close(int port)
{
    (void)port;
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
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
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
    glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGB, GL_UNSIGNED_BYTE, px);
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
