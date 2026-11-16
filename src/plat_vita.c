// The Vita: vitaGL on the screen, SceCtrl/SceTouch/SceMotion for input, a SceAudioOut port fed by a high-priority
// thread, and the CPU/GPU clocks by what's on screen (menus run slower to save battery).
#include <psp2/audioin.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/motion.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/display.h>
#include <psp2/rtc.h>
#include <time.h>
#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/io/stat.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <vitaGL.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <psp2/io/devctl.h>
#include "plat.h"

long long plat_free_bytes(void)
{
    SceIoDevInfo info;
    memset(&info, 0, sizeof(info));
    return sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info)) < 0 ? -1 : (long long)info.free_size;
}

#define DATA_DIR "ux0:data/ghvita/"
#define AUDIO_GRAIN 512

unsigned int _newlib_heap_size_user = 96 * 1024 * 1024;

static int64_t start_us;

void plat_init(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(DATA_DIR "songs", 0777);
    freopen(DATA_DIR "log.txt", "w", stdout);
    freopen(DATA_DIR "log.txt", "a", stderr);
    setvbuf(stdout, NULL, _IOLBF, 0);

    SceAppUtilInitParam init_param;
    SceAppUtilBootParam boot_param;
    memset(&init_param, 0, sizeof(init_param));
    memset(&boot_param, 0, sizeof(boot_param));
    sceAppUtilInit(&init_param, &boot_param);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    sceMotionStartSampling();
    plat_set_perf(1);

    // 2x MSAA keeps the highway's far lines and the gem rims from crawling
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000);      // core 0: the game's own
    vglInitExtended(0, SCREEN_W, SCREEN_H, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_2X);
    vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
    vglWaitVblankStart(GL_TRUE);
    start_us = sceKernelGetProcessTimeWide();
}

void plat_quit(void)
{
    sceKernelExitProcess(0);
}

int64_t plat_now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide() - start_us;
}

static float stick(unsigned char v)
{
    float f = (v - 127.5f) / 127.5f;
    if (fabsf(f) < 0.15f)
        return 0;
    return f;
}

bool plat_poll(Pad * pad)
{
    memset(pad, 0, sizeof(*pad));
    SceCtrlData ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    sceCtrlPeekBufferPositive(0, &ctrl, 1);
    static const struct { unsigned int sce; uint32_t bit; } map[] = {
        {SCE_CTRL_UP, BTN_UP}, {SCE_CTRL_DOWN, BTN_DOWN}, {SCE_CTRL_LEFT, BTN_LEFT}, {SCE_CTRL_RIGHT, BTN_RIGHT},
        {SCE_CTRL_CROSS, BTN_CROSS}, {SCE_CTRL_CIRCLE, BTN_CIRCLE}, {SCE_CTRL_SQUARE, BTN_SQUARE},
        {SCE_CTRL_TRIANGLE, BTN_TRIANGLE}, {SCE_CTRL_LTRIGGER, BTN_L}, {SCE_CTRL_RTRIGGER, BTN_R},
        {SCE_CTRL_START, BTN_START}, {SCE_CTRL_SELECT, BTN_SELECT},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (ctrl.buttons & map[i].sce)
            pad->buttons |= map[i].bit;
    pad->lx = stick(ctrl.lx);
    pad->ly = stick(ctrl.ly);
    pad->rx = stick(ctrl.rx);
    pad->ry = stick(ctrl.ry);

    SceTouchData touch;
    memset(&touch, 0, sizeof(touch));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    for (unsigned int i = 0; i < touch.reportNum && pad->ntouch < MAX_TOUCH; i++) {
        pad->touch_x[pad->ntouch] = touch.report[i].x * 0.5f;
        pad->touch_y[pad->ntouch] = touch.report[i].y * 0.5f;
        pad->touch_id[pad->ntouch] = touch.report[i].id;
        pad->ntouch++;
    }
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1);
    for (unsigned int i = 0; i < touch.reportNum && pad->back_n < 4; i++) {
        pad->back_id[pad->back_n] = touch.report[i].id;
        pad->back_xs[pad->back_n] = touch.report[i].x / 1920.0f;
        pad->back_ys[pad->back_n] = (touch.report[i].y - 108) / 782.0f;
        pad->back_n++;
    }

    SceMotionSensorState motion;
    memset(&motion, 0, sizeof(motion));
    if (sceMotionGetSensorState(&motion, 1) >= 0) {
        pad->accel[0] = motion.accelerometer.x;
        pad->accel[1] = motion.accelerometer.y;
        pad->accel[2] = motion.accelerometer.z;
        pad->has_motion = true;
    }

    // the PS button's quick menu over the app (asked a few times a second)
    static int every;
    static bool overlay;
    if (++every >= 6) {
        every = 0;
        SceAppMgrAppState st;
        memset(&st, 0, sizeof(st));
        overlay = _sceAppMgrGetAppState(&st, sizeof(st), 0x03600011) >= 0 && st.isSystemUiOverlaid;
    }
    pad->overlay = overlay;
    return true;
}

