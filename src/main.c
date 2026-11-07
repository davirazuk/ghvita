#include <ctype.h>
// GH Vita: the screens (title, song list, part and difficulty, the song, pause, results, options, calibration, the
// song downloader)
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "res.h"
#include "audio.h"
#include "shrink.h"
#include "songs.h"
#include "player.h"
#include "achieve.h"
#include "tour.h"
#include "career.h"
#include "chart.h"
#include "game.h"
#include "vocals.h"
#include "dl.h"
#include "update.h"

enum { S_TITLE, S_MAIN, S_TOUR, S_SONGS, S_SELECT, S_PRACTICE, S_PLAY, S_PAUSE, S_RESUME, S_RESULTS, S_OPTIONS, S_CALIBRATE, S_EMPTY,
       S_DOWNLOAD, S_UPDATE, S_CAREER, S_NEWS, S_PLAYERS };

static int screen = S_TITLE;
static double now_s, screen_t;
static double gap_s;                // since the frame before
static int screen_frames;           // frames since the screen changed
static bool overlay_on;             // the system's quick menu just came up
static volatile bool shrink_running;    // the settings' shrink is working through the songs (menu_music stands down)
static Pad pad;
static uint32_t pressed, repeat;    // repeat: pressed, plus held directions repeating
static bool autoplay;

// the song list: sel is the song in the library; the view is what's shown of it, searched and filtered
static int sel, top, pos;
static int * view;
static int view_n, view_cap;
static bool view_stale = true;
static char find_text[64];
// what the song list shows: the made-up lists (played long ago, never five stars, short ones), then SHOW_PART + each part
enum { SHOW_ALL, SHOW_FAVS, SHOW_NEW, SHOW_STALE, SHOW_UNSTARRED, SHOW_SHORT, SHOW_LONG, SHOW_PART };
static int show;
static double sel_t;
static Tex * art;
static int art_for = -1, preview_for = -1;
// part and difficulty
static int sel_part, sel_diff;
static int want_diff;               // the difficulty last picked (a part without it gets the nearest)
static uint8_t sel_diffs[PART_COUNT];   // the difficulties each part of the chosen song has, a bit each
static int menu_index;
static Result result;
static int result_ach[6], result_ach_n;   // achievements this song unlocked
static bool result_best, result_daily;   // a new best; today's daily song done
// the results screen's other page: this chart's last plays (this one last)
#define HIST_MAX 10
static Played hist[HIST_MAX];
static int hist_n;
static bool results_history, hist_has_this;   // this play is hist's last (it was saved)
static int options_from;
// calibration
static double calib_hits[32];
static int calib_count;
static bool calib_playing;
// a line in the corner for a few seconds ("Added Reptilia")
static char toast[160];
static double toast_t = -10;

static void menu_music_stop(void);

static void go(int s)
{
    if (s != S_TITLE && s != S_MAIN)
        menu_music_stop();
    screen = s;
    screen_t = now_s;
    screen_frames = 0;
    menu_index = 0;
}

static void update_repeat(void)
{
    static uint32_t held_dirs;
    static double held_since, last_rep;
    uint32_t dirs = pad.buttons & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_L | BTN_R);
    repeat = pressed;
    if (dirs != held_dirs) {
        held_dirs = dirs;
        held_since = last_rep = now_s;
        return;
    }
    if (dirs && now_s - held_since > 0.35 && now_s - last_rep > 0.065) {
        repeat |= dirs;
        last_rep = now_s;
    }
}

static void help_bar(const char * const * items, int n)
{
    gfx_rect_grad(0, SCREEN_H - 42, SCREEN_W, 42, RGBA(0, 0, 0, 140), RGBA(0, 0, 0, 230));
    // a long row is made smaller to fit, rather than running off the right edge
    float total = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        const Sprite * b = atlas_get(res.hud, items[i]);
        total += b->w * 28.0f / b->h + 6 + font_width(res.ui, items[i + 1], 26) + 22;
    }
    float k = total - 22 > SCREEN_W - 36 ? (SCREEN_W - 36) / (total - 22) : 1;
    float x = 18;
    for (int i = 0; i + 1 < n; i += 2) {
        const Sprite * b = atlas_get(res.hud, items[i]);
        float w = b->w * 28.0f * k / b->h;
        gfx_sprite(b, x, SCREEN_H - 21 - 14 * k, w, 28 * k, WHITE);
        x += w + 6 * k;
        x += font_draw(res.ui, items[i + 1], x, SCREEN_H - 21 - 13 * k, 26 * k, RGBA(230, 230, 230, 255), ALIGN_LEFT) +
             22 * k;
    }
}

static void banner(const char * text)
{
    gfx_sprite(atlas_get(res.hud, "generic_title_bg"), -40, 4, 540, 90, WHITE);
    font_draw_shadow(res.title, text, 30, 16, 52, RGBA(255, 236, 200, 255), ALIGN_LEFT);
}

static void draw_stars(int stars, float x, float y, float size)
{
    for (int i = 0; i < 5; i++) {
        const char * n = stars >= 6 ? "hud_star_gold" : i < stars ? "hud_star_on" : "hud_star_off";
        gfx_sprite(atlas_get(res.hud, n), x + i * size * 0.92f, y, size, size, WHITE);
    }
}

// what a chart has, four to the row, each with its difficulty as six bars; the y under it
static float draw_parts(const int8_t * tier, float x, float y)
{
    float px = x;
    for (int p = 0; p < PART_COUNT; p++) {
        if (tier[p] < 0)
            continue;
        if (px > x + 3 * 76) {
            px = x;
            y += 27;
        }
        font_draw_fit(res.ui, part_name(p), px, y, 14, 72, RGBA(235, 235, 235, 255), ALIGN_LEFT);
        for (int d = 0; d < 6; d++)
            gfx_rect(px + d * 11, y + 17, 8, 5, d < tier[p] ? RGBA(255, 190, 60, 255) : RGBA(70, 70, 70, 255));
        px += 76;
    }
    return y + 27;
}

// a song's cover, or the game's logo where it has none
static void draw_cover(Tex * t, float x, float y, float size)
{
    if (t) {
        gfx_sprite(&(Sprite){t, 0, 0, 1, 1, t->w, t->h}, x, y, size, size, WHITE);
        return;
    }
    gfx_rect(x, y, size, size, RGBA(20, 20, 24, 255));
    if (res.logo)
        gfx_sprite(&(Sprite){res.logo, 0, 0, 1, 1, 512, 370}, x + size * 0.1f, y + size * 0.21f, size * 0.8f, size * 0.58f,
                   RGBA(255, 255, 255, 70));
}

// a panel of GHWT's stitched leather
static void panel(float x, float y, float w, float h)
{
    gfx_nine(atlas_get(res.hud, "list_bg"), x, y, w, h, 40, 0.8f, WHITE);
}

// capitals, accented and Greek and Cyrillic letters too (the list's titles)
static void upper(char * out, size_t size, const char * in)
{
    size_t k = 0;
    const unsigned char * p = (const unsigned char *)in;
    while (*p && k + 4 < size) {
        uint32_t c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) {
            c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = ((c & 31) << 6) | (p[0] & 63);
            p++;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) ||
            (c >= 0x430 && c <= 0x44F))
            c -= 0x20;
        else if (c >= 0x450 && c <= 0x45F)
            c -= 0x50;
        else if (c == 0xFF)
            c = 0x178;
        else if (((c >= 0x100 && c <= 0x12F) || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && (c & 1))
            c--;
        else if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && !(c & 1))
            c--;
        if (c < 0x80)
            out[k++] = (char)c;
        else if (c < 0x800) {
            out[k++] = (char)(0xC0 | c >> 6);
            out[k++] = (char)(0x80 | (c & 63));
        } else {
            out[k++] = (char)(0xE0 | c >> 12);
            out[k++] = (char)(0x80 | ((c >> 6) & 63));
            out[k++] = (char)(0x80 | (c & 63));
        }
    }
    out[k] = 0;
}

static void stop_preview(void)
{
    if (preview_for >= 0)
        audio_song_unload();
    preview_for = -1;
}

static void say(const char * text)
{
    snprintf(toast, sizeof(toast), "%s", text);
    toast_t = now_s;
}

// text in lines of max_w at most (up to max_lines), from y down; the height used
static float draw_wrapped(const char * text, float x, float y, float size, float max_w, int max_lines, uint32_t color)
{
    char line[256];
    int lines = 0;
    const char * p = text;
    while (*p && lines < max_lines) {
        // as many words as fit
        const char * start = p;
        size_t best = 0, n = 0;
        for (;;) {
            size_t k = n;
            while (p[k] == ' ')
                k++;
            while (p[k] && p[k] != ' ')
                k++;
            if (k >= sizeof(line))
                break;
            memcpy(line, p, k);
            line[k] = 0;
            if (best && font_width(res.ui, line, size) > max_w)
                break;
            best = k;
            n = k;
            if (!p[k])
                break;
        }
        memcpy(line, p, best);
        line[best] = 0;
        p += best;
        while (*p == ' ')
            p++;
        // the last line takes the rest, cut short with "..."
        font_draw_fit(res.ui, lines == max_lines - 1 && *p ? start : line, x, y + lines * size * 1.15f, size, max_w,
                      color, ALIGN_LEFT);
        lines++;
    }
    return lines * size * 1.15f;
}

static void draw_toast(void)
{
    double age = now_s - toast_t;
    if (age > 4 || !toast[0])
        return;
    float a = age < 3.5 ? 1 : (float)((4 - age) / 0.5);
    float w = font_width(res.ui, toast, 24) + 40;
    gfx_rect(SCREEN_W - w - 16, 58, w, 38, fade_alpha(RGBA(0, 0, 0, 200), a));
    font_draw(res.ui, toast, SCREEN_W - 36, 64, 24, fade_alpha(RGBA(140, 235, 140, 255), a), ALIGN_RIGHT);
}

// ------------------------------------------------------------------------------------------------------ the title

static int players_row;
static bool players_from_title;    // the picker came up at start: back goes to the title

static void title_screen(void)
{
    if (pressed & (BTN_CROSS | BTN_START)) {
        ui_sound(SND_UI_SELECT);
        // more than one player: who's playing, first
        if (player_count() > 1) {
            players_row = player_current();
            players_from_title = true;
            go(S_PLAYERS);
        } else
            go(S_MAIN);
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_title, WHITE);
    float a = 0.8f + 0.2f * sinf((float)now_s * 4);   // breathes, never fades out
    gfx_rect_grad(0, 420, SCREEN_W, 90, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 210));
    font_draw_shadow(res.title, "PRESS X TO ROCK", SCREEN_W * 0.74f, 452, 46, fade_alpha(RGBA(255, 220, 120, 255), a),
                     ALIGN_CENTER);
    gfx_end();
}

// ----------------------------------------------------------------------------------------------------- main menu

static void tour_enter(void);

// a new build on GitHub: one press and it's in, or it asks again next time
static void update_screen(void)
{
    UpState st = update_state();
    char t[200];
    if (st == UP_READY)
        update_install();
    if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        if (st == UP_DOWNLOADING) {
            update_cancel();
            return;
        }
        if (st != UP_UNPACKING) {
            go(S_MAIN);
            return;
        }
    }
    if ((pressed & BTN_CROSS) && (st == UP_AVAILABLE || st == UP_FAILED)) {
        ui_sound(SND_UI_SELECT);
        update_get();
    }
    plat_keep_awake();
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, WHITE);
    panel(180, 150, 600, 230);
    font_draw_shadow(res.title, "UPDATE", SCREEN_W / 2, 170, 44, RGBA(255, 225, 160, 255), ALIGN_CENTER);
    snprintf(t, sizeof(t), "A new build is here: %d", update_version());
    font_draw_shadow(res.ui, t, SCREEN_W / 2, 232, 26, RGBA(230, 230, 230, 255), ALIGN_CENTER);
    int64_t done, total;
    update_progress(&done, &total);
    t[0] = 0;
    if (st == UP_AVAILABLE)
        snprintf(t, sizeof(t), "%.1f MB to download", update_size() / 1048576.0);
    else if (st == UP_DOWNLOADING)
        snprintf(t, sizeof(t), "Downloading %.1f of %.1f MB", done / 1048576.0, total / 1048576.0);
    else if (st == UP_UNPACKING)
        snprintf(t, sizeof(t), "Unpacking and checking every file");
    else if (st == UP_READY)
        snprintf(t, sizeof(t), "Ready: it restarts to finish");
    else if (st == UP_FAILED)
        snprintf(t, sizeof(t), "%s", update_error());
    font_draw_fit(res.ui, t, SCREEN_W / 2, 270, 24, 560, RGBA(140, 220, 255, 255), ALIGN_CENTER);
    if (st == UP_DOWNLOADING && total > 0) {
        gfx_rect(220, 316, 520, 18, RGBA(0, 0, 0, 180));
        gfx_rect(222, 318, 516.0f * (float)done / (float)total, 14, RGBA(140, 220, 255, 255));
    }
    static const char * help[] = {"btn_cross", "Update", "btn_circle", "Later"};
    static const char * help_wait[] = {"btn_circle", "Stop"};
    if (st == UP_DOWNLOADING)
        help_bar(help_wait, 2);
    else if (st != UP_UNPACKING)
        help_bar(help, 4);
    gfx_end();
}

// a song of yours plays low under the menus, from its chorus, the way the game has its own tracks there
static bool menu_tune;
static double menu_tune_end;
static void menu_music(void)
{
    if (preview_for >= 0 || !songs_count() || cfg.no_menu_music || shrink_running)
        return;
    if (menu_tune && audio_song_time() < menu_tune_end)
        return;
    if (menu_tune)
        audio_song_unload();
    const Song * s = songs_get(rand() % songs_count());
    const char * stems[MAX_STEMS];
    for (int i = 0; i < s->stem_count; i++)
        stems[i] = s->stems[i];
    audio_preview(s->folder, stems, s->stem_count, s->preview_ms / 1000.0, 0.35f, 2.0f);
    menu_tune_end = s->preview_ms / 1000.0 + 55;
    menu_tune = true;
}

static void menu_music_stop(void)
{
    if (menu_tune)
        audio_song_unload();
    menu_tune = false;
}

// ---------------------------------------------------------------------------------------------------- daily song
// One song a day from the library, the same all day whatever the sort; finishing it on any part keeps a streak going.
// daily.txt: "<day> <streak> <best streak>", the day the last one was finished.

static void open_select(const Song * s);

// days since 1970 by the local calendar, so the song changes at the player's midnight
static long local_day(void)
{
    time_t now = time(NULL);
    struct tm * tm = localtime(&now);
    if (!tm)
        return (long)(now / 86400);
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static struct {
    long done_day;          // the day the daily song was last finished
    int streak, best;
    bool loaded;
} daily;

static void daily_load(void)
{
    if (daily.loaded)
        return;
    daily.loaded = true;
    char path[400];
    snprintf(path, sizeof(path), "%sdaily.txt", player_dir());
    FILE * f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%ld %d %d", &daily.done_day, &daily.streak, &daily.best) != 3)
            daily.done_day = daily.streak = daily.best = 0;
        fclose(f);
    }
}

// the streak as it stands today: one missed day and it's gone
static int daily_streak(void)
{
    daily_load();
    return daily.done_day >= local_day() - 1 ? daily.streak : 0;
}

static bool daily_done_today(void)
{
    daily_load();
    return daily.done_day == local_day();
}

// today's song: the one whose name hashes lowest with today's date
static int daily_pick(void)
{
    long day = local_day();
    int pick = -1;
    uint32_t low = 0xffffffffu;
    for (int i = 0; i < songs_count(); i++) {
        const Song * s = songs_get(i);
        const char * name = strrchr(s->folder, '/');
        name = name ? name + 1 : s->folder;
        uint32_t h = 2166136261u ^ (uint32_t)day;
        for (const char * c = name; *c; c++)
            h = (h ^ (uint8_t)*c) * 16777619u;
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        if (pick < 0 || h < low)
            low = h, pick = i;
    }
    return pick;
}

static bool daily_is(const Song * s)
{
    int k = daily_pick();
    return s && k >= 0 && songs_get(k) == s;
}

// a finished play of today's song: true when it made today count
static bool daily_finished(const Song * s)
{
    if (!daily_is(s) || daily_done_today())
        return false;
    long day = local_day();
    daily.streak = daily.done_day == day - 1 ? daily.streak + 1 : 1;
    daily.best = daily.streak > daily.best ? daily.streak : daily.best;
    daily.done_day = day;
    char path[400];
    snprintf(path, sizeof(path), "%sdaily.txt", player_dir());
    FILE * f = fopen(path, "w");
    if (f) {
        fprintf(f, "%ld %d %d\n", daily.done_day, daily.streak, daily.best);
        fclose(f);
    }
    return true;
}

static void main_menu(void)
{
    static int offered;                 // the build last put up: a newer one found later is offered too
    menu_music();
    update_check(gap_s > 60);           // back from sleep: the Wi-Fi's up again, and builds may have come out
    const char * news = update_news();
    if (news)
        say(news);
    if (update_state() == UP_AVAILABLE && offered != update_version()) {
        offered = update_version();
        go(S_UPDATE);
        return;
    }
    static const char * items[] = {"QUICKPLAY", "DAILY SONG", "WORLD TOUR", "DOWNLOAD SONGS", "OPTIONS", "CALIBRATE",
                                   "CAREER", "EXIT"};
    enum { M_QUICK, M_DAILY, M_TOUR, M_DOWNLOAD, M_OPTIONS, M_CALIBRATE, M_CAREER, M_EXIT };
    const int n = 8;
    if (repeat & BTN_UP) {
        menu_index = (menu_index + n - 1) % n;
        ui_sound(SND_UI_MOVE);
    }
    if (repeat & BTN_DOWN) {
        menu_index = (menu_index + 1) % n;
        ui_sound(SND_UI_MOVE);
    }
    if (pressed & BTN_SELECT) {
        ui_sound(SND_UI_SELECT);
        players_row = player_current();
        go(S_PLAYERS);
        return;
    }
    if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        go(S_TITLE);
    } else if (pressed & BTN_CROSS) {
        ui_sound(SND_UI_SELECT);
        int which = menu_index;
        if (which == M_QUICK)
            go(songs_count() ? S_SONGS : S_EMPTY);
        else if (which == M_DAILY) {
            int k = daily_pick();
            if (k < 0)
                go(S_EMPTY);
            else {
                sel = k;
                sel_t = now_s;
                menu_music_stop();
                open_select(songs_get(k));
            }
        } else if (which == M_TOUR)
            tour_enter();
        else if (which == M_DOWNLOAD)
            go(S_DOWNLOAD);
        else if (which == M_OPTIONS) {
            options_from = S_MAIN;
            go(S_OPTIONS);
        } else if (which == M_CALIBRATE) {
            calib_count = 0;
            calib_playing = false;
            go(S_CALIBRATE);
        } else if (which == M_CAREER)
            go(S_CAREER);
        else {
            plat_quit();
            exit(0);
        }
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, WHITE);
    if (res.logo)
        gfx_sprite(&(Sprite){res.logo, 0, 0, 1, 1, 512, 370}, 500, 60, 420, 303, WHITE);
    panel(40, 108, 420, 340);
    for (int i = 0; i < n; i++) {
        float y = 118 + i * 41;
        if (i == menu_index)
            gfx_sprite(atlas_get(res.hud, "wor_select"), 50, y - 5, 400, 42, WHITE);
        font_draw_shadow(res.title, items[i], 80, y, 34, i == menu_index ? RGBA(255, 230, 150, 255)
                                                                           : RGBA(200, 190, 180, 255), ALIGN_LEFT);
        if (i == M_DAILY && songs_count()) {
            // a tick once it's done today, else the streak waiting on it
            char d[32] = "";
            int st = daily_streak();
            if (daily_done_today())
                snprintf(d, sizeof(d), "done  %d day%s", st, st == 1 ? "" : "s");
            else if (st)
                snprintf(d, sizeof(d), "%d day streak", st);
            font_draw_shadow(res.ui, d, 440, y + 8, 20, daily_done_today() ? RGBA(130, 230, 130, 255)
                                                                            : RGBA(255, 200, 110, 255), ALIGN_RIGHT);
        }
    }
    if (menu_index == M_DAILY && songs_count()) {
        const Song * ds = songs_get(daily_pick());
        char d[240];
        snprintf(d, sizeof(d), "Today: %s - %s", ds->name, ds->artist);
        font_draw_fit(res.ui, d, 500, 380, 22, 440, RGBA(255, 230, 160, 255), ALIGN_LEFT);
    }
    char t[200], now[160];
    snprintf(t, sizeof(t), "%d songs", songs_count());
    font_draw_shadow(res.ui, t, 60, 456, 24, RGBA(220, 220, 220, 255), ALIGN_LEFT);
    // which build this is: a card can quietly keep an old one
    font_draw(res.ui, "build " __DATE__ " " __TIME__, SCREEN_W - 20, 490, 16, RGBA(200, 200, 210, 200), ALIGN_RIGHT);
    dl_now(now, sizeof(now));
    if (now[0]) {
        snprintf(t, sizeof(t), "Downloading %s", now);
        font_draw_fit(res.ui, t, 190, 458, 20, 740, RGBA(140, 220, 255, 255), ALIGN_LEFT);
    }
    draw_toast();
    char who[64];
    snprintf(who, sizeof(who), "Player: %s", player_name(player_current()));
    const char * help[] = {"btn_cross", "Select", "btn_circle", "Back", "btn_select", who};
    help_bar(help, 6);
    gfx_end();
}

static void empty_screen(void)
{
    if (pressed & (BTN_CIRCLE | BTN_CROSS)) {
        ui_sound(SND_UI_BACK);
        go(S_MAIN);
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, WHITE);
    banner("NO SONGS");
    panel(60, 120, 840, 330);
    static const char * lines[] = {
        "There are no songs on the memory card yet.",
        "",
        "Get some with DOWNLOAD SONGS on the main menu (Wi-Fi),",
        "or copy Clone Hero song folders or .sng files to",
        "    ux0:data/ghvita/songs/",
        "(they're made ready the next time the game starts).",
    };
    for (int i = 0; i < 6; i++)
        font_draw(res.ui, lines[i], 100, 160 + i * 42, 28, RGBA(235, 230, 220, 255), ALIGN_LEFT);
    static const char * help[] = {"btn_circle", "Back"};
    help_bar(help, 2);
    gfx_end();
}

// ------------------------------------------------------------------------------------------------ the downloader

#define DL_ROWS 6

static int dl_site, dl_sel, dl_marked = -1;
static int dl_top;
static char dl_query[DL_SITES][128];
static bool dl_started, dl_typing;
static char dl_typed[128];
static bool kb_library;             // the keyboard's searching the song list, not the sites
static bool dl_queue_view;          // the queue's own screen: what's waiting, what failed, the retry
static int dl_jobs_sel;
static Tex * dl_art;
static char dl_art_id[80];          // the result the cover is for
static double dl_sel_t;

