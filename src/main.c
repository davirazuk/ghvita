// GH Vita: the screens (title, song list, part and difficulty, the song, pause, results, options, calibration, the
// song downloader)
#include <math.h>
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
#include "songs.h"
#include "chart.h"
#include "game.h"
#include "vocals.h"
#include "dl.h"

enum { S_TITLE, S_MAIN, S_SONGS, S_SELECT, S_PRACTICE, S_PLAY, S_PAUSE, S_RESUME, S_RESULTS, S_OPTIONS, S_CALIBRATE, S_EMPTY,
       S_DOWNLOAD };

static int screen = S_TITLE;
static double now_s, screen_t;
static double gap_s;                // since the frame before
static int screen_frames;           // frames since the screen changed
static bool overlay_on;             // the system's quick menu just came up
static Pad pad;
static uint32_t pressed, repeat;    // repeat: pressed, plus held directions repeating
static bool autoplay;

// the song list
static int sel, top;
static double sel_t;
static Tex * art;
static int art_for = -1, preview_for = -1;
// part and difficulty
static int sel_part, sel_diff;
static int want_diff;               // the difficulty last picked (a part without it gets the nearest)
static uint8_t sel_diffs[PART_COUNT];   // the difficulties each part of the chosen song has, a bit each
static int menu_index;
static Result result;
static bool result_best;
static int options_from;
// calibration
static double calib_hits[32];
static int calib_count;
static bool calib_playing;
// a line in the corner for a few seconds ("Added Reptilia")
static char toast[160];
static double toast_t = -10;