static int perf_level = -1;

// Battery: a song loads onto eco clocks (333/166 MHz) and stays there, because changing them holds a frame or two up:
// mid-song that was a stutter, and read as a late frame it sent the clocks straight back, so no song ever stayed on eco.
// Now they change where nothing's on the screen to miss, and the frames just after a change aren't judged. A real late
// frame (or the audio falling behind) puts them back at full for the rest of the run, and the next two runs start there.
// Nothing is logged mid-song (a card write could make a frame late): a song's frame times go to the log when it ends or
// pauses.
#define ECO_GRACE 30
static bool eco, trouble;
static int eco_grace, eco_skip;
static int64_t last_swap;
static struct {
    int frames, late, eco_frames, eco_tries, eco_fails;
    int64_t dt_sum, dt_max, busy_sum, busy_max;
} st;

static void clocks(int arm, int bus, int gpu, int xbar)
{
    scePowerSetArmClockFrequency(arm);
    scePowerSetBusClockFrequency(bus);
    scePowerSetGpuClockFrequency(gpu);
    scePowerSetGpuXbarClockFrequency(xbar);
}

// dt: since the frame before, busy: the main thread's part of it (microseconds)
static void govern(int64_t dt, int64_t busy)
{
    st.frames++;
    st.dt_sum += dt;
    st.busy_sum += busy;
    if (dt > st.dt_max)
        st.dt_max = dt;
    if (busy > st.busy_max)
        st.busy_max = busy;
    if (eco)
        st.eco_frames++;
    st.late += dt > 20000;
    if (eco_grace > 0) {
        eco_grace--;
        trouble = false;
        return;
    }
    if (eco && (dt > 20000 || trouble)) {
        eco = false;
        clocks(444, 222, 222, 166);
        st.eco_fails++;
        eco_skip = 2;
    }
    trouble = false;
}

void plat_perf_trouble(void)
{
    trouble = true;
}

static bool ime_active;

static bool half_rate;
static unsigned half_vc;                // the refresh count at the last drawn frame

void plat_half_rate(bool on)
{
    if (on && !half_rate)
        half_vc = sceDisplayGetVcount();
    half_rate = on;
}

void plat_swap(void)
{
    int64_t before = sceKernelGetProcessTimeWide();
    if (half_rate) {
        // two refreshes on from the last drawn frame (no more than two waits: a slow frame doesn't fall further behind)
        for (int k = 0; k < 2 && (int)(sceDisplayGetVcount() - half_vc) < 2; k++)
            sceDisplayWaitVblankStart();
        half_vc = sceDisplayGetVcount();
    }
    vglSwapBuffers(ime_active ? GL_TRUE : GL_FALSE);     // GL_TRUE: the keyboard dialog is drawn too
    int64_t after = sceKernelGetProcessTimeWide();
    if (perf_level == 1) {
        // during a song the screen mustn't dim (the buttons the game reads don't always count as activity)
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        if (last_swap)
            govern(after - last_swap, before - last_swap);
    }
    last_swap = after;
}

void plat_skip_frame(void)
{
    // the frame on the screen stays a second vblank
    int64_t before = sceKernelGetProcessTimeWide();
    if (!half_rate || (int)(sceDisplayGetVcount() - half_vc) < 1)
        sceDisplayWaitVblankStart();
    int64_t after = sceKernelGetProcessTimeWide();
    if (perf_level == 1) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        if (last_swap)
            govern(after - last_swap, before - last_swap);
    }
    last_swap = after;
}

void plat_present(void)
{
    vglSwapBuffers(GL_FALSE);
}