// the SEARCH FILTERS: a panel on start. Text rows go through the keyboard (kb_apply picks up what was typed); on
// RhythmVerse a charter or artist needs the site's own name for it, so what's typed is looked up and shown as a list
// (the picker), and the choice carries that name
static bool dl_panel;               // the filter panel is over the results
static int dl_panel_row;            // which row is being looked at
enum { FROW_CHARTER, FROW_ARTIST, FROW_GENRE, FROW_INSTRUMENT, FROW_DIFFICULTY, FROW_DECADE, FROW_YEAR, FROW_SORT,
       FROW_STEMS, FROWS };
static void (*kb_apply)(const char * text);     // when set, the keyboard hands its words here instead of the search

static void dl_panel_search(void)   // filters changed, the results must know
{
    dl_search(dl_site, dl_query[dl_site]);
    dl_sel = dl_top = 0;
    dl_sel_t = now_s;
}

// the picker: the names a lookup found
static DlLookup picks[12];
static volatile int pick_state;     // -1 it's being looked up, N: how many came back, -2 none asked for
static volatile bool pick_run;      // one at a time
static char pick_words[96];
static int pick_kind;               // 0 charter, 1 artist
static int pick_sel;
static void * pick_thread(void * arg)
{
    (void)arg;
    int n = dl_lookup(pick_kind, pick_words, picks, (int)(sizeof(picks) / sizeof(picks[0])));
    pick_state = n;
    pick_run = false;
    return NULL;
}
static void pick_start(int kind, const char * words)
{
    static pthread_t t;
    snprintf(pick_words, sizeof(pick_words), "%s", words);
    pick_kind = kind;
    pick_state = -1;
    pick_sel = 0;
    if (pick_run)
        return;
    pick_run = true;
    if (pthread_create(&t, NULL, pick_thread, NULL) == 0)
        pthread_detach(t);
    else
        pick_run = false, pick_state = -2;
}

// the words and what they're for, drawn over the keyboard's line
static void dl_panel_open_kb(int row)
{
    DlFilter f;
    dl_get_filter(dl_site, &f);
    char buf[96] = "";
    if (row == FROW_CHARTER)
        snprintf(buf, sizeof(buf), "%s", f.charter_name[0] ? f.charter_name : f.charter);
    else if (row == FROW_ARTIST)
        snprintf(buf, sizeof(buf), "%s", f.artist_name[0] ? f.artist_name : f.artist);
    else if (row == FROW_YEAR) {
        if (f.year)
            snprintf(buf, sizeof(buf), "%d", f.year);
    }
    snprintf(dl_typed, sizeof(dl_typed), "%s", buf);
    dl_typing = true;
    kb_library = false;
    ui_sound(SND_UI_SELECT);
}

static void dl_panel_set(int row, const DlFilter * f)
{
    dl_set_filter(dl_site, f);
    (void)row;
    dl_panel_search();
}

static void dl_panel_clear(int row)
{
    DlFilter f;
    dl_get_filter(dl_site, &f);
    switch (row) {
    case FROW_CHARTER:
        f.charter[0] = f.charter_name[0] = 0;
        break;
    case FROW_ARTIST:
        f.artist[0] = f.artist_name[0] = 0;
        break;
    case FROW_GENRE:
        f.genre = -1;
        break;
    case FROW_INSTRUMENT:
        f.instrument = -1;
        break;
    case FROW_DIFFICULTY:
        f.difficulty = -1;
        break;
    case FROW_DECADE:
        f.decade = 0;
        break;
    case FROW_YEAR:
        f.year = 0;
        break;
    case FROW_STEMS:
        f.stems = false;
        break;
    case FROW_SORT:
        f.sort = 0;
        break;
    }
    dl_panel_set(row, &f);
    ui_sound(SND_UI_BACK);
}

static int dl_panel_rows(void)
{
    return dl_site == DL_CHORUS ? FROWS - 1 : FROWS;
}

// the panel's glyphs: what each row shows at its right
static const char * dl_panel_title(int row)
{
    static const char * const titles[FROWS] = {"Charter", "Artist", "Genre", "Instrument", "Difficulty",
                                               "Released in", "Year", "Sort", "Sound"};
    return titles[row];
}

static const char * dl_panel_value(int row, const DlFilter * f)
{
    static char buf[140];
    static const char * const diff_names[] = {"Expert", "Hard", "Medium", "Easy"};
    switch (row) {
    case FROW_CHARTER:
        return f->charter_name[0] ? f->charter_name : f->charter;
    case FROW_ARTIST:
        return f->artist_name[0] ? f->artist_name : f->artist;
    case FROW_GENRE:
        if (f->genre < 0)
            return "";
        return dl_genre_name(f->genre)[0] ? dl_genre_name(f->genre) : dl_genre_slug(f->genre);
    case FROW_INSTRUMENT:
        if (f->instrument < 0)
            return "";
        {
            const char * a, * b, * c;
            dl_instru(f->instrument, &a, &b, &c);
            return c;
        }
    case FROW_DIFFICULTY:
        return f->difficulty < 0 ? "" : diff_names[f->difficulty];
    case FROW_DECADE:
        if (f->decade) {
            snprintf(buf, sizeof(buf), "the %ds", f->decade / 10 % 100);
            return buf;
        }
        return "";
    case FROW_YEAR:
        if (f->year) {
            snprintf(buf, sizeof(buf), "%d", f->year);
            return buf;
        }
        return "";
    case FROW_SORT:
        return dl_sort_label(f->sort);
    case FROW_STEMS:
        return f->stems ? "multitrack only" : "";
    }
    return "";
}

// what a typed row does with its words; declared here, made below the panel draw (with the picker)
static void apply_kb(const char * text);

static bool pick_open;              // the names list is over the panel

// the something the panel hears each frame while it's up
static void dl_panel_input(void)
{
    DlFilter f;
    dl_get_filter(dl_site, &f);
    // the names a lookup returned? they hold the buttons until a name is picked or it's dropped
    if (pick_open) {
        if (pressed & BTN_START) {
            pick_open = false;
            pick_state = -2;
            ui_sound(SND_UI_BACK);
            return;
        }
        if (pick_state > 0) {
            if (repeat & BTN_UP)
                pick_sel--;
            if (repeat & BTN_DOWN)
                pick_sel++;
            if (pick_sel < 0)
                pick_sel = 0;
            if (pick_sel >= pick_state)
                pick_sel = pick_state - 1;
            if (pressed & BTN_CROSS) {
                DlFilter set = f;
                if (pick_kind) {
                    snprintf(set.artist, sizeof(set.artist), "%s", picks[pick_sel].id);
                    snprintf(set.artist_name, sizeof(set.artist_name), "%s", picks[pick_sel].name);
                } else {
                    snprintf(set.charter, sizeof(set.charter), "%s", picks[pick_sel].id);
                    snprintf(set.charter_name, sizeof(set.charter_name), "%s", picks[pick_sel].name);
                }
                pick_open = false;
                dl_panel_set(pick_kind ? FROW_ARTIST : FROW_CHARTER, &set);
                ui_sound(SND_UI_SELECT);
            } else if (pressed & BTN_CIRCLE) {
                pick_open = false;
                pick_state = -2;
                ui_sound(SND_UI_BACK);
            }
        }
        return;
    }
    if (pressed & (BTN_START | BTN_CIRCLE)) {
        dl_panel = false;
        pick_state = -2;
        ui_sound(SND_UI_BACK);
        return;
    }
    // left and right mustn't switch sites under the panel: the results they'd shuffle are behind it
    int rows = dl_panel_rows();
    if (repeat & BTN_UP)
        dl_panel_row = dl_panel_row == 0 ? rows - 1 : dl_panel_row - 1;
    if (repeat & BTN_DOWN)
        dl_panel_row = dl_panel_row == rows - 1 ? 0 : dl_panel_row + 1;
    int sel;
    switch (dl_panel_row) {
    case FROW_CHARTER:
    case FROW_ARTIST:
        if (pressed & BTN_CROSS) {
            kb_apply = apply_kb;
            dl_panel_open_kb(dl_panel_row);
        } else if (pressed & BTN_SQUARE)
            dl_panel_clear(dl_panel_row);
        break;
    case FROW_YEAR:
        if (pressed & BTN_CROSS) {
            kb_apply = apply_kb;
            dl_panel_open_kb(dl_panel_row);
        } else if (pressed & BTN_SQUARE && f.year)
            dl_panel_clear(dl_panel_row);
        break;
    case FROW_SORT:
        if (repeat & (BTN_LEFT | BTN_RIGHT)) {
            int n = dl_sort_count();
            f.sort = (f.sort + (repeat & BTN_RIGHT ? 1 : n - 1)) % n;
            dl_panel_set(FROW_SORT, &f);
            ui_sound(SND_UI_MOVE);
        } else if (pressed & BTN_SQUARE && f.sort)
            dl_panel_clear(FROW_SORT);
        break;
    case FROW_STEMS:
        sel = f.stems ? 1 : 0;
        if (repeat & (BTN_LEFT | BTN_RIGHT))
            sel = !sel;
        else if (pressed & BTN_SQUARE && f.stems) {
            dl_panel_clear(FROW_STEMS);
            break;
        } else
            break;
        f.stems = sel != 0;
        dl_panel_set(FROW_STEMS, &f);
        ui_sound(SND_UI_MOVE);
        break;
    case FROW_GENRE:
        if (dl_genre_count() > 0) {
            int n = dl_genre_count();
            sel = f.genre;
            if (repeat & BTN_RIGHT)
                sel = sel >= n - 1 ? -1 : sel + 1;
            else if (repeat & BTN_LEFT)
                sel = sel == -1 ? n - 1 : sel - 1;
            else if (pressed & BTN_SQUARE && f.genre >= 0) {
                dl_panel_clear(FROW_GENRE);
                break;
            } else
                break;
            if (sel != f.genre) {
                f.genre = sel;
                dl_panel_set(FROW_GENRE, &f);
                ui_sound(SND_UI_MOVE);
            }
        }
        break;
    default: {
        // instrument, difficulty, decade: the same cycle through a list, each with its own count
        int n = 0;
        switch (dl_panel_row) {
        case FROW_INSTRUMENT:
            n = dl_instru_count();
            break;
        case FROW_DIFFICULTY:
            n = dl_diff_count();
            break;
        default:
            n = 6;                      // the decades
        }
        sel = -1;
        switch (dl_panel_row) {
        case FROW_INSTRUMENT:
            sel = f.instrument;
            break;
        case FROW_DIFFICULTY:
            sel = f.difficulty;
            break;
        default:
            sel = -1;
            if (f.decade) {
                static const int decades[] = {1970, 1980, 1990, 2000, 2010, 2020};
                for (int i = 0; i < (int)(sizeof(decades) / sizeof(decades[0])); i++)
                    if (f.decade == decades[i])
                        sel = i;
            }
        }
        if (repeat & BTN_RIGHT)
            sel = sel >= n - 1 ? -1 : sel + 1;
        else if (repeat & BTN_LEFT)
            sel = sel == -1 ? n - 1 : sel - 1;
        else if (pressed & BTN_SQUARE && sel >= 0) {
            dl_panel_clear(dl_panel_row);
            break;
        } else
            break;
        int v;
        if (dl_panel_row == FROW_INSTRUMENT)
            v = sel;
        else if (dl_panel_row == FROW_DIFFICULTY)
            v = sel;
        else {
            static const int decades[] = {1970, 1980, 1990, 2000, 2010, 2020};
            v = sel < 0 ? 0 : decades[sel];
        }
        DlFilter set = f;
        if (dl_panel_row == FROW_INSTRUMENT)
            set.instrument = v;
        else if (dl_panel_row == FROW_DIFFICULTY)
            set.difficulty = v;
        else
            set.decade = v;
        dl_panel_set(dl_panel_row, &set);
        ui_sound(SND_UI_MOVE);
        break;
    }
    }
}

// the words a text row typed: a year goes in as a number, on Chorus a charter or artist's words go straight into the
// filter, on RhythmVerse they first need the site's own names, so the picker is asked
static void apply_kb(const char * text)
{
    DlFilter f;
    dl_get_filter(dl_site, &f);
    if (dl_panel_row == FROW_YEAR) {
        const char * s = text;
        while (*s == ' ')
            s++;
        bool digits = *s;
        for (const char * p = s; *p; p++)
            if (*p < '0' || *p > '9')
                digits = false;
        int y = atoi(s);
        if (y && (y < 1050 || y > 2100))
            y = 0;
        f.year = digits && y ? y : 0;
        dl_panel_set(FROW_YEAR, &f);
        return;
    }
    if (!text[0]) {
        dl_panel_clear(dl_panel_row);
        return;
    }
    if (dl_site == DL_RHYTHMVERSE) {
        if (pick_run) {
            say("Still finding names, one moment");
            return;
        }
        pick_open = true;
        pick_start(dl_panel_row == FROW_CHARTER ? 0 : 1, text);
        return;
    }
    // Chorus searches the words as they are
    DlFilter set = f;
    if (dl_panel_row == FROW_CHARTER) {
        snprintf(set.charter, sizeof(set.charter), "%s", text);
        snprintf(set.charter_name, sizeof(set.charter_name), "%s", text);
    } else {
        snprintf(set.artist, sizeof(set.artist), "%s", text);
        snprintf(set.artist_name, sizeof(set.artist_name), "%s", text);
    }
    dl_panel_set(dl_panel_row, &set);
}

static void dl_panel_draw(void)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(6, 6, 10, 246));
    panel(110, 48, 740, 452);
    font_draw_shadow(res.ui, "SEARCH FILTERS", SCREEN_W / 2, 66, 28, RGBA(255, 230, 150, 255), ALIGN_CENTER);
    DlFilter f;
    dl_get_filter(dl_site, &f);
    int rows = dl_panel_rows();
    for (int r = 0; r < rows; r++) {
        float y = 104 + r * 44;
        bool on = r == dl_panel_row;
        gfx_rect(126, y, 708, 40, on ? RGBA(60, 52, 30, 255) : RGBA(0, 0, 0, 60));
        font_draw(res.ui, dl_panel_title(r), 140, y + 9, 22, on ? WHITE : RGBA(180, 180, 190, 255), ALIGN_LEFT);
        const char * v = dl_panel_value(r, &f);
        bool any = !v || !v[0];
        if (r == FROW_GENRE && f.genre >= 0 && dl_genre_count() < 0)
            v = "Loading...";       // the genre list is still coming from the site
        else if (any)
            v = "Any";              // not filtered
        uint32_t c = any ? RGBA(120, 120, 130, 255) : on ? RGBA(255, 236, 170, 255) : RGBA(230, 225, 215, 255);
        font_draw_fit(res.ui, v, 818, y + 9, 22, 420, c, ALIGN_RIGHT);
    }
    font_draw_shadow(res.ui,
                     "Cross edit   Left/Right change   Square clear   Start done",
                     SCREEN_W / 2, 104 + rows * 44 + 16, 18, RGBA(170, 170, 180, 255), ALIGN_CENTER);
}

// the names a lookup found, over the panel
static void dl_picker_draw(void)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(6, 6, 10, 246));
    panel(130, 64, 700, 340);
    font_draw_shadow(res.ui, pick_kind ? "ARTISTS" : "CHARTERS", SCREEN_W / 2, 82, 28, RGBA(255, 230, 150, 255),
                     ALIGN_CENTER);
    char t[160];
    if (pick_state < 0)
        snprintf(t, sizeof(t), "Asking RhythmVerse who \"%s\" is...", pick_words);
    else if (!pick_state)
        snprintf(t, sizeof(t), "No one the site knows by \"%s\"", pick_words);
    else
        snprintf(t, sizeof(t), "%d found (Cross picks, Circle drops)", pick_state);
    font_draw_fit(res.ui, t, SCREEN_W / 2, 116, 20, 640,
                  pick_state ? RGBA(190, 190, 200, 255) : RGBA(255, 150, 110, 255), ALIGN_CENTER);
    if (pick_state > 0)
        for (int i = 0; i < pick_state; i++) {
            float y = 152 + i * 40;
            bool on = i == pick_sel;
            char line[200];
            if (picks[i].releases > 0)
                snprintf(line, sizeof(line), "%s%s  (%d charts)", on ? "> " : "", picks[i].name,
                         picks[i].releases);
            else
                snprintf(line, sizeof(line), "%s%s", on ? "> " : "", picks[i].name);
            font_draw_fit(res.ui, line, 170, y, 22, 540, on ? RGBA(255, 236, 170, 255) : WHITE, ALIGN_LEFT);
        }
}

static void dl_new_search(void)
{
    dl_search(dl_site, dl_query[dl_site]);
    dl_sel = dl_top = 0;
    dl_sel_t = now_s;
}

// which results the library has (after each search, and as songs come in)
static void dl_mark(void)
{
    dl_lock();
    for (int i = 0; i < dl_count(); i++) {
        DlHit * h = dl_hit(i);
        int have = songs_have(h->id, h->artist, h->name);
        h->have = have == 2;
        h->have_name = have == 1;
    }
    dl_marked = dl_version();
    dl_unlock();
}

// what a result's row says on its right (NULL: nothing), and in what colour
static const char * hit_badge(const DlHit * h, uint32_t * color, char * buf, size_t size)
{
    int pct;
    switch (dl_job_state(h->id, &pct, NULL, 0)) {
    case JOB_QUEUED:
        *color = RGBA(210, 210, 210, 255);
        return "WAITING";
    case JOB_DOWNLOADING:
        *color = RGBA(140, 220, 255, 255);
        if (pct < 0)
            return "DOWNLOADING";
        snprintf(buf, size, "%d%%", pct);
        return buf;
    case JOB_INSTALLING:
        *color = RGBA(140, 220, 255, 255);
        return "ADDING";
    case JOB_DONE:
        *color = RGBA(120, 230, 120, 255);
        return "ADDED";
    case JOB_FAILED:
        *color = RGBA(255, 110, 90, 255);
        return "FAILED";
    default:
        break;
    }
    if (h->have) {
        *color = RGBA(120, 230, 120, 255);
        return "IN LIBRARY";
    }
    if (h->unplayable[0]) {
        *color = RGBA(130, 120, 110, 255);
        return "CAN'T GET";
    }
    if (h->have_name) {
        *color = RGBA(210, 195, 140, 255);
        return "HAVE SONG";
    }
    return NULL;
}

// The search keyboard, drawn by the game: Sony's dialog hung or crashed here on hardware (it renders over the 2X MSAA
// target). D-pad moves, Cross types, Square deletes, Triangle is a space, Start searches, Circle cancels.
static const char * kb_rows[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-.&"};
static int kb_x, kb_y;

static void kb_update(void)
{
    if (repeat & BTN_LEFT)
        kb_x = (kb_x + 9) % 10;
    if (repeat & BTN_RIGHT)
        kb_x = (kb_x + 1) % 10;
    if (repeat & BTN_UP)
        kb_y = (kb_y + 3) % 4;
    if (repeat & BTN_DOWN)
        kb_y = (kb_y + 1) % 4;
    size_t l = strlen(dl_typed);
    if ((pressed & BTN_CROSS) && l + 1 < sizeof(dl_typed)) {
        char c = kb_rows[kb_y][kb_x];
        dl_typed[l] = c >= 'A' && c <= 'Z' ? c + 32 : c;
        dl_typed[l + 1] = 0;
    }
    if ((pressed & BTN_TRIANGLE) && l + 1 < sizeof(dl_typed)) {
        dl_typed[l] = ' ';
        dl_typed[l + 1] = 0;
    }
    if ((repeat & BTN_SQUARE) && l)
        dl_typed[l - 1] = 0;
    if (pressed & BTN_CIRCLE)
        dl_typing = false;
    if (pressed & BTN_START) {
        ui_sound(SND_UI_SELECT);
        // the words, without spaces around them
        char * t = dl_typed;
        while (*t == ' ')
            t++;
        l = strlen(t);
        while (l && t[l - 1] == ' ')
            t[--l] = 0;
        dl_typing = false;
        if (kb_apply) {
            // the filter panel asked for them
            void (*apply)(const char *) = kb_apply;
            kb_apply = NULL;
            plat_log("typed for a filter \"%s\"", t);
            apply(t);
            return;
        }
        if (kb_library) {
            snprintf(find_text, sizeof(find_text), "%s", t);
            view_stale = true;
            return;
        }
        snprintf(dl_query[dl_site], sizeof(dl_query[0]), "%s", t);
        plat_log("search: typed \"%s\"", t);
        dl_new_search();
    }
}

static void kb_draw(void)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 238));
    char t[140];
    snprintf(t, sizeof(t), "%s%s", dl_typed, fmod(now_s, 1.0) < 0.5 ? "_" : " ");
    font_draw_fit(res.ui, t, 150, 110, 32, 660, WHITE, ALIGN_LEFT);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 10; x++) {
            float px = 150 + x * 67, py = 170 + y * 62;
            bool on = x == kb_x && y == kb_y;
            gfx_rect(px, py, 60, 54, on ? RGBA(255, 200, 80, 255) : RGBA(50, 50, 60, 255));
            char c[2] = {kb_rows[y][x], 0};
            font_draw(res.ui, c, px + 30, py + 11, 30, on ? RGBA(0, 0, 0, 255) : WHITE, ALIGN_CENTER);
        }
    font_draw(res.ui, "Cross type   Square delete   Triangle space   Start search   Circle cancel", SCREEN_W / 2, 430, 20,
              RGBA(220, 220, 230, 255), ALIGN_CENTER);
}