static void go(int s)
{
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
    float x = 18;
    for (int i = 0; i + 1 < n; i += 2) {
        const Sprite * b = atlas_get(res.hud, items[i]);
        float w = b->w * 28.0f / b->h;
        gfx_sprite(b, x, SCREEN_H - 35, w, 28, WHITE);
        x += w + 6;
        x += font_draw(res.ui, items[i + 1], x, SCREEN_H - 34, 26, RGBA(230, 230, 230, 255), ALIGN_LEFT) + 22;
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

static void title_screen(void)
{
    if (pressed & (BTN_CROSS | BTN_START)) {
        ui_sound(SND_UI_SELECT);
        go(S_MAIN);
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_title, WHITE);
    float a = 0.55f + 0.45f * sinf((float)now_s * 4);
    gfx_rect_grad(0, 420, SCREEN_W, 90, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 210));
    font_draw_shadow(res.title, "PRESS X TO ROCK", SCREEN_W * 0.74f, 452, 46, fade_alpha(RGBA(255, 220, 120, 255), a),
                     ALIGN_CENTER);
    gfx_end();
}

// ----------------------------------------------------------------------------------------------------- main menu

static void main_menu(void)
{
    static const char * items[] = {"QUICKPLAY", "DOWNLOAD SONGS", "OPTIONS", "CALIBRATE", "EXIT"};
    const int n = 5;
    if (repeat & BTN_UP) {
        menu_index = (menu_index + n - 1) % n;
        ui_sound(SND_UI_MOVE);
    }
    if (repeat & BTN_DOWN) {
        menu_index = (menu_index + 1) % n;
        ui_sound(SND_UI_MOVE);
    }
    if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        go(S_TITLE);
    } else if (pressed & BTN_CROSS) {
        ui_sound(SND_UI_SELECT);
        int which = menu_index;
        if (which == 0)
            go(songs_count() ? S_SONGS : S_EMPTY);
        else if (which == 1)
            go(S_DOWNLOAD);
        else if (which == 2) {
            options_from = S_MAIN;
            go(S_OPTIONS);
        } else if (which == 3) {
            calib_count = 0;
            calib_playing = false;
            go(S_CALIBRATE);
        } else {
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
        float y = 134 + i * 58;
        if (i == menu_index)
            gfx_sprite(atlas_get(res.hud, "wor_select"), 50, y - 6, 400, 54, WHITE);
        font_draw_shadow(res.title, items[i], 80, y, 42, i == menu_index ? RGBA(255, 230, 150, 255)
                                                                           : RGBA(200, 190, 180, 255), ALIGN_LEFT);
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
    static const char * help[] = {"btn_cross", "Select", "btn_circle", "Back"};
    help_bar(help, 4);
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
static Tex * dl_art;
static char dl_art_id[80];          // the result the cover is for
static double dl_sel_t;

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
        dl_typing = false;
        // the words, without spaces around them
        char * t = dl_typed;
        while (*t == ' ')
            t++;
        l = strlen(t);
        while (l && t[l - 1] == ' ')
            t[--l] = 0;
        snprintf(dl_query[dl_site], sizeof(dl_query[0]), "%s", t);
        plat_log("search: typed \"%s\"", t);
        dl_new_search();
        ui_sound(SND_UI_SELECT);
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

static void download_screen(void)
{
    if (!dl_started) {
        dl_started = true;
        dl_new_search();            // the newest charts
    }
    if (dl_typing) {
        kb_update();
        pressed = repeat = 0;       // the keyboard has the buttons
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
        ui_sound(SND_UI_SELECT);
    }
    if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        go(S_MAIN);
        menu_index = 1;             // back on DOWNLOAD SONGS
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
        } else {
            dl_queue(&cur);
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
        if (dl_art && strcmp(dl_art_id, cur.id) == 0)
            gfx_sprite(&(Sprite){dl_art, 0, 0, 1, 1, dl_art->w, dl_art->h}, 716, 138, 124, 124, WHITE);
        else
            gfx_rect(716, 138, 124, 124, RGBA(20, 20, 24, 255));
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
        // what's in the chart, with how hard: known before it's downloaded
        if (cur.any_parts) {
            float px = 630, py = y;
            for (int p = 0; p < PART_COUNT; p++) {
                if (cur.parts[p] < 0)
                    continue;
                font_draw_fit(res.ui, part_name(p), px, py, 14, 72, RGBA(235, 235, 235, 255), ALIGN_LEFT);
                for (int d = 0; d < 6; d++)
                    gfx_rect(px + d * 11, py + 17, 8, 5, d < cur.parts[p] ? RGBA(255, 190, 60, 255) : RGBA(70, 70, 70, 255));
                px += 76;
                if (px > 860) {
                    px = 630;
                    py += 27;
                }
            }
            y = py + (px > 630 ? 27 : 0);
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
    if (dl_typing) {
        // the desktop build's keyboard (the Vita's draws itself over this)
        panel(150, 190, 660, 150);
        snprintf(t, sizeof(t), "Search %s", dl_site == DL_CHORUS ? "Chorus Encore" : "RhythmVerse");
        font_draw_shadow(res.ui, t, SCREEN_W / 2, 214, 26, RGBA(255, 230, 150, 255), ALIGN_CENTER);
        gfx_rect(190, 256, 580, 44, RGBA(10, 10, 12, 230));
        snprintf(t, sizeof(t), "%s%s", dl_typed, fmod(now_s, 1.0) < 0.5 ? "_" : " ");
        font_draw_fit(res.ui, t, 204, 264, 28, 552, WHITE, ALIGN_LEFT);
    }
    static const char * help[] = {"btn_cross", "Download", "btn_triangle", "Search", "btn_l", "Site",
                                  "btn_square", "Cancel", "btn_circle", "Back"};
    help_bar(help, 10);
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

static bool del_ask;

static void song_list(void)
{
    int n = songs_count();
    const int rows = 7;
    if (del_ask) {
        if (pressed & BTN_CROSS) {
            stop_preview();
            art_for = -1;
            songs_delete(sel);
            if (sel >= songs_count())
                sel = songs_count() - 1;
            if (sel < 0)
                sel = 0;
            del_ask = false;
            ui_sound(SND_UI_SELECT);
        } else if (pressed & BTN_CIRCLE)
            del_ask = false;
        pressed = repeat = 0;
    }
    if ((pressed & BTN_SELECT) && songs_get(sel)) {
        del_ask = true;
        pressed = repeat = 0;
    }
    int old = sel;
    if (repeat & BTN_UP)
        sel--;
    if (repeat & BTN_DOWN)
        sel++;
    if (repeat & BTN_L)
        sel -= rows;
    if (repeat & BTN_R)
        sel += rows;
    if (pressed & BTN_SQUARE)
        sel = rand() % n;
    if (sel < 0)
        sel = (repeat & (BTN_UP)) && old == 0 ? n - 1 : 0;
    if (sel >= n)
        sel = (repeat & (BTN_DOWN)) && old == n - 1 ? 0 : n - 1;
    if (sel != old) {
        ui_sound(SND_UI_MOVE);
        sel_t = now_s;
    }
    if (pressed & BTN_TRIANGLE) {
        Song * cur = songs_get(sel);
        char keep[256];
        snprintf(keep, sizeof(keep), "%s", cur ? cur->folder : "");
        cfg.sort = (cfg.sort + 1) % SORT_COUNT;
        songs_sort(cfg.sort);
        for (int i = 0; i < n; i++)
            if (strcmp(songs_get(i)->folder, keep) == 0)
                sel = i;
        stop_preview();
        art_for = -1;
        ui_sound(SND_UI_SELECT);
        config_save();
    }
    if (sel < top)
        top = sel;
    if (sel >= top + rows)
        top = sel - rows + 1;
    Song * s = songs_get(sel);
    if (pressed & BTN_CIRCLE) {
        stop_preview();
        ui_sound(SND_UI_BACK);
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
        read_diffs(s);
        sel_part = first_part(s);
        for (int p = 0; !sel_diffs[sel_part] && p < PART_COUNT; p++)
            if (sel_diffs[p])
                sel_part = p;
        want_diff = cfg.last_diff;
        sel_diff = nearest_diff(sel_part, want_diff);
        go(S_SELECT);
        return;
    }
    // the cover once the choice settles, then the preview
    if (s && art_for != sel && now_s - sel_t > 0.12) {
        tex_free(art);
        char path[400];
        snprintf(path, sizeof(path), "%s/album.jpg", s->folder);
        art = tex_load(path, TEX_MIPMAP);
        art_for = sel;
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
    snprintf(t, sizeof(t), "%d / %d   sorted by %s", sel + 1, n, cfg.sort == SORT_ARTIST ? "artist" : "title");
    font_draw_shadow(res.ui, t, SCREEN_W - 24, 30, 24, RGBA(230, 230, 230, 255), ALIGN_RIGHT);
    // the list
    for (int r = 0; r < rows && top + r < n; r++) {
        int i = top + r;
        Song * x = songs_get(i);
        float y = 104 + r * 56;
        bool on = i == sel;
        gfx_sprite(atlas_get(res.hud, on ? "wor_select" : "wor_row"), 14, y, 580, 54, WHITE);
        char up[128];
        upper(up, sizeof(up), x->name);
        font_draw_fit(res.ui, up, 40, y + 4, 26, 520, on ? RGBA(255, 236, 170, 255) : RGBA(235, 235, 235, 255),
                      ALIGN_LEFT);
        font_draw_fit(res.ui, x->artist, 42, y + 28, 20, 500, on ? RGBA(255, 210, 140, 255) : RGBA(170, 170, 175, 255),
                      ALIGN_LEFT);
        const Score * b = &x->best[first_part(x)][cfg.last_diff];
        if (b->stars)
            draw_stars(b->stars, 470, y + 30, 18);
    }
    // the details
    if (s) {
        panel(608, 96, 340, 396);
        if (art && art_for == sel)
            gfx_sprite(&(Sprite){art, 0, 0, 1, 1, art->w, art->h}, 678, 118, 200, 200, WHITE);
        else
            gfx_rect(678, 118, 200, 200, RGBA(20, 20, 24, 255));
        float y = 326;
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
        // the parts it has, with the chart's difficulty tier as dots
        float px = 630;
        for (int p = 0; p < PART_COUNT; p++) {
            if (!(s->parts & (1 << p)))
                continue;
            font_draw(res.ui, part_name(p), px, y + 4, 18, RGBA(230, 230, 230, 255), ALIGN_LEFT);
            for (int d = 0; d < 6; d++)
                gfx_rect(px + 4 + d * 11, y + 26, 8, 8,
                         d < s->diff[p] ? RGBA(255, 190, 60, 255) : RGBA(70, 70, 70, 255));
            px += 100;
            if (px > 900) {
                px = 630;
                y += 40;
            }
        }
        const Score * b = &s->best[first_part(s)][cfg.last_diff];
        if (b->score) {
            snprintf(t, sizeof(t), "Best %d  %d%%%s", b->score, b->percent, b->fc ? "  FC" : "");
            font_draw(res.ui, t, 630, 456, 20, RGBA(255, 230, 160, 255), ALIGN_LEFT);
        }
    }
    static const char * help[] = {"btn_cross", "Play", "btn_circle", "Back", "btn_triangle", "Sort", "btn_square",
                                  "Random", "btn_l", "Page", "btn_start", "Options", "btn_select", "Delete"};
    help_bar(help, 14);
    if (del_ask) {
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 200));
        font_draw(res.ui, "Delete this song from the card?", SCREEN_W / 2, 200, 34, WHITE, ALIGN_CENTER);
        font_draw_fit(res.ui, songs_get(sel)->name, SCREEN_W / 2, 250, 30, 800, RGBA(255, 220, 140, 255), ALIGN_CENTER);
        font_draw(res.ui, "Cross delete   Circle keep", SCREEN_W / 2, 310, 26, RGBA(220, 220, 230, 255), ALIGN_CENTER);
    }
    gfx_end();
}

// -------------------------------------------------------------------------------------------- part and difficulty

static bool select_starting;        // ✕ was pressed: that frame says LOADING, the next one loads the song

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
static bool practicing, practice_starting;

static void pr_add(double t, const char * name)
{
    if (pr_count == PR_MAX)
        return;
    pr[pr_count].t = t;
    pr[pr_count].notes = 0;
    // "verse_1" reads "Verse 1"; a charting tool's "ugc_section_20_40" says nothing
    if (!strncasecmp(name, "ugc", 3))
        snprintf(pr[pr_count].name, sizeof(pr[0].name), "Section %d", pr_count + 1);
    else
        snprintf(pr[pr_count].name, sizeof(pr[0].name), "%s", name);
    for (char * c = pr[pr_count].name; *c; c++)
        if (*c == '_')
            *c = ' ';
    if (pr[pr_count].name[0] >= 'a' && pr[pr_count].name[0] <= 'z')
        pr[pr_count].name[0] -= 32;
    pr_count++;
}

static void read_sections(const Song * s)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/notes.vhc", s->folder);
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
        if (game_start(s, sel_part, sel_diff)) {
            practicing = true;
            game_set_autoplay(autoplay);
            go(S_PLAY);
            return;
        }
        game_stop();
        plat_log("can't practice %s", s->folder);
    }
    int from = pr_from, to = pr_to, speed = pr_speed;
    if (repeat & BTN_UP)
        pr_from--;
    if (repeat & BTN_DOWN)
        pr_from++;
    pr_from = pr_from < 0 ? 0 : pr_from >= pr_count ? pr_count - 1 : pr_from;
    if (pr_from != from) {
        pr_focus = pr_from;
        if (pr_to < pr_from)
            pr_to = pr_from;
    }
    if (repeat & BTN_L)
        pr_to--;
    if (repeat & BTN_R)
        pr_to++;
    pr_to = pr_to < pr_from ? pr_from : pr_to >= pr_count ? pr_count - 1 : pr_to;
    if (pr_to != to)
        pr_focus = pr_to;
    if (repeat & BTN_LEFT)
        pr_speed -= 5;
    if (repeat & BTN_RIGHT)
        pr_speed += 5;
    pr_speed = pr_speed < 50 ? 50 : pr_speed > 100 ? 100 : pr_speed;
    if (pr_from != from || pr_to != to || pr_speed != speed)
        ui_sound(SND_UI_MOVE);
    if (pressed & BTN_CIRCLE) {
        ui_sound(SND_UI_BACK);
        go(S_SELECT);
        return;
    }
    bool start = pressed & BTN_CROSS;
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
    for (int r = 0; r < PR_ROWS && pr_top + r < pr_count; r++) {
        int i = pr_top + r;
        float y = 104 + r * 56;
        bool on = i >= pr_from && i <= pr_to, cur = i == pr_focus;
        gfx_sprite(atlas_get(res.hud, on ? "wor_select" : "wor_row"), 14, y, 580, 54, WHITE);
        font_draw_fit(res.ui, pr[i].name, 40, y + 11, 28, 380, cur ? RGBA(255, 236, 170, 255) : on ? RGBA(240, 225, 190, 255)
                                                                                                : RGBA(190, 190, 195, 255),
                      ALIGN_LEFT);
        int secs = (int)pr[i].t;
        snprintf(t, sizeof(t), "%d:%02d", secs / 60, secs % 60);
        font_draw(res.ui, t, 490, y + 15, 22, RGBA(200, 200, 200, 255), ALIGN_RIGHT);
        if (sel_part != PART_VOCALS) {
            snprintf(t, sizeof(t), "%d", pr[i].notes);
            font_draw(res.ui, t, 572, y + 15, 22, RGBA(255, 200, 110, 255), ALIGN_RIGHT);
        }
    }
    panel(608, 96, 340, 396);
    font_draw_fit(res.title, s->name, 778, 116, 34, 300, RGBA(255, 220, 140, 255), ALIGN_CENTER);
    snprintf(t, sizeof(t), "%s  %s", part_name(sel_part), diff_name(sel_diff));
    font_draw_fit(res.ui, t, 778, 160, 24, 300, RGBA(230, 230, 230, 255), ALIGN_CENTER);
    font_draw(res.ui, "From", 636, 214, 22, RGBA(180, 180, 180, 255), ALIGN_LEFT);
    font_draw_fit(res.ui, pr[pr_from].name, 636, 240, 28, 290, RGBA(255, 236, 190, 255), ALIGN_LEFT);
    font_draw(res.ui, "To the end of", 636, 284, 22, RGBA(180, 180, 180, 255), ALIGN_LEFT);
    font_draw_fit(res.ui, pr[pr_to].name, 636, 310, 28, 290, RGBA(255, 236, 190, 255), ALIGN_LEFT);
    int len = (int)(pr[pr_to + 1].t - pr[pr_from].t + 0.5);
    snprintf(t, sizeof(t), "%d:%02d", len / 60, len % 60);
    font_draw(res.ui, t, 926, 214, 22, RGBA(200, 200, 200, 255), ALIGN_RIGHT);
    font_draw(res.ui, "Speed", 636, 362, 22, RGBA(180, 180, 180, 255), ALIGN_LEFT);
    snprintf(t, sizeof(t), "< %d%% >", pr_speed);
    font_draw_shadow(res.title, t, 778, 390, 40, WHITE, ALIGN_CENTER);
    if (start)
        font_draw_shadow(res.title, "LOADING", SCREEN_W / 2, 488, 36, RGBA(255, 220, 120, 255), ALIGN_CENTER);
    else {
        static const char * help[] = {"btn_cross", "Start", "btn_up", "From", "btn_r", "To", "btn_left", "Speed", "btn_circle",
                                      "Back"};
        help_bar(help, 10);
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
        go(S_SONGS);
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

static void finish_song(int how)
{
    result = game_result();
    plat_log("result: %s, score %d, %d stars, %d/%d notes, best streak %d, %d/%d phrases, %d overhits%s; hits %+d ms "
             "on average, spread %d", how == GAME_DONE ? "done" : "failed", result.score, result.stars,
             result.notes_hit, result.notes_total, result.best_streak, result.sp_hit, result.sp_total, result.overhits,
             result.fc ? ", full combo" : "", result.off_ms, result.spread_ms);
    Song * s = songs_get(sel);
    result_best = false;
    if (s && !result.failed) {
        Score sc = {result.score, (uint8_t)result.stars,
                    (uint8_t)(result.notes_total ? result.notes_hit * 100 / result.notes_total : 0), result.fc};
        result_best = result.score > s->best[result.part][result.diff].score;
        if (!autoplay)
            songs_save_score(s, result.part, result.diff, sc);
    }
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
    static const char * items[] = {"RESUME", "RESTART", "OPTIONS", "QUIT"};
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
            go(practicing ? S_PRACTICE : S_SONGS);
            practicing = false;
            sel_t = now_s;
            return;
        }
    }
    if (resume) {
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

static void results_screen(void)
{
    Song * s = songs_get(sel);
    if (pressed & (BTN_CROSS | BTN_CIRCLE)) {
        ui_sound(SND_UI_SELECT);
        go(S_SONGS);
        sel_t = now_s;
        return;
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
    font_draw_fit(res.title, result.failed ? "SONG FAILED" : s ? s->name : "", SCREEN_W / 2, 50, 54, 820,
                  result.failed ? RGBA(255, 90, 70, 255) : RGBA(255, 220, 140, 255), ALIGN_CENTER);
    if (s) {
        // failed, the big line doesn't name the song
        char who[200];
        snprintf(who, sizeof(who), "%s  -  %s", s->name, s->artist);
        font_draw_fit(res.ui, result.failed ? who : s->artist, SCREEN_W / 2, 110, 28, 820, RGBA(230, 230, 230, 255),
                      ALIGN_CENTER);
    }
    char t[96];
    snprintf(t, sizeof(t), "%s  %s", part_name(result.part), diff_name(result.diff));
    font_draw(res.ui, t, SCREEN_W / 2, 142, 22, RGBA(190, 190, 190, 255), ALIGN_CENTER);
    if (!result.failed) {
        float a = (float)((now_s - screen_t) / 1.2);
        int shown = (int)(a * result.stars + 0.999f);
        if (shown > result.stars)
            shown = result.stars;
        draw_stars(shown >= 6 || (result.stars >= 6 && a >= 1) ? 6 : shown, SCREEN_W / 2 - 150, 176, 64);
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
        float y = 252 + i * 33;
        font_draw_shadow(res.ui, rows[i].label, 300, y, 30, RGBA(220, 210, 200, 255), ALIGN_LEFT);
        font_draw_shadow(res.ui, rows[i].value, 660, y, 30, RGBA(255, 236, 190, 255), ALIGN_RIGHT);
    }
    if (result.fc)
        font_draw_shadow(res.title, "FULL COMBO!", SCREEN_W / 2, 452, 40, RGBA(255, 210, 60, 255), ALIGN_CENTER);
    else if (result_best)
        font_draw_shadow(res.title, "NEW BEST!", SCREEN_W / 2, 452, 40, RGBA(120, 220, 255, 255), ALIGN_CENTER);
    static const char * help[] = {"btn_cross", "Continue", "btn_triangle", "Retry"};
    help_bar(help, 4);
    gfx_end();
}

// ------------------------------------------------------------------------------------------------------- options

enum { O_SPEED, O_HIGHWAY, O_VENUE, O_BAND, O_SING, O_OFFSET, O_LAYOUT, O_DRUMKEYS, O_DRUMHITS, O_TILT, O_NOFAIL, O_LEFTY, O_MUSIC, O_SFX, O_CROWD, O_FPS, O_COUNT };

static void option_text(int i, char * name, char * value, size_t n)
{
    switch (i) {
    case O_SPEED: strcpy(name, "Note speed"); snprintf(value, n, "%d", cfg.speed); break;
    case O_HIGHWAY: strcpy(name, "Highway"); snprintf(value, n, "%s", cfg.highway); break;
    case O_VENUE:
        strcpy(name, "Venue");
        snprintf(value, n, "%s", strcmp(cfg.venue, "song") == 0 ? "A different one each song"
                                 : strcmp(cfg.venue, "random") == 0 ? "Random"
                                 : strcmp(cfg.venue, "classic") == 0 ? "None (plain stage)"
                                 : res_venue_title(cfg.venue));
        break;
    case O_BAND: strcpy(name, "Band on stage"); snprintf(value, n, "%s", cfg.no_band ? "Off" : "On"); break;
    case O_SING: strcpy(name, "Sing along"); snprintf(value, n, "%s", cfg.sing_along ? "On" : "Off"); break;
    case O_OFFSET: strcpy(name, "Audio offset"); snprintf(value, n, "%+d ms", cfg.offset_ms); break;
    case O_LAYOUT: strcpy(name, "Buttons"); snprintf(value, n, "%s", cfg.layout ? "Layout B" : "Layout A"); break;
    case O_TILT: strcpy(name, "Tilt for Star Power"); snprintf(value, n, "%s", cfg.tilt ? "On" : "Off"); break;
    case O_NOFAIL: strcpy(name, "No fail"); snprintf(value, n, "%s", cfg.nofail ? "On" : "Off"); break;
    case O_DRUMKEYS: strcpy(name, "Drum buttons"); snprintf(value, n, "%s", cfg.drum_layout ? "Shoulders" : "Thumbs"); break;
    case O_DRUMHITS: strcpy(name, "Drum pad sounds"); snprintf(value, n, "%s", cfg.drum_hits ? "Every hit" : "Freestyle only"); break;
    case O_LEFTY: strcpy(name, "Lefty flip"); snprintf(value, n, "%s", cfg.lefty ? "On" : "Off"); break;
    case O_MUSIC: strcpy(name, "Music volume"); snprintf(value, n, "%d", cfg.music_vol); break;
    case O_SFX: strcpy(name, "Effects volume"); snprintf(value, n, "%d", cfg.sfx_vol); break;
    case O_CROWD: strcpy(name, "Crowd volume"); snprintf(value, n, "%d", cfg.crowd_vol); break;
    case O_FPS: strcpy(name, "Show frame rate"); snprintf(value, n, "%s", cfg.show_fps ? "On" : "Off"); break;
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
    case O_OFFSET: cfg.offset_ms = clampi(cfg.offset_ms + d * 5, -300, 300); break;
    case O_LAYOUT: cfg.layout = !cfg.layout; break;
    case O_DRUMKEYS: cfg.drum_layout = !cfg.drum_layout; break;
    case O_DRUMHITS: cfg.drum_hits = !cfg.drum_hits; break;
    case O_TILT: cfg.tilt = !cfg.tilt; break;
    case O_NOFAIL: cfg.nofail = !cfg.nofail; break;
    case O_LEFTY: cfg.lefty = !cfg.lefty; break;
    case O_MUSIC: cfg.music_vol = clampi(cfg.music_vol + d, 0, 10); break;
    case O_SFX: cfg.sfx_vol = clampi(cfg.sfx_vol + d, 0, 10); break;
    case O_CROWD: cfg.crowd_vol = clampi(cfg.crowd_vol + d, 0, 10); break;
    case O_FPS: cfg.show_fps = !cfg.show_fps; break;
    }
    audio_set_volumes(cfg.music_vol / 10.0f, cfg.sfx_vol / 10.0f);
}

static void options_screen(void)
{
    if (repeat & BTN_UP) {
        menu_index = (menu_index + O_COUNT - 1) % O_COUNT;
        ui_sound(SND_UI_MOVE);
    }
    if (repeat & BTN_DOWN) {
        menu_index = (menu_index + 1) % O_COUNT;
        ui_sound(SND_UI_MOVE);
    }
    if (repeat & (BTN_LEFT | BTN_RIGHT | BTN_CROSS)) {
        change_option(menu_index, repeat & BTN_LEFT ? -1 : 1);
        ui_sound(SND_UI_MOVE);
    }
    if (pressed & (BTN_CIRCLE | BTN_START)) {
        config_save();
        ui_sound(SND_UI_BACK);
        int back = options_from;
        go(back);
        if (back == S_PAUSE || back == S_MAIN)
            menu_index = 2;         // back on OPTIONS
        return;
    }
    gfx_begin(0, 0, 0);
    gfx_2d();
    draw_background(res.bg_menu, RGBA(170, 170, 170, 255));
    banner("OPTIONS");
    panel(110, 96, 740, 400);
    for (int i = 0; i < O_COUNT; i++) {
        char name[48], value[48];
        option_text(i, name, value, sizeof(value));
        float y = 102 + i * 22;
        bool on = i == menu_index;
        if (on)
            gfx_sprite(atlas_get(res.hud, "wor_select"), 130, y - 3, 700, 27, WHITE);
        uint32_t c = on ? RGBA(255, 230, 150, 255) : RGBA(220, 215, 205, 255);
        font_draw(res.ui, name, 160, y, 24, c, ALIGN_LEFT);
        font_draw(res.ui, value, 800, y, 24, c, ALIGN_RIGHT);
    }
    static const char * about[O_COUNT] = {
        "How fast the notes come down the highway.",
        "The highway's look, from Guitar Hero 2, 3, World Tour and Metallica.",
        "The stage behind the highway: Guitar Hero World Tour's venues, lit up with the music.",
        "World Tour's band on the venue's stage. Turn it off if a song runs slow.",
        "Sing into the mic over any instrument, when the song has vocals. Headphones help.",
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
    };
    char line[160];
    const char * text = about[menu_index];
    if (menu_index == O_LAYOUT) {
        snprintf(line, sizeof(line), "%s   Whammy: right stick, or rub the back", cfg.layout
                 ? "Green L, Red Left, Yellow Up, Blue Triangle, Orange R, open X"
                 : "Green L, Red Left, Yellow Up or Triangle, Blue Circle, Orange R, open X");
        text = line;
    }
    if (menu_index == O_DRUMKEYS)
        text = cfg.drum_layout ? "Red L, Yellow D-pad, Blue Triangle, Green Circle, Kick R: snare and hi-hat on two hands"
                               : "Red Left, Yellow Up, Blue Triangle, Green Circle, Kick L, R, X or Down";
    font_draw_fit(res.ui, text, SCREEN_W / 2, 462, 22, 820, RGBA(200, 200, 200, 255), ALIGN_CENTER);
    static const char * help[] = {"btn_left", "Change", "btn_circle", "Done"};
    help_bar(help, 4);
    gfx_end();
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
        menu_index = 3;
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
    plat_log("GH Vita starting");
    // QUICKPLAY's song and a random venue differ from boot to boot (test runs on the fixed clock stay repeatable)
    srand((unsigned)(getenv("GHV_FIXED") ? 1 : time(NULL)) ^ (unsigned)plat_now_us());
    gfx_init();
    config_load();
    audio_init();
    audio_set_volumes(cfg.music_vol / 10.0f, cfg.sfx_vol / 10.0f);
    res_load();
    songs_scan(scan_progress);
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
        }
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
        case S_SONGS: song_list(); break;
        case S_SELECT: select_screen(); break;
        case S_PRACTICE: practice_screen(); break;
        case S_PLAY: play_screen(); break;
        case S_PAUSE: pause_screen(); break;
        case S_RESUME: resume_screen(); break;
        case S_RESULTS: results_screen(); break;
        case S_OPTIONS: options_screen(); break;
        case S_CALIBRATE: calibrate_screen(); break;
        case S_EMPTY: empty_screen(); break;
        case S_DOWNLOAD: download_screen(); break;
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