const char * plat_data_dir(void)
{
    return DATA_DIR;
}

const char * plat_asset_dir(void)
{
    return "app0:assets/";
}

const char * plat_app_dir(void)
{
    return "ux0:app/" "GHVT00001";
}

void plat_run_updater(const char * staged, const char * app, const char * old)
{
    (void)staged, (void)app, (void)old;
    // A different title: a running game can't write its own ux0:app directory, but the updater can. It applies the
    // staged files and starts this game again.
    int r = sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=GHVUPD001");
    if (r < 0) {
        plat_log("update: the updater app wouldn't start: %08x", r);    // back to the game, which must never stop starting
        return;
    }
    sceKernelExitProcess(0);
}

static void (*audio_mix)(int16_t * out, int frames);
static void (*audio_started)(int64_t frame, int64_t when_us);
static int audio_port;

static int audio_thread(SceSize args, void * argp)
{
    (void)args;
    (void)argp;
    static int16_t buf[2][AUDIO_GRAIN * 2];
    int64_t frames = 0;
    int which = 0;
    for (;;) {
        audio_mix(buf[which], AUDIO_GRAIN);
        // returns once the port has taken this buffer, which is when the one before it has finished playing:
        // this one starts now
        sceAudioOutOutput(audio_port, buf[which]);
        if (audio_started)
            audio_started(frames, plat_now_us());
        frames += AUDIO_GRAIN;
        which ^= 1;
    }
    return 0;
}

void plat_audio_start(void (*mix)(int16_t * out, int frames), void (*started)(int64_t frame, int64_t when_us))
{
    audio_mix = mix;
    audio_started = started;
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) {
        plat_log("audio: can't open the port (%08x)", audio_port);
        return;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    // the highest user priority, on its own core
    SceUID th = sceKernelCreateThread("audio", audio_thread, 64, 0x10000, 0, 0x20000 /* core 1 */, NULL);
    sceKernelStartThread(th, 0, NULL);
}

typedef struct {
    int (*fn)(void * arg);
    void * arg;
} ThreadStart;

static int thread_entry(SceSize args, void * argp)
{
    ThreadStart * ts = (ThreadStart *)argp;
    (void)args;
    int r = ts->fn(ts->arg);
    sceKernelExitDeleteThread(r);       // a one-off (the sound loader) leaves nothing behind
    return r;
}

void plat_thread(const char * name, int (*fn)(void * arg), void * arg, int core)
{
    ThreadStart ts = {fn, arg};
    // the arguments are copied onto the new thread's stack
    // 64 is the most urgent a program's thread can be, 191 the least; the game's own thread runs at 160. A background
    // thread at 120 outranked it: a download converting audio could take the game's core from it (the lag while
    // downloading). Now below everything, and the game's thread is kept on core 0, which no worker uses
    int prio = core == PLAT_BACKGROUND ? 185 : 80, cores = core == PLAT_BACKGROUND ? 0x60000 : core == 1 ? 0x20000
                                                                                                         : 0x40000;
    SceUID th = sceKernelCreateThread(name, thread_entry, prio, 0x40000, 0, cores, NULL);
    sceKernelStartThread(th, sizeof(ts), &ts);
}

int plat_mic_open(int grain, int rate)
{
    int port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, grain, rate, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (port < 0)
        plat_log("mic: 0x%08x", port);
    return port;
}

void plat_mic_read(int port, short * out)
{
    // a muted or lost mic returns at once: this must not spin
    if (sceAudioInInput(port, out) < 0) {
        memset(out, 0, 512 * sizeof(short));
        sceKernelDelayThread(32000);
    }
}

void plat_mic_close(int port)
{
    sceAudioInReleasePort(port);
}

void plat_sleep_us(int us)
{
    sceKernelDelayThread(us);
}

