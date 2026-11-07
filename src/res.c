#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "res.h"
#include "plat.h"

Res res;

typedef struct {
    char name[40];
    Sprite sprite;
} Entry;

struct Atlas {
    Tex * tex;
    int count;
    Entry * entries;
};

static int by_name(const void * a, const void * b)
{
    return strcmp(((const Entry *)a)->name, ((const Entry *)b)->name);
}

Atlas * atlas_load(const char * png, const char * txt, int flags)
{
    FILE * f = fopen(txt, "r");
    if (!f) {
        plat_log("atlas: can't open %s", txt);
        return NULL;
    }
    Atlas * a = calloc(1, sizeof(Atlas));
    a->tex = tex_load(png, flags);
    int cap = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        Entry e;
        int x, y, w, h;
        if (sscanf(line, "%39s %d %d %d %d", e.name, &x, &y, &w, &h) != 5)
            continue;
        if (a->count == cap) {
            cap = cap ? cap * 2 : 128;
            a->entries = realloc(a->entries, cap * sizeof(Entry));
        }
        float iw = a->tex ? 1.0f / a->tex->w : 0, ih = a->tex ? 1.0f / a->tex->h : 0;
        Sprite s = {a->tex, x * iw, y * ih, (x + w) * iw, (y + h) * ih, w, h};
        e.sprite = s;
        a->entries[a->count++] = e;
    }
    fclose(f);
    qsort(a->entries, a->count, sizeof(Entry), by_name);
    return a;
}

const Sprite * atlas_get(Atlas * a, const char * name)
{
    static Sprite missing = {NULL, 0, 0, 1, 1, 4, 4};
    if (a) {
        Entry key;
        snprintf(key.name, sizeof(key.name), "%s", name);
        Entry * e = bsearch(&key, a->entries, a->count, sizeof(Entry), by_name);
        if (e)
            return &e->sprite;
    }
    plat_log("atlas: no sprite %s", name);
    return &missing;
}

static Tex * load_tex(const char * name, int flags)
{
    char path[512];
    snprintf(path, sizeof(path), "%s%s", plat_asset_dir(), name);
    return tex_load(path, flags);
}

static Sound * load_sound(const char * name)
{
    char path[512];
    snprintf(path, sizeof(path), "%ssfx/%s.ogg", plat_asset_dir(), name);
    return sound_load(path);
}

static int by_string(const void * a, const void * b)
{
    return strcmp((const char *)a, (const char *)b);
}

// the title: World Tour's own logo on its menu wall (not the picture with a made-up edition's name on it)
void res_title_art(void)
{
    // the Definitive Edition picture, logo and all
    draw_background(res.bg_title, WHITE);
}

// a hint for while something loads: a different one each time it's asked for a new one
static const char * const tips[] = {
    "Tilt the Vita up quickly to use Star Power. Select works too.",
    "Rub the rear touchpad to whammy a sustain and fill Star Power faster.",
    "Pause a song and pick PRACTICE THIS PART to loop the section you're stuck on.",
    "Practice can speed up by itself each time you clear the section: Speed up, +5% a clear.",
    "Triangle on GET READY starts a battle. Star Power phrases become attacks.",
    "Select on GET READY starts a FACE-OFF: pass the Vita to a friend for the same song.",
    "Gold medal: a full combo. Silver: 95% of the notes. Bronze: four stars.",
    "R on the results screen shows your last ten plays of that chart.",
    "Calibrate from the main menu if the notes feel early or late.",
    "The Daily Song changes at midnight. Finish it every day to keep your streak.",
    "Check for newer charts from the song list's Select menu.",
    "Each leg of the World Tour ends on a boss battle.",
    "Pick a title for your player card from any achievement you've unlocked.",
    "Race your best: GHOST shows how far ahead of your best run you are.",
};
static int tip_at = -1;

const char * res_tip(bool next)
{
    int n = (int)(sizeof(tips) / sizeof(tips[0]));
    if (tip_at < 0)
        tip_at = (int)(plat_now_us() / 1000) % n;
    else if (next)
        tip_at = (tip_at + 1) % n;
    return tips[tip_at];
}

void res_loading(float done, const char * text)
{
    gfx_begin(0, 0, 0);
    gfx_2d();
    res_title_art();
    gfx_rect_grad(0, 420, SCREEN_W, 90, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 210));
    if (res.title)
        font_draw_shadow(res.title, text, SCREEN_W / 2, 440, 34, RGBA(255, 220, 120, 255), ALIGN_CENTER);
    float w = 300, x = (SCREEN_W - w) / 2, y = 486;
    gfx_rect(x - 2, y - 2, w + 4, 10, RGBA(0, 0, 0, 220));
    gfx_rect(x, y, w * (done < 0 ? 0 : done > 1 ? 1 : done), 6, RGBA(255, 200, 90, 255));
    if (res.ui)
        font_draw(res.ui, res_tip(false), SCREEN_W / 2, 504, 17, RGBA(220, 215, 205, 220), ALIGN_CENTER);
    gfx_end();
    plat_present();
}