// the queue: what's waiting, what's under way, what failed, and the retry. It's saved with the game closed, so this
// is also where a failed download is picked back up after a restart
static void dl_queue_screen(void)
{
    static DlJob jobs[24];
    int n = dl_jobs_list(jobs, 24);
    if (dl_jobs_sel >= n)
        dl_jobs_sel = n - 1;
    if (dl_jobs_sel < 0)
        dl_jobs_sel = 0;
    if ((repeat & BTN_UP) && dl_jobs_sel > 0) {
        dl_jobs_sel--;
        ui_sound(SND_UI_MOVE);
    }
    if ((repeat & BTN_DOWN) && dl_jobs_sel < n - 1) {
        dl_jobs_sel++;
        ui_sound(SND_UI_MOVE);
    }
    bool has = n > 0;
    DlJob * j = has ? &jobs[dl_jobs_sel] : NULL;
    if ((pressed & BTN_CROSS) && has && j->state == JOB_FAILED) {
        dl_queue(&j->hit);
        ui_sound(SND_UI_SELECT);
    }
    if ((pressed & BTN_SQUARE) && has) {
        if (j->state == JOB_QUEUED || j->state == JOB_DOWNLOADING || j->state == JOB_INSTALLING)
            dl_cancel(j->hit.id);
        else
            dl_forget(j->hit.id);
        ui_sound(SND_UI_BACK);
    }
    if ((pressed & BTN_TRIANGLE) && has && j->state == JOB_QUEUED) {
        dl_prioritize(j->hit.id);
        char msg[128];
        snprintf(msg, sizeof(msg), "%s downloads next", j->hit.name);
        say(msg);
        ui_sound(SND_UI_SELECT);
    }
    if ((pressed & BTN_START) && has) {
        dl_clear_done();
        say("The finished ones left the list");
        ui_sound(SND_UI_BACK);
    }
    if (pressed & (BTN_CIRCLE | BTN_SELECT)) {
        dl_queue_view = false;
        ui_sound(SND_UI_BACK);
    }

    char t[200];
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, WHITE);
    banner("DOWNLOAD QUEUE");
    // the running one's speed, from the byte counts a third of a second apart
    static struct {
        char id[80];
        int64_t done;
        double t, speed;
    } speed[16];
    for (int i = 0; i < n; i++) {
        if (jobs[i].state != JOB_DOWNLOADING || jobs[i].total <= 0)
            continue;
        int slot = 0;
        while (slot < 16 && speed[slot].id[0] && strcmp(speed[slot].id, jobs[i].hit.id))
            slot++;
        if (slot == 16)
            continue;
        if (!speed[slot].id[0]) {
            snprintf(speed[slot].id, sizeof(speed[slot].id), "%s", jobs[i].hit.id);
            speed[slot].t = speed[slot].speed = 0;
        }
        if (jobs[i].done < speed[slot].done)        // it started over
            speed[slot].t = 0;
        double dt = now_s - speed[slot].t;
        if (dt >= 0.3) {
            speed[slot].speed = dt > 0 ? (double)(jobs[i].done - speed[slot].done) / dt : 0;
            if (speed[slot].speed < 0)
                speed[slot].speed = 0;
            speed[slot].done = jobs[i].done;
            speed[slot].t = now_s;
        }
    }
    int rows = n < 7 ? n : 7, top = dl_jobs_sel - 3;
    if (top > n - rows)
        top = n - rows;
    if (top < 0)
        top = 0;
    for (int r = 0; r < rows; r++) {
        const DlJob * q = &jobs[top + r];
        float y = 130 + r * 56;
        bool on = top + r == dl_jobs_sel;
        gfx_sprite(atlas_get(res.hud, on ? "wor_select" : "wor_row"), 14, y, 580, 54, WHITE);
        font_draw_fit(res.ui, q->hit.site == DL_CHORUS ? "CH" : "RV", 42, y + 9, 16, 40, RGBA(135, 140, 150, 255),
                      ALIGN_LEFT);
        font_draw_fit(res.ui, q->hit.name, 80, y + 6, 22, 380, on ? RGBA(255, 220, 160, 255) : RGBA(220, 220, 225, 255),
                      ALIGN_LEFT);
        const char * badge = "WAITING";
        uint32_t bc = RGBA(210, 210, 210, 255);
        if (q->state == JOB_DOWNLOADING) {
            bc = RGBA(140, 220, 255, 255);
            if (q->percent < 0)
                badge = "DOWNLOADING";
            else
                snprintf(t, sizeof(t), "%d%%", q->percent), badge = t;
        } else if (q->state == JOB_INSTALLING) {
            badge = "ADDING";
            bc = RGBA(140, 220, 255, 255);
        } else if (q->state == JOB_DONE) {
            badge = "ADDED";
            bc = RGBA(120, 230, 120, 255);
        } else if (q->state == JOB_FAILED) {
            badge = "FAILED";
            bc = RGBA(255, 110, 90, 255);
        }
        font_draw_shadow(res.ui, badge, 572, y + 8, 20, bc, ALIGN_RIGHT);
        char line2[190];
        line2[0] = 0;
        if (q->msg[0])
            snprintf(line2, sizeof(line2), "%s", q->msg);
        if (q->state == JOB_DOWNLOADING) {
            if (q->total > 0)
                snprintf(line2 + strlen(line2), sizeof(line2) - strlen(line2), "%s%.0f of %.1f MB", line2[0] ? "  " : "",
                         (double)(q->done > 0 ? q->done : 0) / 1048576, (double)q->total / 1048576);
            double sp = 0;
            for (int k = 0; k < 8; k++)
                if (speed[k].id[0] && !strcmp(speed[k].id, q->hit.id))
                    sp = speed[k].speed;
            if (sp > 1)
                snprintf(line2 + strlen(line2), sizeof(line2) - strlen(line2), "  %.1f MB/s", sp / 1048576);
        }
        if (line2[0])
            font_draw_fit(res.ui, line2, 42, y + 30, 18, 500, q->state == JOB_FAILED ? RGBA(255, 150, 110, 255)
                                                                                    : RGBA(170, 170, 175, 255), ALIGN_LEFT);
    }
    if (!n)
        font_draw_fit(res.ui, "Nothing waiting, nothing failed", 304, 260, 26, 560, RGBA(230, 225, 215, 255),
                      ALIGN_CENTER);
    snprintf(t, sizeof(t), "RV RhythmVerse, CH Chorus. The queue carries over when the game closes; failed ones wait here for X");
    font_draw_fit(res.ui, t, 24, 470, 19, 900, RGBA(160, 175, 185, 255), ALIGN_LEFT);
    static const char * const help[] = {"btn_cross", "Retry", "btn_square", "Remove", "btn_triangle", "Next",
                                        "btn_start", "Clear done", "btn_select", "Songs", "btn_circle", "Back"};
    help_bar(help, 12);
    draw_toast();
    gfx_end();
}

static void download_screen(void)
{
    if (!dl_started) {
        dl_started = true;
        dl_queue_view = false;
        dl_new_search();            // the newest charts
    }
    if (dl_typing) {
        kb_update();
        pressed = repeat = 0;       // the keyboard has the buttons
    }
    if (pressed & BTN_SELECT) {
        dl_queue_view = true;
        dl_jobs_sel = 0;
        ui_sound(SND_UI_SELECT);
        return;                     // it opens next frame: the same press must not close it again here
    }
    if (dl_queue_view) {
        dl_queue_screen();
        return;
    }
    // the filters' panel over the results: its buttons don't reach the list behind it
    if (dl_panel && !dl_typing) {
        dl_panel_input();
        pressed = repeat = 0;
    }
    if (dl_marked != dl_version())
        dl_mark();

    // the results the screen needs, copied (the search thread may replace them)
    DlHit rows[DL_ROWS], cur;
    int n, nrows = 0;
    bool more = dl_can_more();
    dl_lock();
    n = dl_count();
    int old = dl_sel;
    if (repeat & BTN_UP)
        dl_sel--;
    if (repeat & BTN_DOWN)
        dl_sel++;
    if (repeat & BTN_LEFT)
        dl_sel -= DL_ROWS;
    if (repeat & BTN_RIGHT)
        dl_sel += DL_ROWS;
    if (dl_sel < 0)
        dl_sel = (repeat & BTN_UP) && old == 0 && !more ? n - 1 : 0;
    if (dl_sel >= n)
        dl_sel = (repeat & BTN_DOWN) && old == n - 1 && !more ? 0 : n - 1;
    if (dl_sel < 0)
        dl_sel = 0;
    if (dl_sel < dl_top)
        dl_top = dl_sel;
    if (dl_sel >= dl_top + DL_ROWS)
        dl_top = dl_sel - DL_ROWS + 1;
    for (int r = 0; r < DL_ROWS && dl_top + r < n; r++)
        rows[nrows++] = *dl_hit(dl_top + r);
    bool has = n > 0;
    if (has)
        cur = *dl_hit(dl_sel);
    dl_unlock();
    if (dl_sel != old) {
        ui_sound(SND_UI_MOVE);
        dl_sel_t = now_s;
    }
    // near the end of what's there: the next page
    if (more && n && dl_sel >= n - 3)
        dl_more();

    if (pressed & BTN_START) {
        // the filter panel: which charter, which kind of song, and so on
        dl_panel = true;
        dl_panel_row = 0;
        pick_open = false;
        pick_state = -2;
        dl_genre_count();           // the list gets asked of the site the first time it's needed
        ui_sound(SND_UI_SELECT);
    }
    if (pressed & (BTN_L | BTN_R)) {
        dl_site = (dl_site + 1) % DL_SITES;
        dl_new_search();
        ui_sound(SND_UI_MOVE);
    }
    if (pressed & BTN_TRIANGLE) {
        plat_log("search: keyboard open");
        char title[64];
        snprintf(title, sizeof(title), "Search %s", dl_site == DL_CHORUS ? "Chorus Encore" : "RhythmVerse");
        snprintf(dl_typed, sizeof(dl_typed), "%s", dl_query[dl_site]);
        dl_typing = true;
        kb_library = false;
        kb_apply = NULL;            // these words are for the search itself
        ui_sound(SND_UI_SELECT);
    }
    if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        dl_panel = false;
        pick_open = false;
        go(S_MAIN);
        menu_index = 3;             // back on DOWNLOAD SONGS
        return;
    }
    if ((pressed & BTN_CROSS) && has) {
        JobState st = dl_job_state(cur.id, NULL, NULL, 0);
        if (cur.unplayable[0]) {
            char t[120];
            snprintf(t, sizeof(t), "Can't get it: %s", cur.unplayable);
            say(t);
            ui_sound(SND_UI_BACK);
        } else if (cur.have) {
            say("That one's in your library");
            ui_sound(SND_UI_BACK);
        } else if (st == JOB_QUEUED || st == JOB_DOWNLOADING || st == JOB_INSTALLING) {
            say("It's on its way");
            ui_sound(SND_UI_BACK);
        } else if (!dl_queue(&cur)) {
            say("The queue is full: one has to finish first");
            ui_sound(SND_UI_BACK);
        } else {
            ui_sound(SND_UI_SELECT);
        }
    }
    if ((pressed & BTN_SQUARE) && has) {
        JobState st = dl_job_state(cur.id, NULL, NULL, 0);
        if (st == JOB_QUEUED || st == JOB_DOWNLOADING) {
            dl_cancel(cur.id);
            say("Cancelled");
            ui_sound(SND_UI_BACK);
        }
    }
    // the cover once the choice settles
    if (has && strcmp(dl_art_id, cur.id) != 0 && now_s - dl_sel_t > 0.15) {
        const char * path = cur.art[0] ? dl_cover(&cur) : NULL;
        if (path || !cur.art[0]) {
            tex_free(dl_art);
            dl_art = path ? tex_load(path, TEX_MIPMAP) : NULL;
            snprintf(dl_art_id, sizeof(dl_art_id), "%s", cur.id);
        }
    }

    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, WHITE);
    banner("DOWNLOAD SONGS");
    // the two sites, L and R between them
    {
        const Sprite * bl = atlas_get(res.hud, "btn_l"), * br = atlas_get(res.hud, "btn_r");
        float x = SCREEN_W - 24, wr = br->w * 28.0f / br->h, wl = bl->w * 28.0f / bl->h;
        gfx_sprite(br, x - wr, 22, wr, 28, WHITE);
        x -= wr + 12;
        for (int s = DL_SITES - 1; s >= 0; s--) {
            const char * name = s == DL_CHORUS ? "CHORUS ENCORE" : "RHYTHMVERSE";
            float w = font_width(res.ui, name, 26);
            if (s == dl_site)
                gfx_rect(x - w - 6, 52, w + 12, 4, RGBA(255, 200, 90, 255));
            font_draw_shadow(res.ui, name, x, 22, 26, s == dl_site ? RGBA(255, 230, 150, 255)
                                                                    : RGBA(150, 145, 140, 255), ALIGN_RIGHT);
            x -= w + 24;
        }
        gfx_sprite(bl, x - wl + 12, 22, wl, 28, WHITE);
    }
    char t[200], status[200];
    dl_status(status, sizeof(status));
    if (dl_query[dl_site][0])
        snprintf(t, sizeof(t), "Search: %s", dl_query[dl_site]);
    else
        snprintf(t, sizeof(t), "Newest charts");
    font_draw_fit(res.ui, t, 24, 98, 24, 560, RGBA(255, 236, 200, 255), ALIGN_LEFT);
    font_draw_fit(res.ui, status, SCREEN_W - 24, 98, 22, 340, RGBA(200, 200, 200, 255), ALIGN_RIGHT);
    // what's filtering or ordering this site, when anything is
    char filt[192];
    dl_filter_text(dl_site, filt, sizeof(filt));
    if (filt[0])
        font_draw_fit(res.ui, filt, 24, 64, 18, 900, RGBA(150, 165, 180, 255), ALIGN_LEFT);
    // the results
    for (int r = 0; r < nrows; r++) {
        const DlHit * h = &rows[r];
        float y = 130 + r * 56;
        bool on = dl_top + r == dl_sel, dim = h->unplayable[0] != 0;
        gfx_sprite(atlas_get(res.hud, on ? "wor_select" : "wor_row"), 14, y, 580, 54, WHITE);
        char up[128], badge_buf[16], sub[200];
        upper(up, sizeof(up), h->name);
        if (h->charter[0])
            snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", h->artist, h->charter);
        else
            snprintf(sub, sizeof(sub), "%s", h->artist);
        uint32_t bc = 0;
        const char * badge = hit_badge(h, &bc, badge_buf, sizeof(badge_buf));
        float bw = badge ? font_width(res.ui, badge, 20) + 16 : 0;
        font_draw_fit(res.ui, up, 40, y + 4, 26, 520 - bw, dim ? RGBA(130, 125, 120, 255) : on ? RGBA(255, 236, 170, 255)
                                                                                                 : RGBA(235, 235, 235, 255),
                      ALIGN_LEFT);
        font_draw_fit(res.ui, sub, 42, y + 28, 20, 500 - bw, dim ? RGBA(110, 105, 100, 255)
                                                                       : on ? RGBA(255, 210, 140, 255)
                                                                            : RGBA(170, 170, 175, 255), ALIGN_LEFT);
        if (badge)
            font_draw_shadow(res.ui, badge, 572, y + 16, 20, bc, ALIGN_RIGHT);
    }
    if (!nrows)
        font_draw_fit(res.ui, status, 304, 260, 26, 560, RGBA(230, 225, 215, 255), ALIGN_CENTER);
    else if (more || dl_searching())
        font_draw(res.ui, dl_searching() ? "..." : "more below", 304, 130 + DL_ROWS * 56, 18,
                  RGBA(170, 170, 170, 255), ALIGN_CENTER);
    // what room's left on the card, looked at every few seconds
    static long long room = -1;
    static int room_wait;
    if (--room_wait <= 0) {
        room = plat_free_bytes();
        room_wait = 180;
    }
    if (room >= 0) {
        snprintf(t, sizeof(t), room >= (1LL << 30) ? "%.1f GB free" : "%.0f MB free",
                 room >= (1LL << 30) ? room / 1073741824.0 : room / 1048576.0);
        font_draw_shadow(res.ui, t, 936, 68, 20, room < (200LL << 20) ? RGBA(255, 120, 100, 255) : RGBA(190, 230, 190, 255),
                         ALIGN_RIGHT);
    }
    // what's downloading
    char now[160];
    dl_now(now, sizeof(now));
    if (now[0]) {
        snprintf(t, sizeof(t), "Downloading %s", now);
        font_draw_fit(res.ui, t, 24, 470, 20, 570, RGBA(140, 220, 255, 255), ALIGN_LEFT);
    }
    // the chosen one
    if (has) {
        panel(608, 124, 340, 370);
        draw_cover(dl_art && strcmp(dl_art_id, cur.id) == 0 ? dl_art : NULL, 716, 138, 124);
        float y = 270;
        font_draw_fit(res.ui, cur.name, 630, y, 24, 300, RGBA(255, 220, 160, 255), ALIGN_LEFT);
        y += 26;
        font_draw_fit(res.ui, cur.artist, 630, y, 20, 300, RGBA(230, 230, 230, 255), ALIGN_LEFT);
        y += 22;
        if (cur.album[0]) {
            if (cur.year[0])
                snprintf(t, sizeof(t), "%s (%s)", cur.album, cur.year);
            else
                snprintf(t, sizeof(t), "%s", cur.album);
            font_draw_fit(res.ui, t, 630, y, 18, 300, RGBA(200, 200, 200, 255), ALIGN_LEFT);
            y += 21;
        }
        if (cur.genre[0]) {
            snprintf(t, sizeof(t), "Genre: %s", cur.genre);
            font_draw_fit(res.ui, t, 630, y, 18, 300, RGBA(170, 170, 170, 255), ALIGN_LEFT);
            y += 21;
        }
        int secs = cur.length_ms / 1000;
        if (cur.size > 0)
            snprintf(t, sizeof(t), "%d:%02d   %.1f MB", secs / 60, secs % 60, cur.size / 1048576.0);
        else
            snprintf(t, sizeof(t), "%d:%02d", secs / 60, secs % 60);
        font_draw(res.ui, t, 630, y, 18, RGBA(200, 200, 200, 255), ALIGN_LEFT);
        y += 21;
        if (cur.charter[0]) {
            snprintf(t, sizeof(t), "Charted by %s%s", cur.charter, cur.rock_band ? "  (Rock Band)" : "");
            font_draw_fit(res.ui, t, 630, y, 18, 300, RGBA(170, 170, 170, 255), ALIGN_LEFT);
            y += 22;
        }
        if (cur.comment[0]) {
            font_draw_fit(res.ui, cur.comment, 630, y, 16, 300, RGBA(190, 190, 200, 255), ALIGN_LEFT);
            y += 44;
        }
        // what's in the chart, with how hard: known before it's downloaded
        if (cur.any_parts) {
            y = draw_parts(cur.parts, 630, y);
        }
        // why it failed, why it can't be had, or that there's one already
        char msg[120] = "";
        int pct;
        JobState st = dl_job_state(cur.id, &pct, msg, sizeof(msg));
        uint32_t mc = RGBA(255, 150, 110, 255);
        if (st == JOB_FAILED)
            snprintf(t, sizeof(t), "Failed: %s", msg);
        else if (st == JOB_DONE)
            snprintf(t, sizeof(t), "%s", msg[0] ? msg : "Added to your songs");
        else if (cur.unplayable[0])
            snprintf(t, sizeof(t), "Can't get it: %s", cur.unplayable);
        else if (cur.have_name && st == JOB_NONE)
            snprintf(t, sizeof(t), "You have this song (another chart)");
        else
            t[0] = 0;
        if (st == JOB_DONE || (cur.have_name && st == JOB_NONE && !cur.unplayable[0]))
            mc = RGBA(170, 230, 160, 255);
        draw_wrapped(t, 630, y + 2, 18, 300, 3, mc);
    }
    // the filters over everything (the keyboard still goes on top of them)
    if (dl_panel) {
        dl_panel_draw();
        if (pick_open)
            dl_picker_draw();
    }
    if (dl_typing) {
        // the desktop build's keyboard (the Vita's draws itself over this)
        panel(150, 190, 660, 150);
        snprintf(t, sizeof(t), "Search %s", dl_site == DL_CHORUS ? "Chorus Encore" : "RhythmVerse");
        font_draw_shadow(res.ui, t, SCREEN_W / 2, 214, 26, RGBA(255, 230, 150, 255), ALIGN_CENTER);
        gfx_rect(190, 256, 580, 44, RGBA(10, 10, 12, 230));
        snprintf(t, sizeof(t), "%s%s", dl_typed, fmod(now_s, 1.0) < 0.5 ? "_" : " ");
        font_draw_fit(res.ui, t, 204, 264, 28, 552, WHITE, ALIGN_LEFT);
    }
    static const char * help[] = {"btn_cross", "Download", "btn_triangle", "Search", "btn_start", "Filters",
                                  "btn_l", "Site", "btn_square", "Cancel", "btn_select", "Queue",
                                  "btn_circle", "Back"};
    help_bar(help, 14);
    draw_toast();
    if (dl_typing)
        kb_draw();
    gfx_end();
}

// songs the downloader has finished join the library while no song list or song is on screen (their indexes
// mustn't move meanwhile)
static void take_downloads(void)
{
    if (screen == S_SONGS || screen == S_SELECT || screen == S_PRACTICE || screen == S_PLAY || screen == S_PAUSE ||
        screen == S_RESUME || screen == S_RESULTS)
        return;
    char folder[400];
    if (!dl_take_installed(folder, sizeof(folder)))
        return;
    Song * cur = songs_get(sel);
    char keep[400], name[120] = "", t[160];
    snprintf(keep, sizeof(keep), "%s", cur ? cur->folder : "");
    int added = 0;
    do {
        int i = songs_add(folder);
        if (i >= 0) {
            added++;
            snprintf(name, sizeof(name), "%s", songs_get(i)->name);
        }
    } while (dl_take_installed(folder, sizeof(folder)));
    int k = songs_find(keep);
    sel = k >= 0 ? k : 0;
    art_for = -1;
    view_stale = true;
    dl_marked = -1;
    if (added == 1)
        snprintf(t, sizeof(t), "Added %s", name);
    else
        snprintf(t, sizeof(t), "Added %d songs", added);
    if (added)
        say(t);
}

// ------------------------------------------------------------------------------------------------- the song list

static int first_part(const Song * s)
{
    if (s->parts & (1 << cfg.last_part))
        return cfg.last_part;
    for (int p = 0; p < PART_COUNT; p++)
        if (s->parts & (1 << p))
            return p;
    return 0;
}

// what the chosen song's chart has: many custom charts are Expert only
static void read_diffs(const Song * s)
{
    memset(sel_diffs, 0, sizeof(sel_diffs));
    char path[600];
    snprintf(path, sizeof(path), "%s/notes.vhc", s->folder);
    Chart * c = chart_load(path);
    bool any = false;
    for (int p = 0; c && p < PART_COUNT; p++)
        for (int d = 0; d < DIFF_COUNT; d++) {
            const Part * pt = chart_part(c, p, d);
            if (pt && pt->note_count > 0) {
                sel_diffs[p] |= 1 << d;
                any = true;
            }
        }
    if (c)
        chart_free(c);
    if (vox_exists(s->folder)) {
        sel_diffs[PART_VOCALS] = 0xf;
        any = true;
    }
    if (!any)                       // unreadable: offer what song.ini says (the game will say if it can't)
        for (int p = 0; p < PART_COUNT; p++)
            sel_diffs[p] = s->parts & (1 << p) ? 0xf : 0;
}