void plat_set_perf(int level)
{
    if (level == perf_level)
        return;
    if (perf_level == 1 && st.frames > 0)
        plat_log("song frames: %d, avg %.1f ms, slowest %.1f ms, %d late; main thread avg %.1f ms, max %.1f ms; "
                 "eco clocks %d%% of it (%d tries, %d back to full)", st.frames, st.dt_sum / 1000.0 / st.frames,
                 st.dt_max / 1000.0, st.late, st.busy_sum / 1000.0 / st.frames, st.busy_max / 1000.0,
                 st.eco_frames * 100 / st.frames, st.eco_tries, st.eco_fails);
    perf_level = level;
    eco = trouble = false;
    last_swap = 0;                      // the first frame after a change carries the loading
    memset(&st, 0, sizeof(st));
    if (!level)
        clocks(333, 166, 166, 111);     // menus
    else if (eco_skip > 0) {
        eco_skip--;
        clocks(444, 222, 222, 166);
    } else {
        eco = true;
        eco_grace = ECO_GRACE;
        st.eco_tries = 1;
        clocks(333, 222, 166, 111);
    }
}

void plat_log(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void plat_screenshot(const char * name)
{
    (void)name;
}

bool plat_net_up(char * err, int err_size)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static bool started;
    pthread_mutex_lock(&lock);
    if (!started) {
        sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        static char pool[1024 * 1024];
        SceNetInitParam p = {pool, sizeof(pool), 0};
        int r = sceNetInit(&p);
        if (r < 0 && r != (int)SCE_NET_ERROR_EBUSY)
            plat_log("net: sceNetInit %08x", r);
        r = sceNetCtlInit();
        if (r < 0)
            plat_log("net: sceNetCtlInit %08x", r);
        started = true;
    }
    pthread_mutex_unlock(&lock);
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        snprintf(err, err_size, "no Wi-Fi: connect the Vita to the internet");
        return false;
    }
    return true;
}

void plat_keep_awake(void)
{
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
}

static SceWChar16 ime_title[64], ime_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], ime_initial[128];

static void to_utf16(SceWChar16 * out, int size, const char * in)
{
    int k = 0;
    const unsigned char * p = (const unsigned char *)in;
    while (*p && k + 1 < size) {
        uint32_t c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) {
            c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = ((c & 31) << 6) | (p[0] & 63);
            p++;
        }
        out[k++] = (SceWChar16)c;
    }
    out[k] = 0;
}

void plat_text_begin(const char * title, const char * initial)
{
    if (ime_active)
        return;
    to_utf16(ime_title, 64, title);
    to_utf16(ime_initial, 128, initial);
    memset(ime_text, 0, sizeof(ime_text));
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = ime_title;
    p.maxTextLength = 100;
    p.initialText = ime_initial;
    p.inputTextBuffer = ime_text;
    int r = sceImeDialogInit(&p);
    if (r < 0)
        plat_log("ime: %08x", r);
    ime_active = r >= 0;
}

int plat_text_poll(char * text, int size)
{
    if (!ime_active)
        return -1;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;
    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    ime_active = false;
    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER)
        return -1;
    int k = 0;
    for (int i = 0; ime_text[i] && k + 4 < size; i++) {
        uint32_t c = ime_text[i];
        if (c < 0x80)
            text[k++] = (char)c;
        else if (c < 0x800) {
            text[k++] = (char)(0xC0 | c >> 6);
            text[k++] = (char)(0x80 | (c & 63));
        } else {
            text[k++] = (char)(0xE0 | c >> 12);
            text[k++] = (char)(0x80 | ((c >> 6) & 63));
            text[k++] = (char)(0x80 | (c & 63));
        }
    }
    text[k] = 0;
    return 1;
}

// the Vita's own clock and time zone: the C library's localtime() has no time zone here and gives UTC
void plat_local_time(struct tm * out)
{
    SceDateTime t;
    memset(out, 0, sizeof(*out));
    if (sceRtcGetCurrentClockLocalTime(&t) < 0) {
        time_t now = time(NULL);
        struct tm * g = gmtime(&now);
        if (g)
            *out = *g;
        return;
    }
    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    out->tm_year = t.year - 1900;
    out->tm_mon = t.month - 1;
    out->tm_mday = t.day;
    out->tm_hour = t.hour;
    out->tm_min = t.minute;
    out->tm_sec = t.second;
    out->tm_wday = sceRtcGetDayOfWeek(t.year, t.month, t.day);
    out->tm_yday = before[(t.month + 11) % 12] + t.day - 1 + (t.month > 2 && sceRtcIsLeapYear(t.year));
}

int plat_battery(bool * charging)
{
    int pct = scePowerGetBatteryLifePercent();
    if (charging)
        *charging = scePowerIsBatteryCharging();
    return pct >= 0 && pct <= 100 ? pct : -1;
}