static const char * sound_names[SND_COUNT] = {
    "miss", "overstrum_1", "overstrum_2", "overstrum_3", "overstrum_4", "sp_award", "sp_deploy", "sp_ready",
    "sp_release", "star", "star_gold", "sp_crowd", "crowd_good", "crowd_neutral", "crowd_bad", "cheer_1",
    "cheer_2", "cheer_3", "boo_1", "boo_2", "boo_3", "applause", "ui_move", "ui_select", "ui_back",
    "drum_kick_1", "drum_kick_2", "drum_kick_3", "drum_snare_1", "drum_snare_2", "drum_snare_3", "drum_hat_1",
    "drum_hat_2", "drum_hat_3", "drum_tom_hi_1", "drum_tom_hi_2", "drum_tom_mid_1", "drum_tom_mid_2", "drum_tom_lo_1",
    "drum_tom_lo_2", "drum_ride_1", "drum_ride_2", "drum_crash_1", "drum_crash_2",
};
static volatile bool sounds_ready;

// unpacking a minute of Vorbis takes the Vita seconds: it happens while the title is up
static int load_sounds(void * arg)
{
    (void)arg;
    int64_t t0 = plat_now_us();
    for (int i = 0; i < SND_COUNT; i++)
        if (!res.snd[i])
            res.snd[i] = load_sound(sound_names[i]);
    plat_log("loaded: sounds %d ms", (int)((plat_now_us() - t0) / 1000));
    __sync_synchronize();
    sounds_ready = true;
    return 0;
}

void res_wait_sounds(void)
{
    while (!sounds_ready)
        plat_sleep_us(2000);
}

void res_load(void)
{
    char a[512], b[512];
    const char * dir = plat_asset_dir();
    int64_t t0 = plat_now_us();
    // first what the loading screen shows: the title picture and the title font
    res.bg_title = load_tex("bg_title.jpg", 0);
    snprintf(a, sizeof(a), "%sfont_title.png", dir);
    snprintf(b, sizeof(b), "%sfont_title.fnt", dir);
    res.title = font_load(a, b);
    res_loading(0.08f, "LOADING");
    snprintf(a, sizeof(a), "%sfont_ui.png", dir);
    snprintf(b, sizeof(b), "%sfont_ui.fnt", dir);
    res.ui = font_load(a, b);
    res_loading(0.15f, "LOADING");
    snprintf(a, sizeof(a), "%sgame.png", dir);
    snprintf(b, sizeof(b), "%sgame.txt", dir);
    res.game = atlas_load(a, b, TEX_MIPMAP);
    res_loading(0.42f, "LOADING");
    snprintf(a, sizeof(a), "%shud.png", dir);
    snprintf(b, sizeof(b), "%shud.txt", dir);
    res.hud = atlas_load(a, b, TEX_MIPMAP);
    snprintf(a, sizeof(a), "%sstage.png", dir);
    snprintf(b, sizeof(b), "%sstage.txt", dir);
    res.stage = atlas_load(a, b, TEX_MIPMAP);
    res_loading(0.82f, "LOADING");
    res.bg_menu = load_tex("bg_menu.jpg", 0);
    res.bg_results = load_tex("bg_results.jpg", 0);
    res.bg_stage = load_tex("bg_stage.jpg", 0);
    res.logo = load_tex("logo.png", TEX_MIPMAP);
    res.vignette = load_tex("vignette.png", 0);
    res.hands = load_tex("crowd_hands.png", 0);
    res_loading(1, "LOADING");
    plat_log("loaded: pictures %d ms", (int)((plat_now_us() - t0) / 1000));
    // the menus' own sounds now, the rest on a thread
    for (int i = SND_UI_MOVE; i <= SND_UI_BACK; i++)
        res.snd[i] = load_sound(sound_names[i]);
    plat_thread("sounds", load_sounds, NULL, 2);
    // the highways on offer: assets/hw_<name>.png
    DIR * d = opendir(dir);
    if (d) {
        struct dirent * e;
        while ((e = readdir(d)) && res.highway_count < 24) {
            size_t n = strlen(e->d_name);
            if (n > 7 && strncmp(e->d_name, "hw_", 3) == 0 && strcmp(e->d_name + n - 4, ".png") == 0 && n - 7 < 32) {
                memcpy(res.highway_names[res.highway_count], e->d_name + 3, n - 7);
                res.highway_names[res.highway_count][n - 7] = 0;
                res.highway_count++;
            }
        }
        closedir(d);
    }
    qsort(res.highway_names, res.highway_count, sizeof(res.highway_names[0]), by_string);
    // the venues on offer: "<zone>|<title>|<where the band stands>" lines
    char path[512];
    snprintf(path, sizeof(path), "%svenues/venues.txt", dir);
    FILE * f = fopen(path, "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f) && res.venue_count < 48) {
        char * bar = strchr(line, '|');
        if (!bar || bar - line >= 16)
            continue;
        *bar = 0;
        char * title = bar + 1, * end = strchr(title, '|');
        if (end) {
            *end = 0;
            static const char * roles[4] = {"drummer", "vocalist", "guitarist", "bassist"};
            for (int r = 0; r < 4; r++) {
                char * m = strstr(end + 1, roles[r]);
                float * v = res.venue_marks[res.venue_count][r];
                if (m)
                    sscanf(m + strlen(roles[r]), "%f %f %f", &v[0], &v[1], &v[2]);
            }
        }
        snprintf(res.venue_zones[res.venue_count], 16, "%s", line);
        snprintf(res.venue_titles[res.venue_count], 32, "%s", title);
        res.venue_count++;
    }
    if (f)
        fclose(f);
    snprintf(path, sizeof(path), "%sband/band.txt", dir);
    f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f) && res.band_count < 24) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0])
            snprintf(res.band_names[res.band_count++], 16, "%s", line);
    }
    if (f)
        fclose(f);
}