// the difficulty the part has nearest to want (the harder one of two as near)
static int nearest_diff(int part, int want)
{
    for (int k = 0; k < DIFF_COUNT; k++) {
        if (want + k < DIFF_COUNT && (sel_diffs[part] & (1 << (want + k))))
            return want + k;
        if (want - k >= 0 && (sel_diffs[part] & (1 << (want - k))))
            return want - k;
    }
    return want;
}

// the part and difficulty screen for a song
static void open_select(const Song * s)
{
    read_diffs(s);
    sel_part = first_part(s);
    for (int p = 0; !sel_diffs[sel_part] && p < PART_COUNT; p++)
        if (sel_diffs[p])
            sel_part = p;
    want_diff = cfg.last_diff;
    sel_diff = nearest_diff(sel_part, want_diff);
    go(S_SELECT);
}

static void load_art(const Song * s)
{
    tex_free(art);
    art = NULL;
    char path[400];
    snprintf(path, sizeof(path), "%s/album.jpg", s->folder);
    FILE * f = fopen(path, "rb");
    if (f) {
        fclose(f);
        art = tex_load(path, TEX_MIPMAP);
    }
    art_for = sel;
}

static bool del_ask, list_menu;
static int list_row;
static double group_t;              // a jump just happened: its heading shows for a moment

static bool has_text(const char * in, const char * word, size_t n)
{
    for (; *in; in++)
        if (strncasecmp(in, word, n) == 0)
            return true;
    return false;
}

// every word found somewhere in what's known of the song
static bool song_matches(const Song * s, const char * text)
{
    while (*text) {
        while (*text == ' ')
            text++;
        size_t n = strcspn(text, " ");
        if (n && !has_text(s->name, text, n) && !has_text(s->artist, text, n) && !has_text(s->album, text, n) &&
            !has_text(s->genre, text, n) && !has_text(s->charter, text, n) && !has_text(s->year, text, n))
            return false;
        text += n;
    }
    return true;
}

// five stars or better on any part at any difficulty
static bool song_starred(const Song * s)
{
    for (int p = 0; p < PART_COUNT; p++)
        for (int d = 0; d < DIFF_COUNT; d++)
            if (s->best[p][d].stars >= 5)
                return true;
    return false;
}

static bool song_shown(const Song * s)
{
    // played, but not in the last 30 days (plays from before times were kept count as long ago)
    long month_ago = (long)time(NULL) - 30L * 24 * 3600;
    if ((show == SHOW_FAVS && !s->fav) || (show == SHOW_NEW && s->plays) ||
        (show == SHOW_STALE && (!s->plays || s->last_played > month_ago)) ||
        (show == SHOW_UNSTARRED && (!s->plays || song_starred(s))) ||
        (show == SHOW_SHORT && (s->length_ms <= 0 || s->length_ms > 180000)) ||
        (show == SHOW_LONG && s->length_ms <= 360000) ||
        (show >= SHOW_PART && !(s->parts & (1 << (show - SHOW_PART)))))
        return false;
    return song_matches(s, find_text);
}

// the list again, staying on the song it was on, or as near as the list allows
static void view_build(void)
{
    int n = songs_count();
    if (n > view_cap) {
        view_cap = n + 256;
        view = realloc(view, view_cap * sizeof(int));
    }
    view_n = 0;
    int near = 0;
    for (int i = 0; i < n; i++)
        if (song_shown(songs_get(i))) {
            if (i <= sel)
                near = view_n;
            view[view_n++] = i;
        }
    pos = near;
    view_stale = false;
    if (view_n && view[pos] != sel) {
        sel = view[pos];
        sel_t = now_s;
    }
}

static const char * show_name(void)
{
    static char t[32];
    if (show == SHOW_FAVS)
        return "Favourites";
    if (show == SHOW_NEW)
        return "Not played yet";
    if (show == SHOW_STALE)
        return "Not played in a month";
    if (show == SHOW_UNSTARRED)
        return "Played, never 5 stars";
    if (show == SHOW_SHORT)
        return "Short (3 min or less)";
    if (show == SHOW_LONG)
        return "Long (over 6 min)";
    if (show >= SHOW_PART) {
        snprintf(t, sizeof(t), "Has %s", part_name(show - SHOW_PART));
        return t;
    }
    return "All songs";
}

static void change_sort(int d)
{
    Song * cur = songs_get(sel);
    char keep[256];
    snprintf(keep, sizeof(keep), "%s", cur ? cur->folder : "");
    cfg.sort = (cfg.sort + d + SORT_COUNT) % SORT_COUNT;
    songs_sort(cfg.sort);
    int k = songs_find(keep);
    sel = k >= 0 ? k : 0;
    art_for = preview_for = -1;
    view_stale = true;
    config_save();
}

// the next song under another heading
static void jump_group(int dir)
{
    char from[64], g[64];
    songs_group(songs_get(view[pos]), cfg.sort, from, sizeof(from));
    int p = pos;
    while (p + dir >= 0 && p + dir < view_n) {
        p += dir;
        songs_group(songs_get(view[p]), cfg.sort, g, sizeof(g));
        if (strcmp(g, from) == 0)
            continue;
        // going back: to the top of the heading before
        while (dir < 0 && p > 0) {
            char up[64];
            songs_group(songs_get(view[p - 1]), cfg.sort, up, sizeof(up));
            if (strcmp(up, g) != 0)
                break;
            p--;
        }
        break;
    }
    pos = p;
    group_t = now_s;
}

// Select: what to sort by, what to show, this song
static void scan_progress(int found, const char * converting);

// songs played one after another, each with its results, like a set at a gig
enum { SETLIST_MAX = 20 };
static int setlist[SETLIST_MAX], setlist_n, setlist_at = -1;

static int setlist_find(int song)
{
    for (int i = 0; i < setlist_n; i++)
        if (setlist[i] == song)
            return i;
    return -1;
}

static void list_menu_update(void)
{
    enum { ROWS = 8 };
    Song * s = songs_get(sel);
    if (repeat & BTN_UP)
        list_row = (list_row + ROWS - 1) % ROWS;
    if (repeat & BTN_DOWN)
        list_row = (list_row + 1) % ROWS;
    int d = repeat & BTN_LEFT ? -1 : repeat & BTN_RIGHT ? 1 : 0;
    if (repeat & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT))
        ui_sound(SND_UI_MOVE);
    if (d && list_row == 0)
        change_sort(d);
    if (d && list_row == 1) {
        show = (show + d + SHOW_PART + PART_COUNT) % (SHOW_PART + PART_COUNT);
        view_stale = true;
    }
    if (pressed & BTN_CROSS) {
        ui_sound(SND_UI_SELECT);
        if (list_row == 0)
            change_sort(1);
        else if (list_row == 1) {
            show = (show + 1) % (SHOW_PART + PART_COUNT);
            view_stale = true;
        } else if (list_row == 2 && s) {
            songs_set_fav(s, !s->fav);
            if (show == SHOW_FAVS)
                view_stale = true;
        } else if (list_row == 3 && s) {
            del_ask = true;
            setlist_n = 0;              // songs are by number: a gone one moves the rest
            list_menu = false;
        } else if (list_row == 4) {
            // songs changed by hand on a PC can keep their folder's time: this reads them all
            stop_preview();
            art_for = preview_for = -1;
            sel = pos = top = 0;
            songs_forget();
            songs_scan(scan_progress);
            view_stale = true;
            setlist_n = 0;
            list_menu = false;
        } else if (list_row == 5 && s) {
            int at = setlist_find(sel);
            if (at >= 0) {
                memmove(setlist + at, setlist + at + 1, (setlist_n - at - 1) * sizeof(setlist[0]));
                setlist_n--;
            } else if (setlist_n < SETLIST_MAX)
                setlist[setlist_n++] = sel;
        } else if (list_row == 6 && setlist_n) {
            setlist_at = 0;
            sel = setlist[0];
            list_menu = false;
            stop_preview();
            open_select(songs_get(sel));
            pressed = repeat = 0;
            return;
        } else if (list_row == 7)
            setlist_n = 0;
    }
    if (pressed & (BTN_CIRCLE | BTN_SELECT)) {
        ui_sound(SND_UI_BACK);
        list_menu = false;
    }
    pressed = repeat = 0;
}

static void list_menu_draw(const Song * s)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 170));
    panel(200, 84, 560, 384);
    char rows[8][64];
    snprintf(rows[0], sizeof(rows[0]), "Sort by   %s", sort_name(cfg.sort));
    snprintf(rows[1], sizeof(rows[1]), "Show   %s", show_name());
    snprintf(rows[2], sizeof(rows[2]), "%s", s && s->fav ? "Take out of favourites" : "Add to favourites");
    snprintf(rows[3], sizeof(rows[3]), "Delete this song");
    snprintf(rows[4], sizeof(rows[4]), "Read the whole card again");
    snprintf(rows[5], sizeof(rows[5]), "%s", setlist_find(sel) >= 0 ? "Take out of the setlist" : "Add to the setlist");
    snprintf(rows[6], sizeof(rows[6]), "Play the setlist (%d %s)", setlist_n, setlist_n == 1 ? "song" : "songs");
    snprintf(rows[7], sizeof(rows[7]), "Clear the setlist");
    for (int i = 0; i < 8; i++) {
        float y = 102 + i * 44;
        if (i == list_row)
            gfx_sprite(atlas_get(res.hud, "wor_select"), 224, y - 4, 512, 42, WHITE);
        font_draw_fit(res.ui, rows[i], SCREEN_W / 2, y + 2, 28, 460, i == list_row ? RGBA(255, 230, 150, 255)
                                                                                   : RGBA(220, 215, 205, 255), ALIGN_CENTER);
    }
}

static void song_list(void)
{
    if (view_stale)
        view_build();
    int n = view_n;
    const int rows = 7;
    if (dl_typing) {
        // searching the library: the same keyboard as the downloader's
        kb_update();
        pressed = repeat = 0;
        if (view_stale)
            view_build();
        n = view_n;
    }
    if (list_menu) {
        list_menu_update();
        if (screen != S_SONGS)
            return;             // the setlist's started: this list has no say in the song now
    }
    if (view_stale)
        view_build();
    n = view_n;
    if (del_ask) {
        if (pressed & BTN_CROSS) {
            stop_preview();
            art_for = -1;
            songs_delete(sel);
            sel = sel >= songs_count() ? songs_count() - 1 : sel;
            sel = sel < 0 ? 0 : sel;
            view_stale = true;
            del_ask = false;
            ui_sound(SND_UI_SELECT);
        } else if (pressed & BTN_CIRCLE)
            del_ask = false;
        pressed = repeat = 0;
        if (view_stale)
            view_build();
        n = view_n;
    }
    if (pressed & BTN_SELECT) {
        list_menu = true;
        list_row = 0;
        pressed = repeat = 0;
        ui_sound(SND_UI_SELECT);
    }
    if (pressed & BTN_TRIANGLE) {
        snprintf(dl_typed, sizeof(dl_typed), "%s", find_text);
        dl_typing = true;
        kb_library = true;
        pressed = repeat = 0;
        ui_sound(SND_UI_SELECT);
    }
    int old = pos;
    if (n) {
        if (repeat & BTN_UP)
            pos--;
        if (repeat & BTN_DOWN)
            pos++;
        if (repeat & BTN_LEFT)
            pos -= rows;
        if (repeat & BTN_RIGHT)
            pos += rows;
        if (repeat & (BTN_L | BTN_R))
            jump_group(repeat & BTN_L ? -1 : 1);
        if (pressed & BTN_SQUARE)
            pos = rand() % n;
        if (pos < 0)
            pos = (repeat & BTN_UP) && old == 0 ? n - 1 : 0;
        if (pos >= n)
            pos = (repeat & BTN_DOWN) && old == n - 1 ? 0 : n - 1;
        sel = view[pos];
    }
    if (pos != old) {
        ui_sound(SND_UI_MOVE);
        sel_t = now_s;
    }
    if (pos < top)
        top = pos;
    if (pos >= top + rows)
        top = pos - rows + 1;
    if (top > 0 && top + rows > n)
        top = n > rows ? n - rows : 0;
    Song * s = n ? songs_get(sel) : NULL;
    if (pressed & BTN_CIRCLE) {
        // back out of a search first
        ui_sound(SND_UI_BACK);
        if (find_text[0]) {
            find_text[0] = 0;
            view_stale = true;
            return;
        }
        stop_preview();
        go(S_MAIN);
        return;
    }
    if (pressed & BTN_START) {
        options_from = S_SONGS;
        stop_preview();
        go(S_OPTIONS);
        return;
    }
    if ((pressed & BTN_CROSS) && s) {
        ui_sound(SND_UI_SELECT);
        open_select(s);
        return;
    }
    // the cover once the choice settles, then the preview
    if (s && art_for != sel && now_s - sel_t > 0.12) {
        load_art(s);
    }
    if (s && preview_for != sel && now_s - sel_t > 0.5) {
        stop_preview();
        const char * stems[MAX_STEMS];
        for (int i = 0; i < s->stem_count; i++)
            stems[i] = s->stems[i];
        audio_preview(s->folder, stems, s->stem_count, s->preview_ms / 1000.0, 0.8f, 1.0f);
        preview_for = sel;
    }
    if (preview_for == sel && s && audio_song_time() > s->preview_ms / 1000.0 + 28)
        audio_song_fade(0, 2.0f);

    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, WHITE);
    banner("QUICKPLAY");
    char t[160];
    if (find_text[0])
        snprintf(t, sizeof(t), "\"%s\"   %d of %d", find_text, n, songs_count());
    else if (show != SHOW_ALL)
        snprintf(t, sizeof(t), "%s   %d of %d", show_name(), n ? pos + 1 : 0, n);
    else
        snprintf(t, sizeof(t), "%d / %d   by %s", n ? pos + 1 : 0, n, sort_name(cfg.sort));
    font_draw_fit(res.ui, t, SCREEN_W - 24, 30, 24, 420, RGBA(230, 230, 230, 255), ALIGN_RIGHT);
    // the list
    for (int r = 0; r < rows && top + r < n; r++) {
        int i = view[top + r];
        Song * x = songs_get(i);
        float y = 104 + r * 56;
        bool on = top + r == pos;
        gfx_sprite(atlas_get(res.hud, on ? "wor_select" : "wor_row"), 14, y, 580, 54, WHITE);
        char up[128];
        upper(up, sizeof(up), x->name);
        font_draw_fit(res.ui, up, 40, y + 4, 26, 520, on ? RGBA(255, 236, 170, 255) : RGBA(235, 235, 235, 255),
                      ALIGN_LEFT);
        font_draw_fit(res.ui, x->artist, 42, y + 28, 20, 410, on ? RGBA(255, 210, 140, 255) : RGBA(170, 170, 175, 255),
                      ALIGN_LEFT);
        const Score * b = &x->best[first_part(x)][cfg.last_diff];
        if (b->stars)
            draw_stars(b->stars, 470, y + 30, 18);
        if (x->fav)
            gfx_sprite(atlas_get(res.hud, "hud_star_gold"), 20, y + 16, 20, 20, WHITE);
    }
    if (!n) {
        font_draw_shadow(res.ui, find_text[0] ? "No song matches that" : "No songs here", 304, 240, 30,
                         RGBA(240, 235, 225, 255), ALIGN_CENTER);
        font_draw(res.ui, find_text[0] ? "Circle clears the search" : "Select changes what's shown", 304, 282, 22,
                  RGBA(190, 190, 190, 255), ALIGN_CENTER);
    }
    // the heading a jump landed on
    if (s && now_s - group_t < 0.9) {
        char g[64];
        songs_group(s, cfg.sort, g, sizeof(g));
        float a = now_s - group_t < 0.6 ? 1 : (float)((0.9 - (now_s - group_t)) / 0.3);
        if (g[0]) {
            gfx_rect(160, 236, 300, 84, fade_alpha(RGBA(0, 0, 0, 215), a));
            font_draw_fit(res.title, g, 310, 250, 56, 280, fade_alpha(RGBA(255, 225, 150, 255), a), ALIGN_CENTER);
        }
    }
    // the details
    if (s) {
        panel(608, 96, 340, 396);
        draw_cover(art_for == sel ? art : NULL, 698, 112, 160);
        float y = 280;
        font_draw_fit(res.ui, s->artist, 630, y, 24, 300, RGBA(255, 220, 160, 255), ALIGN_LEFT);
        y += 26;
        if (s->album[0]) {
            if (s->year[0])
                snprintf(t, sizeof(t), "%s (%s)", s->album, s->year);
            else
                snprintf(t, sizeof(t), "%s", s->album);
            font_draw_fit(res.ui, t, 630, y, 20, 300, RGBA(220, 220, 220, 255), ALIGN_LEFT);
            y += 22;
        }
        int secs = s->length_ms / 1000;
        snprintf(t, sizeof(t), "%d:%02d  %s", secs / 60, secs % 60, s->genre);
        font_draw_fit(res.ui, t, 630, y, 20, 300, RGBA(200, 200, 200, 255), ALIGN_LEFT);
        y += 22;
        if (s->charter[0]) {
            snprintf(t, sizeof(t), "Charted by %s", s->charter);
            font_draw_fit(res.ui, t, 630, y, 18, 300, RGBA(170, 170, 170, 255), ALIGN_LEFT);
            y += 22;
        }
        // the parts it has, with the chart's difficulty tier as bars
        int8_t tier[PART_COUNT];
        for (int p = 0; p < PART_COUNT; p++)
            tier[p] = s->parts & (1 << p) ? (s->diff[p] < 0 ? 0 : s->diff[p]) : -1;
        draw_parts(tier, 630, y + 4);
        const Score * b = &s->best[first_part(s)][cfg.last_diff];
        if (b->score) {
            snprintf(t, sizeof(t), "Best %d   %d%%%s", b->score, b->percent, b->fc ? "   FC" : "");
            font_draw(res.ui, t, 630, 456, 20, RGBA(255, 230, 160, 255), ALIGN_LEFT);
        }
    }
    static const char * help[] = {"btn_cross", "Play", "btn_circle", "Back", "btn_triangle", "Search", "btn_square",
                                  "Random", "btn_l", "Jump", "btn_select", "Menu", "btn_start", "Options"};
    help_bar(help, 14);
    if (list_menu)
        list_menu_draw(s);
    if (del_ask && s) {
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 200));
        font_draw(res.ui, "Delete this song from the card?", SCREEN_W / 2, 200, 34, WHITE, ALIGN_CENTER);
        font_draw_fit(res.ui, s->name, SCREEN_W / 2, 250, 30, 800, RGBA(255, 220, 140, 255), ALIGN_CENTER);
        font_draw(res.ui, "Cross delete   Circle keep", SCREEN_W / 2, 310, 26, RGBA(220, 220, 230, 255), ALIGN_CENTER);
    }
    if (dl_typing)
        kb_draw();
    gfx_end();
}

// -------------------------------------------------------------------------------------------- part and difficulty

static bool select_starting;        // ✕ was pressed: that frame says LOADING, the next one loads the song
static bool tour_on;                // the song comes from the tour: it counts there, and that's where it goes back to

static int from_list(void)
{
    return tour_on ? S_TOUR : S_SONGS;
}

static int tour_leg, tour_gig, tour_slot;

static void draw_select(const Song * s, bool loading)
{
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(150, 150, 150, 255));
    banner("GET READY");
    panel(180, 110, 600, 360);
    font_draw_fit(res.title, s->name, SCREEN_W / 2, 126, 44, 560, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    font_draw_fit(res.ui, s->artist, SCREEN_W / 2, 176, 26, 560, RGBA(230, 230, 230, 255), ALIGN_CENTER);
    char t[64];
    snprintf(t, sizeof(t), "<  %s  >", part_name(sel_part));
    font_draw_shadow(res.title, t, SCREEN_W / 2, 214, 44, RGBA(255, 255, 255, 255), ALIGN_CENTER);
    for (int d = 0; d < DIFF_COUNT; d++) {
        float y = 274 + d * 46;
        bool on = d == sel_diff, has = sel_diffs[sel_part] & (1 << d);
        if (on)
            gfx_sprite(atlas_get(res.hud, "wor_select"), 290, y - 4, 380, 44, WHITE);
        font_draw_shadow(res.ui, diff_name(d), 330, y, 32,
                         on ? RGBA(255, 230, 150, 255) : has ? RGBA(190, 190, 190, 255) : RGBA(85, 80, 75, 255),
                         ALIGN_LEFT);
        if (!has)
            font_draw(res.ui, "not charted", 520, y + 6, 22, RGBA(85, 80, 75, 255), ALIGN_LEFT);
        const Score * b = &s->best[sel_part][d];
        if (b->stars)
            draw_stars(b->stars, 520, y + 4, 24);
    }
    if (tour_on) {
        char t2[80];
        snprintf(t2, sizeof(t2), "%s%s", tour_slot == TOUR_ENCORE ? "Encore at " : "Live at ",
                 res_venue_title(tour_zone(tour_leg, tour_gig)));
        font_draw_fit(res.ui, t2, SCREEN_W / 2, 84, 24, 560, RGBA(255, 200, 120, 255), ALIGN_CENTER);
    }
    if (loading)
        font_draw_shadow(res.title, "LOADING", SCREEN_W / 2, 488, 36, RGBA(255, 220, 120, 255), ALIGN_CENTER);
    else {
        static const char * help[] = {"btn_cross", "Rock!", "btn_circle", "Back", "btn_left", "Part", "btn_up",
                                      "Difficulty", "btn_square", "Practice"};
        help_bar(help, 10);
    }
    gfx_end();
}

// ------------------------------------------------------------------------------------------------------- practice

#define PR_MAX 160
#define PR_ROWS 7

// the song's sections as the chart names them, or every eight bars; one more just ends the last
static struct {
    double t;
    char name[44];
    int notes;
} pr[PR_MAX + 1];
static int pr_count, pr_from, pr_to, pr_top, pr_focus, pr_speed = 100;
// the menu goes a step at a time: where to start, where to end, then how fast
enum { PR_STEP_FROM, PR_STEP_TO, PR_STEP_SET };
static int pr_step, pr_set_row, pr_ramp, pr_target = 100;
static char pr_song[sizeof(((Song *)0)->folder)];
static int pr_song_part = -1, pr_song_diff = -1, pr_song_count;
static bool practicing, practice_starting;

// "verse_1" reads "Verse 1"
static void section_title(char * name)
{
    for (char * c = name; *c; c++)
        if (*c == '_')
            *c = ' ';
    if (name[0] >= 'a' && name[0] <= 'z')
        name[0] -= 32;
}

static void pr_add(double t, const char * name)
{
    if (pr_count == PR_MAX)
        return;
    pr[pr_count].t = t;
    pr[pr_count].notes = 0;
    // a charting tool's "ugc_section_20_40" says nothing
    if (!strncasecmp(name, "ugc", 3))
        snprintf(pr[pr_count].name, sizeof(pr[0].name), "Section %d", pr_count + 1);
    else
        snprintf(pr[pr_count].name, sizeof(pr[0].name), "%s", name);
    section_title(pr[pr_count].name);
    pr_count++;
}

