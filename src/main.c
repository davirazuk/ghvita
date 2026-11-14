#include <ctype.h>
// GH Vita: the screens (title, song list, part and difficulty, the song, pause, results, options, calibration, the
// song downloader)
#include <math.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
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
#include "songconv.h"
#include "net.h"
#include "achieve.h"
#include "tour.h"
#include "career.h"
#include "chart.h"
#include "game.h"
#include "vocals.h"
#include "dl.h"
#include "update.h"

enum { S_TITLE, S_MAIN, S_TOUR, S_SONGS, S_SELECT, S_PRACTICE, S_PLAY, S_PAUSE, S_RESUME, S_RESULTS, S_OPTIONS, S_CALIBRATE, S_EMPTY,
       S_DOWNLOAD, S_UPDATE, S_CAREER, S_NEWS, S_PLAYERS, S_PLAYER_EDIT, S_FACEOFF, S_GIGDONE };

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
enum { SHOW_ALL, SHOW_FAVS, SHOW_NEW, SHOW_STALE, SHOW_UNSTARRED, SHOW_NOGOLD, SHOW_SHORT, SHOW_LONG, SHOW_PART };
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
static int result_medal;                  // a better medal than the chart had, or 0
static bool result_best, result_daily;   // a new best; today's daily song done
static int result_slowed;                 // the Song speed % of a run played slower than the song (it isn't saved), or 0
static int result_faster;                 // the Song speed % of a run played faster (it's saved), or 0
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

// a setlist's running totals, for its summary at the end
static struct {
    int songs, score, stars, hit, total, fcs, best_score;
    char best[96];
} gig;
static double outro_t = -1;          // the song's done: when its closing moment began (-1 none)