Tex * res_highway(const char * name)
{
    static Tex * tex;
    static char loaded[32];
    if (tex && strcmp(loaded, name) == 0)
        return tex;
    tex_free(tex);
    char path[96];
    snprintf(path, sizeof(path), "hw_%s.png", name);
    tex = load_tex(path, TEX_MIPMAP | TEX_REPEAT);
    if (!tex && strcmp(name, "classic") != 0)
        tex = load_tex("hw_classic.png", TEX_MIPMAP | TEX_REPEAT);
    snprintf(loaded, sizeof(loaded), "%s", name);
    return tex;
}

// the Options' preview of a highway: its own copy, so the one a paused song is drawing is never freed under it
Tex * res_highway_preview(const char * name)
{
    static Tex * tex;
    static char loaded[32];
    if (tex && strcmp(loaded, name) == 0)
        return tex;
    tex_free(tex);
    char path[96];
    snprintf(path, sizeof(path), "hw_%s.png", name);
    tex = load_tex(path, TEX_MIPMAP);
    snprintf(loaded, sizeof(loaded), "%s", name);
    return tex;
}

bool res_venue(const char * zone, Tex ** stage, Tex ** lights)
{
    static Tex * base, * glow;
    static char loaded[16];
    if (!base || strcmp(loaded, zone) != 0) {
        tex_free(base);
        tex_free(glow);
        base = glow = NULL;
        loaded[0] = 0;
        char path[64];
        snprintf(path, sizeof(path), "venues/%s.jpg", zone);
        base = load_tex(path, 0);
        snprintf(path, sizeof(path), "venues/%s_glow.jpg", zone);
        glow = base ? load_tex(path, 0) : NULL;
        if (base)
            snprintf(loaded, sizeof(loaded), "%s", zone);
    }
    *stage = base;
    *lights = glow;
    return base != NULL;
}

const float (*res_venue_marks(const char * zone))[3]
{
    for (int i = 0; i < res.venue_count; i++)
        if (strcmp(res.venue_zones[i], zone) == 0)
            return (const float (*)[3])res.venue_marks[i];
    return NULL;
}

static Tex * sheet[4];
static char loaded[4][16];

// about 17 MB of pictures, wanted only during a song
void res_band_free(void)
{
    for (int r = 0; r < 4; r++) {
        tex_free(sheet[r]);
        sheet[r] = NULL;
        loaded[r][0] = 0;
    }
}

Tex * res_band_sheet(int role, const char * name)
{
    if (!sheet[role] || strcmp(loaded[role], name) != 0) {
        tex_free(sheet[role]);
        char path[64];
        snprintf(path, sizeof(path), "band/%s.png", name);
        sheet[role] = load_tex(path, 0);
        snprintf(loaded[role], sizeof(loaded[role]), "%s", name);
    }
    return sheet[role];
}

const char * res_venue_title(const char * zone)
{
    for (int i = 0; i < res.venue_count; i++)
        if (strcmp(res.venue_zones[i], zone) == 0)
            return res.venue_titles[i];
    return zone;
}

void ui_sound(int which)
{
    sound_play(res.snd[which], 0.6f, false);
}

void draw_background(Tex * t, uint32_t color)
{
    if (!t) {
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(10, 10, 14, 255));
        return;
    }
    Sprite s = sprite_whole(t);
    gfx_sprite_uv(&s, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 960.0f / t->w, 544.0f / t->h, color);
}