static void read_sections(const Song * s)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/notes.vhc", s->folder);
    int saved_from = pr_from, saved_to = pr_to;
    pr_count = pr_from = pr_to = pr_top = pr_focus = 0;
    Chart * c = chart_load(path);
    if (!c)
        return;
    const Part * p = sel_part == PART_VOCALS ? NULL : chart_part(c, sel_part, sel_diff);
    double end = s->length_ms / 1000.0;
    if (p && p->note_count) {
        const Note * n = &p->notes[p->note_count - 1];
        if ((n->time + (double)n->len) / 1e6 + 1 < end || end <= 0)
            end = (n->time + (double)n->len) / 1e6 + 1;
    }
    if (!c->section_count || c->sections[0].time > 500000)
        pr_add(0, c->section_count ? "Start" : "Bar 1");
    for (int i = 0; i < c->section_count; i++)
        if (c->sections[i].time / 1e6 < end)
            pr_add(c->sections[i].time / 1e6, c->sections[i].name);
    if (!c->section_count)
        for (int i = 0, bar = 0; i < c->beat_count; i++)
            if (c->beats[i].kind == 0 && ++bar % 8 == 1 && bar > 1 && c->beats[i].time / 1e6 < end) {
                char t[16];
                snprintf(t, sizeof(t), "Bar %d", bar);
                pr_add(c->beats[i].time / 1e6, t);
            }
    pr[pr_count].t = end;
    for (int i = 0, k = 0; p && i < p->note_count; i++) {
        while (k + 1 < pr_count && p->notes[i].time / 1e6 >= pr[k + 1].t)
            k++;
        pr[k].notes++;
    }
    pr_to = pr_count - 1;
    chart_free(c);
    // the same song, part and difficulty as last time: the same stretch, picked already
    if (!strcmp(pr_song, s->folder) && pr_song_part == sel_part && pr_song_diff == sel_diff && pr_song_count == pr_count) {
        pr_from = saved_from, pr_to = saved_to;
        pr_step = PR_STEP_SET;
    } else
        pr_step = PR_STEP_FROM;
    pr_focus = pr_step == PR_STEP_SET ? pr_from : 0;
    pr_set_row = 0;
    snprintf(pr_song, sizeof(pr_song), "%s", s->folder);
    pr_song_part = sel_part, pr_song_diff = sel_diff, pr_song_count = pr_count;
}

static void practice_screen(void)
{
    Song * s = songs_get(sel);
    if (!s || !pr_count) {
        go(S_SELECT);
        return;
    }
    if (practice_starting) {
        practice_starting = false;
        game_practice(pr[pr_from].t, pr[pr_to + 1].t, pr_speed / 100.0f);
        game_practice_ramp(pr_ramp, pr_target);
        if (game_start(s, sel_part, sel_diff)) {
            practicing = true;
            game_set_autoplay(autoplay);
            go(S_PLAY);
            return;
        }
        game_stop();
        plat_log("can't practice %s", s->folder);
    }
    bool moved = false, start = false;
    if (pr_step != PR_STEP_SET) {
        // a cursor over the sections; the end can't come before the start
        int lo = pr_step == PR_STEP_TO ? pr_from : 0, was = pr_focus;
        if (repeat & BTN_UP)
            pr_focus--;
        if (repeat & BTN_DOWN)
            pr_focus++;
        if (repeat & BTN_L)
            pr_focus -= PR_ROWS;
        if (repeat & BTN_R)
            pr_focus += PR_ROWS;
        pr_focus = pr_focus < lo ? lo : pr_focus >= pr_count ? pr_count - 1 : pr_focus;
        moved = pr_focus != was;
        if (pr_step == PR_STEP_FROM)
            pr_from = pr_focus, pr_to = pr_to < pr_from ? pr_from : pr_to;
        else
            pr_to = pr_focus;
        if (pressed & BTN_CROSS) {
            ui_sound(SND_UI_SELECT);
            if (pr_step == PR_STEP_FROM) {
                pr_step = PR_STEP_TO;
                pr_focus = pr_to = pr_from;
            } else {
                pr_step = PR_STEP_SET;
                pr_set_row = 0;
            }
        } else if (pressed & BTN_TRIANGLE) {
            // the whole song in one press
            ui_sound(SND_UI_SELECT);
            pr_from = 0, pr_to = pr_count - 1, pr_focus = 0;
            pr_step = PR_STEP_SET;
            pr_set_row = 0;
        } else if (pressed & BTN_CIRCLE) {
            ui_sound(SND_UI_BACK);
            if (pr_step == PR_STEP_TO) {
                pr_step = PR_STEP_FROM;
                pr_focus = pr_from;
            } else {
                go(S_SELECT);
                return;
            }
        }
    } else {
        // Start, Speed, Speed up, Up to (only while speeding up)
        int rows = pr_ramp ? 4 : 3, was_row = pr_set_row, sp = pr_speed, ra = pr_ramp, ta = pr_target;
        if (repeat & BTN_UP)
            pr_set_row = (pr_set_row + rows - 1) % rows;
        if (repeat & BTN_DOWN)
            pr_set_row = (pr_set_row + 1) % rows;
        int dir = (repeat & BTN_LEFT) ? -1 : (repeat & BTN_RIGHT) ? 1 : 0;
        if (pr_set_row == 1)
            pr_speed += 5 * dir;
        else if (pr_set_row == 2)
            pr_ramp = (pr_ramp / 5 + dir + 3) % 3 * 5;     // off, +5%, +10%
        else if (pr_set_row == 3)
            pr_target += 5 * dir;
        pr_speed = pr_speed < 50 ? 50 : pr_speed > 150 ? 150 : pr_speed;
        pr_target = pr_target < 55 ? 55 : pr_target > 150 ? 150 : pr_target;
        if (pr_ramp && pr_target <= pr_speed)
            pr_target = pr_speed + 5 > 150 ? 150 : pr_speed + 5;
        if (!pr_ramp && pr_set_row > 2)
            pr_set_row = 2;
        moved = pr_set_row != was_row || sp != pr_speed || ra != pr_ramp || ta != pr_target;
        if (pressed & BTN_CROSS) {
            if (pr_set_row == 0) {
                start = true;
            } else {
                pr_set_row = 0;
                ui_sound(SND_UI_MOVE);
            }
        } else if (pressed & BTN_START)
            start = true;
        else if (pressed & BTN_CIRCLE) {
            ui_sound(SND_UI_BACK);
            pr_step = PR_STEP_TO;
            pr_focus = pr_to;
        }
    }
    if (moved)
        ui_sound(SND_UI_MOVE);
    if (start) {
        stop_preview();
        ui_sound(SND_UI_SELECT);
        practice_starting = true;
    }
    if (pr_focus < pr_top)
        pr_top = pr_focus;
    if (pr_focus >= pr_top + PR_ROWS)
        pr_top = pr_focus - PR_ROWS + 1;
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(150, 150, 150, 255));
    banner("PRACTICE");
    char t[120];
    static const char * step_name[] = {"Pick where to start", "Pick where to end", "Set the speed"};
    font_draw(res.ui, step_name[pr_step], 40, 76, 22, RGBA(255, 225, 150, 255), ALIGN_LEFT);
    font_draw(res.ui, "Starts", 490, 80, 18, RGBA(170, 170, 175, 255), ALIGN_RIGHT);
    if (sel_part != PART_VOCALS)
        font_draw(res.ui, "Notes", 572, 80, 18, RGBA(170, 170, 175, 255), ALIGN_RIGHT);
    for (int r = 0; r < PR_ROWS && pr_top + r < pr_count; r++) {
        int i = pr_top + r;
        float y = 104 + r * 56;
        bool on = i >= pr_from && i <= pr_to, cur = pr_step != PR_STEP_SET && i == pr_focus;
        bool dim = pr_step == PR_STEP_TO && i < pr_from;
        gfx_sprite(atlas_get(res.hud, on ? "wor_select" : "wor_row"), 14, y, 580, 54, dim ? RGBA(255, 255, 255, 110) : WHITE);
        if (cur)
            font_draw_shadow(res.title, ">", 24, y + 9, 30, RGBA(255, 236, 170, 255), ALIGN_LEFT);
        font_draw_fit(res.ui, pr[i].name, 44, y + 11, 28, 376, cur ? RGBA(255, 236, 170, 255) : on ? RGBA(240, 225, 190, 255)
                                                                                         : RGBA(190, 190, 195, dim ? 120 : 255),
                      ALIGN_LEFT);
        int secs = (int)pr[i].t;
        snprintf(t, sizeof(t), "%d:%02d", secs / 60, secs % 60);
        font_draw(res.ui, t, 490, y + 15, 22, RGBA(200, 200, 200, 255), ALIGN_RIGHT);
        if (sel_part != PART_VOCALS) {
            snprintf(t, sizeof(t), "%d", pr[i].notes);
            font_draw(res.ui, t, 572, y + 15, 22, RGBA(255, 200, 110, 255), ALIGN_RIGHT);
        }
    }
    if (pr_top > 0)
        font_draw(res.ui, "more above", 304, 92, 16, RGBA(170, 170, 175, 255), ALIGN_CENTER);
    if (pr_top + PR_ROWS < pr_count)
        font_draw(res.ui, "more below", 304, 104 + PR_ROWS * 56 - 4, 16, RGBA(170, 170, 175, 255), ALIGN_CENTER);
    panel(608, 96, 340, 396);
    font_draw_fit(res.title, s->name, 778, 112, 34, 300, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    snprintf(t, sizeof(t), "%s  %s", part_name(sel_part), diff_name(sel_diff));
    font_draw_fit(res.ui, t, 778, 152, 22, 300, RGBA(230, 230, 230, 255), ALIGN_CENTER);
    // the stretch: from, to, how long, how many notes
    int notes = 0;
    for (int i = pr_from; i <= pr_to; i++)
        notes += pr[i].notes;
    int len = (int)(pr[pr_to + 1].t - pr[pr_from].t + 0.5);
    snprintf(t, sizeof(t), "%s", pr[pr_from].name);
    font_draw(res.ui, "From", 636, 188, 18, RGBA(180, 180, 180, 255), ALIGN_LEFT);
    font_draw_fit(res.ui, t, 636, 206, 24, 290, RGBA(255, 236, 190, 255), ALIGN_LEFT);
    font_draw(res.ui, "To the end of", 636, 238, 18, RGBA(180, 180, 180, 255), ALIGN_LEFT);
    font_draw_fit(res.ui, pr[pr_to].name, 636, 256, 24, 290, RGBA(255, 236, 190, 255), ALIGN_LEFT);
    if (sel_part != PART_VOCALS)
        snprintf(t, sizeof(t), "%d:%02d long, %d notes", len / 60, len % 60, notes);
    else
        snprintf(t, sizeof(t), "%d:%02d long", len / 60, len % 60);
    font_draw(res.ui, t, 636, 288, 18, RGBA(200, 200, 200, 255), ALIGN_LEFT);
    // the settings: live in the last step, a summary before it
    const char * labels[] = {"START", "Speed", "Speed up", "Up to"};
    char vals[4][24] = {""};
    snprintf(vals[1], sizeof(vals[1]), "< %d%% >", pr_speed);
    snprintf(vals[2], sizeof(vals[2]), pr_ramp ? "< +%d%% a clear >" : "< Off >", pr_ramp);
    snprintf(vals[3], sizeof(vals[3]), "< %d%% >", pr_target);
    int rows = pr_ramp ? 4 : 3;
    for (int r = 0; r < rows; r++) {
        float y = 318 + r * 40;
        bool cur = pr_step == PR_STEP_SET && r == pr_set_row;
        if (cur)
            gfx_sprite(atlas_get(res.hud, "wor_select"), 622, y - 4, 312, 38, WHITE);
        uint32_t col = pr_step != PR_STEP_SET ? RGBA(150, 150, 150, 255) : cur ? RGBA(255, 236, 170, 255) : RGBA(220, 220, 220, 255);
        if (r == 0)
            font_draw_shadow(res.title, labels[0], 778, y - 2, 32, col, ALIGN_CENTER);
        else {
            font_draw(res.ui, labels[r], 636, y + 4, 20, col, ALIGN_LEFT);
            font_draw(res.ui, vals[r], 924, y + 4, 20, col, ALIGN_RIGHT);
        }
    }
    if (start)
        font_draw_shadow(res.title, "LOADING", SCREEN_W / 2, 488, 36, RGBA(255, 220, 120, 255), ALIGN_CENTER);
    else if (pr_step == PR_STEP_FROM) {
        static const char * help[] = {"btn_cross", "Start here", "btn_triangle", "Whole song", "btn_up", "Move", "btn_circle", "Back"};
        help_bar(help, 8);
    } else if (pr_step == PR_STEP_TO) {
        static const char * help[] = {"btn_cross", "End here", "btn_up", "Move", "btn_circle", "Change start"};
        help_bar(help, 6);
    } else {
        static const char * help[] = {"btn_cross", "Choose", "btn_left", "Change", "btn_start", "Start", "btn_circle", "Change end"};
        help_bar(help, 8);
    }
    gfx_end();
}

static void select_screen(void)
{
    Song * s = songs_get(sel);
    if (!s) {
        go(S_SONGS);
        return;
    }
    if (select_starting) {
        // the chart, the highway picture and the stems take the Vita a second or so: the screen says so meanwhile
        select_starting = false;
        if (game_start(s, sel_part, sel_diff)) {
            practicing = false;
            game_set_autoplay(autoplay);
            go(S_PLAY);
            draw_select(s, true);
            return;
        }
        plat_log("can't start %s", s->folder);
    }
    if (repeat & (BTN_LEFT | BTN_RIGHT)) {
        int dir = repeat & BTN_LEFT ? -1 : 1, was = sel_part;
        for (int k = 0; k < PART_COUNT; k++) {
            sel_part = (sel_part + dir + PART_COUNT) % PART_COUNT;
            if (sel_diffs[sel_part])
                break;
        }
        sel_diff = nearest_diff(sel_part, want_diff);
        if (sel_part != was)
            ui_sound(SND_UI_MOVE);
    }
    if (repeat & (BTN_UP | BTN_DOWN)) {
        int dir = repeat & BTN_UP ? -1 : 1, was = sel_diff;
        for (int k = 0; k < DIFF_COUNT; k++) {
            sel_diff = (sel_diff + dir + DIFF_COUNT) % DIFF_COUNT;
            if (sel_diffs[sel_part] & (1 << sel_diff))
                break;
        }
        want_diff = sel_diff;
        if (sel_diff != was)
            ui_sound(SND_UI_MOVE);
    }
    if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        go(from_list());
        return;
    }
    if (pressed & BTN_SQUARE) {
        read_sections(s);
        ui_sound(pr_count ? SND_UI_SELECT : SND_UI_BACK);
        if (pr_count) {
            go(S_PRACTICE);
            return;
        }
    }
    if (pressed & BTN_CROSS) {
        stop_preview();
        cfg.last_part = sel_part;
        cfg.last_diff = sel_diff;
        snprintf(cfg.last_song, sizeof(cfg.last_song), "%s", s->folder);
        config_save();
        ui_sound(SND_UI_SELECT);
        select_starting = true;
    }
    draw_select(s, select_starting);
}

// --------------------------------------------------------------------------------------------- the song and pause

static void results_screen(void);
static void tour_played(void);
static void tour_next(void);

static void finish_song(int how)
{
    result = game_result();
    section_title(result.worst);
    if (tour_on && !result.failed)
        tour_played();
    plat_log("result: %s, score %d, %d stars, %d/%d notes, best streak %d, %d/%d phrases, %d overhits%s; hits %+d ms "
             "on average, spread %d", how == GAME_DONE ? "done" : "failed", result.score, result.stars,
             result.notes_hit, result.notes_total, result.best_streak, result.sp_hit, result.sp_total, result.overhits,
             result.fc ? ", full combo" : "", result.off_ms, result.spread_ms);
    game_log_whammy();
    Song * s = songs_get(sel);
    result_best = false;
    if (s && !result.failed) {
        Score sc = {result.score, (uint8_t)result.stars,
                    (uint8_t)(result.notes_total ? result.notes_hit * 100 / result.notes_total : 0), result.fc};
        result_best = result.score > s->best[result.part][result.diff].score;
        if (!autoplay)
            songs_save_score(s, result.part, result.diff, sc);
    }
    result_daily = s && !result.failed && !autoplay && !practicing && daily_finished(s);
    hist_n = s && !practicing ? songs_history(s, result.part, result.diff, hist, HIST_MAX) : 0;
    hist_has_this = s && !result.failed && !autoplay;
    results_history = false;
    if (!autoplay && !practicing)
        career_add(result.part, result.score, result.stars, result.notes_hit, result.notes_total, result.best_streak,
                   result.overhits, result.played_s, result.failed, result.fc);
    AchPlay ap = {result.part, result.diff, result.stars, result.score, result.best_streak, result.overhits, result.sp_hit,
                  result.sp_total, result.notes_total ? result.notes_hit * 100 / result.notes_total : 0, result.failed,
                  result.fc, practicing, autoplay, daily_streak(), songs_count()};
    result_ach_n = achieve_check(&ap, result_ach, 6);
    for (int i = 0; i < result_ach_n && i < 6; i++)
        plat_log("achievement: %s", achieve_get(result_ach[i])->name);
    game_stop();
    if (how == GAME_DONE) {
        sound_play(res.snd[SND_CHEER1 + rand() % 3], cfg.crowd_vol * 0.1f, false);
        sound_play(res.snd[SND_APPLAUSE], cfg.crowd_vol * 0.08f, false);
        sound_play(res.snd[result.stars >= 6 ? SND_STAR_GOLD : SND_STAR], 0.7f, false);
    }
    go(S_RESULTS);
}

static void play_screen(void)
{
    // the audio falling behind: full clocks
    static int underruns;
    int u = audio_underruns();
    if (u != underruns) {
        underruns = u;
        plat_perf_trouble();
    }
    // back from the PS button or sleep (a long gap between frames), or the quick menu came up: wait in the pause menu
    if (screen_frames >= 2 && (gap_s > 1.0 || overlay_on)) {
        game_pause(true);
        go(S_PAUSE);
        game_draw();
        return;
    }
    int r = game_update(&pad, pressed);
    if (r == GAME_PAUSE) {
        game_pause(true);
        go(S_PAUSE);
    } else if (r == GAME_DONE || r == GAME_FAILED) {
        finish_song(r);
        pressed = repeat = 0;
        results_screen();
        return;
    }
    game_draw();
}

static void pause_screen(void)
{
    const char * items[] = {"RESUME", "RESTART", "OPTIONS", practicing ? "PRACTICE MENU" : "QUIT"};
    const int n = 4;
    if (repeat & BTN_UP) {
        menu_index = (menu_index + n - 1) % n;
        ui_sound(SND_UI_MOVE);
    }
    if (repeat & BTN_DOWN) {
        menu_index = (menu_index + 1) % n;
        ui_sound(SND_UI_MOVE);
    }
    bool resume = pressed & (BTN_START | BTN_CIRCLE);
    if (pressed & BTN_CROSS) {
        ui_sound(SND_UI_SELECT);
        if (menu_index == 0)
            resume = true;
        else if (menu_index == 1) {
            game_pause(false);
            game_restart();
            go(S_PLAY);
            return;
        } else if (menu_index == 2) {
            options_from = S_PAUSE;
            go(S_OPTIONS);
            return;
        } else {
            game_stop();
            setlist_at = -1;
            go(practicing ? S_PRACTICE : from_list());
            practicing = false;
            sel_t = now_s;
            return;
        }
    }
    if (resume) {
        if (cfg.no_countdown) {
            game_pause(false);
            go(S_PLAY);
        } else
            go(S_RESUME);
        return;
    }
    game_draw();
    gfx_2d();
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 150));
    // GHWT's stitched patch with its emblem
    gfx_sprite(atlas_get(res.hud, "dialog_bg"), SCREEN_W / 2 - 210, 40, 420, 420, WHITE);
    font_draw_shadow(res.title, "PAUSED", SCREEN_W / 2, 138, 46, RGBA(255, 225, 160, 255), ALIGN_CENTER);
    for (int i = 0; i < n; i++) {
        float y = 200 + i * 54;
        if (i == menu_index)
            gfx_sprite(atlas_get(res.hud, "wor_select"), SCREEN_W / 2 - 160, y - 4, 320, 48, WHITE);
        font_draw_shadow(res.title, items[i], SCREEN_W / 2, y, 40, i == menu_index ? RGBA(255, 230, 150, 255)
                                                                                    : RGBA(210, 200, 190, 255),
                         ALIGN_CENTER);
    }
    int song_pct, notes_pct;
    game_status(&song_pct, &notes_pct);
    char line[64];
    if (notes_pct >= 0)
        snprintf(line, sizeof(line), "%d%% through   %d%% of notes", song_pct, notes_pct);
    else
        snprintf(line, sizeof(line), "%d%% through", song_pct);
    int pspeed, pruns, pbest;
    if (practicing && game_practice_info(&pspeed, &pruns, &pbest) && pruns) {
        // practice: the stretch, and how the runs through it have gone
        char more[64];
        snprintf(more, sizeof(more), "   %d %s, best %d%%", pruns, pruns == 1 ? "run" : "runs", pbest);
        strncat(line, more, sizeof(line) - strlen(line) - 1);
    }
    font_draw_shadow(res.ui, line, SCREEN_W / 2, 418, 22, RGBA(225, 215, 200, 255), ALIGN_CENTER);
    gfx_end();
}

// back from the pause menu: three, two, one over the waiting highway, so the fingers are ready when it moves
static void resume_screen(void)
{
    const double step = 0.5;
    int left = 3 - (int)((now_s - screen_t) / step);
    static int said;
    if (pressed & BTN_START) {
        ui_sound(SND_UI_BACK);
        go(S_PAUSE);
        return;
    }
    if (left <= 0) {
        game_pause(false);
        go(S_PLAY);
        return;
    }
    if (screen_frames == 0 || left != said) {
        said = left;
        ui_sound(SND_UI_MOVE);
    }
    game_draw();
    gfx_2d();
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 90));
    char t[4];
    snprintf(t, sizeof(t), "%d", left);
    float a = (float)fmod(now_s - screen_t, step) / (float)step;
    font_draw_shadow(res.title, t, SCREEN_W / 2, 190 - a * 8, 110 + a * 16, fade_alpha(RGBA(255, 225, 120, 255), 1 - a * a),
                     ALIGN_CENTER);
    gfx_end();
}

// ------------------------------------------------------------------------------------------------------- results