static void go(int s)
{
    if (s != S_TITLE && s != S_MAIN)
        menu_music_stop();
    if (s != S_PLAY)
        outro_t = -1;           // a song's closing moment never carries over
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

// a song's best rank from its best accuracy (S+ needs the full combo as well), coloured like the results'
static void draw_rank(const Score * b, float x, float y, float size)
{
    int a = b->acc;
    if (!a)
        return;
    const char * rk = a >= 980 && b->fc ? "S+" : a >= 950 ? "S" : a >= 900 ? "A" : a >= 800 ? "B" : a >= 700 ? "C" : "D";
    uint32_t c = rk[0] == 'S' ? RGBA(255, 214, 70, 255) : rk[0] == 'A' ? RGBA(120, 230, 130, 255) : RGBA(210, 210, 220, 255);
    font_draw_shadow(res.title, rk, x, y, size, c, ALIGN_LEFT);
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
// the menus' highlight on the chosen row: World Tour's bar, breathing, a soft glow sweeping along it
static void select_bar(float x, float y, float w, float h, uint32_t col)
{
    (void)col;
    float b = 0.82f + 0.18f * sinf((float)now_s * 5);
    int v = (int)(255 * b);
    gfx_sprite(atlas_get(res.hud, "wor_select"), x, y, w, h, RGBA(v, v, v, 255));
    float sweep = (float)fmod(now_s * 0.6, 1.6) - 0.3f;
    if (sweep > 0 && sweep < 1) {
        gfx_blend(BLEND_ADD);
        gfx_sprite(atlas_get(res.game, "glow"), x + w * sweep - h, y - h * 0.2f, h * 2, h * 1.4f, RGBA(255, 220, 140, 70));
        gfx_blend(BLEND_ALPHA);
    }
}

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
// FACE-OFF: two players take turns on the same chart. 1: the first is playing; 2: handing over; 3: the second is
// playing; 4: the verdict
static int faceoff, faceoff_p[2], faceoff_part, faceoff_diff, faceoff_pick;
static Result faceoff_r[2];
static int pause_options_at = 2;   // OPTIONS' row in the pause menu (it moves when PRACTICE THIS PART shows)
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
    res_title_art();
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

// a chart's medal from its best: 0 none, 1 bronze (4 stars), 2 silver (95% of the notes), 3 gold (full combo)
enum { MEDAL_NONE, MEDAL_BRONZE, MEDAL_SILVER, MEDAL_GOLD };
static const char * const medal_names[] = {"", "BRONZE", "SILVER", "GOLD"};
static const uint32_t medal_cols[] = {0, RGBA(205, 127, 50, 255), RGBA(200, 205, 215, 255), RGBA(255, 205, 60, 255)};

static int medal_of(const Score * b)
{
    return b->fc ? MEDAL_GOLD : b->percent >= 95 ? MEDAL_SILVER : b->stars >= 4 ? MEDAL_BRONZE : MEDAL_NONE;
}

static void draw_medal(int m, float x, float y, float r)
{
    if (!m)
        return;
    gfx_rect(x - r, y - r, 2 * r, 2 * r, RGBA(0, 0, 0, 160));
    gfx_rect(x - r + 2, y - r + 2, 2 * r - 4, 2 * r - 4, medal_cols[m]);
    char c[2] = {medal_names[m][0], 0};
    font_draw(res.ui, c, x, y - r * 0.8f, r * 1.5f, RGBA(40, 30, 20, 255), ALIGN_CENTER);
}

// a full combo on some part at some difficulty
static bool song_has_gold(const Song * s)
{
    for (int p = 0; p < PART_COUNT; p++)
        for (int d = 0; d < DIFF_COUNT; d++)
            if (s->best[p][d].fc)
                return true;
    return false;
}

// all the medals won, over every song: {bronze, silver, gold}
static void medal_totals(int out[4])
{
    memset(out, 0, 4 * sizeof(int));
    for (int i = 0; i < songs_count(); i++) {
        const Song * s = songs_get(i);
        for (int p = 0; p < PART_COUNT; p++)
            for (int d = 0; d < DIFF_COUNT; d++)
                out[medal_of(&s->best[p][d])]++;
    }
}

// ---------------------------------------------------------------------------------------------------- daily song
// One song a day from the library, the same all day whatever the sort; finishing it on any part keeps a streak going.
// daily.txt: "<day> <streak> <best streak>", the day the last one was finished.

static void open_select(const Song * s);
static void draw_player_face(int i, float x, float top, float w, bool lit);
static void faceoff_screen(void);
static Tex * thumb_get(const Song * s);
static void gigdone_screen(void);
static void player_switch(int i);
static const char * player_title_text(void);

// the time and the battery, top right: a long session on the Vita shouldn't end in a flat battery mid-song
static void status_corner(void)
{
    struct tm now;
    plat_local_time(&now);
    char t[16];
    float x = SCREEN_W - 16;
    bool charging = false;
    int pct = plat_battery(&charging);
    if (pct >= 0) {
        // a battery: its outline, its nub, and how full it is in green, amber or red
        const float bw = 28, bh = 13, by = 13;
        uint32_t fill = charging ? RGBA(120, 200, 255, 255) : pct > 50 ? RGBA(110, 220, 110, 255)
                        : pct > 20 ? RGBA(240, 190, 60, 255) : RGBA(235, 70, 50, 255);
        x -= 3;
        gfx_rect(x, by + 4, 3, bh - 8, RGBA(220, 215, 205, 230));
        x -= bw;
        gfx_rect(x, by, bw, bh, RGBA(220, 215, 205, 230));
        gfx_rect(x + 2, by + 2, bw - 4, bh - 4, RGBA(10, 8, 14, 255));
        gfx_rect(x + 3, by + 3, (bw - 6) * (pct > 100 ? 100 : pct) / 100.0f, bh - 6, fill);
        snprintf(t, sizeof(t), charging ? "%d%%+" : "%d%%", pct);
        x -= 6;
        font_draw_shadow(res.ui, t, x, 8, 18, pct > 20 || charging ? RGBA(225, 220, 210, 255) : RGBA(255, 120, 100, 255),
                         ALIGN_RIGHT);
        x -= font_width(res.ui, t, 18) + 16;
    }
    snprintf(t, sizeof(t), "%d:%02d", now.tm_hour, now.tm_min);
    font_draw_shadow(res.ui, t, x, 8, 18, RGBA(225, 220, 210, 255), ALIGN_RIGHT);
}

// days since 1970 by the local calendar, so the song changes at the player's midnight
static long local_day(void)
{
    struct tm local;
    plat_local_time(&local);
    if (local.tm_year < 70)
        return (long)(time(NULL) / 86400);
    const struct tm * tm = &local;
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
    {
        // two stage lights drifting over the wall, in World Tour's amber and blue
        const Sprite * glow = atlas_get(res.game, "glow");
        float t = (float)now_s;
        gfx_blend(BLEND_ADD);
        for (int k = 0; k < 2; k++) {
            float x = SCREEN_W / 2 + sinf(t * (0.23f + k * 0.11f) + k * 2.4f) * 380;
            float y = 220 + sinf(t * (0.31f + k * 0.07f) + k) * 120, r = 260 + 40 * sinf(t * 0.5f + k);
            gfx_sprite(glow, x - r, y - r, r * 2, r * 2, k ? RGBA(70, 120, 255, 140) : RGBA(255, 160, 60, 140));
        }
        gfx_blend(BLEND_ALPHA);
    }
    if (res.logo)
        gfx_sprite(&(Sprite){res.logo, 0, 0, 1, 1, 512, 370}, 535, 46, 350, 253, WHITE);
    panel(40, 108, 420, 340);
    for (int i = 0; i < n; i++) {
        float y = 118 + i * 41;
        if (i == menu_index)
            select_bar(50, y - 5, 400, 42, WHITE);
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
        // today's song: its cover over the logo's corner, its name under
        const Song * ds = songs_get(daily_pick());
        Tex * th = thumb_get(ds);
        if (th) {
            gfx_rect(646, 186, 128, 128, RGBA(0, 0, 0, 200));
            gfx_sprite(&(Sprite){th, 0, 0, 1, 1, 120, 120}, 650, 190, 120, 120, WHITE);
            gfx_rect(646, 186, 128, 2, RGBA(255, 210, 90, 255));
        }
        char d[240];
        snprintf(d, sizeof(d), "Today: %s - %s", ds->name, ds->artist);
        font_draw_fit(res.ui, d, 710, 318, 20, 420, RGBA(255, 230, 160, 255), ALIGN_CENTER);
    }
    // who's playing: their card, their rank, their stars and medals
    {
        const float x = 500, y = 344, w = 420, h = 96;
        gfx_rect(x, y, w, h, RGBA(0, 0, 0, 150));
        gfx_rect(x, y, w, 2, RGBA(255, 215, 110, 200));
        int me = player_current();
        draw_player_face(me, x + 48, y + 8, 80, true);
        font_draw_fit(res.title, player_name(me), x + 100, y + 10, 30, 300, RGBA(255, 225, 150, 255), ALIGN_LEFT);
        char line[96];
        snprintf(line, sizeof(line), "%s   %d stars", player_title_text(), career.stars);
        font_draw_fit(res.ui, line, x + 100, y + 46, 18, 300, RGBA(215, 210, 200, 255), ALIGN_LEFT);
        int medals[4];
        medal_totals(medals);
        for (int m = MEDAL_GOLD, k = 0; m >= MEDAL_BRONZE; m--, k++) {
            float mx = x + 112 + k * 74;
            draw_medal(m, mx, y + 80, 9);
            snprintf(line, sizeof(line), "%d", medals[m]);
            font_draw(res.ui, line, mx + 14, y + 70, 18, medal_cols[m], ALIGN_LEFT);
        }
        snprintf(line, sizeof(line), "%d/%d", achieve_unlocked(), achieve_count());
        font_draw(res.ui, line, x + w - 14, y + 70, 18, RGBA(255, 210, 90, 255), ALIGN_RIGHT);
    }
    char t[200], now[160];
    snprintf(t, sizeof(t), "%d %s", songs_count(), songs_count() == 1 ? "song" : "songs");
    font_draw_shadow(res.ui, t, 60, 456, 24, RGBA(220, 220, 220, 255), ALIGN_LEFT);
    // which build this is: a card can quietly keep an old one
    font_draw(res.ui, "build " __DATE__ " " __TIME__, SCREEN_W - 20, 490, 16, RGBA(200, 200, 210, 200), ALIGN_RIGHT);
    status_corner();
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
        if (on)
            select_bar(14, y, 580, 54, WHITE);
        else
            gfx_sprite(atlas_get(res.hud, "wor_row"), 14, y, 580, 54, WHITE);
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
        if (on)
            select_bar(14, y, 580, 54, WHITE);
        else
            gfx_sprite(atlas_get(res.hud, "wor_row"), 14, y, 580, 54, WHITE);
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
// the song list's little covers: a few kept by folder, the least lately drawn going first; one loaded a frame at most,
// so scrolling never waits on a pile of JPEGs
#define THUMBS 16
static struct {
    char folder[256];
    Tex * tex;              // NULL: the song has no cover (still remembered, so it isn't looked for again)
    double used;
} thumbs[THUMBS];
static int thumb_loads;     // loaded this frame

static Tex * thumb_get(const Song * s)
{
    int free_at = 0;
    for (int k = 0; k < THUMBS; k++) {
        if (thumbs[k].folder[0] && !strcmp(thumbs[k].folder, s->folder)) {
            thumbs[k].used = now_s;
            return thumbs[k].tex;
        }
        if (thumbs[k].used < thumbs[free_at].used)
            free_at = k;
    }
    if (thumb_loads >= 1)
        return NULL;
    thumb_loads++;
    tex_free(thumbs[free_at].tex);
    char path[400];
    snprintf(path, sizeof(path), "%s/album.jpg", s->folder);
    struct stat st;
    thumbs[free_at].tex = stat(path, &st) == 0 ? tex_load(path, TEX_MIPMAP) : NULL;
    snprintf(thumbs[free_at].folder, sizeof(thumbs[0].folder), "%s", s->folder);
    thumbs[free_at].used = now_s;
    return thumbs[free_at].tex;
}

// chart updates under way: the new chart's id and the folder it takes the place of
#define SWAP_MAX 64
static struct {
    char id[80];
    char old[256];
} swaps[SWAP_MAX];
static int swaps_n;
static bool upd_was_running, upd_repair;

// a new download that's an update: the old folder goes and the new one takes its name, so its scores still count
static void swap_in_update(char * folder, size_t size)
{
    char ini[460], line[200], src[100] = "";
    snprintf(ini, sizeof(ini), "%s/ghvita.ini", folder);
    FILE * f = fopen(ini, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "source = ", 9)) {
            snprintf(src, sizeof(src), "%s", line + 9);
            src[strcspn(src, "\r\n ")] = 0;
        }
    fclose(f);
    for (int i = 0; i < swaps_n; i++) {
        if (strcmp(swaps[i].id, src))
            continue;
        int k = songs_find(swaps[i].old);
        if (k >= 0)
            songs_delete(k);
        else
            dl_remove_tree(swaps[i].old);
        if (rename(folder, swaps[i].old) == 0)
            snprintf(folder, size, "%s", swaps[i].old);
        plat_log("update: %s replaced", swaps[i].old);
        swaps[i] = swaps[--swaps_n];
        return;
    }
}

// the update check: when its worker's done, what it found goes in the download queue
static void updates_poll(void)
{
    int done, total, found;
    bool running = dl_updates_progress(&done, &total, &found);
    if (running || !upd_was_running)
        return;
    upd_was_running = false;
    static DlHit hits[64];
    static char old[64][256];
    int n = dl_updates_take(hits, old, 64), queued = 0;
    if (upd_repair) {
        // the same charts again: each installs over its own folder, scores and all
        upd_repair = false;
        for (int i = 0; i < n; i++)
            queued += dl_queue(&hits[i]) ? 1 : 0;
        char t[96];
        if (!total)
            snprintf(t, sizeof(t), "Every downloaded song's audio is fine");
        else if (!queued)
            snprintf(t, sizeof(t), "Couldn't find those songs online: download them again by hand");
        else
            snprintf(t, sizeof(t), "%d %s downloading again with clean audio%s", queued, queued == 1 ? "song" : "songs",
                     queued < total ? " (some couldn't be found)" : "");
        say(t);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (swaps_n < SWAP_MAX && dl_queue(&hits[i])) {
            snprintf(swaps[swaps_n].id, sizeof(swaps[0].id), "%s", hits[i].id);
            snprintf(swaps[swaps_n].old, sizeof(swaps[0].old), "%s", old[i]);
            swaps_n++;
            queued++;
        }
    }
    char t[96];
    if (!total)
        snprintf(t, sizeof(t), "No songs from Chorus to check");
    else if (!n)
        snprintf(t, sizeof(t), "All %d Chorus songs are up to date", total);
    else
        snprintf(t, sizeof(t), "%d updated %s downloading", queued, queued == 1 ? "chart" : "charts");
    say(t);
}

static void updates_check(void)
{
    static DlUpdateAsk ask[2000];
    int n = 0;
    for (int i = 0; i < songs_count() && n < 2000; i++) {
        const Song * s = songs_get(i);
        if (strncmp(s->source, "chorus:", 7))
            continue;
        DlUpdateAsk * a = &ask[n++];
        snprintf(a->source, sizeof(a->source), "%s", s->source);
        snprintf(a->name, sizeof(a->name), "%s", s->name);
        snprintf(a->artist, sizeof(a->artist), "%s", s->artist);
        snprintf(a->charter, sizeof(a->charter), "%s", s->charter);
        snprintf(a->folder, sizeof(a->folder), "%s", s->folder);
    }
    if (!n) {
        say("No songs from Chorus to check");
        return;
    }
    if (dl_updates_start(ask, n)) {
        upd_was_running = true;
        char t[64];
        snprintf(t, sizeof(t), "Checking %d songs for newer charts", n);
        say(t);
    }
}

// songs whose audio this game converted before v228 (gritty highs, MP3s late): each downloaded again
static void repair_check(void)
{
    static DlUpdateAsk ask[64];
    int n = 0;
    for (int i = 0; i < songs_count() && n < 64; i++) {
        const Song * s = songs_get(i);
        if ((strncmp(s->source, "chorus:", 7) && strncmp(s->source, "rv:", 3)) || !shrink_old(s->folder))
            continue;
        DlUpdateAsk * a = &ask[n++];
        memset(a, 0, sizeof(*a));
        a->repair = true;
        snprintf(a->source, sizeof(a->source), "%s", s->source);
        snprintf(a->name, sizeof(a->name), "%s", s->name);
        snprintf(a->artist, sizeof(a->artist), "%s", s->artist);
        snprintf(a->charter, sizeof(a->charter), "%s", s->charter);
        snprintf(a->folder, sizeof(a->folder), "%s", s->folder);
    }
    if (!n) {
        say("Every downloaded song's audio is fine");
        return;
    }
    if (dl_updates_start(ask, n)) {
        upd_was_running = upd_repair = true;
        char t[80];
        snprintf(t, sizeof(t), "Finding %d %s to download again", n, n == 1 ? "song" : "songs");
        say(t);
    }
}

static void take_downloads(void)
{
    updates_poll();
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
        swap_in_update(folder, sizeof(folder));
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
    // a chart swapped in for an older one keeps the folder: its cover may be new
    for (int t = 0; t < THUMBS; t++) {
        tex_free(thumbs[t].tex);
        memset(&thumbs[t], 0, sizeof(thumbs[t]));
    }
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
        (show == SHOW_NOGOLD && (!s->plays || song_has_gold(s))) ||
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
    if (show == SHOW_NOGOLD)
        return "Played, no gold medal yet";
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
    enum { ROWS = 11 };
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
            memset(&gig, 0, sizeof(gig));
            sel = setlist[0];
            list_menu = false;
            stop_preview();
            open_select(songs_get(sel));
            pressed = repeat = 0;
            return;
        } else if (list_row == 7)
            setlist_n = 0;
        else if (list_row == 8) {
            updates_check();
            list_menu = false;
        } else if (list_row == 10) {
            repair_check();
            list_menu = false;
        } else if (list_row == 9 && view_n) {
            // a gig of five songs picked at random from what the list shows, played as a setlist
            setlist_n = 0;
            for (int tries = 0; setlist_n < 5 && setlist_n < view_n && tries < 200; tries++) {
                int k = view[rand() % view_n];
                if (setlist_find(k) < 0)
                    setlist[setlist_n++] = k;
            }
            setlist_at = 0;
            memset(&gig, 0, sizeof(gig));
            sel = setlist[0];
            list_menu = false;
            stop_preview();
            open_select(songs_get(sel));
            pressed = repeat = 0;
            return;
        }
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
    panel(200, 62, 560, 440);
    char rows[11][64];
    snprintf(rows[0], sizeof(rows[0]), "Sort by   %s", sort_name(cfg.sort));
    snprintf(rows[1], sizeof(rows[1]), "Show   %s", show_name());
    snprintf(rows[2], sizeof(rows[2]), "%s", s && s->fav ? "Take out of favourites" : "Add to favourites");
    snprintf(rows[3], sizeof(rows[3]), "Delete this song");
    snprintf(rows[4], sizeof(rows[4]), "Read the whole card again");
    snprintf(rows[5], sizeof(rows[5]), "%s", setlist_find(sel) >= 0 ? "Take out of the setlist" : "Add to the setlist");
    snprintf(rows[6], sizeof(rows[6]), "Play the setlist (%d %s)", setlist_n, setlist_n == 1 ? "song" : "songs");
    snprintf(rows[7], sizeof(rows[7]), "Clear the setlist");
    int done, total, found;
    if (!upd_repair && dl_updates_progress(&done, &total, &found))
        snprintf(rows[8], sizeof(rows[8]), "Checking for newer charts %d/%d", done, total);
    else
        snprintf(rows[8], sizeof(rows[8]), "Check for newer charts");
    snprintf(rows[9], sizeof(rows[9]), "Random gig: 5 songs from this list");
    if (upd_repair && dl_updates_progress(&done, &total, &found))
        snprintf(rows[10], sizeof(rows[10]), "Finding songs to download again %d/%d", done, total);
    else
        snprintf(rows[10], sizeof(rows[10]), "Repair downloaded songs' audio");
    for (int i = 0; i < 11; i++) {
        float y = 74 + i * 38.5f;
        if (i == list_row)
            select_bar(224, y - 4, 512, 37, WHITE);
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
    if (art && art_for == sel) {
        // the chosen song's cover, huge and soft behind the list, fading in as it changes
        static int cover_for = -1;
        static double cover_t;
        if (cover_for != sel) {
            cover_for = sel;
            cover_t = now_s;
        }
        float a = (float)((now_s - cover_t) / 0.4);
        a = a > 1 ? 1 : a;
        gfx_sprite(&(Sprite){art, 0, 0, 1, 1, 256, 256}, 0, (SCREEN_H - SCREEN_W) / 2, SCREEN_W, SCREEN_W,
                   RGBA(255, 255, 255, (int)(110 * a)));
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 120));
    }
    banner("QUICKPLAY");
    char t[160];
    if (find_text[0])
        snprintf(t, sizeof(t), "\"%s\"   %d of %d", find_text, n, songs_count());
    else if (show != SHOW_ALL)
        snprintf(t, sizeof(t), "%s   %d of %d", show_name(), n ? pos + 1 : 0, n);
    else
        snprintf(t, sizeof(t), "%d / %d   by %s", n ? pos + 1 : 0, n, sort_name(cfg.sort));
    font_draw_fit(res.ui, t, SCREEN_W - 24, 30, 24, 420, RGBA(230, 230, 230, 255), ALIGN_RIGHT);
    // the list: each row with its cover
    thumb_loads = 0;
    for (int r = 0; r < rows && top + r < n; r++) {
        int i = view[top + r];
        Song * x = songs_get(i);
        float y = 104 + r * 56;
        bool on = top + r == pos;
        if (on)
            select_bar(14, y, 580, 54, WHITE);
        else
            gfx_sprite(atlas_get(res.hud, "wor_row"), 14, y, 580, 54, WHITE);
        Tex * th = thumb_get(x);
        gfx_rect(26, y + 5, 44, 44, RGBA(0, 0, 0, 160));
        if (th)
            gfx_sprite(&(Sprite){th, 0, 0, 1, 1, 44, 44}, 26, y + 5, 44, 44, WHITE);
        else if (res.logo)
            gfx_sprite(&(Sprite){res.logo, 0, 0, 1, 1, 512, 370}, 29, y + 13, 38, 27, RGBA(150, 150, 150, 200));
        char up[128];
        upper(up, sizeof(up), x->name);
        font_draw_fit(res.ui, up, 82, y + 4, 26, 480, on ? RGBA(255, 236, 170, 255) : RGBA(235, 235, 235, 255),
                      ALIGN_LEFT);
        font_draw_fit(res.ui, x->artist, 84, y + 28, 20, x->best[first_part(x)][cfg.last_diff].acc ? 340 : 370,
                      on ? RGBA(255, 210, 140, 255) : RGBA(170, 170, 175, 255),
                      ALIGN_LEFT);
        const Score * b = &x->best[first_part(x)][cfg.last_diff];
        if (b->stars)
            draw_stars(b->stars, 470, y + 30, 18);
        draw_medal(medal_of(b), 572, y + 39, 9);
        draw_rank(b, 440, y + 24, 22);
        if (x->fav)
            gfx_sprite(atlas_get(res.hud, "hud_star_gold"), 56, y + 34, 18, 18, WHITE);
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
// the last gig of a leg ends on a boss battle: its encore against a rival who's tougher every two legs
static bool tour_boss(void)
{
    return tour_on && tour_slot == TOUR_ENCORE && tour_gig == TOUR_GIGS - 1;
}

static int tour_boss_level(void)
{
    return tour_leg / 2 > 3 ? 3 : tour_leg / 2;
}

static void draw_select(const Song * s, bool loading)
{
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(150, 150, 150, 255));
    banner("GET READY");
    // left: the song, the part and its difficulties
    panel(30, 100, 560, 380);
    font_draw_fit(res.title, s->name, 310, 116, 42, 510, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    font_draw_fit(res.ui, s->artist, 310, 164, 24, 510, RGBA(230, 230, 230, 255), ALIGN_CENTER);
    char t[96];
    snprintf(t, sizeof(t), "<  %s  >", part_name(sel_part));
    font_draw_shadow(res.title, t, 310, 198, 40, RGBA(255, 255, 255, 255), ALIGN_CENTER);
    for (int d = 0; d < DIFF_COUNT; d++) {
        float y = 254 + d * 46;
        bool on = d == sel_diff, has = sel_diffs[sel_part] & (1 << d);
        if (on)
            select_bar(60, y - 4, 500, 44, WHITE);
        font_draw_shadow(res.ui, diff_name(d), 90, y, 30,
                         on ? RGBA(255, 230, 150, 255) : has ? RGBA(190, 190, 190, 255) : RGBA(85, 80, 75, 255),
                         ALIGN_LEFT);
        if (!has)
            font_draw(res.ui, "not charted", 330, y + 6, 22, RGBA(85, 80, 75, 255), ALIGN_LEFT);
        const Score * b = &s->best[sel_part][d];
        if (b->stars)
            draw_stars(b->stars, 330, y + 4, 24);
        draw_medal(medal_of(b), 532, y + 16, 13);
        draw_rank(b, 566, y + 2, 28);
    }
    if (cfg.song_speed != 100) {
        // a slowed song isn't saved: said before it's played, not after
        snprintf(t, sizeof(t), cfg.song_speed < 100 ? "Song speed %d%%: this run won't be saved" : "Song speed %d%%",
                 cfg.song_speed);
        font_draw(res.ui, t, 310, 443, 18, cfg.song_speed < 100 ? RGBA(255, 160, 90, 255) : RGBA(255, 225, 150, 255),
                  ALIGN_CENTER);
    } else
        font_draw(res.ui, "Medals:  Bronze 4 stars   Silver 95%   Gold full combo", 310, 444, 17, RGBA(190, 185, 175, 255),
                  ALIGN_CENTER);
    // right: the cover, the song's details and the chosen chart's best
    panel(610, 100, 320, 380);
    draw_cover(art_for == sel ? art : NULL, 680, 116, 180);
    float y = 306;
    if (s->album[0] || s->year[0]) {
        snprintf(t, sizeof(t), "%s%s%s", s->album, s->album[0] && s->year[0] ? ", " : "", s->year);
        font_draw_fit(res.ui, t, 770, y, 18, 290, RGBA(210, 205, 195, 255), ALIGN_CENTER);
        y += 22;
    }
    int secs = s->length_ms / 1000;
    if (s->charter[0])
        snprintf(t, sizeof(t), "%d:%02d   charted by %s", secs / 60, secs % 60, s->charter);
    else
        snprintf(t, sizeof(t), "%d:%02d", secs / 60, secs % 60);
    font_draw_fit(res.ui, t, 770, y, 18, 290, RGBA(180, 178, 172, 255), ALIGN_CENTER);
    const Score * b = &s->best[sel_part][sel_diff];
    gfx_rect(632, 360, 276, 1, RGBA(255, 215, 110, 120));
    snprintf(t, sizeof(t), "%s %s", part_name(sel_part), diff_name(sel_diff));
    font_draw_fit(res.ui, t, 770, 368, 20, 280, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    if (b->score) {
        snprintf(t, sizeof(t), "Best %d", b->score);
        font_draw_shadow(res.title, t, 770, 394, 30, WHITE, ALIGN_CENTER);
        snprintf(t, sizeof(t), "%d%% of notes%s", b->percent, b->fc ? ", full combo" : "");
        font_draw_fit(res.ui, t, 770, 432, 18, 280, b->fc ? RGBA(255, 210, 60, 255) : RGBA(210, 205, 195, 255),
                      ALIGN_CENTER);
    } else
        font_draw(res.ui, "Not played yet", 770, 404, 22, RGBA(180, 178, 172, 255), ALIGN_CENTER);
    {
        // the song's speed, changed right here with L and R
        snprintf(t, sizeof(t), "Speed %d%%", cfg.song_speed);
        float tw = font_width(res.ui, t, 18), bx = 770 - tw / 2;
        const Sprite * l = atlas_get(res.hud, "btn_l"), * r = atlas_get(res.hud, "btn_r");
        float lw = l->w * 20.0f / l->h, rw = r->w * 20.0f / r->h;
        uint32_t c = cfg.song_speed < 100 ? RGBA(255, 160, 90, 255) : cfg.song_speed > 100 ? RGBA(255, 225, 150, 255)
                                                                                        : RGBA(170, 168, 162, 255);
        gfx_sprite(l, bx - lw - 8, 455, lw, 20, WHITE);
        font_draw(res.ui, t, bx, 454, 18, c, ALIGN_LEFT);
        gfx_sprite(r, bx + tw + 8, 455, rw, 20, WHITE);
    }
    if (setlist_at >= 0 && setlist_n > 1 && !tour_on) {
        char t2[48];
        snprintf(t2, sizeof(t2), "Setlist: song %d of %d", setlist_at + 1, setlist_n);
        font_draw_fit(res.ui, t2, SCREEN_W / 2, 74, 22, 560, RGBA(255, 200, 120, 255), ALIGN_CENTER);
    }
    if (tour_on) {
        char t2[80];
        snprintf(t2, sizeof(t2), "%s%s", tour_boss() ? "Boss battle at " : tour_slot == TOUR_ENCORE ? "Encore at " : "Live at ",
                 res_venue_title(tour_zone(tour_leg, tour_gig)));
        font_draw_fit(res.ui, t2, SCREEN_W / 2, 84, 24, 560, RGBA(255, 200, 120, 255), ALIGN_CENTER);
    }
    if (loading) {
        gfx_rect(0, 480, SCREEN_W, 64, RGBA(0, 0, 0, 200));
        font_draw_shadow(res.title, "LOADING", SCREEN_W / 2, 482, 30, RGBA(255, 220, 120, 255), ALIGN_CENTER);
        font_draw(res.ui, res_tip(false), SCREEN_W / 2, 516, 17, RGBA(220, 215, 205, 255), ALIGN_CENTER);
    } else {
        const char * help[16] = {"btn_cross", "Rock!", "btn_circle", "Back", "btn_left", "Part", "btn_up", "Difficulty",
                                 "btn_square", "Practice"};
        int nh = 10;
        if (sel_part != PART_VOCALS && !tour_on)
            help[nh++] = "btn_triangle", help[nh++] = "Battle";
        if (player_count() > 1 && !tour_on)
            help[nh++] = "btn_select", help[nh++] = "Face-off";
        help_bar(help, nh);
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

static int clampi(int v, int lo, int hi);

// the settings rows of practice's last step, as what each is: Up to only while it speeds up
enum { PR_START, PR_SPEED, PR_RAMP, PR_TARGET, PR_METRO };
static int pr_rows(int kinds[5])
{
    int n = 0;
    kinds[n++] = PR_START;
    kinds[n++] = PR_SPEED;
    kinds[n++] = PR_RAMP;
    if (pr_ramp)
        kinds[n++] = PR_TARGET;
    kinds[n++] = PR_METRO;
    return n;
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
        // Start, Speed, Speed up, Up to (only while speeding up), Metronome
        int kinds[5], rows = pr_rows(kinds), was_row = pr_set_row, sp = pr_speed, ra = pr_ramp, ta = pr_target;
        bool me = cfg.metronome;
        if (repeat & BTN_UP)
            pr_set_row = (pr_set_row + rows - 1) % rows;
        if (repeat & BTN_DOWN)
            pr_set_row = (pr_set_row + 1) % rows;
        int dir = (repeat & BTN_LEFT) ? -1 : (repeat & BTN_RIGHT) ? 1 : 0;
        int kind = kinds[pr_set_row];
        if (kind == PR_SPEED)
            pr_speed += 5 * dir;
        else if (kind == PR_RAMP)
            pr_ramp = (pr_ramp / 5 + dir + 3) % 3 * 5;     // off, +5%, +10%
        else if (kind == PR_TARGET)
            pr_target += 5 * dir;
        else if (kind == PR_METRO && dir)
            cfg.metronome = !cfg.metronome;
        pr_speed = pr_speed < 50 ? 50 : pr_speed > 150 ? 150 : pr_speed;
        pr_target = pr_target < 55 ? 55 : pr_target > 150 ? 150 : pr_target;
        if (pr_ramp && pr_target <= pr_speed)
            pr_target = pr_speed + 5 > 150 ? 150 : pr_speed + 5;
        rows = pr_rows(kinds);
        if (pr_set_row >= rows)
            pr_set_row = rows - 1;
        if (me != cfg.metronome)
            config_save();
        moved = pr_set_row != was_row || sp != pr_speed || ra != pr_ramp || ta != pr_target || me != cfg.metronome;
        if (pressed & BTN_CROSS) {
            if (pr_set_row == 0) {
                start = true;
            } else if (kinds[pr_set_row] == PR_METRO) {
                cfg.metronome = !cfg.metronome;
                config_save();
                ui_sound(SND_UI_MOVE);
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
            // how dense the section is (notes a second against the song's busiest): the hard parts stand out
            float secs = (float)(pr[i + 1].t - pr[i].t), dens = secs > 0.5f ? pr[i].notes / secs : 0, top = 0.01f;
            for (int k = 0; k < pr_count; k++) {
                float sk = (float)(pr[k + 1].t - pr[k].t);
                if (sk > 0.5f && pr[k].notes / sk > top)
                    top = pr[k].notes / sk;
            }
            float f = dens / top;
            uint32_t dc = f > 0.8f ? RGBA(240, 80, 60, 255) : f > 0.5f ? RGBA(245, 180, 60, 255) : RGBA(110, 210, 110, 255);
            gfx_rect(430, y + 44, 140, 4, RGBA(0, 0, 0, 160));
            gfx_rect(430, y + 44, 140 * f, 4, dc);
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
    const char * labels[] = {"START", "Speed", "Speed up", "Up to", "Metronome"};
    char vals[5][24] = {""};
    snprintf(vals[PR_SPEED], sizeof(vals[1]), "< %d%% >", pr_speed);
    snprintf(vals[PR_RAMP], sizeof(vals[2]), pr_ramp ? "< +%d%% a clear >" : "< Off >", pr_ramp);
    snprintf(vals[PR_TARGET], sizeof(vals[3]), "< %d%% >", pr_target);
    snprintf(vals[PR_METRO], sizeof(vals[4]), "< %s >", cfg.metronome ? "On" : "Off");
    int kinds[5], rows = pr_rows(kinds);
    const float gap = rows > 4 ? 34 : 40;
    for (int r = 0; r < rows; r++) {
        float y = 318 + r * gap;
        bool cur = pr_step == PR_STEP_SET && r == pr_set_row;
        if (cur)
            select_bar(622, y - 4, 312, gap - 2, WHITE);
        uint32_t col = pr_step != PR_STEP_SET ? RGBA(150, 150, 150, 255) : cur ? RGBA(255, 236, 170, 255) : RGBA(220, 220, 220, 255);
        if (kinds[r] == PR_START)
            font_draw_shadow(res.title, labels[0], 778, y - 2, gap > 36 ? 32 : 28, col, ALIGN_CENTER);
        else {
            font_draw(res.ui, labels[kinds[r]], 636, y + (gap - 32) / 2 + 1, 20, col, ALIGN_LEFT);
            font_draw(res.ui, vals[kinds[r]], 924, y + (gap - 32) / 2 + 1, 20, col, ALIGN_RIGHT);
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
    if (art_for != sel)
        load_art(s);            // straight here from the Daily Song or a setlist: the cover isn't loaded yet
    if (select_starting) {
        // the chart, the highway picture and the stems take the Vita a second or so: the screen says so meanwhile
        select_starting = false;
        res_tip(true);
        if (tour_boss() && sel_part != PART_VOCALS)
            game_battle(tour_boss_level());
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
    if (repeat & (BTN_L | BTN_R)) {
        // the song's speed, 5% a press (saved with the rest when the song starts)
        int was = cfg.song_speed;
        cfg.song_speed = clampi(cfg.song_speed + (repeat & BTN_L ? -5 : 5), 50, 150);
        if (cfg.song_speed != was)
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
    if ((pressed & BTN_SELECT) && !tour_on && player_count() > 1) {
        // a face-off: this player now, someone else after
        ui_sound(SND_UI_SELECT);
        faceoff = 1;
        faceoff_p[0] = player_current();
        faceoff_part = sel_part;
        faceoff_diff = sel_diff;
        select_starting = true;
    }
    if ((pressed & BTN_TRIANGLE) && sel_part != PART_VOCALS && !tour_on) {
        // a battle with a computer rival on this chart
        stop_preview();
        ui_sound(SND_UI_SELECT);
        game_battle(-1);
        if (game_start(s, sel_part, sel_diff)) {
            practicing = false;
            game_set_autoplay(autoplay);
            go(S_PLAY);
            return;
        }
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
    // a run counts (scores, medals, the career, the tour, achievements) at the song's own speed or faster, and not in
    // practice: a practised stretch, or a slowed song, would otherwise go down as the chart's best
    result_slowed = !practicing && game_song_speed() < 0.995f ? (int)(game_song_speed() * 100 + 0.5f) : 0;
    result_faster = !practicing && game_song_speed() > 1.005f ? (int)(game_song_speed() * 100 + 0.5f) : 0;
    bool counts = !autoplay && !practicing && !result_slowed;
    if (setlist_at >= 0 && !tour_on) {
        gig.songs++;
        gig.score += result.failed ? 0 : result.score;
        gig.stars += result.failed ? 0 : result.stars;
        gig.hit += result.notes_hit;
        gig.total += result.notes_total;
        gig.fcs += result.fc;
        Song * gs = songs_get(sel);
        if (gs && result.score > gig.best_score) {
            gig.best_score = result.score;
            snprintf(gig.best, sizeof(gig.best), "%s", gs->name);
        }
    }
    if (tour_on && !result.failed && result.battle != 2 && !result_slowed)     // a boss battle lost isn't a gig played
        tour_played();
    plat_log("result: %s, score %d, %d stars, %d/%d notes, best streak %d, %d/%d phrases, %d overhits%s; hits %+d ms "
             "on average, spread %d", how == GAME_DONE ? "done" : "failed", result.score, result.stars,
             result.notes_hit, result.notes_total, result.best_streak, result.sp_hit, result.sp_total, result.overhits,
             result.fc ? ", full combo" : "", result.off_ms, result.spread_ms);
    if (result.rank[0])
        plat_log("accuracy: %d.%d%%, rank %s, %d perfect, %d great, %d good", result.accuracy_x10 / 10, result.accuracy_x10 % 10,
                 result.rank, result.perfects, result.greats, result.goods);
    game_log_whammy();
    Song * s = songs_get(sel);
    result_best = false;
    result_medal = 0;
    if (s && !result.failed) {
        Score sc = {result.score, (uint8_t)result.stars,
                    (uint8_t)(result.notes_total ? result.notes_hit * 100 / result.notes_total : 0), result.fc,
                    (uint16_t)result.accuracy_x10};
        result_best = counts && result.score > s->best[result.part][result.diff].score;
        int had = medal_of(&s->best[result.part][result.diff]), got = medal_of(&sc);
        result_medal = got > had && counts ? got : 0;
        if (counts)
            songs_save_score(s, result.part, result.diff, sc);
    }
    result_daily = s && !result.failed && counts && daily_finished(s);
    hist_n = s && !practicing ? songs_history(s, result.part, result.diff, hist, HIST_MAX) : 0;
    hist_has_this = s && !result.failed && counts;
    if (result.battle)
        plat_log("battle: %s, you %d, %s %d (%d%% of notes), attacks thrown %d, taken %d", result.battle == 1 ? "won" : "lost",
                 result.score, result.rival_name, result.rival_score, result.rival_pct, result.attacks_thrown, result.attacks_taken);
    results_history = false;
    if (counts && result.battle)
        career_battle(result.battle == 1);
    if (counts)
        career_add(result.part, result.score, result.stars, result.notes_hit, result.notes_total, result.best_streak,
                   result.overhits, result.played_s, result.failed, result.fc);
    AchPlay ap = {result.part, result.diff, result.stars, result.score, result.best_streak, result.overhits, result.sp_hit,
                  result.sp_total, result.notes_total ? result.notes_hit * 100 / result.notes_total : 0, result.failed,
                  result.fc, practicing || result_slowed, autoplay, daily_streak(), songs_count(), result.battle, result.rival_level,
                  (int)(game_song_speed() * 100 + 0.5f), result.accuracy_x10, result.perfects, cfg.sudden_death, cfg.hard_rock,
                  cfg.hit_window};
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

static bool skip_draw;             // this frame wasn't drawn: the main loop lets it pass without a swap

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
    } else if (r == GAME_DONE && outro_t < 0) {
        // the last note's rung out: a moment for the crowd before the results
        outro_t = now_s;
        sound_play(res.snd[SND_CHEER1 + rand() % 3], cfg.crowd_vol * 0.1f, false);
    }
    if (r == GAME_FAILED || (r == GAME_DONE && now_s - outro_t > 1.1)) {
        outro_t = -1;
        finish_song(r);
        pressed = repeat = 0;
        results_screen();
        return;
    }
    // 30 fps: every other frame isn't drawn (the buttons were still read and judged in it)
    if (cfg.frame_rate == 30 && (screen_frames & 1) && outro_t < 0) {
        skip_draw = true;
        return;
    }
    game_draw();
    if (outro_t >= 0) {
        float k = (float)((now_s - outro_t) / 1.1);
        k = k > 1 ? 1 : k;
        gfx_2d();
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, (int)(200 * k * k)));
        float sz = 64 + 20 * (1 - (k < 0.2f ? k / 0.2f : 1));
        int bs = game_battle_state();
        font_draw_shadow(res.title, bs == 1 ? "YOU WIN!" : bs ? "TIME'S UP!" : "SONG COMPLETE!", SCREEN_W / 2, 200, sz,
                         fade_alpha(RGBA(255, 220, 120, 255), k < 0.15f ? k / 0.15f : 1), ALIGN_CENTER);
        gfx_end();
    }
}

static void pause_screen(void)
{
    // RESUME, RESTART, PRACTICE THIS PART (a named section, out of practice and the tour), OPTIONS, QUIT
    enum { P_RESUME, P_RESTART, P_PART, P_OPTIONS, P_QUIT };
    char part[48];
    game_section_now(part, sizeof(part));
    section_title(part);
    int ids[5], n = 0;
    ids[n++] = P_RESUME;
    ids[n++] = P_RESTART;
    if (part[0] && !practicing && !tour_on)
        ids[n++] = P_PART;
    ids[n++] = P_OPTIONS;
    ids[n++] = P_QUIT;
    static const char * const names[] = {"RESUME", "RESTART", "PRACTICE THIS PART", "OPTIONS", "QUIT"};
    const char * items[5];
    for (int i = 0; i < n; i++)
        items[i] = ids[i] == P_QUIT && practicing ? "PRACTICE MENU" : names[ids[i]];
    if (menu_index >= n)
        menu_index = 0;
    for (int i = 0; i < n; i++)
        if (ids[i] == P_OPTIONS)
            pause_options_at = i;      // where Options comes back to
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
        int id = ids[menu_index];
        if (id == P_RESUME)
            resume = true;
        else if (id == P_RESTART) {
            game_pause(false);
            game_restart();
            go(S_PLAY);
            return;
        } else if (id == P_PART) {
            // straight into practice on the section the song was in, at the speed practice had last
            // (not game_result: half a run mustn't be kept as the chart's best ghost)
            Song * s = songs_get(sel);
            game_playing(&sel_part, &sel_diff);
            game_stop();
            setlist_at = -1;
            if (s)
                read_sections(s);
            for (int i = 0; i < pr_count; i++)
                if (!strcmp(pr[i].name, part))
                    pr_from = pr_to = pr_focus = i;
            pr_step = PR_STEP_SET;
            pr_set_row = 0;
            go(pr_count ? S_PRACTICE : S_SELECT);
            return;
        } else if (id == P_OPTIONS) {
            options_from = S_PAUSE;
            go(S_OPTIONS);
            return;
        } else {
            game_stop();
            setlist_at = -1;
            if (faceoff) {
                // a face-off called off mid-song: back to whoever started it
                player_switch(faceoff_p[0]);
                faceoff = 0;
            }
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
    // the rows close up a little when there are five
    float step = n > 4 ? 44 : 54, size = n > 4 ? 34 : 40;
    for (int i = 0; i < n; i++) {
        float y = 192 + i * step;
        if (i == menu_index)
            select_bar(SCREEN_W / 2 - 170, y - 4, 340, step - 4, WHITE);
        uint32_t col = i == menu_index ? RGBA(255, 230, 150, 255) : RGBA(210, 200, 190, 255);
        if (font_width(res.title, items[i], size) > 320)
            font_draw_fit(res.title, items[i], SCREEN_W / 2, y, size, 320, col, ALIGN_CENTER);
        else
            font_draw_shadow(res.title, items[i], SCREEN_W / 2, y, size, col, ALIGN_CENTER);
    }
    // where in the song: the section, and a bar, on a plate of their own so they read over the highway's cups
    gfx_rect_grad(SCREEN_W / 2 - 200, 402, 400, 72, RGBA(12, 8, 18, 230), RGBA(4, 2, 8, 240));
    gfx_rect(SCREEN_W / 2 - 200, 402, 400, 2, RGBA(210, 170, 90, 210));
    gfx_rect(SCREEN_W / 2 - 200, 472, 400, 2, RGBA(210, 170, 90, 120));
    if (part[0])
        font_draw_fit(res.ui, part, SCREEN_W / 2, 408, 18, 360, RGBA(255, 215, 110, 255), ALIGN_CENTER);
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
    font_draw_fit(res.ui, line, SCREEN_W / 2, 429, 20, 380, RGBA(225, 215, 200, 255), ALIGN_CENTER);
    gfx_rect(SCREEN_W / 2 - 171, 454, 342, 10, RGBA(70, 60, 50, 255));
    gfx_rect(SCREEN_W / 2 - 170, 455, 340, 8, RGBA(0, 0, 0, 255));
    gfx_rect_grad(SCREEN_W / 2 - 170, 455, 340 * song_pct / 100.0f, 8, RGBA(255, 220, 120, 255), RGBA(220, 140, 40, 255));
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
    if ((pressed & (BTN_CROSS | BTN_CIRCLE)) && (faceoff == 1 || faceoff == 3)) {
        // a face-off's turn is over: on to the hand-over, or the verdict
        ui_sound(SND_UI_SELECT);
        faceoff_r[faceoff == 1 ? 0 : 1] = result;
        if (faceoff == 1) {
            faceoff = 2;
            faceoff_pick = (faceoff_p[0] + 1) % player_count();
        } else
            faceoff = 4;
        go(S_FACEOFF);
        return;
    }
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
        bool was_gig = setlist_at >= 0 && setlist_n > 1 && !tour_on && gig.songs > 1;
        setlist_at = -1;
        go(was_gig ? S_GIGDONE : from_list());
        sel_t = now_s;
        if (tour_on)
            tour_next();
        return;
    }
    if ((pressed & BTN_SQUARE) && s && results_can_practice()) {
        ui_sound(SND_UI_SELECT);
        if (faceoff) {
            player_switch(faceoff_p[0]);
            faceoff = 0;
        }
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
        if (result.battle)
            game_battle(result.rival_level);    // a battle's retry is a battle again, at the same rival level
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
    if (result.battle)
        font_draw_fit(res.title, result.battle == 1 ? "YOU WIN!" : "RIVAL WINS", SCREEN_W / 2, 48, 52, 820,
                      result.battle == 1 ? RGBA(120, 230, 120, 255) : RGBA(255, 90, 70, 255), ALIGN_CENTER);
    else
        font_draw_fit(res.title, result.failed ? "SONG FAILED" : s ? s->name : "", SCREEN_W / 2, 48, 52, 820,
                      result.failed ? RGBA(255, 90, 70, 255) : RGBA(255, 220, 140, 255), ALIGN_CENTER);
    if (s) {
        // failed, the big line doesn't name the song
        char who[200];
        snprintf(who, sizeof(who), "%s  -  %s", s->name, s->artist);
        if (result.battle) {
            // head to head: a bar each, as long as its score, the winner's lit
            int top = result.score > result.rival_score ? result.score : result.rival_score;
            if (top < 1)
                top = 1;
            const float bx = 330, bw = 420;
            for (int k = 0; k < 2; k++) {
                bool me = k == 0;
                int sc = me ? result.score : result.rival_score;
                bool won = me == (result.battle == 1);
                float y = 98 + k * 22;
                font_draw_fit(res.ui, me ? "You" : result.rival_name, bx - 12, y - 2, 18, 160,
                              me ? RGBA(150, 215, 255, 255) : RGBA(255, 160, 140, 255), ALIGN_RIGHT);
                gfx_rect(bx, y, bw, 14, RGBA(0, 0, 0, 170));
                gfx_rect(bx, y, bw * sc / top, 14, me ? RGBA(70, 190, 255, won ? 255 : 150)
                                                     : RGBA(235, 70, 55, won ? 255 : 150));
                snprintf(who, sizeof(who), "%d", sc);
                font_draw(res.ui, who, bx + bw + 10, y - 2, 18, WHITE, ALIGN_LEFT);
            }
            snprintf(who, sizeof(who), "%s hit %d%% of notes   attacks thrown %d, taken %d", result.rival_name,
                     result.rival_pct, result.attacks_thrown, result.attacks_taken);
            font_draw_fit(res.ui, who, SCREEN_W / 2, 464, 17, 820, RGBA(200, 195, 185, 255), ALIGN_CENTER);
        } else
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
        // a tick as each star lands
        static int ticked;
        static double ticked_for;
        if (ticked_for != screen_t) {
            ticked_for = screen_t;
            ticked = 0;
        }
        if (shown > ticked && shown < 6) {
            ticked = shown;
            sound_play(res.snd[SND_STAR], 0.35f, false);
        }
        draw_stars(shown >= 6 || (result.stars >= 6 && a >= 1) ? 6 : shown, 460, 136, 56);
        if (result.rank[0] && result.part != PART_VOCALS && a >= 1) {
            // the rank lands once the stars are in
            uint32_t rc = result.rank[0] == 'S' ? RGBA(255, 214, 70, 255) : result.rank[0] == 'A' ? RGBA(120, 230, 130, 255)
                          : result.rank[0] == 'F' ? RGBA(255, 80, 60, 255) : RGBA(220, 220, 230, 255);
            font_draw_shadow(res.title, result.rank, 395, 112, 72, rc, ALIGN_CENTER);
        }
    }
    int pct = result.notes_total ? result.notes_hit * 100 / result.notes_total : 0;
    struct {
        const char * label;
        char value[48];
    } rows[6];
    {
        // the score counts up as the stars come
        float a = (float)((now_s - screen_t) / 1.2);
        a = a > 1 ? 1 : a < 0 ? 0 : a;
        a = 1 - (1 - a) * (1 - a) * (1 - a);
        snprintf(rows[0].value, sizeof(rows[0].value), "%d", (int)(result.score * a));
    }
    rows[0].label = "Score";
    snprintf(rows[1].value, sizeof(rows[1].value), "%d%%  (%d / %d)", pct, result.notes_hit, result.notes_total);
    rows[1].label = "Notes hit";
    if (result.overhits)
        snprintf(rows[2].value, sizeof(rows[2].value), "%d   (%d overhit%s)", result.best_streak, result.overhits,
                 result.overhits == 1 ? "" : "s");
    else
        snprintf(rows[2].value, sizeof(rows[2].value), "%d", result.best_streak);
    rows[2].label = "Best streak";
    snprintf(rows[3].value, sizeof(rows[3].value), "%d / %d", result.sp_hit, result.sp_total);
    rows[3].label = "Star Power phrases";
    snprintf(rows[4].value, sizeof(rows[4].value), "%d.%d%%   P %d  G %d  OK %d", result.accuracy_x10 / 10,
             result.accuracy_x10 % 10, result.perfects, result.greats, result.goods);
    rows[4].label = "Accuracy";
    rows[5].label = "Your timing";
    if (!result.timed)
        strcpy(rows[5].value, "-");
    else {
        // how many hits were within 30 ms, and which way the rest leaned
        int all = result.early + result.on_time + result.late, on = all ? result.on_time * 100 / all : 0;
        if (abs(result.off_ms) < 5)
            snprintf(rows[5].value, sizeof(rows[5].value), "%d%% on time, right on it", on);
        else
            snprintf(rows[5].value, sizeof(rows[5].value), "%d%% on time, %d ms %s", on, abs(result.off_ms),
                     result.off_ms > 0 ? "late" : "early");
    }
    int nrows = 6;
    if (result.part == PART_VOCALS) {
        // a singer's results: phrases, not notes; no frets, so no overhits, Star Power notes or timing
        rows[1].label = "Phrases sung";
        rows[2].label = "Best run of phrases";
        nrows = 3;
    }
    for (int i = 0; i < nrows; i++) {
        float y = 200 + i * 31;
        font_draw_shadow(res.ui, rows[i].label, 330, y, 28, RGBA(220, 210, 200, 255), ALIGN_LEFT);
        font_draw_shadow(res.ui, rows[i].value, 870, y, 28, RGBA(255, 236, 190, 255), ALIGN_RIGHT);
    }
    bool strip = false;
    for (int i = 0; i < RESULT_STRIP; i++)
        strip |= result.strip[i] != 255;
    if (result.timed && !results_history && result.part != PART_VOCALS) {
        // early, on time, late: a thin bar under the timing row
        int all = result.early + result.on_time + result.late;
        if (all) {
            const float x = 330, w = 540, y = 386;
            float e = w * result.early / all, o = w * result.on_time / all;
            gfx_rect(x, y, e, 5, RGBA(110, 170, 255, 255));
            gfx_rect(x + e, y, o, 5, RGBA(110, 220, 120, 255));
            gfx_rect(x + e + o, y, w - e - o, 5, RGBA(255, 150, 90, 255));
        }
    }
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
    if (result.fc || result_best || result.battle == 1) {
        // a celebration: confetti falling over the results for the first few seconds
        static const uint32_t cols[5] = {RGBA(255, 214, 90, 255), RGBA(255, 96, 190, 255), RGBA(100, 225, 255, 255),
                                         RGBA(176, 120, 255, 255), RGBA(255, 255, 255, 255)};
        float since = (float)(now_s - screen_t);
        float fade = since > 4 ? (5 - since) : 1;
        for (int i = 0; fade > 0 && i < 70; i++) {
            uint32_t h = (uint32_t)i * 2654435761u;
            float tau = since - (h >> 8 & 127) / 100.0f;
            if (tau < 0)
                continue;
            float x = (h >> 14 & 1023) * 0.94f + sinf(tau * 3 + i) * 20, y = -20 + tau * (140 + (h >> 24 & 127));
            float w = 6 + (i % 3) * 3, ht = 10 - (i % 3) * 2 + 4 * fabsf(sinf(tau * 6 + i));
            gfx_rect(x, y, w, ht, fade_alpha(cols[i % 5], fade));
        }
    }
    if (result.vox_graded) {
        // two playing: the singer's share, apart from the instrument's
        char d[96];
        snprintf(d, sizeof(d), "Vocals: %d points, %d of %d phrases strong   %s: %d", result.vox_points, result.vox_good,
                 result.vox_graded, part_name(result.part), result.score - result.vox_points);
        font_draw_fit(res.ui, d, SCREEN_W / 2, 470, 20, 800, RGBA(150, 210, 255, 255), ALIGN_CENTER);
    }
    if (result_medal) {
        char d[48];
        snprintf(d, sizeof(d), "%s MEDAL!", medal_names[result_medal]);
        // stamped on once the stars are in: big, then settling
        float a = (float)((now_s - screen_t - 1.3) / 0.25);
        if (a > 0) {
            float k = a > 1 ? 1 : a, sz = 16 * (2.2f - 1.2f * k);
            draw_medal(result_medal, 70, 54, sz);
            font_draw_shadow(res.title, d, 96, 38, 32, fade_alpha(medal_cols[result_medal], k), ALIGN_LEFT);
        }
    }
    for (int i = 0; i < result_ach_n && i < 2; i++) {
        // what this song unlocked, under the stars
        char d[96];
        snprintf(d, sizeof(d), "UNLOCKED: %s%s", achieve_get(result_ach[i])->name,
                 i == 1 && result_ach_n > 2 ? " and more" : "");
        font_draw_shadow(res.ui, d, SCREEN_W - 90, 76 + i * 22, 20, RGBA(255, 210, 90, 255), ALIGN_RIGHT);
    }
    // under the cover, two lines: out of the way of the stars and the rows
    if (result_slowed) {
        char d[48];
        snprintf(d, sizeof(d), "SONG SPEED %d%%", result_slowed);
        font_draw_shadow(res.ui, d, 185, 398, 22, RGBA(255, 160, 90, 255), ALIGN_CENTER);
        font_draw_shadow(res.ui, "Not saved", 185, 424, 20, RGBA(255, 190, 140, 255), ALIGN_CENTER);
    } else if (result_faster && !result_daily) {
        char d[48];
        snprintf(d, sizeof(d), "SONG SPEED %d%%", result_faster);
        font_draw_shadow(res.ui, d, 185, 398, 22, RGBA(255, 225, 150, 255), ALIGN_CENTER);
    }
    if (result_daily) {
        char d[48];
        snprintf(d, sizeof(d), "%d day%s in a row", daily_streak(), daily_streak() == 1 ? "" : "s");
        font_draw_shadow(res.ui, "DAILY SONG DONE", 185, 398, 22, RGBA(130, 230, 130, 255), ALIGN_CENTER);
        font_draw_shadow(res.ui, d, 185, 424, 20, RGBA(170, 235, 170, 255), ALIGN_CENTER);
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
            select_bar(30, y - 6, 376, 50, tour_in_set ? RGBA(150, 150, 150, 255) : WHITE);
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
                select_bar(444, y - 8, 484, 74, WHITE);
            const Song * s = tour_ids[i] >= 0 ? songs_get(tour_ids[i]) : NULL;
            uint32_t c = locked ? RGBA(120, 115, 110, 255) : on ? RGBA(255, 230, 150, 255) : RGBA(225, 220, 212, 255);
            if (i == TOUR_ENCORE)
                font_draw(res.ui, tour_gig == TOUR_GIGS - 1 ? "BOSS BATTLE" : "ENCORE", 900, y - 2, 18,
                          tour_gig == TOUR_GIGS - 1 ? RGBA(255, 120, 90, 255) : RGBA(255, 200, 120, 255), ALIGN_RIGHT);
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

// the graphics presets: 0 Full, 1 Balanced, 2 Battery; 3 when the settings match none of them
static int graphics_preset(void)
{
    if (cfg.frame_rate != 30 && !cfg.still_camera && !cfg.light_fx && cfg.fire == 0)
        return 0;
    if (cfg.frame_rate != 30 && !cfg.still_camera && cfg.light_fx && cfg.fire == 1)
        return 1;
    if (cfg.frame_rate == 30 && cfg.still_camera && cfg.light_fx && cfg.fire == 1)
        return 2;
    return 3;
}

static void set_graphics_preset(int p)
{
    cfg.frame_rate = p == 2 ? 30 : 60;
    cfg.still_camera = p == 2;
    cfg.light_fx = p != 0;
    cfg.fire = p == 0 ? 0 : 1;
}

enum { O_SPEED, O_HIGHWAY, O_VENUE, O_BAND, O_SING, O_SINGDIFF, O_OFFSET, O_LAYOUT, O_DRUMKEYS, O_DRUMHITS, O_TILT, O_NOFAIL, O_LEFTY, O_MUSIC, O_SFX, O_CROWD, O_FPS,
       O_CAMERA, O_HWALPHA, O_VLIGHT, O_FLASH, O_POPUPS, O_TIMING, O_LIVE, O_VANISH, O_MISSSND, O_REAR, O_BEND, O_COUNTDOWN,
       O_RESET, O_FIVEPADS, O_DOUBLEKICK, O_EASYSUS, O_COUNTIN, O_SCREENS, O_PROGRESS, O_CUPS, O_AUTOREPRESS, O_FLATNOTES, O_NOTESIZE, O_WOBBLE, O_FIRE, O_CROWDHANDS, O_VIGNETTE, O_LYRICS, O_MENUMUSIC, O_SINGSP, O_SHRINKCHECK, O_GHOST, O_RIVAL, O_WINDOW, O_LIGHTFX, O_SONGSPEED,
       O_FRAMERATE, O_STILLCAM, O_PRESET,
       O_BACKDROP, O_RATINGS, O_HWLEN, O_SUDDEN, O_HARDROCK, O_ENGINE, O_STEPS, O_COUNT };

// the options by subject, L and R between them
enum { OPT_TABS = 8, OPT_TAB_MAX = 18 };
static const char * opt_tab_names[OPT_TABS] = {"GAME", "CONTROLS", "HIGHWAY", "STAGE", "SCREEN", "GRAPHICS", "SOUND", "LIBRARY"};
static const int opt_tabs[OPT_TABS][OPT_TAB_MAX + 1] = {
    {O_SPEED, O_SONGSPEED, O_OFFSET, O_WINDOW, O_ENGINE, O_STEPS, O_SUDDEN, O_HARDROCK, O_NOFAIL, O_EASYSUS, O_AUTOREPRESS, O_VANISH, O_COUNTDOWN, O_SING, O_SINGDIFF, O_RIVAL, O_RESET, -1},
    {O_LAYOUT, O_DRUMKEYS, O_FIVEPADS, O_DOUBLEKICK, O_LEFTY, O_TILT, O_REAR, O_SINGSP, -1},
    {O_HIGHWAY, O_HWALPHA, O_CAMERA, O_CUPS, O_FLATNOTES, O_NOTESIZE, O_WOBBLE, O_FIRE, -1},
    {O_VENUE, O_VLIGHT, O_BAND, O_CROWDHANDS, O_VIGNETTE, O_SCREENS, O_FLASH, -1},
    {O_LYRICS, O_POPUPS, O_PROGRESS, O_TIMING, O_LIVE, O_GHOST, O_LIGHTFX, O_FPS, -1},
    {O_PRESET, O_FRAMERATE, O_FPS, O_BACKDROP, O_RATINGS, O_HWLEN, O_STILLCAM, O_LIGHTFX, O_BAND, O_FIRE, O_FLASH, O_CROWDHANDS, O_VIGNETTE, -1},
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
    case O_LIGHTFX: strcpy(name, "Effects"); snprintf(value, n, "%s", cfg.light_fx ? "Light" : "Full"); break;
    case O_FRAMERATE: strcpy(name, "Frame rate"); snprintf(value, n, "%d fps", cfg.frame_rate == 30 ? 30 : 60); break;
    case O_STILLCAM: strcpy(name, "Venue camera"); snprintf(value, n, "%s", cfg.still_camera ? "Wide shot only" : "Cuts between shots"); break;
    case O_BACKDROP: {
        static const char * const bn[3] = {"Venue and band", "Song's cover art", "Dark"};
        strcpy(name, "Background");
        snprintf(value, n, "%s", bn[cfg.backdrop % 3]);
        break;
    }
    case O_RATINGS: {
        static const char * const rn[3] = {"Off", "On", "On, with ms"};
        strcpy(name, "Hit ratings");
        snprintf(value, n, "%s", rn[cfg.hit_ratings % 3]);
        break;
    }
    case O_HWLEN: {
        static const char * const ln[3] = {"Short", "Normal", "Long"};
        strcpy(name, "Highway length");
        snprintf(value, n, "%s", ln[cfg.hw_length % 3]);
        break;
    }
    case O_SUDDEN: strcpy(name, "Sudden death"); snprintf(value, n, "%s", cfg.sudden_death ? "On" : "Off"); break;
    case O_HARDROCK: strcpy(name, "Hard rock meter"); snprintf(value, n, "%s", cfg.hard_rock ? "On" : "Off"); break;
    case O_ENGINE: strcpy(name, "Engine"); snprintf(value, n, "%s", cfg.engine ? "Guitar Flash" : "World Tour"); break;
    case O_STEPS: strcpy(name, "Note motion"); snprintf(value, n, "%s", cfg.note_steps ? "24 fps (Flash)" : "Smooth"); break;
    case O_PRESET: {
        static const char * const pn[] = {"Full", "Balanced", "Battery", "Custom"};
        strcpy(name, "Graphics preset");
        snprintf(value, n, "%s", pn[graphics_preset()]);
        break;
    }
    case O_SONGSPEED:
        strcpy(name, "Song speed");
        snprintf(value, n, cfg.song_speed < 100 ? "%d%%  (not saved)" : "%d%%", cfg.song_speed);
        break;
    case O_WINDOW: {
        static const char * const wn[5] = {"Normal", "Strict", "Lenient", "Pro", "Guitar Hero 3"};
        strcpy(name, "Timing window");
        snprintf(value, n, "%s", wn[cfg.hit_window % 5]);
        break;
    }
    case O_RIVAL: {
        static const char * const lv[4] = {"Easy", "Normal", "Hard", "Brutal"};
        strcpy(name, "Battle rival");
        snprintf(value, n, "%s", lv[cfg.rival & 3]);
        break;
    }
    case O_GHOST: strcpy(name, "Race your best"); snprintf(value, n, "%s", cfg.no_ghost ? "Off" : "On"); break;
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
    case O_GHOST: cfg.no_ghost = !cfg.no_ghost; break;
    case O_RIVAL: cfg.rival = (cfg.rival + d + 4) % 4; break;
    case O_WINDOW: cfg.hit_window = (cfg.hit_window + d + 5) % 5; break;
    case O_LIGHTFX: cfg.light_fx = !cfg.light_fx; break;
    case O_FRAMERATE: cfg.frame_rate = cfg.frame_rate == 30 ? 60 : 30; break;
    case O_STILLCAM: cfg.still_camera = !cfg.still_camera; break;
    case O_BACKDROP: cfg.backdrop = (cfg.backdrop + d + 3) % 3; break;
    case O_RATINGS: cfg.hit_ratings = (cfg.hit_ratings + d + 3) % 3; break;
    case O_HWLEN: cfg.hw_length = (cfg.hw_length + d + 3) % 3; break;
    case O_SUDDEN: cfg.sudden_death = !cfg.sudden_death; break;
    case O_HARDROCK: cfg.hard_rock = !cfg.hard_rock; break;
    case O_ENGINE: cfg.engine = !cfg.engine; break;
    case O_STEPS: cfg.note_steps = !cfg.note_steps; break;
    case O_PRESET: {
        int p = graphics_preset();
        set_graphics_preset(((p > 2 ? (d > 0 ? -1 : 3) : p) + d + 3) % 3);
        break;
    }
    case O_SONGSPEED: cfg.song_speed = clampi(cfg.song_speed + 5 * d, 50, 150); break;
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
            menu_index = back == S_MAIN ? 4 : pause_options_at;    // back on OPTIONS
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(170, 170, 170, 255));
    banner("OPTIONS");
    panel(110, 96, 740, 356);
    // the tabs spaced by their widths across the panel, the current one in a lit box
    {
        const float size = 19, left = 128, room = 704;
        float widths[OPT_TABS], total = 0;
        for (int i = 0; i < OPT_TABS; i++)
            total += widths[i] = font_width(res.ui, opt_tab_names[i], size);
        float gap = (room - total) / OPT_TABS, x = left + gap / 2;
        for (int i = 0; i < OPT_TABS; i++) {
            bool cur = i == opt_tab;
            if (cur) {
                gfx_rect(x - 8, 104, widths[i] + 16, 26, RGBA(255, 215, 110, 60));
                gfx_rect(x - 8, 128, widths[i] + 16, 2, RGBA(255, 215, 110, 255));
            }
            font_draw(res.ui, opt_tab_names[i], x, 107, size, cur ? RGBA(255, 225, 140, 255) : RGBA(160, 155, 150, 255),
                      ALIGN_LEFT);
            x += widths[i] + gap;
        }
    }
    enum { ROWS = 9 };
    if (menu_index < opt_top)
        opt_top = menu_index;
    if (menu_index >= opt_top + ROWS)
        opt_top = menu_index - ROWS + 1;
    for (int r = 0; r < ROWS && opt_top + r < count; r++) {
        int i = opt_tabs[opt_tab][opt_top + r];
        char name[48], value[48];
        option_text(i, name, value, sizeof(value));
        float y = 140 + r * 32;
        bool on = opt_top + r == menu_index;
        if (on)
            select_bar(130, y - 3, 700, 34, WHITE);
        uint32_t c = on ? RGBA(255, 230, 150, 255) : RGBA(220, 215, 205, 255);
        font_draw(res.ui, name, 160, y + 1, 26, c, ALIGN_LEFT);
        // the volumes and percentages show as a bar as well
        int bar = i == O_MUSIC ? cfg.music_vol : i == O_SFX ? cfg.sfx_vol : i == O_CROWD ? cfg.crowd_vol
                  : i == O_HWALPHA ? cfg.highway_alpha : i == O_VLIGHT ? cfg.venue_light : i == O_SPEED ? cfg.speed : -1;
        if (bar >= 0) {
            gfx_rect(548, y + 13, 170, 8, RGBA(0, 0, 0, 170));
            gfx_rect(549, y + 14, 168 * bar / 10.0f, 6, on ? RGBA(255, 215, 110, 255) : RGBA(170, 150, 110, 255));
        }
        font_draw_fit(res.ui, value, 800, y + 1, 26, bar >= 0 ? 66 : 380, c, ALIGN_RIGHT);
    }
    if (opt == O_VENUE && strcmp(cfg.venue, "song") && strcmp(cfg.venue, "random") && strcmp(cfg.venue, "classic")) {
        // the chosen venue, small, in the panel's corner under the rows
        Tex * v = res_venue_preview(cfg.venue);
        if (v) {
            const float w = 128, h = 73, x = 820 - w, y = 372;
            gfx_rect(x - 3, y - 3, w + 6, h + 6, RGBA(255, 210, 90, 255));
            gfx_sprite(&(Sprite){v, 0, 0, 1, 1, 960, 544}, x, y, w, h, WHITE);
        }
    }
    if (opt == O_HIGHWAY) {
        // the highway as it'll look: a strip of it running away into the distance beside the panel
        Tex * hw = res_highway_preview(cfg.highway);
        if (hw) {
            gfx_tex(hw);
            const float x = 906, top = 130, bot = 440, wn = 50, wf = 20;
            const float p[4][3] = {{x - wf, top, 0}, {x + wf, top, 0}, {x + wn, bot, 0}, {x - wn, bot, 0}};
            const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            const uint32_t c[4] = {RGBA(150, 150, 150, 120), RGBA(150, 150, 150, 120), RGBA(225, 225, 225, 255),
                                   RGBA(225, 225, 225, 255)};
            gfx_quad(p, uv, c);
            gfx_tex(NULL);
            // silver rails, the strikeline, and a few notes coming down it
            for (int side = -1; side <= 1; side += 2) {
                const float r[4][3] = {{x + side * wf - 1, top, 0}, {x + side * wf + 1, top, 0}, {x + side * wn + 1.5f, bot, 0},
                                       {x + side * wn - 1.5f, bot, 0}};
                const uint32_t rc[4] = {RGBA(230, 230, 240, 80), RGBA(230, 230, 240, 80), WHITE, WHITE};
                gfx_quad(r, uv, rc);
            }
            gfx_rect(x - wn, bot - 22, wn * 2, 3, RGBA(235, 235, 245, 220));
            static const float gy[3] = {0.35f, 0.6f, 0.85f};
            static const int gl[3] = {3, 1, 0};
            for (int k = 0; k < 3; k++) {
                float fy = top + (bot - 30 - top) * gy[k], half = wf + (wn - wf) * gy[k];
                float gx = x - half + half * 2 * (gl[k] + 0.5f) / 5, gw = half * 2 / 5 * 0.95f;
                char gname[20];
                snprintf(gname, sizeof(gname), "gem_normal_%d", gl[k]);
                gfx_sprite(atlas_get(res.game, gname), gx - gw / 2, fy - gw * 0.25f, gw, gw * 0.5f, WHITE);
            }
        }
    }
    // there's more above or below
    if (opt_top > 0)
        font_draw(res.ui, "...", SCREEN_W / 2, 116, 22, RGBA(220, 215, 205, 255), ALIGN_CENTER);
    if (opt_top + ROWS < count)
        font_draw(res.ui, "...", SCREEN_W / 2, 432, 22, RGBA(220, 215, 205, 255), ALIGN_CENTER);
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
        "GHOST +1200: how far ahead (or behind) your best run on the chart you were at this point.",
        "How good the computer rival is in a battle (Triangle on GET READY).",
        "How early or late a note can be hit: Normal 85 ms, Strict 70, Lenient 110, Pro 55, or GH3's own 116 early / 100 late.",
        "Light drops the hit rings, Star Power glitter, smoke, band backlight, lasers and sparks, for a steadier frame rate.",
        "The song's speed. Below 100% the run isn't saved (scores, medals, the tour); practice has its own speed.",
        "30 fps draws every other frame and saves power; your buttons are still read 60 times a second, so timing stays exact.",
        "Cuts between each venue's four camera angles with the song, or keeps the wide shot (loads quicker, less memory).",
        "Full: everything. Balanced: lighter effects at 60 fps. Battery: 30 fps, wide shot only, lighter effects.",
        "Behind the highway: the venue with the band, the song's cover art, or plain dark.",
        "PERFECT (25 ms), GREAT (50 ms) or GOOD over the strikeline as you hit. With ms adds how early (-) or late (+).",
        "How far up the highway you can see the notes coming. Long gives more time to read fast songs.",
        "One missed note and the song's over. For the brave.",
        "The rock meter falls twice as fast when you miss: keep the crowd with you.",
        "Guitar Flash's rules on guitar and bass: its timing per difficulty, every stray press is an error, chords note by note.",
        "24 fps moves the notes in Guitar Flash's 24 steps a second, at any frame rate. Smooth moves them every frame.",
    };
    char line[160];
    const char * text = opt >= 0 && opt < O_COUNT && about[opt] ? about[opt] : "";   // a gap in the table: no help, no crash
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
    // what the option does, on up to two lines
    float tw = font_width(res.ui, text, 20);
    if (tw <= 840)
        font_draw(res.ui, text, SCREEN_W / 2, 460, 20, RGBA(200, 200, 200, 255), ALIGN_CENTER);
    else
        draw_wrapped(text, 60, 452, 18, 840, 2, RGBA(200, 200, 200, 255));
    static const char * help[] = {"btn_left", "Change", "btn_l", "Tab", "btn_r", "Tab", "btn_circle", "Done"};
    help_bar(help, 8);
    gfx_end();
}

// -------------------------------------------------------------------------------------------------------- career

// --------------------------------------------------------------------------------------------------------- players
// who's playing: Select on the main menu. Each has their own scores, favourites, settings, career, tour and streak.


static void player_reload(void)
{
    config_load();
    career_load();
    achieve_load();
    tour_load();
    songs_reload_player();
    songs_sort(cfg.sort);
    daily.loaded = false;
    view_stale = true;
    art_for = preview_for = -1;
}

// a player's title: an achievement of theirs they show on their card, by its id in title.txt ("" for their rank)
static void player_title_get(int i, char * out, int size)
{
    char d[400], p[460];
    player_dir_of(i, d, sizeof(d));
    snprintf(p, sizeof(p), "%stitle.txt", d);
    out[0] = 0;
    FILE * f = fopen(p, "r");
    if (f) {
        if (fgets(out, size, f))
            out[strcspn(out, "\r\n")] = 0;
        fclose(f);
    }
}

static void player_title_set(int i, const char * id)
{
    char d[400], p[460];
    player_dir_of(i, d, sizeof(d));
    snprintf(p, sizeof(p), "%stitle.txt", d);
    FILE * f = fopen(p, "w");
    if (f) {
        fprintf(f, "%s\n", id);
        fclose(f);
    }
}

// the title's words: the achievement's name, or the career rank when none is chosen (or it isn't theirs any more)
static const char * player_title_text(void)
{
    static char id[40];
    player_title_get(player_current(), id, sizeof(id));
    for (int k = 0; id[0] && k < achieve_count(); k++)
        if (!strcmp(achieve_get(k)->id, id) && achieve_has(k))
            return achieve_get(k)->name;
    int next;
    return career_rank(&next);
}

// another player's stars, read from their own career file (the loaded career is only the current player's)
static int player_stars_of(int i)
{
    char d[400], p[460], line[96];
    player_dir_of(i, d, sizeof(d));
    snprintf(p, sizeof(p), "%scareer.txt", d);
    FILE * f = fopen(p, "r");
    int stars = 0;
    if (f) {
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "stars = %d", &stars) == 1)
                break;
        fclose(f);
    }
    return stars;
}

// after a delete: the current player may be someone else now
static void player_switch_force(void)
{
    player_select(player_current());
    player_reload();
}

static void player_switch(int i)
{
    if (i == player_current())
        return;
    player_select(i);
    player_reload();
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

// each card's picture, loaded again whenever a picture changes
static Tex * pic_tex[PLAYER_MAX];
static int pic_tex_version = -1;

static Tex * player_tex(int i)
{
    if (pic_tex_version != player_picture_version()) {
        for (int k = 0; k < PLAYER_MAX; k++) {
            tex_free(pic_tex[k]);
            pic_tex[k] = NULL;
        }
        for (int k = 0; k < player_count(); k++) {
            char p[460];
            player_picture(k, p, sizeof(p));
            if (p[0])
                pic_tex[k] = tex_load(p, TEX_MIPMAP);
        }
        pic_tex_version = player_picture_version();
    }
    return i >= 0 && i < PLAYER_MAX ? pic_tex[i] : NULL;
}

// a player's card face: their picture, or their initial on their colour
static void draw_player_face(int i, float x, float top, float w, bool lit)
{
    const uint32_t * c = player_tones[player_color(i)];
    gfx_rect_grad(x - w / 2, top, w, w, c[0], c[1]);
    Tex * t = player_tex(i);
    if (t)
        gfx_sprite(&(Sprite){t, 0, 0, 1, 1, w, w}, x - w / 2, top, w, w, WHITE);
    else {
        char initial[2] = {player_name(i)[0], 0};
        if (initial[0] >= 'a' && initial[0] <= 'z')
            initial[0] -= 32;
        font_draw_shadow(res.title, initial, x, top + w * 0.16f, w * 0.68f, lit ? WHITE : RGBA(170, 170, 170, 255),
                         ALIGN_CENTER);
    }
    if (!lit)
        gfx_rect(x - w / 2, top, w, w, RGBA(0, 0, 0, 90));
}

static int edit_player;     // whose card the edit screen has
static int pe_row;
static bool pe_delete_armed;

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
    if ((pressed & BTN_SQUARE) && players_row < n) {
        ui_sound(SND_UI_SELECT);
        edit_player = players_row;
        pe_row = 0;
        pe_delete_armed = false;
        go(S_PLAYER_EDIT);
        return;
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
            draw_player_face(i, x, top, w, cur);
            font_draw_fit(res.ui, player_name(i), x, top + w + 14, cur ? 30 : 24, gap - 16,
                          cur ? RGBA(255, 236, 190, 255) : RGBA(200, 200, 200, 255), ALIGN_CENTER);
            // their stars, so the cards tell apart at a glance; who played last
            if (cur) {
                static int stars_for = -1, stars_n;
                if (stars_for != i) {
                    stars_for = i;
                    stars_n = player_stars_of(i);
                }
                char st[32];
                snprintf(st, sizeof(st), "%d stars", stars_n);
                font_draw(res.ui, st, x, top + w + 50, 18, RGBA(255, 215, 110, 255), ALIGN_CENTER);
            }
            if (i == player_current())
                font_draw(res.ui, "last played", x, top + w + (cur ? 72 : 50), 16, RGBA(130, 230, 130, 255), ALIGN_CENTER);
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
        static const char * help[] = {"btn_cross", "Play", "btn_left", "Choose", "btn_square", "Edit", "btn_triangle", "Colour",
                                      "btn_circle", "Back"};
        help_bar(help, 10);
    } else {
        static const char * help[] = {"btn_cross", "Add", "btn_left", "Choose", "btn_circle", "Back"};
        help_bar(help, 6);
    }
    gfx_end();
}

// ----------------------------------------------------------------------------------------------- editing a player
// Square on a card: the name, the colour, a picture (one of your songs' covers, one downloaded, or one of yours from
// the card's avatars folder), or the player gone.

enum { PE_NAME, PE_COLOR, PE_TITLE, PE_GH, PE_SONGS, PE_WEB, PE_CARD, PE_CLEAR, PE_DELETE, PE_ROWS };
static int pe_cover = -1, pe_card, pe_gh;
static char pe_ghs[48][64];                // the game's own Guitar Hero pictures (assets/avatars)
static int pe_gh_n = -1;
static Tex * pe_prev;                   // the picture the row offers, before it's taken
static char pe_prev_path[460];
static char pe_files[64][128];          // the avatars folder's pictures
static int pe_file_n = -1;
// album covers off Chorus: a search (typed), its covers, the one shown
#define PE_COVERS 40
static char pe_query[96], pe_cover_url[PE_COVERS][160], pe_cover_label[PE_COVERS][120];
static volatile int pe_cover_n, pe_search_state;    // 0 none yet, 1 searching, 2 done
static int pe_cv;

static void * pe_search_thread(void * arg)
{
    (void)arg;
    pe_cover_n = dl_cover_search(pe_query, pe_cover_url, pe_cover_label, PE_COVERS);
    pe_search_state = 2;
    return NULL;
}

static void pe_search(const char * text)
{
    if (pe_search_state == 1 || !text[0])
        return;
    snprintf(pe_query, sizeof(pe_query), "%s", text);
    pe_cover_n = pe_cv = 0;
    pe_search_state = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, pe_search_thread, NULL) != 0)
        pe_search_state = 2;
    else
        pthread_detach(t);
}

// the download: a worker fetches a cover into the preview file
static volatile int pe_web_state;       // 0 idle, 1 fetching, 2 ready, 3 failed
static char pe_web_url[200];
static int pe_web_shown = -1;           // which (style, seed) the preview is of

static void * pe_web_thread(void * arg)
{
    (void)arg;
    char * body = NULL, err[160];
    size_t len = 0;
    HttpReq r = {.url = pe_web_url, .stall_s = 15};
    int status = http_fetch(&r, &body, &len, err, sizeof(err));
    bool ok = status == 200 && len > 8 &&
              (!memcmp(body, "\x89PNG", 4) || ((unsigned char)body[0] == 0xFF && (unsigned char)body[1] == 0xD8));
    if (ok) {
        char p[460];
        snprintf(p, sizeof(p), "%stmp_cover.img", plat_data_dir());
        FILE * f = fopen(p, "wb");
        ok = f && fwrite(body, 1, len, f) == len;
        if (f)
            fclose(f);
    } else
        plat_log("avatar: %s answered %d %s", pe_web_url, status, err);
    free(body);
    pe_web_state = ok ? 2 : 3;
    return NULL;
}

static void pe_web_fetch(void)
{
    if (pe_web_state == 1)
        return;
    if (pe_cv >= pe_cover_n)
        return;
    snprintf(pe_web_url, sizeof(pe_web_url), "%s", pe_cover_url[pe_cv]);
    pe_web_state = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, pe_web_thread, NULL) != 0)
        pe_web_state = 3;
    else
        pthread_detach(t);
}

static void pe_show(const char * path)
{
    if (!strcmp(path, pe_prev_path) && pe_prev)
        return;
    tex_free(pe_prev);
    pe_prev = path[0] ? tex_load(path, TEX_MIPMAP) : NULL;
    snprintf(pe_prev_path, sizeof(pe_prev_path), "%s", path);
}

static void pe_list_gh(void)
{
    pe_gh_n = 0;
    char d[400];
    snprintf(d, sizeof(d), "%savatars", plat_asset_dir());
    DIR * dir = opendir(d);
    if (!dir)
        return;
    struct dirent * e;
    while ((e = readdir(dir)) && pe_gh_n < 48) {
        const char * x = strrchr(e->d_name, '.');
        if (x && !strcasecmp(x, ".jpg"))
            snprintf(pe_ghs[pe_gh_n++], sizeof(pe_ghs[0]), "%s", e->d_name);
    }
    closedir(dir);
    qsort(pe_ghs, pe_gh_n, sizeof(pe_ghs[0]), (int (*)(const void *, const void *))strcmp);
}

static void pe_list_files(void)
{
    pe_file_n = 0;
    char d[400];
    snprintf(d, sizeof(d), "%savatars", plat_data_dir());
    mkdir(d, 0777);
    DIR * dir = opendir(d);
    if (!dir)
        return;
    struct dirent * e;
    while ((e = readdir(dir)) && pe_file_n < 64) {
        const char * x = strrchr(e->d_name, '.');
        if (x && (!strcasecmp(x, ".png") || !strcasecmp(x, ".jpg") || !strcasecmp(x, ".jpeg")))
            snprintf(pe_files[pe_file_n++], sizeof(pe_files[0]), "%s", e->d_name);
    }
    closedir(dir);
}

// the next song (dir 1) or the one before with a cover
static void pe_next_cover(int dir)
{
    int n = songs_count();
    for (int k = 0; k < n; k++) {
        pe_cover = ((pe_cover < 0 ? (dir > 0 ? -1 : 0) : pe_cover) + dir + n) % n;
        char p[460];
        snprintf(p, sizeof(p), "%s/album.jpg", songs_get(pe_cover)->folder);
        struct stat st;
        if (stat(p, &st) == 0)
            return;
    }
    pe_cover = -1;
}

static bool pe_take_file(const char * path, bool normalise)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char * b = n > 0 && n < 16 * 1024 * 1024 ? malloc(n) : NULL;
    bool ok = b && fread(b, 1, n, f) == (size_t)n;
    fclose(f);
    if (ok && normalise) {
        // a picture of any size from the card: a 256x256 JPEG, as the song covers are
        char tmp[460];
        snprintf(tmp, sizeof(tmp), "%stmp_avatar.jpg", plat_data_dir());
        ok = songconv_cover(b, n, tmp);
        free(b);
        return ok && pe_take_file(tmp, false);
    }
    ok = ok && player_set_picture(edit_player, b, (int)n);
    free(b);
    return ok;
}

static void pe_renamed(const char * name)
{
    if (!player_rename(edit_player, name))
        say("That name's empty or taken");
}

static void player_edit_screen(void)
{
    int me = edit_player;
    if (me < 0 || me >= player_count()) {
        go(S_PLAYERS);
        return;
    }
    if (dl_typing) {
        kb_update();
        pressed = repeat = 0;
    }
    if (pe_file_n < 0)
        pe_list_files();
    if (pe_gh_n < 0)
        pe_list_gh();
    int was = pe_row;
    if (repeat & BTN_UP)
        pe_row = (pe_row + PE_ROWS - 1) % PE_ROWS;
    if (repeat & BTN_DOWN)
        pe_row = (pe_row + 1) % PE_ROWS;
    if (pe_row != was) {
        ui_sound(SND_UI_MOVE);
        pe_delete_armed = false;
    }
    int d = (repeat & BTN_LEFT) ? -1 : (repeat & BTN_RIGHT) ? 1 : 0;
    if (d) {
        ui_sound(SND_UI_MOVE);
        if (pe_row == PE_COLOR)
            player_set_color(me, player_color(me) + d);
        else if (pe_row == PE_TITLE) {
            // through the achievements this player has, and back to their rank
            char cur[40];
            player_title_get(me, cur, sizeof(cur));
            int n = achieve_count(), at = -1;
            for (int k = 0; k < n; k++)
                if (!strcmp(achieve_get(k)->id, cur))
                    at = k;
            for (int step = 0; step <= n; step++) {
                at += d;
                if (at < -1)
                    at = n - 1;
                if (at >= n)
                    at = -1;
                if (at == -1 || achieve_has(at))
                    break;
            }
            player_title_set(me, at < 0 ? "" : achieve_get(at)->id);
        }
        else if (pe_row == PE_GH && pe_gh_n > 0)
            pe_gh = (pe_gh + d + pe_gh_n) % pe_gh_n;
        else if (pe_row == PE_SONGS)
            pe_next_cover(d);
        else if (pe_row == PE_WEB && pe_cover_n > 0) {
            pe_cv = (pe_cv + d + pe_cover_n) % pe_cover_n;
            pe_web_shown = -1;
        } else if (pe_row == PE_CARD && pe_file_n > 0)
            pe_card = (pe_card + d + pe_file_n) % pe_file_n;
    }
    bool search_now = (pressed & BTN_TRIANGLE) && pe_row == PE_WEB;
    if ((pressed & BTN_CROSS) && pe_row == PE_WEB && pe_cover_n == 0 && pe_search_state != 1) {
        search_now = true;      // nothing found yet: X searches
        pressed &= ~BTN_CROSS;
    }
    if (search_now) {
        // a song, an artist or an album: its covers come back to browse
        ui_sound(SND_UI_SELECT);
        snprintf(dl_typed, sizeof(dl_typed), "%s", pe_query);
        kb_apply = pe_search;
        kb_library = false;
        dl_typing = true;
    }
    // what the row offers, in the preview
    char offer[460] = "";
    if (pe_row == PE_GH && pe_gh_n > 0)
        snprintf(offer, sizeof(offer), "%savatars/%s", plat_asset_dir(), pe_ghs[pe_gh % pe_gh_n]);
    else if (pe_row == PE_SONGS) {
        if (pe_cover < 0)
            pe_next_cover(1);
        if (pe_cover >= 0)
            snprintf(offer, sizeof(offer), "%s/album.jpg", songs_get(pe_cover)->folder);
    } else if (pe_row == PE_WEB) {
        int want = pe_search_state * 1000 + pe_cv;
        if (pe_cover_n > 0 && pe_web_shown != want && pe_web_state != 1) {
            pe_web_fetch();
            pe_web_shown = want;
        }
        if (pe_web_state == 2 && pe_cover_n > 0)
            snprintf(offer, sizeof(offer), "%stmp_cover.img", plat_data_dir());
    } else if (pe_row == PE_CARD && pe_file_n > 0)
        snprintf(offer, sizeof(offer), "%savatars/%s", plat_data_dir(), pe_files[pe_card % pe_file_n]);
    // a new download lands in the same file: load it again
    if (pe_row == PE_WEB && pe_web_state == 2 && pe_web_shown >= 0) {
        static int loaded = -1;
        if (loaded != pe_web_shown) {
            pe_prev_path[0] = 0;
            loaded = pe_web_shown;
        }
    }
    pe_show(offer);
    if (pressed & BTN_CROSS) {
        ui_sound(SND_UI_SELECT);
        bool ok = true;
        switch (pe_row) {
        case PE_NAME:
            snprintf(dl_typed, sizeof(dl_typed), "%s", player_name(me));
            kb_apply = pe_renamed;
            kb_library = false;
            dl_typing = true;
            break;
        case PE_COLOR: player_set_color(me, player_color(me) + 1); break;
        case PE_GH:
        case PE_SONGS: ok = offer[0] && pe_take_file(offer, false); break;
        case PE_WEB: ok = pe_web_state == 2 && offer[0] && pe_take_file(offer, true); break;
        case PE_CARD: ok = offer[0] && pe_take_file(offer, true); break;
        case PE_CLEAR: player_clear_picture(me); break;
        case PE_DELETE:
            if (player_count() <= 1) {
                say("The last player can't go");
                break;
            }
            if (!pe_delete_armed) {
                pe_delete_armed = true;
                break;
            }
            {
                bool was_current = me == player_current();
                char gone[PLAYER_NAME + 16];
                snprintf(gone, sizeof(gone), "%s is gone", player_name(me));
                player_delete(me);
                if (was_current) {
                    // whoever's left first plays now
                    player_switch_force();
                }
                say(gone);
                pe_delete_armed = false;
                players_row = player_current();
                go(S_PLAYERS);
                return;
            }
        }
        if (!ok)
            say(pe_row == PE_WEB && pe_web_state == 1   ? "Still downloading, a moment"
                : pe_row == PE_WEB && pe_web_state != 2 ? "The download failed: Triangle tries another"
                                                        : "That picture couldn't be used");
        else if (pe_row == PE_GH || pe_row == PE_SONGS || pe_row == PE_WEB || pe_row == PE_CARD)
            say("Picture set");
    } else if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        pe_delete_armed = false;
        pe_file_n = pe_gh_n = -1;
        tex_free(pe_prev);
        pe_prev = NULL;
        pe_prev_path[0] = 0;
        go(S_PLAYERS);
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(110, 110, 110, 255));
    banner("EDIT PLAYER");
    panel(30, 84, 560, 400);
    char vals[PE_ROWS][96];
    static const char * const labels[PE_ROWS] = {"Name", "Colour", "Title", "Guitar Hero picture", "Picture from your songs",
                                                 "Download an album cover",
                                                 "Picture from the card", "No picture", "Delete player"};
    snprintf(vals[PE_NAME], 96, "%s", player_name(me));
    snprintf(vals[PE_COLOR], 96, "< %d >", player_color(me) + 1);
    {
        char cur[40];
        player_title_get(me, cur, sizeof(cur));
        const char * tn = "Career rank";
        for (int k = 0; cur[0] && k < achieve_count(); k++)
            if (!strcmp(achieve_get(k)->id, cur))
                tn = achieve_get(k)->name;
        snprintf(vals[PE_TITLE], 96, "< %s >", tn);
    }
    if (pe_gh_n > 0)
        snprintf(vals[PE_GH], 96, "< %d of %d >", pe_gh % pe_gh_n + 1, pe_gh_n);
    else
        snprintf(vals[PE_GH], 96, "none");
    snprintf(vals[PE_SONGS], 96, "%s", pe_cover >= 0 ? songs_get(pe_cover)->name : "no covers");
    if (pe_search_state == 1)
        snprintf(vals[PE_WEB], 96, "searching...");
    else if (pe_cover_n > 0)
        snprintf(vals[PE_WEB], 96, "< %d of %d >", pe_cv + 1, pe_cover_n);
    else
        snprintf(vals[PE_WEB], 96, pe_search_state == 2 ? "none found: Triangle" : "X to search");
    snprintf(vals[PE_CARD], 96, "%s", pe_file_n > 0 ? pe_files[pe_card % pe_file_n] : "none yet");
    snprintf(vals[PE_CLEAR], 96, "%s", "");
    snprintf(vals[PE_DELETE], 96, "%s", pe_delete_armed ? "X again to delete" : "");
    for (int i = 0; i < PE_ROWS; i++) {
        float y = 96 + i * 43;
        bool cur = i == pe_row;
        if (cur)
            select_bar(44, y - 4, 532, 40, WHITE);
        uint32_t col = i == PE_DELETE ? RGBA(255, 140, 120, 255) : cur ? RGBA(255, 230, 150, 255) : RGBA(215, 205, 195, 255);
        font_draw_shadow(res.ui, labels[i], 60, y + 6, 24, col, ALIGN_LEFT);
        font_draw_fit(res.ui, vals[i], 562, y + 9, 20, 230, cur ? RGBA(255, 236, 190, 255) : RGBA(180, 180, 185, 255),
                      ALIGN_RIGHT);
    }
    // the card as it is, and what the row offers
    draw_player_face(me, 770, 100, 170, true);
    font_draw_fit(res.ui, player_name(me), 770, 280, 28, 300, RGBA(255, 236, 190, 255), ALIGN_CENTER);
    if (pe_row == PE_GH || pe_row == PE_SONGS || pe_row == PE_WEB || pe_row == PE_CARD) {
        font_draw(res.ui, "Picture:", 650, 330, 18, RGBA(180, 180, 185, 255), ALIGN_LEFT);
        if (pe_prev)
            gfx_sprite(&(Sprite){pe_prev, 0, 0, 1, 1, 120, 120}, 770 - 60, 350, 120, 120, WHITE);
        else
            font_draw(res.ui, pe_row == PE_WEB && (pe_web_state == 1 || pe_search_state == 1) ? "downloading..."
                              : pe_row == PE_WEB && pe_cover_n == 0                       ? "search for a song"
                              : pe_row == PE_WEB                                          ? "couldn't download"
                                                                                          : "-",
                      770, 400, 20, RGBA(200, 200, 205, 255), ALIGN_CENTER);
        if (pe_row == PE_WEB && pe_cover_n > 0)
            font_draw_fit(res.ui, pe_cover_label[pe_cv], 770, 476, 16, 330, RGBA(220, 215, 205, 255), ALIGN_CENTER);
        if (pe_row == PE_CARD && pe_file_n <= 0)
            font_draw_fit(res.ui, "Put PNG or JPEG pictures in ux0:data/ghvita/avatars", 770, 440, 16, 330,
                          RGBA(200, 200, 205, 255), ALIGN_CENTER);
    }
    draw_toast();
    if (dl_typing) {
        kb_draw();
        font_draw(res.ui, dl_typing && kb_apply == pe_search ? "Type a song, artist or album, then Start" : "Type the name, then Start", SCREEN_W / 2, 470, 22, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    } else if (pe_row == PE_WEB) {
        static const char * help[] = {"btn_cross", "Use it", "btn_left", "Browse", "btn_triangle", "Search", "btn_circle", "Back"};
        help_bar(help, 8);
    } else if (pe_row == PE_GH || pe_row == PE_SONGS || pe_row == PE_CARD) {
        static const char * help[] = {"btn_cross", "Use it", "btn_left", "Browse", "btn_circle", "Back"};
        help_bar(help, 6);
    } else {
        static const char * help[] = {"btn_cross", "Choose", "btn_up", "Move", "btn_circle", "Back"};
        help_bar(help, 6);
    }
    gfx_end();
}

// a setlist's end: how the whole gig went
static void gigdone_screen(void)
{
    if (pressed & (BTN_CROSS | BTN_CIRCLE)) {
        ui_sound(SND_UI_SELECT);
        memset(&gig, 0, sizeof(gig));
        go(from_list());
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_results, WHITE);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 90));
    panel(140, 40, 680, 440);
    font_draw_shadow(res.title, "GIG COMPLETE!", SCREEN_W / 2, 58, 52, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    char t[128];
    float a = (float)((now_s - screen_t) / 1.2);
    a = a > 1 ? 1 : a;
    snprintf(t, sizeof(t), "%d", (int)(gig.score * (1 - (1 - a) * (1 - a))));
    font_draw_shadow(res.title, t, SCREEN_W / 2, 124, 60, WHITE, ALIGN_CENTER);
    struct { const char * k; char v[96]; } rows[5];
    rows[0].k = "Songs played";
    snprintf(rows[0].v, 96, "%d", gig.songs);
    rows[1].k = "Stars";
    snprintf(rows[1].v, 96, "%d", gig.stars);
    rows[2].k = "Notes hit";
    snprintf(rows[2].v, 96, "%d%%  (%d / %d)", gig.total ? gig.hit * 100 / gig.total : 0, gig.hit, gig.total);
    rows[3].k = "Full combos";
    snprintf(rows[3].v, 96, "%d", gig.fcs);
    rows[4].k = "Best song";
    snprintf(rows[4].v, 96, "%s", gig.best[0] ? gig.best : "-");
    for (int i = 0; i < 5; i++) {
        float y = 214 + i * 40;
        font_draw_shadow(res.ui, rows[i].k, 190, y, 28, RGBA(220, 210, 200, 255), ALIGN_LEFT);
        font_draw_fit(res.ui, rows[i].v, 770, y, 28, 380, RGBA(255, 236, 190, 255), ALIGN_RIGHT);
    }
    static const char * help[] = {"btn_cross", "Continue"};
    help_bar(help, 2);
    gfx_end();
}

// FACE-OFF: the hand-over (who plays next) and the verdict
static void faceoff_screen(void)
{
    Song * s = songs_get(sel);
    int n = player_count();
    if (faceoff == 2) {
        if (repeat & (BTN_LEFT | BTN_RIGHT)) {
            int d = repeat & BTN_LEFT ? -1 : 1;
            do
                faceoff_pick = (faceoff_pick + d + n) % n;
            while (faceoff_pick == faceoff_p[0] && n > 1);
            ui_sound(SND_UI_MOVE);
        }
        if ((pressed & BTN_CROSS) && s) {
            ui_sound(SND_UI_SELECT);
            faceoff_p[1] = faceoff_pick;
            player_switch(faceoff_pick);
            if (game_start(s, faceoff_part, faceoff_diff)) {
                faceoff = 3;
                practicing = false;
                game_set_autoplay(autoplay);
                go(S_PLAY);
                return;
            }
        }
    } else if (pressed & (BTN_CROSS | BTN_CIRCLE)) {
        // the verdict's read: back to the first player and the list
        ui_sound(SND_UI_SELECT);
        player_switch(faceoff_p[0]);
        faceoff = 0;
        go(from_list());
        return;
    }
    if (faceoff == 2 && (pressed & BTN_CIRCLE)) {
        ui_sound(SND_UI_BACK);
        faceoff = 0;
        go(from_list());
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(150, 150, 150, 255));
    banner("FACE-OFF");
    char t[120];
    if (faceoff == 2) {
        font_draw_shadow(res.title, "PASS THE VITA!", SCREEN_W / 2, 96, 46, RGBA(255, 220, 140, 255), ALIGN_CENTER);
        snprintf(t, sizeof(t), "%s scored %d. Who's taking them on?", player_name(faceoff_p[0]), faceoff_r[0].score);
        font_draw_fit(res.ui, t, SCREEN_W / 2, 152, 24, 820, WHITE, ALIGN_CENTER);
        draw_player_face(faceoff_pick, SCREEN_W / 2, 196, 170, true);
        snprintf(t, sizeof(t), "<  %s  >", player_name(faceoff_pick));
        font_draw_shadow(res.title, t, SCREEN_W / 2, 378, 36, RGBA(255, 236, 190, 255), ALIGN_CENTER);
        static const char * help[] = {"btn_cross", "Play", "btn_left", "Choose", "btn_circle", "Call it off"};
        help_bar(help, 6);
    } else {
        // the verdict: both cards, both scores, the winner crowned
        int win = faceoff_r[0].score == faceoff_r[1].score ? -1 : faceoff_r[0].score > faceoff_r[1].score ? 0 : 1;
        font_draw_shadow(res.title, win < 0 ? "A DRAW!" : "WINNER!", SCREEN_W / 2, 84, 46,
                         RGBA(255, 220, 140, 255), ALIGN_CENTER);
        for (int k = 0; k < 2; k++) {
            float x = SCREEN_W / 2 + (k ? 210 : -210);
            const Result * r = &faceoff_r[k];
            float sz = win == k ? 170 : 140;
            if (win == k)
                gfx_rect(x - sz / 2 - 6, 150 - 6, sz + 12, sz + 12, RGBA(255, 210, 90, 255));
            draw_player_face(faceoff_p[k], x, 150, sz, win != 1 - k);
            font_draw_shadow(res.title, player_name(faceoff_p[k]), x, 150 + sz + 12, 32,
                             win == k ? RGBA(255, 220, 140, 255) : RGBA(210, 205, 195, 255), ALIGN_CENTER);
            snprintf(t, sizeof(t), "%d", r->score);
            font_draw_shadow(res.title, t, x, 150 + sz + 48, 34, WHITE, ALIGN_CENTER);
            int pct = r->notes_total ? r->notes_hit * 100 / r->notes_total : 0;
            snprintf(t, sizeof(t), "%d%%   streak %d%s", pct, r->best_streak, r->fc ? "   FC" : "");
            font_draw(res.ui, t, x, 150 + sz + 88, 20, RGBA(210, 205, 195, 255), ALIGN_CENTER);
        }
        font_draw_shadow(res.title, "VS", SCREEN_W / 2, 220, 40, RGBA(255, 120, 90, 255), ALIGN_CENTER);
        static const char * help[] = {"btn_cross", "Done"};
        help_bar(help, 2);
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
    int medals[4];
    medal_totals(medals);
    for (int m = MEDAL_GOLD, k = 0; m >= MEDAL_BRONZE; m--, k++) {
        // gold, silver, bronze: how many charts have each
        float x = SCREEN_W / 2 - 150 + k * 120;
        draw_medal(m, x, 64, 12);
        snprintf(line, sizeof(line), "%d", medals[m]);
        font_draw_shadow(res.ui, line, x + 18, 52, 24, medal_cols[m], ALIGN_LEFT);
    }
    int fav = 0;
    for (int i = 1; i < PART_COUNT; i++)
        if (career.part_plays[i] > career.part_plays[fav])
            fav = i;
    struct {
        const char * label;
        char value[48];
    } rows[9];
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
    rows[8].label = "Battles won";
    snprintf(rows[8].value, 48, "%d of %d", career.battles_won, career.battles_won + career.battles_lost);
    for (int i = 0; i < 9; i++) {
        float y = 186 + i * 29;
        font_draw(res.ui, rows[i].label, 170, y, 26, RGBA(220, 210, 200, 255), ALIGN_LEFT);
        font_draw(res.ui, rows[i].value, 790, y, 26, RGBA(255, 236, 190, 255), ALIGN_RIGHT);
    }
    static const char * help[] = {"btn_r", "Achievements", "btn_circle", "Back"};
    help_bar(help, 4);
    gfx_end();
}

// what changed in this build, shown once the first time it starts (or after an update)
static void news_screen(void)
{
    static const char * const lines[] = {
        "The downloader keeps its folder tidy: old search covers (they had piled up past 100 MB) and dead download folders go.",
        "Flash mode is now just its engine: GAME, Engine: Guitar Flash plays by Flash's rules on the normal highway.",
        "GAME, Note motion: 24 fps steps the notes like Guitar Flash, at 30 or 60 fps. The flat Flash look is gone.",
        "The venue lights follow the song's own light show (Rock Band charts): verses, choruses, strobes, blackouts, silhouettes.",
        "Songs without a light show get one from their sections: verses cool, choruses slam in, builds strobe, bridges go dark.",
        "The guitarist and bassist really move now: they rock through GH's own groove animation, in time with the song.",
        "World Tour's HUD as in the game: the ROCK dome on the score amp, your multiplier a glowing nixie tube (blue in Star Power).",
        "Star Power works like GH's: turning it on lifts the rock meter, and while it lasts hits fill it twice as fast, misses cost half.",
        "Flash's notes are chunkier, with a lit rim.",
        "Flash note style redrawn flat and 2D: a tall narrow highway, starry night board, bold outlined candy notes and rings.",
        "Flash's rock and Star Power meters now stand beside the highway; its fire and tails are outlined bold.",
        "Smoother scrolling: the song clock no longer judders with the audio buffers.",
        "Flash note style now plays like Guitar Flash: its timing per difficulty, chords note by note, frets held below a note count.",
        "In Flash, a press that takes no note is an error (it breaks your streak). Its meter, x4 multiplier and timed Star Power too.",
        "Flash's notes move like Flash's: 24 steps a second, speeding up as they fall, and a lower, closer camera. Smooth in GRAPHICS.",
        "GAME, Flash note style plays: Like Guitar Flash or Like World Tour. New timing window: Guitar Hero 3 (116 early, 100 late).",
        "Late hits are capped at half the way to the next note, as in GH: a fast run no longer eats the next note early.",
        "Fixed 30 fps: it still drew 60 frames a second. Now it really draws 30 (and reads the buttons 60 times a second).",
        "Downloaded songs sound clean now: their audio was resampled badly (gritty cymbals and voices) and MP3s ran 25 ms late.",
        "Songs you downloaded before: SELECT in the song list, Repair downloaded songs' audio. They download again, scores kept.",
        "Fixed downloads sometimes running twice at once and spoiling each other's files.",
        "Flash looks more like Guitar Flash: white-topped pucks, ring targets, black lane lines, big fire, its own camera.",
        "Every hit is rated PERFECT, GREAT or GOOD. Results show your accuracy and a rank, S+ to D; the song list keeps your best.",
        "Tighter, fairer timing: Normal window 85 ms (was 95), new Pro 55 ms, and a chord's keys have to land together.",
        "More GRAPHICS options: HUD style, background (venue / cover art / dark), hit ratings, highway length.",
        "New modifiers in GAME: Sudden death (one miss ends it) and Hard rock meter. Seven new achievements to chase.",
        "Venues have 4 camera angles from the real 3D stages: the camera cuts on sections, solos and Star Power.",
        "New GRAPHICS tab: 60 or 30 fps (buttons still read at 60), venue camera, note style, Full/Balanced/Battery.",
        "Battles get GH3's attacks: Broken String (tap the fret 5x), Double Notes, Whammy and Amp Overload.",
        "Holding a fret, its lane's light now comes out of the cup instead of from the bottom edge of the highway.",
        "Streak lamps fixed: full-size gold bulbs on the rail again (the last builds shrank them to dots), domes tipped up-right.",
        "Fixed: a RhythmVerse search that had to be asked twice leaked its first answer's memory.",
        "GET READY: L and R change the song speed right there.",
        "The stage lights flash white on the drummer's crash cymbals, from the song's drum chart.",
        "Main menu: the time and the Vita's battery, top right.",
        "Fixed: the daily song changed at midnight UTC instead of the Vita's own midnight.",
        "Pause: the section and how far through the song you are sit on their own plate, readable over the highway.",
        "Options > Game > Song speed, 50% to 150%: shown on the HUD. A run below 100% isn't saved.",
        "Achievements: Speed Demon (finish a song at 125% speed) and Ludicrous (4 stars at 150%).",
        "Fixed: a practice run was saved as a score, so a slowed whole-song practice could earn the gold medal.",
        "Earlier: Star Power spin and sparks, lasers, camera reveal, practice metronome.",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, WHITE);
    banner("WHAT'S NEW IN THIS BUILD");
    panel(150, 96, 660, 400);
    // each item on up to two lines
    // up and down scroll it when it runs past the panel
    static float scroll;
    if (repeat & BTN_DOWN)
        scroll += 40;
    if (repeat & BTN_UP)
        scroll -= 40;
    scroll = scroll < 0 ? 0 : scroll > 600 ? 600 : scroll;
    float y = 114 - scroll, bottom = 114;
    for (int i = 0; i < n; i++) {
        float h = 0;
        if (y > 100 && y < 470)
            h = draw_wrapped(lines[i], 170, y, 18, 620, 2, RGBA(230, 230, 235, 255));
        else
            h = 40;
        y += h + 11;
        bottom = y + scroll;
    }
    if (scroll > bottom - 470)
        scroll = bottom - 470 > 0 ? bottom - 470 : 0;
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
    // the beat as the game hears it: a lamp that flashes on each click and fades
    double ph = t < 1.0 ? 1 : fmod(t - 1.0, 0.5) / 0.5;
    float pulse = (float)(1 - ph);
    pulse = pulse * pulse * pulse;
    const float cx = SCREEN_W / 2, cy = 214;
    gfx_rect(cx - 42, cy - 42, 84, 84, RGBA(0, 0, 0, 170));
    gfx_rect(cx - 36, cy - 36, 72, 72, RGBA((int)(70 + 185 * pulse), (int)(50 + 150 * pulse), (int)(20 + 60 * pulse), 255));
    gfx_blend(BLEND_ADD);
    gfx_sprite(atlas_get(res.game, "glow"), cx - 90, cy - 90, 180, 180, fade_alpha(RGBA(255, 200, 90, 255), pulse));
    gfx_blend(BLEND_ALPHA);
    // every tap on a line: left of centre was early, right was late, the newest brightest
    const float lx = 230, lw = 500, ly = 286, range = 0.15f;    // +-150 ms across
    gfx_rect(lx, ly, lw, 4, RGBA(0, 0, 0, 200));
    // within 30 ms either way: green
    float green = lw / 2 * 0.03f / range;
    gfx_rect(lx + lw / 2 - green, ly, green * 2, 4, RGBA(70, 200, 90, 220));
    gfx_rect(lx + lw / 2 - 1, ly - 10, 2, 24, WHITE);
    font_draw(res.ui, "early", lx, ly + 10, 16, RGBA(150, 200, 255, 255), ALIGN_LEFT);
    font_draw(res.ui, "late", lx + lw, ly + 10, 16, RGBA(255, 170, 120, 255), ALIGN_RIGHT);
    for (int i = 0; i < calib_count; i++) {
        float off = (float)calib_hits[i];
        off = off < -range ? -range : off > range ? range : off;
        float a = i == calib_count - 1 ? 1 : 0.35f + 0.5f * i / (calib_count ? calib_count : 1);
        gfx_rect(lx + lw / 2 + off / range * lw / 2 - 2, ly - 8, 4, 20, fade_alpha(RGBA(255, 230, 150, 255), a));
    }
    // how many of the sixteen
    gfx_rect(lx, 252, lw, 6, RGBA(0, 0, 0, 180));
    gfx_rect(lx, 252, lw * (calib_count > 16 ? 16 : calib_count) / 16.0f, 6, RGBA(255, 200, 90, 255));
    char s[96];
    double sum = 0;
    for (int i = 0; i < calib_count; i++)
        sum += calib_hits[i];
    snprintf(s, sizeof(s), "%d of 16 clicks   (so far %+d ms)", calib_count,
             calib_count ? (int)(sum / calib_count * 1000) : 0);
    font_draw(res.ui, s, SCREEN_W / 2, 330, 26, RGBA(255, 230, 160, 255), ALIGN_CENTER);
    snprintf(s, sizeof(s), "Current offset %+d ms", cfg.offset_ms);
    font_draw(res.ui, s, SCREEN_W / 2, 370, 22, RGBA(200, 200, 200, 255), ALIGN_CENTER);
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
        if (getenv("GHV_BATTLE"))
            game_battle(-1);        // tests: a battle (GHV_BATTLE=1)
        if (s && getenv("GHV_PRACTICE")) {
            // tests: the song's practice menu (GHV_PRACTICE=1), to drive with GHV_SCRIPT
            sel_part = part, sel_diff = diff;
            read_sections(s);
            go(S_PRACTICE);
        } else if (s && game_start(s, part, diff)) {
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
        int drawn = screen;
        switch (screen) {
        case S_TITLE: title_screen(); break;
        case S_MAIN: main_menu(); break;
        case S_TOUR: tour_screen(); break;
        case S_SONGS: song_list(); break;
        case S_SELECT: select_screen(); break;
        case S_PRACTICE: practice_screen(); break;
        case S_CAREER: career_screen(); break;
        case S_PLAYERS: players_screen(); break;
        case S_PLAYER_EDIT: player_edit_screen(); break;
        case S_FACEOFF: faceoff_screen(); break;
        case S_GIGDONE: gigdone_screen(); break;
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
        // a menu comes in out of black, quickly, rather than cutting (not the song and its pause: those stay instant)
        {
            double in = now_s - screen_t;
            if (in < 0.16 && drawn == screen && screen != S_PLAY && screen != S_PAUSE && screen != S_RESUME) {
                gfx_2d();
                gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, (int)(230 * (1 - in / 0.16))));
                gfx_flush();
            }
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
        plat_half_rate(drawn == S_PLAY && screen == S_PLAY && cfg.frame_rate == 30 && outro_t < 0);
        if (skip_draw) {
            skip_draw = false;
            plat_skip_frame();
        } else
            plat_swap();
    }
    plat_quit();
    return 0;
}