// straight from the results to the part that went worst
static bool results_can_practice(void)
{
    return !tour_on && setlist_at < 0 && !practicing && result.worst[0] && result.worst_pct < 100;
}

static void results_screen(void)
{
    Song * s = songs_get(sel);
    if (pressed & (BTN_CROSS | BTN_CIRCLE)) {
        ui_sound(SND_UI_SELECT);
        // on to the next of the setlist, on the same part and difficulty when it has them
        if (setlist_at >= 0 && setlist_at + 1 < setlist_n) {
            sel = setlist[++setlist_at];
            Song * next = songs_get(sel);
            if (next) {
                open_select(next);
                // straight in when it has this part at this difficulty; otherwise the choice is the player's
                if (sel_diffs[result.part] & (1 << result.diff)) {
                    sel_part = result.part;
                    sel_diff = result.diff;
                    select_starting = true;
                }
            } else
                go(from_list());
            return;
        }
        setlist_at = -1;
        go(from_list());
        sel_t = now_s;
        if (tour_on)
            tour_next();
        return;
    }
    if ((pressed & BTN_SQUARE) && s && results_can_practice()) {
        ui_sound(SND_UI_SELECT);
        sel_part = result.part;
        sel_diff = result.diff;
        read_sections(s);
        for (int i = 0; i < pr_count; i++)
            if (!strcmp(pr[i].name, result.worst))
                pr_from = pr_to = pr_focus = i;
        go(pr_count ? S_PRACTICE : S_SELECT);
        return;
    }
    if ((pressed & (BTN_L | BTN_R | BTN_SELECT)) && hist_n >= (hist_has_this ? 2 : 1)) {
        ui_sound(SND_UI_MOVE);
        results_history = !results_history;
    }
    if ((pressed & BTN_TRIANGLE) && s) {
        ui_sound(SND_UI_SELECT);
        if (game_start(s, result.part, result.diff)) {
            game_set_autoplay(autoplay);
            go(S_PLAY);
        }
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_results, WHITE);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 90));
    panel(40, 30, 880, 470);
    font_draw_fit(res.title, result.failed ? "SONG FAILED" : s ? s->name : "", SCREEN_W / 2, 48, 52, 820,
                  result.failed ? RGBA(255, 90, 70, 255) : RGBA(255, 220, 140, 255), ALIGN_CENTER);
    if (s) {
        // failed, the big line doesn't name the song
        char who[200];
        snprintf(who, sizeof(who), "%s  -  %s", s->name, s->artist);
        font_draw_fit(res.ui, result.failed ? who : s->artist, SCREEN_W / 2, 104, 26, 820, RGBA(230, 230, 230, 255),
                      ALIGN_CENTER);
    }
    draw_cover(art_for == sel ? art : NULL, 84, 150, 200);
    char t[96];
    snprintf(t, sizeof(t), "%s  %s", part_name(result.part), diff_name(result.diff));
    font_draw_fit(res.ui, t, 184, 358, 22, 200, RGBA(190, 190, 190, 255), ALIGN_CENTER);
    if (!result.failed) {
        float a = (float)((now_s - screen_t) / 1.2);
        int shown = (int)(a * result.stars + 0.999f);
        if (shown > result.stars)
            shown = result.stars;
        draw_stars(shown >= 6 || (result.stars >= 6 && a >= 1) ? 6 : shown, 460, 136, 56);
    }
    int pct = result.notes_total ? result.notes_hit * 100 / result.notes_total : 0;
    struct {
        const char * label;
        char value[48];
    } rows[6];
    snprintf(rows[0].value, sizeof(rows[0].value), "%d", result.score);
    rows[0].label = "Score";
    snprintf(rows[1].value, sizeof(rows[1].value), "%d%%  (%d / %d)", pct, result.notes_hit, result.notes_total);
    rows[1].label = "Notes hit";
    snprintf(rows[2].value, sizeof(rows[2].value), "%d", result.best_streak);
    rows[2].label = "Best streak";
    snprintf(rows[3].value, sizeof(rows[3].value), "%d / %d", result.sp_hit, result.sp_total);
    rows[3].label = "Star Power phrases";
    snprintf(rows[4].value, sizeof(rows[4].value), "%d", result.overhits);
    rows[4].label = "Overhits";
    rows[5].label = "Your timing";
    if (!result.timed)
        strcpy(rows[5].value, "-");
    else if (abs(result.off_ms) < 5)
        strcpy(rows[5].value, "Right on it");
    else
        snprintf(rows[5].value, sizeof(rows[5].value), "%d ms %s", abs(result.off_ms),
                 result.off_ms > 0 ? "late" : "early");
    for (int i = 0; i < 6; i++) {
        float y = 200 + i * 31;
        font_draw_shadow(res.ui, rows[i].label, 330, y, 28, RGBA(220, 210, 200, 255), ALIGN_LEFT);
        font_draw_shadow(res.ui, rows[i].value, 870, y, 28, RGBA(255, 236, 190, 255), ALIGN_RIGHT);
    }
    bool strip = false;
    for (int i = 0; i < RESULT_STRIP; i++)
        strip |= result.strip[i] != 255;
    if (results_history) {
        // the last plays as bars, this one lit; the best and the average beside
        const float x = 84, w = 792, top = 396, h = 34;
        int best = 0;
        long sum = 0;
        for (int i = 0; i < hist_n; i++) {
            best = hist[i].score.score > best ? hist[i].score.score : best;
            sum += hist[i].score.score;
        }
        gfx_rect(x - 3, top - 3, w + 6, h + 6, RGBA(0, 0, 0, 200));
        float cell = w / HIST_MAX;
        for (int i = 0; i < hist_n; i++) {
            const Score * sc = &hist[i].score;
            float bh = best ? h * sc->score / (float)best : 0;
            bh = bh < 3 ? 3 : bh;
            bool last = i == hist_n - 1 && hist_has_this;
            uint32_t c = last ? RGBA(255, 210, 90, 255) : sc->fc ? RGBA(120, 220, 255, 255) : RGBA(150, 150, 160, 255);
            gfx_rect(x + i * cell + 4, top + h - bh, cell - 8, bh, c);
            char v[16];
            snprintf(v, sizeof(v), "%d%%", sc->percent);
            font_draw(res.ui, v, x + i * cell + cell / 2, top + h - 18, 16,
                      bh >= 18 ? RGBA(20, 20, 20, 255) : RGBA(230, 230, 230, 255), ALIGN_CENTER);
        }
        char line[120];
        snprintf(line, sizeof(line), "Last %d %s   best %d   average %ld", hist_n, hist_n == 1 ? "play" : "plays", best,
                 sum / hist_n);
        font_draw_fit(res.ui, line, x, 436, 24, 600, RGBA(220, 210, 200, 255), ALIGN_LEFT);
        int before = hist_n - (hist_has_this ? 2 : 1);   // the play before this one
        if (before >= 0 && !result.failed) {
            int prev = hist[before].score.score, d = result.score - prev;
            snprintf(line, sizeof(line), "%+d vs last time", d);
            font_draw_fit(res.ui, line, x + w, 436, 24, 260, d >= 0 ? RGBA(130, 230, 130, 255) : RGBA(255, 150, 120, 255),
                          ALIGN_RIGHT);
        }
    } else if (result.fc)
        font_draw_shadow(res.title, "FULL COMBO!", SCREEN_W / 2, 392, 56, RGBA(255, 210, 60, 255), ALIGN_CENTER);
    else if (strip) {
        // the song start to end: green all hit, down to red; it fills in as the stars come
        const float x = 84, w = 792, cell = w / RESULT_STRIP;
        float a = (float)((now_s - screen_t) / 1.2);
        int shown = a >= 1 ? RESULT_STRIP : (int)(a * RESULT_STRIP);
        gfx_rect(x - 3, 397, w + 6, 30, RGBA(0, 0, 0, 200));
        for (int i = 0; i < shown; i++) {
            int v = result.strip[i];
            if (v == 255)
                continue;
            uint32_t c = v >= 100 ? RGBA(70, 200, 90, 255) : v >= 85 ? RGBA(170, 205, 70, 255)
                         : v >= 60 ? RGBA(240, 170, 50, 255) : RGBA(225, 60, 45, 255);
            gfx_rect(x + i * cell + 0.5f, 400, cell - 1, 24, c);
        }
        char line[120];
        if (result.worst[0] && result.worst_pct < 100)
            snprintf(line, sizeof(line), "Toughest part: %s, %d%%", result.worst, result.worst_pct);
        else
            snprintf(line, sizeof(line), "Start to end, where the notes went");
        font_draw_fit(res.ui, line, x, 432, 24, 560, RGBA(220, 210, 200, 255), ALIGN_LEFT);
        if (result.weak_pad && result.weak_pct < 100) {
            snprintf(line, sizeof(line), "%s: %d%%", result.weak_pad, result.weak_pct);
            font_draw_fit(res.ui, line, x + w, 432, 24, 220, RGBA(255, 190, 120, 255), ALIGN_RIGHT);
        }
    }
    if (result_best)
        font_draw_shadow(res.title, "NEW BEST!", 870, 148, 36, RGBA(120, 220, 255, 255), ALIGN_RIGHT);
    for (int i = 0; i < result_ach_n && i < 2; i++) {
        // what this song unlocked, under the stars
        char d[96];
        snprintf(d, sizeof(d), "UNLOCKED: %s%s", achieve_get(result_ach[i])->name,
                 i == 1 && result_ach_n > 2 ? " and more" : "");
        font_draw_shadow(res.ui, d, SCREEN_W - 90, 76 + i * 22, 20, RGBA(255, 210, 90, 255), ALIGN_RIGHT);
    }
    if (result_daily) {
        char d[48];
        snprintf(d, sizeof(d), "DAILY SONG DONE  %d day%s", daily_streak(), daily_streak() == 1 ? "" : "s");
        font_draw_shadow(res.ui, d, 330, 160, 24, RGBA(130, 230, 130, 255), ALIGN_LEFT);
    }
    const char * help[8] = {"btn_cross", "Continue", "btn_triangle", "Retry"};
    int nh = 4;
    if (results_can_practice())
        help[nh++] = "btn_square", help[nh++] = "Practice it";
    if (hist_n >= (hist_has_this ? 2 : 1))
        help[nh++] = "btn_r", help[nh++] = results_history ? "This play" : "History";
    help_bar(help, nh);
    gfx_end();
}

// ---------------------------------------------------------------------------------------------------- World Tour

static int tour_ids[TOUR_SET];
static bool tour_in_set;            // the cursor's in the setlist, not on the gigs
static double tour_moved;           // when the gig changed: the venue's picture comes up once it settles
static int tour_shown = -1;
static Tex * tour_stage;

static void tour_look(void)
{
    tour_setlist(tour_leg, tour_gig, cfg.last_part, tour_ids);
    tour_moved = now_s;
}

static int first_unplayed(void)
{
    for (int i = 0; i < TOUR_SET; i++)
        if (!tour_slot_stars(tour_leg, tour_gig, i) && tour_ids[i] >= 0)
            return i;
    return 0;
}

static void tour_enter(void)
{
    if (!songs_count()) {
        go(S_EMPTY);
        return;
    }
    tour_on = true;
    tour_in_set = false;
    // the furthest leg that's open, at the first gig left to play
    tour_leg = 0;
    while (tour_leg + 1 < TOUR_LEGS && tour_leg_open(tour_leg + 1))
        tour_leg++;
    tour_gig = 0;
    while (tour_gig + 1 < TOUR_GIGS && tour_gig_done(tour_leg, tour_gig))
        tour_gig++;
    tour_shown = -1;
    tour_look();
    game_force_venue(tour_zone(tour_leg, tour_gig));
    go(S_TOUR);
}

// a song went by: what it changed is said on the next screen
static void tour_played(void)
{
    bool opened = tour_leg + 1 < TOUR_LEGS && tour_leg_open(tour_leg + 1);
    tour_record(tour_leg, tour_gig, tour_slot, result.stars > 0 ? result.stars : 1);
    if (tour_leg + 1 < TOUR_LEGS && !opened && tour_leg_open(tour_leg + 1)) {
        char t[96];
        snprintf(t, sizeof(t), "On to the next leg: %s", tour_leg_name(tour_leg + 1));
        say(t);
    } else if (tour_gig_done(tour_leg, tour_gig))
        say("Gig played!");
    else if (tour_songs_done(tour_leg, tour_gig) == TOUR_ENCORE && tour_slot != TOUR_ENCORE)
        say("The crowd wants more: ENCORE!");
}

// back from a song: the next one on the setlist, or the gigs once it's all played
static void tour_next(void)
{
    tour_look();
    tour_moved = -10;
    if (tour_gig_done(tour_leg, tour_gig))
        tour_in_set = false;
    else
        tour_slot = first_unplayed();
}

static void tour_screen(void)
{
    bool leg_open = tour_leg_open(tour_leg);
    bool encore_open = true;
    for (int i = 0; i < TOUR_ENCORE; i++)
        encore_open &= tour_slot_stars(tour_leg, tour_gig, i) > 0;
    if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        if (tour_in_set)
            tour_in_set = false;
        else {
            tour_on = false;
            game_force_venue(NULL);
            go(S_MAIN);
            return;
        }
    }
    int dir = repeat & BTN_UP ? -1 : repeat & BTN_DOWN ? 1 : 0;
    if (dir && tour_in_set) {
        tour_slot = (tour_slot + dir + TOUR_SET) % TOUR_SET;
        ui_sound(SND_UI_MOVE);
    } else if (dir) {
        tour_gig = (tour_gig + dir + TOUR_GIGS) % TOUR_GIGS;
        tour_look();
        ui_sound(SND_UI_MOVE);
    }
    int leg_dir = pressed & BTN_L ? -1 : pressed & BTN_R ? 1 : 0;
    if (leg_dir && !tour_in_set && tour_leg + leg_dir >= 0 && tour_leg + leg_dir < TOUR_LEGS) {
        tour_leg += leg_dir;
        tour_gig = 0;
        tour_look();
        ui_sound(SND_UI_MOVE);
        leg_open = tour_leg_open(tour_leg);
    }
    if (pressed & BTN_CROSS) {
        if (!leg_open)
            ui_sound(SND_UI_BACK);
        else if (!tour_in_set) {
            ui_sound(SND_UI_SELECT);
            tour_in_set = true;
            tour_slot = first_unplayed();
        } else if (tour_ids[tour_slot] >= 0 && (tour_slot != TOUR_ENCORE || encore_open)) {
            ui_sound(SND_UI_SELECT);
            sel = tour_ids[tour_slot];
            view_stale = true;
            game_force_venue(tour_zone(tour_leg, tour_gig));
            load_art(songs_get(sel));
            open_select(songs_get(sel));
            return;
        } else
            ui_sound(SND_UI_BACK);
    }
    // the venue, once the cursor stops on it (the same pictures the gig will play on)
    int here = tour_leg * TOUR_GIGS + tour_gig;
    if (tour_shown != here && now_s - tour_moved > 0.15) {
        Tex * lights;
        tour_stage = NULL;
        if (leg_open)
            res_venue(tour_zone(tour_leg, tour_gig), &tour_stage, &lights);
        tour_shown = here;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    if (tour_stage && tour_shown == here)
        gfx_sprite(&(Sprite){tour_stage, 0, 0, 1, 1, tour_stage->w, tour_stage->h}, 0, 0, SCREEN_W, SCREEN_H,
                   RGBA(140, 140, 140, 255));
    else
        draw_background(res.bg_menu, RGBA(120, 120, 120, 255));
    banner("WORLD TOUR");
    char t[120];
    snprintf(t, sizeof(t), "%d", tour_stars());
    font_draw_shadow(res.title, t, 886, 14, 44, RGBA(255, 236, 190, 255), ALIGN_RIGHT);
    gfx_sprite(atlas_get(res.hud, "hud_star_on"), 892, 14, 42, 42, WHITE);
    font_draw_shadow(res.ui, tour_rank(), 932, 60, 24, RGBA(230, 225, 215, 255), ALIGN_RIGHT);
    // the leg and its gigs
    panel(20, 92, 396, 404);
    snprintf(t, sizeof(t), "%s  %d of %d", tour_leg_name(tour_leg), tour_leg + 1, TOUR_LEGS);
    font_draw_fit(res.title, t, 218, 112, 32, 340, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    for (int g = 0; g < TOUR_GIGS; g++) {
        float y = 158 + g * 54;
        bool on = g == tour_gig;
        if (on)
            gfx_sprite(atlas_get(res.hud, "wor_select"), 30, y - 6, 376, 50, tour_in_set ? RGBA(150, 150, 150, 255) : WHITE);
        const char * title = res_venue_title(tour_zone(tour_leg, g));
        font_draw_fit(res.ui, leg_open ? title : "???", 56, y + 2, 28, 250,
                      on ? RGBA(255, 230, 150, 255) : leg_open ? RGBA(205, 200, 195, 255) : RGBA(100, 95, 90, 255),
                      ALIGN_LEFT);
        int have = tour_gig_stars(tour_leg, g);
        if (leg_open && tour_songs_done(tour_leg, g)) {
            snprintf(t, sizeof(t), "%d", have);
            font_draw_shadow(res.ui, t, 362, y + 3, 26, RGBA(255, 236, 190, 255), ALIGN_RIGHT);
            gfx_sprite(atlas_get(res.hud, tour_gig_done(tour_leg, g) ? "hud_star_gold" : "hud_star_on"), 364, y + 2, 26,
                       26, WHITE);
        }
    }
    // the setlist
    panel(432, 92, 508, 404);
    if (leg_open) {
        font_draw_fit(res.title, res_venue_title(tour_zone(tour_leg, tour_gig)), 686, 112, 34, 450,
                      RGBA(255, 220, 140, 255), ALIGN_CENTER);
        for (int i = 0; i < TOUR_SET; i++) {
            float y = 162 + i * 78;
            bool on = tour_in_set && i == tour_slot, locked = i == TOUR_ENCORE && !encore_open;
            if (on)
                gfx_sprite(atlas_get(res.hud, "wor_select"), 444, y - 8, 484, 74, WHITE);
            const Song * s = tour_ids[i] >= 0 ? songs_get(tour_ids[i]) : NULL;
            uint32_t c = locked ? RGBA(120, 115, 110, 255) : on ? RGBA(255, 230, 150, 255) : RGBA(225, 220, 212, 255);
            if (i == TOUR_ENCORE)
                font_draw(res.ui, "ENCORE", 900, y - 2, 18, RGBA(255, 200, 120, 255), ALIGN_RIGHT);
            font_draw_fit(res.ui, locked ? "???" : s ? s->name : "(no longer on the card)", 466, y, 28, 330, c,
                          ALIGN_LEFT);
            if (s && !locked)
                font_draw_fit(res.ui, s->artist, 466, y + 30, 20, 330, on ? RGBA(255, 240, 215, 255) : RGBA(175, 170, 165, 255),
                              ALIGN_LEFT);
            int st = tour_slot_stars(tour_leg, tour_gig, i);
            if (st)
                draw_stars(st, 800, y + 28, 22);
        }
    } else {
        snprintf(t, sizeof(t), "Play %d gigs of %s", TOUR_TO_OPEN, tour_leg_name(tour_leg - 1));
        font_draw_fit(res.ui, t, 686, 260, 28, 450, RGBA(220, 215, 205, 255), ALIGN_CENTER);
        font_draw_fit(res.ui, "to open this leg of the tour", 686, 298, 24, 450, RGBA(190, 185, 180, 255), ALIGN_CENTER);
    }
    static const char * help[] = {"btn_cross", "Pick", "btn_circle", "Back", "btn_l", "Leg"};
    help_bar(help, 6);
    gfx_end();
}

// ------------------------------------------------------------------------------------------------------- options

enum { O_SPEED, O_HIGHWAY, O_VENUE, O_BAND, O_SING, O_SINGDIFF, O_OFFSET, O_LAYOUT, O_DRUMKEYS, O_DRUMHITS, O_TILT, O_NOFAIL, O_LEFTY, O_MUSIC, O_SFX, O_CROWD, O_FPS,
       O_CAMERA, O_HWALPHA, O_VLIGHT, O_FLASH, O_POPUPS, O_TIMING, O_LIVE, O_VANISH, O_MISSSND, O_REAR, O_BEND, O_COUNTDOWN,
       O_RESET, O_FIVEPADS, O_DOUBLEKICK, O_EASYSUS, O_COUNTIN, O_SCREENS, O_PROGRESS, O_CUPS, O_AUTOREPRESS, O_FLATNOTES, O_NOTESIZE, O_WOBBLE, O_FIRE, O_CROWDHANDS, O_VIGNETTE, O_LYRICS, O_MENUMUSIC, O_SINGSP, O_SHRINKCHECK, O_COUNT };

// the options by subject, L and R between them
enum { OPT_TABS = 7, OPT_TAB_MAX = 11 };
static const char * opt_tab_names[OPT_TABS] = {"GAME", "CONTROLS", "HIGHWAY", "STAGE", "SCREEN", "SOUND", "LIBRARY"};
static const int opt_tabs[OPT_TABS][OPT_TAB_MAX + 1] = {
    {O_SPEED, O_OFFSET, O_NOFAIL, O_EASYSUS, O_AUTOREPRESS, O_VANISH, O_COUNTDOWN, O_SING, O_SINGDIFF, O_RESET, -1},
    {O_LAYOUT, O_DRUMKEYS, O_FIVEPADS, O_DOUBLEKICK, O_LEFTY, O_TILT, O_REAR, O_SINGSP, -1},
    {O_HIGHWAY, O_HWALPHA, O_CAMERA, O_CUPS, O_FLATNOTES, O_NOTESIZE, O_WOBBLE, O_FIRE, -1},
    {O_VENUE, O_VLIGHT, O_BAND, O_CROWDHANDS, O_VIGNETTE, O_SCREENS, O_FLASH, -1},
    {O_LYRICS, O_POPUPS, O_PROGRESS, O_TIMING, O_LIVE, O_FPS, -1},
    {O_MUSIC, O_SFX, O_CROWD, O_MENUMUSIC, O_COUNTIN, O_DRUMHITS, O_MISSSND, O_BEND, -1},
    {O_SHRINKCHECK, -1},
};
static int opt_tab, opt_top;
static bool opt_reset_armed;
// the shrink check: a song a frame, so the menu keeps moving. X then shrinks what the check found, on a worker
// thread; each song waits for a song being played to end before its files are swapped. The menus' tune is a preview
// whose decode would eat the worker's cores, so it rests while this runs
static int shrink_next = -1, shrink_left, shrink_seen;
static volatile int shrink_done_n, shrink_all_n;
static char shrink_cur[96];

static void shrink_check_step(void)
{
    if (shrink_running)
        return;
    if (shrink_next < 0 || shrink_next >= songs_count()) {
        shrink_next = -1;
        return;
    }
    const Song * so = songs_get(shrink_next++);
    shrink_seen++;
    if (!shrink_done(so->folder)) {
        shrink_left++;
        plat_log("not shrunk: %s", so->folder);
    }
}

static int shrink_worker(void * arg)
{
    (void)arg;
    int total = 0;
    for (int i = 0; i < songs_count(); i++)
        if (!shrink_done(songs_get(i)->folder))
            total++;
    shrink_all_n = total;
    for (int i = 0; i < songs_count() && shrink_running; i++) {
        const Song * so = songs_get(i);
        if (shrink_done(so->folder))
            continue;
        snprintf(shrink_cur, sizeof(shrink_cur), "%s", so->name);
        shrink_song(so->folder);
        shrink_done_n++;
    }
    plat_log("shrink: %d of %d songs shrunk", shrink_done_n, shrink_all_n);
    shrink_cur[0] = 0;
    shrink_running = false;
    shrink_next = shrink_left = shrink_seen = 0;    // the walk counts them again from the top
    return 0;
}

static int opt_tab_count(void)
{
    int n = 0;
    while (opt_tabs[opt_tab][n] >= 0)
        n++;
    return n;
}

static void option_text(int i, char * name, char * value, size_t n)
{
    switch (i) {
    case O_SPEED: strcpy(name, "Note speed"); snprintf(value, n, "%d", cfg.speed); break;
    case O_HIGHWAY: {
        strcpy(name, "Highway");
        // gh3_judy reads Judy, GH3
        char pretty[32];
        const char * h = cfg.highway, * u = strchr(h, '_');
        snprintf(pretty, sizeof(pretty), "%s", u ? u + 1 : h);
        pretty[0] = (char)toupper((unsigned char)pretty[0]);
        if (u)
            snprintf(value, n, "%s, %.*s", pretty, (int)(u - h), h);
        else
            snprintf(value, n, "%s", pretty);
        if (u)
            for (char * v = value + strlen(pretty) + 2; *v; v++)
                *v = (char)toupper((unsigned char)*v);
        break;
    }
    case O_VENUE:
        strcpy(name, "Venue");
        snprintf(value, n, "%s", strcmp(cfg.venue, "song") == 0 ? "A different one each song"
                                 : strcmp(cfg.venue, "random") == 0 ? "Random"
                                 : strcmp(cfg.venue, "classic") == 0 ? "None (plain stage)"
                                 : res_venue_title(cfg.venue));
        break;
    case O_BAND: strcpy(name, "Band on stage"); snprintf(value, n, "%s", cfg.no_band ? "Off" : "On"); break;
    case O_SING: strcpy(name, "Sing along"); snprintf(value, n, "%s", cfg.sing_along ? "On" : "Off"); break;
    case O_SINGDIFF:
        strcpy(name, "Sing along vocals");
        snprintf(value, n, "%s", diff_name(cfg.sing_diff));
        break;
    case O_SINGSP: strcpy(name, "Singing starts Star Power"); snprintf(value, n, "%s", cfg.sing_sp ? "On" : "Off"); break;
    case O_OFFSET: strcpy(name, "Audio offset"); snprintf(value, n, "%+d ms", cfg.offset_ms); break;
    case O_LAYOUT: strcpy(name, "Buttons"); snprintf(value, n, "%s", cfg.layout ? "Layout B" : "Layout A"); break;
    case O_TILT: strcpy(name, "Tilt for Star Power"); snprintf(value, n, "%s", cfg.tilt ? "On" : "Off"); break;
    case O_NOFAIL: strcpy(name, "No fail"); snprintf(value, n, "%s", cfg.nofail ? "On" : "Off"); break;
    case O_DRUMKEYS: strcpy(name, "Drum buttons"); snprintf(value, n, "%s", cfg.drum_layout == 1 ? "Shoulders" : cfg.drum_layout == 2 ? "Guitar" : "Thumbs"); break;
    case O_DRUMHITS: strcpy(name, "Drum pad sounds"); snprintf(value, n, "%s", cfg.drum_hits ? "Every hit" : "Freestyle only"); break;
    case O_LEFTY: strcpy(name, "Lefty flip"); snprintf(value, n, "%s", cfg.lefty ? "On" : "Off"); break;
    case O_MUSIC: strcpy(name, "Music volume"); snprintf(value, n, "%d", cfg.music_vol); break;
    case O_SFX: strcpy(name, "Effects volume"); snprintf(value, n, "%d", cfg.sfx_vol); break;
    case O_CROWD: strcpy(name, "Crowd volume"); snprintf(value, n, "%d", cfg.crowd_vol); break;
    case O_FPS: strcpy(name, "Show frame rate"); snprintf(value, n, "%s", cfg.show_fps ? "On" : "Off"); break;
    case O_CAMERA: {
        static const char * views[3] = {"World Tour", "Close", "High"};
        strcpy(name, "Highway camera");
        snprintf(value, n, "%s", views[cfg.camera]);
        break;
    }
    case O_HWALPHA: strcpy(name, "Highway solid"); snprintf(value, n, "%d%%", cfg.highway_alpha * 10); break;
    case O_VLIGHT: strcpy(name, "Venue brightness"); snprintf(value, n, "%d%%", cfg.venue_light * 10); break;
    case O_FLASH: {
        static const char * how[3] = {"Full", "Soft", "None"};
        strcpy(name, "Screen flashes");
        snprintf(value, n, "%s", how[cfg.flashes]);
        break;
    }
    case O_POPUPS: strcpy(name, "Streak messages"); snprintf(value, n, "%s", cfg.no_popups ? "Off" : "On"); break;
    case O_TIMING: strcpy(name, "Timing bar"); snprintf(value, n, "%s", cfg.timing_bar ? "On" : "Off"); break;
    case O_LIVE: strcpy(name, "Live percent"); snprintf(value, n, "%s", cfg.live_stats ? "On" : "Off"); break;
    case O_VANISH: strcpy(name, "Vanishing notes"); snprintf(value, n, "%s", cfg.vanish ? "On" : "Off"); break;
    case O_MISSSND: strcpy(name, "Wrong note sounds"); snprintf(value, n, "%s", cfg.no_miss_sound ? "Off" : "On"); break;
    case O_REAR: strcpy(name, "Rear pad whammy"); snprintf(value, n, "%s", cfg.no_rear_whammy ? "Off" : "On"); break;
    case O_BEND: strcpy(name, "Whammy bends the sound"); snprintf(value, n, "%s", cfg.no_bend ? "Off" : "On"); break;
    case O_COUNTDOWN: strcpy(name, "Countdown on resume"); snprintf(value, n, "%s", cfg.no_countdown ? "Off" : "On"); break;
    case O_FIVEPADS: strcpy(name, "Drum pads"); snprintf(value, n, "%s", cfg.five_pads ? "Five, as World Tour" : "As charted"); break;
    case O_SCREENS: strcpy(name, "Stage screens"); snprintf(value, n, "%s", cfg.stage_screens ? "On" : "Off"); break;
    case O_AUTOREPRESS: strcpy(name, "Auto re-press"); snprintf(value, n, "%s", cfg.auto_repress ? "On" : "Off"); break;
    case O_FLATNOTES: strcpy(name, "Notes"); snprintf(value, n, "%s", cfg.flat_notes ? "On the neck" : "Facing you"); break;
    case O_NOTESIZE: strcpy(name, "Note size"); snprintf(value, n, "%s", cfg.note_size == 0 ? "Small" : cfg.note_size == 1 ? "Normal" : "Big"); break;
    case O_WOBBLE: strcpy(name, "Sustain wobble"); snprintf(value, n, "%s", cfg.no_wobble ? "Off" : "On"); break;
    case O_FIRE: strcpy(name, "Hit fire"); snprintf(value, n, "%s", cfg.fire == 0 ? "Full" : cfg.fire == 1 ? "Small" : "Off"); break;
    case O_CROWDHANDS: strcpy(name, "Crowd hands"); snprintf(value, n, "%s", cfg.no_crowd ? "Off" : "On"); break;
    case O_VIGNETTE: strcpy(name, "Dark edges"); snprintf(value, n, "%s", cfg.no_vignette ? "Off" : "On"); break;
    case O_LYRICS: strcpy(name, "Lyrics"); snprintf(value, n, "%s", cfg.no_lyrics ? "Off" : "On"); break;
    case O_MENUMUSIC: strcpy(name, "Menu music"); snprintf(value, n, "%s", cfg.no_menu_music ? "Off" : "On"); break;
    case O_CUPS: strcpy(name, "Cup skin"); snprintf(value, n, "%s", cfg.cup_skin ? "Deep" : "World Tour"); break;
    case O_PROGRESS: strcpy(name, "Progress line"); snprintf(value, n, "%s", cfg.no_progress ? "Off" : "On"); break;
    case O_COUNTIN: strcpy(name, "Count-in"); snprintf(value, n, "%s", cfg.no_count_in ? "Off" : "On"); break;
    case O_EASYSUS: strcpy(name, "Easy chord sustains"); snprintf(value, n, "%s", cfg.easy_sustains ? "On" : "Off"); break;
    case O_DOUBLEKICK: strcpy(name, "Double kick"); snprintf(value, n, "%s", cfg.double_kick ? "On" : "Off"); break;
    case O_SHRINKCHECK:
        strcpy(name, "Check songs are shrunk");
        if (shrink_running)
            snprintf(value, n, "shrinking %d of %d: %s", shrink_done_n + 1 <= shrink_all_n ? shrink_done_n + 1 : shrink_all_n,
                     shrink_all_n, shrink_cur);
        else if (shrink_next >= 0)
            snprintf(value, n, "%d/%d", shrink_seen, songs_count());
        else if (!shrink_seen)
            snprintf(value, n, "Press X");
        else if (shrink_left)
            snprintf(value, n, "%d of %d not, X shrinks", shrink_left, shrink_seen);
        else
            snprintf(value, n, "All %d", shrink_seen);
        break;
    case O_RESET: strcpy(name, "Reset settings"); snprintf(value, n, "%s", opt_reset_armed ? "Press X again" : ""); break;
    }
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void change_option(int i, int d)
{
    switch (i) {
    case O_SPEED: cfg.speed = clampi(cfg.speed + d, 1, 10); break;
    case O_HIGHWAY: {
        int cur = 0;
        for (int k = 0; k < res.highway_count; k++)
            if (strcmp(res.highway_names[k], cfg.highway) == 0)
                cur = k;
        if (res.highway_count)
            snprintf(cfg.highway, sizeof(cfg.highway), "%s",
                     res.highway_names[(cur + d + res.highway_count) % res.highway_count]);
        break;
    }
    case O_VENUE: {
        // each song's own, random, none, then every venue by name
        int n = res.venue_count + 3, cur = 0;
        if (strcmp(cfg.venue, "random") == 0)
            cur = 1;
        else if (strcmp(cfg.venue, "classic") == 0)
            cur = 2;
        for (int k = 0; k < res.venue_count; k++)
            if (strcmp(res.venue_zones[k], cfg.venue) == 0)
                cur = k + 3;
        cur = (cur + d + n) % n;
        snprintf(cfg.venue, sizeof(cfg.venue), "%s", cur == 0 ? "song" : cur == 1 ? "random" : cur == 2 ? "classic"
                                                     : res.venue_zones[cur - 3]);
        break;
    }
    case O_BAND: cfg.no_band = !cfg.no_band; break;
    case O_SING: cfg.sing_along = !cfg.sing_along; break;
    case O_SINGDIFF: cfg.sing_diff = (cfg.sing_diff + d + DIFF_COUNT) % DIFF_COUNT; break;
    case O_SINGSP: cfg.sing_sp = !cfg.sing_sp; break;
    case O_OFFSET: cfg.offset_ms = clampi(cfg.offset_ms + d * 5, -300, 300); break;
    case O_LAYOUT: cfg.layout = !cfg.layout; break;
    case O_DRUMKEYS: cfg.drum_layout = (cfg.drum_layout + d + 3) % 3; break;
    case O_DRUMHITS: cfg.drum_hits = !cfg.drum_hits; break;
    case O_TILT: cfg.tilt = !cfg.tilt; break;
    case O_NOFAIL: cfg.nofail = !cfg.nofail; break;
    case O_LEFTY: cfg.lefty = !cfg.lefty; break;
    case O_MUSIC: cfg.music_vol = clampi(cfg.music_vol + d, 0, 10); break;
    case O_SFX: cfg.sfx_vol = clampi(cfg.sfx_vol + d, 0, 10); break;
    case O_CROWD: cfg.crowd_vol = clampi(cfg.crowd_vol + d, 0, 10); break;
    case O_FPS: cfg.show_fps = !cfg.show_fps; break;
    case O_CAMERA: cfg.camera = (cfg.camera + d + 3) % 3; break;
    case O_HWALPHA: cfg.highway_alpha = clampi(cfg.highway_alpha + d, 0, 10); break;
    case O_VLIGHT: cfg.venue_light = clampi(cfg.venue_light + d, 0, 10); break;
    case O_FLASH: cfg.flashes = (cfg.flashes + d + 3) % 3; break;
    case O_POPUPS: cfg.no_popups = !cfg.no_popups; break;
    case O_TIMING: cfg.timing_bar = !cfg.timing_bar; break;
    case O_LIVE: cfg.live_stats = !cfg.live_stats; break;
    case O_VANISH: cfg.vanish = !cfg.vanish; break;
    case O_MISSSND: cfg.no_miss_sound = !cfg.no_miss_sound; break;
    case O_REAR: cfg.no_rear_whammy = !cfg.no_rear_whammy; break;
    case O_BEND: cfg.no_bend = !cfg.no_bend; break;
    case O_COUNTDOWN: cfg.no_countdown = !cfg.no_countdown; break;
    case O_FIVEPADS: cfg.five_pads = !cfg.five_pads; break;
    case O_DOUBLEKICK: cfg.double_kick = !cfg.double_kick; break;
    case O_EASYSUS: cfg.easy_sustains = !cfg.easy_sustains; break;
    case O_COUNTIN: cfg.no_count_in = !cfg.no_count_in; break;
    case O_SCREENS: cfg.stage_screens = !cfg.stage_screens; break;
    case O_PROGRESS: cfg.no_progress = !cfg.no_progress; break;
    case O_CUPS: cfg.cup_skin = !cfg.cup_skin; break;
    case O_AUTOREPRESS: cfg.auto_repress = !cfg.auto_repress; break;
    case O_FLATNOTES: cfg.flat_notes = !cfg.flat_notes; break;
    case O_NOTESIZE: cfg.note_size = (cfg.note_size + d + 3) % 3; break;
    case O_WOBBLE: cfg.no_wobble = !cfg.no_wobble; break;
    case O_FIRE: cfg.fire = (cfg.fire + d + 3) % 3; break;
    case O_CROWDHANDS: cfg.no_crowd = !cfg.no_crowd; break;
    case O_VIGNETTE: cfg.no_vignette = !cfg.no_vignette; break;
    case O_LYRICS: cfg.no_lyrics = !cfg.no_lyrics; break;
    case O_MENUMUSIC:
        cfg.no_menu_music = !cfg.no_menu_music;
        if (cfg.no_menu_music)
            menu_music_stop();
        break;
    }
    audio_set_volumes(cfg.music_vol / 10.0f, cfg.sfx_vol / 10.0f);
}

static void options_screen(void)
{
    if (pressed & (BTN_L | BTN_R)) {
        opt_tab = (opt_tab + (pressed & BTN_L ? OPT_TABS - 1 : 1)) % OPT_TABS;
        menu_index = opt_top = 0;
        opt_reset_armed = false;
        ui_sound(SND_UI_MOVE);
    }
    int count = opt_tab_count();
    if (menu_index >= count)
        menu_index = 0;
    if (repeat & (BTN_UP | BTN_DOWN)) {
        menu_index = (menu_index + (repeat & BTN_UP ? count - 1 : 1)) % count;
        opt_reset_armed = false;
        ui_sound(SND_UI_MOVE);
    }
    shrink_check_step();
    int opt = opt_tabs[opt_tab][menu_index];
    if (opt == O_SHRINKCHECK && (pressed & BTN_CROSS) && !shrink_running) {
        if (shrink_seen && shrink_left) {
            shrink_done_n = 0;
            shrink_cur[0] = 0;
            shrink_running = true;
            menu_music_stop();                          // its decode would eat the worker's cores
            plat_thread("shrink", shrink_worker, NULL, 1);      // a worker's place, not the downloader's crawl
        } else
            shrink_next = shrink_left = shrink_seen = 0;
        ui_sound(SND_UI_SELECT);
    }
    if (opt == O_RESET) {
        if (pressed & BTN_CROSS) {
            if (opt_reset_armed) {
                // what's been measured and where you were stay
                Config keep = cfg;
                config_defaults();
                cfg.offset_ms = keep.offset_ms;
                cfg.last_part = keep.last_part;
                cfg.last_diff = keep.last_diff;
                cfg.sort = keep.sort;
                memcpy(cfg.last_song, keep.last_song, sizeof(cfg.last_song));
                audio_set_volumes(cfg.music_vol / 10.0f, cfg.sfx_vol / 10.0f);
            }
            opt_reset_armed = !opt_reset_armed;
            ui_sound(SND_UI_SELECT);
        }
    } else if (repeat & (BTN_LEFT | BTN_RIGHT | BTN_CROSS)) {
        change_option(opt, repeat & BTN_LEFT ? -1 : 1);
        ui_sound(SND_UI_MOVE);
    }
    if (pressed & (BTN_CIRCLE | BTN_START)) {
        config_save();
        ui_sound(SND_UI_BACK);
        int back = options_from;
        go(back);
        if (back == S_PAUSE || back == S_MAIN)
            menu_index = back == S_MAIN ? 3 : 2;    // back on OPTIONS
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(170, 170, 170, 255));
    banner("OPTIONS");
    panel(110, 96, 740, 356);
    for (int i = 0; i < OPT_TABS; i++) {
        float x = 110 + 740 * (i + 0.5f) / OPT_TABS;
        font_draw(res.ui, opt_tab_names[i], x, 106, 21, i == opt_tab ? RGBA(255, 215, 110, 255) : RGBA(150, 145, 140, 255),
                  ALIGN_CENTER);
        if (i == opt_tab)
            gfx_rect(x - 50, 134, 100, 2, RGBA(255, 215, 110, 255));
    }
    enum { ROWS = 8 };
    if (menu_index < opt_top)
        opt_top = menu_index;
    if (menu_index >= opt_top + ROWS)
        opt_top = menu_index - ROWS + 1;
    for (int r = 0; r < ROWS && opt_top + r < count; r++) {
        int i = opt_tabs[opt_tab][opt_top + r];
        char name[48], value[48];
        option_text(i, name, value, sizeof(value));
        float y = 142 + r * 36;
        bool on = opt_top + r == menu_index;
        if (on)
            gfx_sprite(atlas_get(res.hud, "wor_select"), 130, y - 3, 700, 38, WHITE);
        uint32_t c = on ? RGBA(255, 230, 150, 255) : RGBA(220, 215, 205, 255);
        font_draw(res.ui, name, 160, y + 1, 28, c, ALIGN_LEFT);
        // the volumes and percentages show as a bar as well
        int bar = i == O_MUSIC ? cfg.music_vol : i == O_SFX ? cfg.sfx_vol : i == O_CROWD ? cfg.crowd_vol
                  : i == O_HWALPHA ? cfg.highway_alpha : i == O_VLIGHT ? cfg.venue_light : i == O_SPEED ? cfg.speed : -1;
        if (bar >= 0) {
            gfx_rect(560, y + 13, 170, 8, RGBA(0, 0, 0, 170));
            gfx_rect(561, y + 14, 168 * bar / 10.0f, 6, on ? RGBA(255, 215, 110, 255) : RGBA(170, 150, 110, 255));
        }
        font_draw_fit(res.ui, value, 800, y + 1, 28, bar >= 0 ? 60 : 380, c, ALIGN_RIGHT);
    }
    // there's more above or below
    if (opt_top > 0)
        font_draw(res.ui, "...", SCREEN_W / 2, 116, 22, RGBA(220, 215, 205, 255), ALIGN_CENTER);
    if (opt_top + ROWS < count)
        font_draw(res.ui, "...", SCREEN_W / 2, 434, 22, RGBA(220, 215, 205, 255), ALIGN_CENTER);
    static const char * about[O_COUNT] = {
        "How fast the notes come down the highway.",
        "The highway's look, from Guitar Hero 2, 3, World Tour and Metallica.",
        "The stage behind the highway: Guitar Hero World Tour's venues, lit up with the music.",
        "World Tour's band on the venue's stage. Turn it off if a song runs slow.",
        "Sing into the mic over any instrument, when the song has vocals. Headphones help.",
        "What difficulty the vocals you sing along to are judged at. Your instrument keeps its own.",
        "Move the notes earlier (-) or later (+) to match the sound. Calibrate sets it for you.",
        NULL,
        NULL,
        "On top of the song: the kit answers every pad you hit, not just in the gaps.",
        "Tilt the Vita quickly to use Star Power (Select always works too).",
        "The song keeps going however badly it goes.",
        "Green on the right, for left-handed players.",
        "The song's volume.",
        "The volume of misses, Star Power and menu sounds.",
        "The volume of the crowd.",
        "Shows the frame rate during a song.",
        "World Tour's view, closer with bigger gems, or higher to read further ahead.",
        "Lower it to see more of the stage through the highway.",
        "Darken the stage if the lights make the notes hard to read.",
        "The screen flash of Star Power, and the highway's rails on a mistake.",
        "Messages like a note streak or Star Power ready.",
        "A bar under the highway: each hit left of centre was early, right was late.",
        "The percent of notes hit so far, with FC while there's been no mistake.",
        "Notes vanish before the strikeline. Play them from memory.",
        "The clank when you press a wrong fret.",
        "Rubbing the back of the Vita works the whammy. The right stick always does.",
        "Off: the whammy still earns Star Power, without changing the sound.",
        "Three, two, one before the song goes on after a pause.",
        "Back to how the game came. Your audio offset stays.",
        "Lay 4-pad charts on World Tour's five pads: cymbals yellow and orange, toms blue and green.",
        "Expert+: play the extra kicks charted for a second foot, in new downloads that have them.",
        "One finger on a held chord keeps it ringing, so your thumbs are free for the next note.",
        "Four clicks of the sticks before the first note, when there's room for them.",
        "The song's cover on two screens hanging over the stage.",
        "A thin line along the top edge showing how far through the song you are.",
        "The look of the cups at the end of the highway.",
        "Green, red over it, green again: the green plays by itself even with red still down.",
        "Lying on the neck, turned with the lanes, or standing up facing you.",
        "How big the notes are.",
        "Held notes shake on the highway.",
        "The flames when you hit a note.",
        "World Tour's hands at the foot of the picture.",
        "Darkens the edges of the picture to keep your eye on the highway.",
        "The song's words along the top while a line is sung. Sing along is the mic, separately.",
        "One of your songs plays quietly in the menus.",
        "While Star Power is banked, singing a note in tune starts it, like YARG's voice-activated option.",
        "X counts the songs whose audio isn't shrunk to Opus yet, then X shrinks them, right on the Vita.",
    };
    char line[160];
    const char * text = about[opt];
    if (!text)                  // an option without a description must never crash the draw
        text = "";
    if (opt == O_LAYOUT) {
        snprintf(line, sizeof(line), "%s   Whammy: right stick, or rub the back", cfg.layout
                 ? "Green L, Red Left, Yellow Up, Blue Triangle, Orange R, open X"
                 : "Green L, Red Left, Yellow Up or Triangle, Blue Circle, Orange R, open X");
        text = line;
    }
    if (opt == O_DRUMKEYS)
        text = cfg.drum_layout == 2 ? "The guitar's own buttons: Red Left, Yellow Up or Triangle, Blue Circle, Green L,"
                 " Orange R on five pads. Kick R, X or Down — on five pads X or Down"
               : cfg.drum_layout == 1 ? cfg.five_pads ? "Red L or Down, Yellow Up or Left, Blue Triangle or Square, Orange"
                                " Right, Green Circle or Cross, Kick R"
                                               : "Red L or Down, Yellow Up, Left or Right, Blue Triangle or Square, Green"
                                " Circle or Cross, Kick R"
               : cfg.five_pads ? "Red Left or Down, Yellow Up or Triangle, Blue Square or Circle, Orange Right, Green L,"
                                " Kick R or X"
                                               : "Red Left or Down, Yellow Up or Triangle, Blue Square or Circle, Green L,"
                                " Kick R or X";
    font_draw_fit(res.ui, text, SCREEN_W / 2, 462, 22, 820, RGBA(200, 200, 200, 255), ALIGN_CENTER);
    static const char * help[] = {"btn_left", "Change", "btn_l", "Tab", "btn_r", "Tab", "btn_circle", "Done"};
    help_bar(help, 8);
    gfx_end();
}

// -------------------------------------------------------------------------------------------------------- career

// --------------------------------------------------------------------------------------------------------- players
// who's playing: Select on the main menu. Each has their own scores, favourites, settings, career, tour and streak.


static void player_switch(int i)
{
    if (i == player_current())
        return;
    player_select(i);
    config_load();
    career_load();
    achieve_load();
    tour_load();
    songs_reload_player();
    songs_sort(cfg.sort);
    daily.loaded = false;
    view_stale = true;
    art_for = preview_for = -1;
    plat_log("player: %s", player_name(i));
}

static void player_named(const char * name)
{
    int i = player_add(name);
    if (i >= 0) {
        player_switch(i);
        players_row = i;
        char hi[64];
        snprintf(hi, sizeof(hi), "Hi, %s!", player_name(i));
        say(hi);
    } else
        say(player_count() >= PLAYER_MAX ? "That's as many players as there's room for" : "That name's taken");
}

// the cards' colours: two tones each, top and bottom
static const uint32_t player_tones[PLAYER_COLORS][2] = {
    {RGBA(230, 70, 60, 255), RGBA(140, 25, 30, 255)},   {RGBA(60, 140, 235, 255), RGBA(25, 60, 140, 255)},
    {RGBA(70, 200, 110, 255), RGBA(25, 110, 55, 255)},  {RGBA(250, 190, 50, 255), RGBA(170, 100, 20, 255)},
    {RGBA(170, 90, 230, 255), RGBA(85, 35, 140, 255)},  {RGBA(240, 120, 190, 255), RGBA(150, 45, 105, 255)},
    {RGBA(60, 205, 210, 255), RGBA(20, 110, 120, 255)}, {RGBA(150, 150, 160, 255), RGBA(70, 70, 80, 255)},
};

static void players_screen(void)
{
    static float scroll;                // eases toward the chosen card
    int n = player_count(), cards = n < PLAYER_MAX ? n + 1 : n;   // the last card adds a player
    if (dl_typing) {
        kb_update();
        pressed = repeat = 0;
    }
    if (players_row >= cards)
        players_row = cards - 1;
    if ((repeat & BTN_LEFT) && players_row > 0) {
        players_row--;
        ui_sound(SND_UI_MOVE);
    }
    if ((repeat & BTN_RIGHT) && players_row < cards - 1) {
        players_row++;
        ui_sound(SND_UI_MOVE);
    }
    if ((pressed & BTN_TRIANGLE) && players_row < n) {
        // another colour for this player's card
        player_set_color(players_row, player_color(players_row) + 1);
        ui_sound(SND_UI_MOVE);
    }
    if (pressed & BTN_CROSS) {
        ui_sound(SND_UI_SELECT);
        if (players_row < n) {
            player_switch(players_row);
            players_from_title = false;
            go(S_MAIN);
            pressed = 0;
            return;
        }
        dl_typed[0] = 0;
        kb_apply = player_named;
        kb_library = false;
        dl_typing = true;
    } else if (pressed & (BTN_CIRCLE | BTN_SELECT)) {
        ui_sound(SND_UI_BACK);
        go(players_from_title ? S_TITLE : S_MAIN);
        players_from_title = false;
        return;
    }
    scroll += (players_row - scroll) * 0.25f;
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(110, 110, 110, 255));
    gfx_rect_grad(0, 0, SCREEN_W, 160, RGBA(0, 0, 0, 200), RGBA(0, 0, 0, 0));
    font_draw_shadow(res.title, "WHO'S PLAYING?", SCREEN_W / 2, 50, 52, RGBA(255, 225, 150, 255), ALIGN_CENTER);
    // the cards in a row, the chosen one in the middle and bigger
    const float size = 170, gap = 210, cy = 270;
    for (int i = 0; i < cards; i++) {
        float x = SCREEN_W / 2 + (i - scroll) * gap;
        if (x < -size || x > SCREEN_W + size)
            continue;
        float d = fabsf(i - scroll), grow = d < 1 ? 1 + 0.18f * (1 - d) : 1;
        float w = size * grow, top = cy - w / 2;
        bool cur = i == players_row;
        uint32_t dim = cur ? WHITE : RGBA(170, 170, 170, 255);
        if (cur) {
            // a gold frame around the chosen one
            float b = 5;
            gfx_rect(x - w / 2 - b, top - b, w + 2 * b, w + 2 * b, RGBA(255, 210, 90, 255));
        }
        if (i < n) {
            const uint32_t * c = player_tones[player_color(i)];
            gfx_rect_grad(x - w / 2, top, w, w, c[0], c[1]);
            if (!cur)
                gfx_rect(x - w / 2, top, w, w, RGBA(0, 0, 0, 90));
            char initial[2] = {player_name(i)[0], 0};
            if (initial[0] >= 'a' && initial[0] <= 'z')
                initial[0] -= 32;
            font_draw_shadow(res.title, initial, x, top + w * 0.16f, w * 0.68f, dim, ALIGN_CENTER);
            font_draw_fit(res.ui, player_name(i), x, top + w + 14, cur ? 30 : 24, gap - 16,
                          cur ? RGBA(255, 236, 190, 255) : RGBA(200, 200, 200, 255), ALIGN_CENTER);
            if (i == player_current())
                font_draw(res.ui, "last played", x, top + w + 50, 18, RGBA(130, 230, 130, 255), ALIGN_CENTER);
        } else {
            gfx_rect(x - w / 2, top, w, w, RGBA(40, 40, 48, 230));
            gfx_rect(x - 4, top + w * 0.25f, 8, w * 0.5f, dim);
            gfx_rect(x - w * 0.25f, top + w / 2 - 4, w * 0.5f, 8, dim);
            font_draw_fit(res.ui, "Add player", x, top + w + 14, cur ? 30 : 24, gap - 16,
                          cur ? RGBA(255, 236, 190, 255) : RGBA(200, 200, 200, 255), ALIGN_CENTER);
        }
    }
    draw_toast();
    if (dl_typing) {
        kb_draw();
        font_draw(res.ui, "Type a name, then Start", SCREEN_W / 2, 470, 22, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    } else if (players_row < n) {
        static const char * help[] = {"btn_cross", "Play", "btn_left", "Choose", "btn_triangle", "Colour", "btn_circle", "Back"};
        help_bar(help, 8);
    } else {
        static const char * help[] = {"btn_cross", "Add", "btn_left", "Choose", "btn_circle", "Back"};
        help_bar(help, 6);
    }
    gfx_end();
}

// the career's other pages: the achievements, fourteen to a page
static int career_page;

static void achievements_draw(int page)
{
    banner("ACHIEVEMENTS");
    panel(40, 84, 880, 400);
    char t[64];
    snprintf(t, sizeof(t), "%d of %d", achieve_unlocked(), achieve_count());
    font_draw_shadow(res.ui, t, SCREEN_W - 50, 40, 26, RGBA(255, 215, 110, 255), ALIGN_RIGHT);
    for (int k = 0; k < 14; k++) {
        int i = page * 14 + k;
        const Achievement * a = achieve_get(i);
        if (!a)
            break;
        bool got = achieve_has(i);
        float x = 70 + (k / 7) * 430, y = 104 + (k % 7) * 52;
        gfx_rect(x, y + 4, 34, 34, got ? RGBA(255, 200, 70, 255) : RGBA(60, 60, 66, 255));
        if (got) {
            // a tick
            gfx_rect(x + 7, y + 20, 8, 5, RGBA(80, 45, 0, 255));
            gfx_rect(x + 12, y + 12, 5, 13, RGBA(80, 45, 0, 255));
            gfx_rect(x + 15, y + 10, 13, 5, RGBA(80, 45, 0, 255));
        }
        font_draw_fit(res.ui, a->name, x + 46, y, 24, 360, got ? RGBA(255, 236, 190, 255) : RGBA(170, 170, 175, 255),
                      ALIGN_LEFT);
        font_draw_fit(res.ui, a->how, x + 46, y + 25, 17, 360, got ? RGBA(210, 205, 195, 255) : RGBA(130, 130, 135, 255),
                      ALIGN_LEFT);
    }
}

static void career_screen(void)
{
    int pages = 1 + (achieve_count() + 13) / 14;
    if (pressed & BTN_R && career_page < pages - 1) {
        career_page++;
        ui_sound(SND_UI_MOVE);
    }
    if (pressed & BTN_L && career_page > 0) {
        career_page--;
        ui_sound(SND_UI_MOVE);
    }
    if (pressed & (BTN_CIRCLE | BTN_CROSS)) {
        ui_sound(SND_UI_BACK);
        go(S_MAIN);
        menu_index = 6;             // back on CAREER
        career_page = 0;
        return;
    }
    if (career_page > 0) {
        gfx_begin(0, 0, 0);
        gfx_2d();
        draw_background(res.bg_menu, RGBA(170, 170, 170, 255));
        achievements_draw(career_page - 1);
        static const char * help[] = {"btn_l", "Back a page", "btn_r", "Next page", "btn_circle", "Back"};
        help_bar(help, 6);
        gfx_end();
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(170, 170, 170, 255));
    banner("CAREER");
    panel(110, 96, 740, 356);
    int next;
    const char * rank = career_rank(&next);
    font_draw_shadow(res.title, rank, SCREEN_W / 2, 106, 44, RGBA(255, 215, 110, 255), ALIGN_CENTER);
    char line[96];
    if (next)
        snprintf(line, sizeof(line), "%d stars  -  next title at %d", career.stars, next);
    else
        snprintf(line, sizeof(line), "%d stars", career.stars);
    font_draw(res.ui, line, SCREEN_W / 2, 154, 24, RGBA(220, 215, 205, 255), ALIGN_CENTER);
    int fav = 0;
    for (int i = 1; i < PART_COUNT; i++)
        if (career.part_plays[i] > career.part_plays[fav])
            fav = i;
    struct {
        const char * label;
        char value[48];
    } rows[8];
    rows[0].label = "Songs finished";
    snprintf(rows[0].value, 48, "%d of %d played", career.finished, career.plays);
    rows[1].label = "Full combos";
    snprintf(rows[1].value, 48, "%d", career.full_combos);
    rows[2].label = "Notes hit";
    snprintf(rows[2].value, 48, "%lld of %lld", career.notes_hit, career.notes_total);
    rows[3].label = "Accuracy";
    snprintf(rows[3].value, 48, "%.1f%%", career.notes_total ? career.notes_hit * 100.0 / career.notes_total : 0.0);
    rows[4].label = "Best streak";
    snprintf(rows[4].value, 48, "%d", career.best_streak);
    rows[5].label = "Best score";
    snprintf(rows[5].value, 48, "%d", career.best_score);
    rows[6].label = "Time on stage";
    snprintf(rows[6].value, 48, "%lld h %02lld min", career.seconds / 3600, career.seconds / 60 % 60);
    rows[7].label = "Plays most";
    snprintf(rows[7].value, 48, "%s", career.plays ? part_name(fav) : "-");
    for (int i = 0; i < 8; i++) {
        float y = 190 + i * 31;
        font_draw(res.ui, rows[i].label, 170, y, 28, RGBA(220, 210, 200, 255), ALIGN_LEFT);
        font_draw(res.ui, rows[i].value, 790, y, 28, RGBA(255, 236, 190, 255), ALIGN_RIGHT);
    }
    static const char * help[] = {"btn_r", "Achievements", "btn_circle", "Back"};
    help_bar(help, 4);
    gfx_end();
}

// what changed in this build, shown once the first time it starts (or after an update)
static void news_screen(void)
{
    static const char * const lines[] = {
        "NEW HIGH SCORE! pops up mid-song, with the crowd cheering, the moment you pass your best on that chart.",
        "PRESS X TO ROCK on the title no longer fades almost out.",
        "The bassist no longer wanders into the singer on stage.",
        "Button bars at the bottom shrink to fit: \"Back\" was cut off on DOWNLOAD SONGS.",
        "Search filters line up on the right; unset ones say Any, and Genre says Loading while the list comes in.",
        "Earlier: streak lamps after World Tour, three drum layouts, sing along at its own difficulty.",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, WHITE);
    banner("WHAT'S NEW IN THIS BUILD");
    panel(150, 96, 660, 400);
    // each item on up to two lines
    float y = 114;
    for (int i = 0; i < n; i++)
        y += draw_wrapped(lines[i], 170, y, 18, 620, 2, RGBA(230, 230, 235, 255)) + 11;
    font_draw_shadow(res.ui, "X  continue", SCREEN_W / 2, 498, 20, RGBA(180, 220, 255, 255), ALIGN_CENTER);
    draw_toast();
    gfx_end();
    if (pressed & (BTN_CROSS | BTN_CIRCLE)) {
        char path[400];
        snprintf(path, sizeof(path), "%sseen.txt", plat_data_dir());
        FILE * f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d\n", GHV_BUILD);
            fclose(f);
        }
        go(S_TITLE);
    }
}

// --------------------------------------------------------------------------------------------------- calibration

static int by_double(const void * a, const void * b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void calibrate_screen(void)
{
    if (!calib_playing) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", plat_asset_dir());
        size_t l = strlen(dir);
        if (l && dir[l - 1] == '/')
            dir[l - 1] = 0;
        const char * stem = "calib";
        if (audio_song_load(dir, &stem, 1)) {
            audio_song_fade(1, 0);
            audio_song_play(0, 0);
        }
        calib_playing = true;
        calib_count = 0;
    }
    double t = audio_song_time();
    // the clicks are at 1.0 s and every half second after
    if ((pressed & BTN_CROSS) && t > 0.6 && calib_count < 32) {
        double k = floor((t - 1.0) / 0.5 + 0.5);
        double off = t - (1.0 + k * 0.5);
        calib_hits[calib_count++] = off;
    }
    bool done = calib_count >= 16;
    if (pressed & BTN_CIRCLE || (done && (pressed & BTN_TRIANGLE)) || t > 58) {
        if (done && (pressed & BTN_TRIANGLE)) {
            double sorted[32];
            memcpy(sorted, calib_hits, sizeof(double) * calib_count);
            qsort(sorted, calib_count, sizeof(double), by_double);
            // the middle half's average
            double sum = 0;
            int n = 0;
            for (int i = calib_count / 4; i < calib_count * 3 / 4; i++) {
                sum += sorted[i];
                n++;
            }
            cfg.offset_ms = (int)floor(sum / n * 1000 + 0.5);
            config_save();
            ui_sound(SND_UI_SELECT);
        } else
            ui_sound(SND_UI_BACK);
        audio_song_unload();
        calib_playing = false;
        go(S_MAIN);
        menu_index = 5;             // back on CALIBRATE
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(140, 140, 140, 255));
    banner("CALIBRATE");
    panel(110, 100, 740, 380);
    font_draw(res.ui, "Press X on every click you hear.", SCREEN_W / 2, 130, 30, RGBA(240, 235, 225, 255),
              ALIGN_CENTER);
    // a pulse on the beat (what the game thinks)
    double ph = fmod(t - 1.0 + 100, 0.5) / 0.5;
    float pulse = (float)(1 - ph);
    gfx_blend(BLEND_ADD);
    gfx_sprite(atlas_get(res.game, "glow"), SCREEN_W / 2 - 60, 190, 120, 120, fade_alpha(RGBA(255, 200, 90, 255),
                                                                                        pulse * pulse));
    gfx_blend(BLEND_ALPHA);
    char s[96];
    double sum = 0;
    for (int i = 0; i < calib_count; i++)
        sum += calib_hits[i];
    snprintf(s, sizeof(s), "%d of 16 clicks   (so far %+d ms)", calib_count,
             calib_count ? (int)(sum / calib_count * 1000) : 0);
    font_draw(res.ui, s, SCREEN_W / 2, 340, 28, RGBA(255, 230, 160, 255), ALIGN_CENTER);
    snprintf(s, sizeof(s), "Current offset %+d ms", cfg.offset_ms);
    font_draw(res.ui, s, SCREEN_W / 2, 380, 24, RGBA(200, 200, 200, 255), ALIGN_CENTER);
    if (done)
        font_draw(res.ui, "Press Triangle to keep the new offset", SCREEN_W / 2, 420, 26, RGBA(120, 220, 255, 255),
                  ALIGN_CENTER);
    static const char * help[] = {"btn_cross", "Tap", "btn_triangle", "Save", "btn_circle", "Cancel"};
    help_bar(help, 6);
    gfx_end();
}

// ---------------------------------------------------------------------------------------------------------- main

static void scan_progress(int found, const char * converting)
{
    char t[160];
    if (converting)
        snprintf(t, sizeof(t), "ADDING  %s", converting);
    else
        snprintf(t, sizeof(t), "FINDING SONGS  %d", found);
    res_loading(1, t);
}

int main(void)
{
    plat_init();
    update_boot();
    plat_log("GH Vita starting");
    // QUICKPLAY's song and a random venue differ from boot to boot (test runs on the fixed clock stay repeatable)
    srand((unsigned)(getenv("GHV_FIXED") ? 1 : time(NULL)) ^ (unsigned)plat_now_us());
    gfx_init();
    player_init();
    config_load();
    career_load();
    achieve_load();
    audio_init();
    audio_set_volumes(cfg.music_vol / 10.0f, cfg.sfx_vol / 10.0f);
    res_load();
    songs_scan(scan_progress);
    tour_load();
    for (int i = 0; i < songs_count(); i++)
        if (strcmp(songs_get(i)->folder, cfg.last_song) == 0)
            sel = i;
    plat_set_perf(0);
    const char * e = getenv("GHV_AUTOPLAY");
    autoplay = e && atoi(e);
    plat_log("build " __DATE__ " " __TIME__);
    if (getenv("GHV_SONG")) {
        // tests: straight into a song (GHV_SONG=index, GHV_PART, GHV_DIFF)
        const char * want = getenv("GHV_SONG");
        sel = atoi(want);
        for (int i = 0; i < songs_count(); i++)
            if (strstr(songs_get(i)->folder, want))
                sel = i;
        Song * s = songs_get(sel);
        int part = getenv("GHV_PART") ? atoi(getenv("GHV_PART")) : 0;
        int diff = getenv("GHV_DIFF") ? atoi(getenv("GHV_DIFF")) : 3;
        if (s && game_start(s, part, diff)) {
            game_set_autoplay(autoplay);
            go(S_PLAY);
        } else {
            // a test asking for a part the song hasn't got would wait at the title until its timeout
            plat_log("result: can't start %s part %d diff %d", s ? s->folder : want, part, diff);
            return 1;
        }
    } else {
        // the first time this build starts, say what's new; one press and it never shows again
        char seen_path[400], line[32] = "";
        int seen = 0;
        snprintf(seen_path, sizeof(seen_path), "%sseen.txt", plat_data_dir());
        FILE * sf = fopen(seen_path, "r");
        if (sf) {
            if (fgets(line, sizeof(line), sf))
                seen = atoi(line);
            fclose(sf);
        }
        if (seen != GHV_BUILD)
            go(S_NEWS);
    }
    uint32_t last_buttons = 0;
    bool last_overlay = false;
    for (;;) {
        if (!plat_poll(&pad))
            break;
        double t = plat_now_us() / 1e6;
        gap_s = t - now_s;
        now_s = t;
        // only the moment it comes up, so a flag that stays on can't keep pausing
        overlay_on = pad.overlay && !last_overlay;
        last_overlay = pad.overlay;
        pressed = pad.buttons & ~last_buttons;
        last_buttons = pad.buttons;
        update_repeat();
        switch (screen) {
        case S_TITLE: title_screen(); break;
        case S_MAIN: main_menu(); break;
        case S_TOUR: tour_screen(); break;
        case S_SONGS: song_list(); break;
        case S_SELECT: select_screen(); break;
        case S_PRACTICE: practice_screen(); break;
        case S_CAREER: career_screen(); break;
        case S_PLAYERS: players_screen(); break;
        case S_NEWS: news_screen(); break;
        case S_PLAY: play_screen(); break;
        case S_PAUSE: pause_screen(); break;
        case S_RESUME: resume_screen(); break;
        case S_RESULTS: results_screen(); break;
        case S_OPTIONS: options_screen(); break;
        case S_CALIBRATE: calibrate_screen(); break;
        case S_EMPTY: empty_screen(); break;
        case S_DOWNLOAD: download_screen(); break;
        case S_UPDATE: update_screen(); break;
        }
        take_downloads();
        // downloads keep the Vita from sleeping (a second at a time)
        static double awake_t;
        if (now_s - awake_t > 1) {
            awake_t = now_s;
            if (dl_jobs_active())
                plat_keep_awake();
        }
        screen_frames++;
        plat_swap();
    }
    plat_quit();
    return 0;
}
