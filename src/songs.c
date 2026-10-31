#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "songs.h"
#include "dl.h"
#include "songconv.h"
#include "plat.h"

Config cfg;
static Song * songs;
static int count, cap;

static const char * part_names[PART_COUNT] = {"Guitar", "Bass", "Rhythm", "Co-op Guitar", "Keys", "Drums", "Vocals"};
static const char * part_keys[PART_COUNT] = {"guitar", "bass", "rhythm", "coop", "keys", "drums", "vocals"};
static const char * diff_names[DIFF_COUNT] = {"Easy", "Medium", "Hard", "Expert"};

const char * part_name(int part)
{
    return part >= 0 && part < PART_COUNT ? part_names[part] : "?";
}

const char * diff_name(int diff)
{
    return diff >= 0 && diff < DIFF_COUNT ? diff_names[diff] : "?";
}

static void trim(char * s)
{
    char * e = s + strlen(s);
    while (e > s && (unsigned char)e[-1] <= ' ')
        *--e = 0;
    char * b = s;
    while (*b && (unsigned char)*b <= ' ')
        b++;
    if (b != s)
        memmove(s, b, strlen(b) + 1);
}

// "The Strokes" sorts as "strokes"
static void sort_key(char * out, size_t size, const char * in)
{
    if (strncasecmp(in, "the ", 4) == 0)
        in += 4;
    size_t i = 0;
    for (; in[i] && i + 1 < size; i++)
        out[i] = tolower((unsigned char)in[i]);
    out[i] = 0;
}

static bool load_song(const char * folder)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/ghvita.ini", folder);
    FILE * f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "%s/song.ini", folder);
        f = fopen(path, "r");
    }
    if (!f)
        return false;
    if (count == cap) {
        cap = cap ? cap * 2 : 256;
        songs = realloc(songs, cap * sizeof(Song));
    }
    Song * s = &songs[count];
    memset(s, 0, sizeof(*s));
    snprintf(s->folder, sizeof(s->folder), "%s", folder);
    for (int i = 0; i < PART_COUNT; i++) {
        s->diff[i] = -1;
        s->part_stem[i] = -1;
    }
    char line[512];
    char part_stem_names[PART_COUNT][24];
    memset(part_stem_names, 0, sizeof(part_stem_names));
    while (fgets(line, sizeof(line), f)) {
        char * eq = strchr(line, '=');
        if (!eq || line[0] == '[')
            continue;
        *eq = 0;
        char * k = line, * v = eq + 1;
        trim(k);
        trim(v);
#define STR(key, field) else if (strcmp(k, key) == 0) snprintf(s->field, sizeof(s->field), "%s", v);
        if (0) {
        }
        STR("name", name)
        STR("artist", artist)
        STR("album", album)
        STR("genre", genre)
        STR("year", year)
        STR("charter", charter)
        STR("source", source)
#undef STR
        else if (strcmp(k, "song_length") == 0)
            s->length_ms = atoi(v);
        else if (strcmp(k, "preview_start_time") == 0)
            s->preview_ms = atoi(v);
        else if (strcmp(k, "parts") == 0) {
            for (char * t = strtok(v, ","); t; t = strtok(NULL, ","))
                for (int p = 0; p < PART_COUNT; p++)
                    if (strcmp(t, part_keys[p]) == 0)
                        s->parts |= 1 << p;
        } else if (strcmp(k, "stems") == 0) {
            for (char * t = strtok(v, ","); t && s->stem_count < MAX_STEMS; t = strtok(NULL, ","))
                snprintf(s->stems[s->stem_count++], sizeof(s->stems[0]), "%s", t);
        } else if (strncmp(k, "stem_", 5) == 0) {
            int p = atoi(k + 5);
            if (p >= 0 && p < PART_COUNT)
                snprintf(part_stem_names[p], sizeof(part_stem_names[p]), "%s", v);
        } else if (strncmp(k, "diff_", 5) == 0) {
            const char * which = k + 5;
            int p = strcmp(which, "guitar") == 0 ? PART_GUITAR : strcmp(which, "bass") == 0 ? PART_BASS
                  : strcmp(which, "rhythm") == 0 ? PART_RHYTHM : strcmp(which, "guitar_coop") == 0 ? PART_COOP
                  : strcmp(which, "keys") == 0 ? PART_KEYS : -1;
            if (p >= 0)
                s->diff[p] = atoi(v);
        }
    }
    fclose(f);
    if (!s->parts || !s->stem_count)
        return false;
    for (int p = 0; p < PART_COUNT; p++)
        for (int i = 0; i < s->stem_count; i++)
            if (part_stem_names[p][0] && strcmp(part_stem_names[p], s->stems[i]) == 0)
                s->part_stem[p] = i;
    if (!s->name[0])
        snprintf(s->name, sizeof(s->name), "%s", strrchr(folder, '/') ? strrchr(folder, '/') + 1 : folder);
    sort_key(s->sort_name, sizeof(s->sort_name), s->name);
    sort_key(s->sort_artist, sizeof(s->sort_artist), s->artist);
    count++;
    return true;
}

static void (*scan_progress)(int found, const char * converting);

static bool has_ext(const char * name, const char * ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static void scan_dir(const char * dir, int depth)
{
    DIR * d = opendir(dir);
    if (!d)
        return;
    // names first (a .sng unpacked into this folder mustn't upset readdir)
    char (*names)[256] = NULL;
    int n = 0, cap = 0;
    struct dirent * e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, cap * sizeof(*names));
        }
        snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char path[400];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (!S_ISDIR(st.st_mode)) {
            // a .sng copied onto the card: unpacked into a song folder of its own, then the package goes
            if (!has_ext(names[i], ".sng"))
                continue;
            if (scan_progress)
                scan_progress(count, names[i]);
            char out[400], err[160];
            if (songconv_install(path, dir, NULL, out, sizeof(out), err, sizeof(err))) {
                remove(path);
                if (load_song(out))
                    continue;
            }
            plat_log("songs: can't add %s: %s", path, err);
            continue;
        }
        if (load_song(path)) {
            if (scan_progress && count % 25 == 0)
                scan_progress(count, NULL);
        } else if (songconv_is_raw(path)) {
            // a Clone Hero song folder copied onto the card: converted where it is, once
            if (scan_progress)
                scan_progress(count, names[i]);
            ConvOptions opt = {.in_place = true};
            char out[400], err[160];
            if (songconv_install(path, dir, &opt, out, sizeof(out), err, sizeof(err)) && load_song(path))
                continue;
            plat_log("songs: can't add %s: %s", path, err);
        } else if (depth < 2)
            scan_dir(path, depth + 1);
    }
    free(names);
}

static const char * base_name(const char * folder)
{
    const char * b = strrchr(folder, '/');
    return b ? b + 1 : folder;
}

static void load_scores(void)
{
    char path[400];
    snprintf(path, sizeof(path), "%sscores.txt", plat_data_dir());
    FILE * f = fopen(path, "r");
    if (!f)
        return;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        char * tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        int part, diff, score, stars, percent, fc;
        if (sscanf(tab + 1, "%d %d %d %d %d %d", &part, &diff, &score, &stars, &percent, &fc) != 6)
            continue;
        if (part < 0 || part >= PART_COUNT || diff < 0 || diff >= DIFF_COUNT)
            continue;
        for (int i = 0; i < count; i++) {
            if (strcmp(base_name(songs[i].folder), line) == 0) {
                Score * b = &songs[i].best[part][diff];
                if (score > b->score) {
                    b->score = score;
                    b->stars = stars;
                    b->percent = percent;
                }
                b->fc |= fc;
                break;
            }
        }
    }
    fclose(f);
}

int songs_scan(void (*progress)(int found, const char * converting))
{
    scan_progress = progress;
    count = 0;
    char dir[400];
    snprintf(dir, sizeof(dir), "%ssongs", plat_data_dir());
    scan_dir(dir, 0);
    load_scores();
    songs_sort(cfg.sort);
    plat_log("songs: %d", count);
    return count;
}

int songs_find(const char * folder)
{
    for (int i = 0; i < count; i++)
        if (strcmp(songs[i].folder, folder) == 0)
            return i;
    return -1;
}

int songs_add(const char * folder)
{
    int old = songs_find(folder);
    if (old >= 0) {
        // installed again over itself: read afresh
        memmove(&songs[old], &songs[old + 1], (count - old - 1) * sizeof(Song));
        count--;
    }
    if (!load_song(folder))
        return -1;
    songs_sort(cfg.sort);
    plat_log("songs: added %s (%d now)", folder, count);
    return songs_find(folder);
}

// "AC/DC" and "ac-dc" alike
static void match_key(char * out, size_t size, const char * in)
{
    size_t k = 0;
    if (strncasecmp(in, "the ", 4) == 0)
        in += 4;
    for (; *in && k + 1 < size; in++)
        if (isalnum((unsigned char)*in) || (unsigned char)*in >= 0x80)
            out[k++] = (char)tolower((unsigned char)*in);
    out[k] = 0;
}

int songs_have(const char * source, const char * artist, const char * name)
{
    char a[96], n[96], x[96];
    match_key(a, sizeof(a), artist ? artist : "");
    match_key(n, sizeof(n), name ? name : "");
    int have = 0;
    for (int i = 0; i < count && have < 2; i++) {
        if (source && source[0] && strcmp(songs[i].source, source) == 0)
            have = 2;
        else if (!have) {
            match_key(x, sizeof(x), songs[i].name);
            if (strcmp(x, n) != 0)
                continue;
            match_key(x, sizeof(x), songs[i].artist);
            if (strcmp(x, a) == 0)
                have = 1;
        }
    }
    return have;
}

void songs_delete(int index)
{
    if (index < 0 || index >= count)
        return;
    plat_log("songs: deleted %s", songs[index].folder);
    dl_remove_tree(songs[index].folder);
    memmove(&songs[index], &songs[index + 1], (count - index - 1) * sizeof(Song));
    count--;
}

int songs_count(void)
{
    return count;
}

Song * songs_get(int index)
{
    return index >= 0 && index < count ? &songs[index] : NULL;
}

static int by_title(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = strcmp(x->sort_name, y->sort_name);
    return c ? c : strcmp(x->sort_artist, y->sort_artist);
}

static int by_artist(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = strcmp(x->sort_artist, y->sort_artist);
    return c ? c : strcmp(x->sort_name, y->sort_name);
}

void songs_sort(int mode)
{
    if (count)
        qsort(songs, count, sizeof(Song), mode == SORT_ARTIST ? by_artist : by_title);
}

void songs_save_score(Song * s, int part, int diff, Score score)
{
    Score * b = &s->best[part][diff];
    if (score.score > b->score) {
        b->score = score.score;
        b->stars = score.stars;
        b->percent = score.percent;
    }
    b->fc |= score.fc;
    char path[400];
    snprintf(path, sizeof(path), "%sscores.txt", plat_data_dir());
    FILE * f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%s\t%d %d %d %d %d %d\n", base_name(s->folder), part, diff, score.score, score.stars, score.percent,
            score.fc);
    fclose(f);
}

// ------------------------------------------------------------------------------------------------------- settings

static void config_defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.speed = 5;
    snprintf(cfg.highway, sizeof(cfg.highway), "classic");
    snprintf(cfg.venue, sizeof(cfg.venue), "song");
    cfg.tilt = true;
    cfg.music_vol = 10;
    cfg.sfx_vol = 8;
    cfg.crowd_vol = 5;
    cfg.last_diff = DIFF_MEDIUM;
}

void config_load(void)
{
    config_defaults();
    char path[400];
    snprintf(path, sizeof(path), "%ssettings.txt", plat_data_dir());
    FILE * f = fopen(path, "r");
    if (!f)
        return;
    char line[400];
    while (fgets(line, sizeof(line), f)) {
        char * eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char * k = line, * v = eq + 1;
        trim(k);
        trim(v);
        int n = atoi(v);
        if (strcmp(k, "speed") == 0)
            cfg.speed = n < 1 ? 1 : n > 10 ? 10 : n;
        else if (strcmp(k, "highway") == 0)
            snprintf(cfg.highway, sizeof(cfg.highway), "%s", v);
        else if (strcmp(k, "venue") == 0)
            snprintf(cfg.venue, sizeof(cfg.venue), "%s", v);
        else if (strcmp(k, "no_band") == 0)
            cfg.no_band = atoi(v) != 0;
        else if (strcmp(k, "sing_along") == 0)
            cfg.sing_along = atoi(v) != 0;
        else if (strcmp(k, "offset_ms") == 0)
            cfg.offset_ms = n;
        else if (strcmp(k, "layout") == 0)
            cfg.layout = n;
        else if (strcmp(k, "drum_layout") == 0)
            cfg.drum_layout = n;
        else if (strcmp(k, "drum_hits") == 0)
            cfg.drum_hits = n != 0;
        else if (strcmp(k, "tilt") == 0)
            cfg.tilt = n;
        else if (strcmp(k, "nofail") == 0)
            cfg.nofail = n;
        else if (strcmp(k, "music_vol") == 0)
            cfg.music_vol = n;
        else if (strcmp(k, "sfx_vol") == 0)
            cfg.sfx_vol = n;
        else if (strcmp(k, "crowd_vol") == 0)
            cfg.crowd_vol = n;
        else if (strcmp(k, "lefty") == 0)
            cfg.lefty = n;
        else if (strcmp(k, "show_fps") == 0)
            cfg.show_fps = n;
        else if (strcmp(k, "last_part") == 0)
            cfg.last_part = n;
        else if (strcmp(k, "last_diff") == 0)
            cfg.last_diff = n;
        else if (strcmp(k, "sort") == 0)
            cfg.sort = n;
        else if (strcmp(k, "last_song") == 0)
            snprintf(cfg.last_song, sizeof(cfg.last_song), "%s", v);
    }
    fclose(f);
}

void config_save(void)
{
    char path[400], tmp[410];
    snprintf(path, sizeof(path), "%ssettings.txt", plat_data_dir());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "speed = %d\nhighway = %s\noffset_ms = %d\nlayout = %d\ntilt = %d\nnofail = %d\nmusic_vol = %d\n"
               "sfx_vol = %d\ncrowd_vol = %d\nlefty = %d\nshow_fps = %d\nlast_part = %d\nlast_diff = %d\nsort = %d\n"
               "last_song = %s\nvenue = %s\nno_band = %d\nsing_along = %d\ndrum_layout = %d\ndrum_hits = %d\n",
            cfg.speed, cfg.highway, cfg.offset_ms, cfg.layout, cfg.tilt, cfg.nofail, cfg.music_vol, cfg.sfx_vol,
            cfg.crowd_vol, cfg.lefty, cfg.show_fps, cfg.last_part, cfg.last_diff, cfg.sort, cfg.last_song, cfg.venue, cfg.no_band, cfg.sing_along, cfg.drum_layout, cfg.drum_hits);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

uint32_t lane_buttons(int lane)
{
    // default: the shoulders take the outer lanes, the thumbs the three in the middle, so chords mostly use two
    // thumbs; alternate: one D-pad direction or face button each
    static const uint32_t layouts[2][6] = {
        {BTN_L, BTN_LEFT, BTN_UP | BTN_TRIANGLE, BTN_CIRCLE, BTN_R, BTN_CROSS | BTN_DOWN},
        {BTN_L, BTN_LEFT, BTN_UP, BTN_TRIANGLE, BTN_R, BTN_CROSS | BTN_DOWN | BTN_CIRCLE},
    };
    int l = cfg.layout == 1 ? 1 : 0;
    if (lane < 0 || lane > 5)
        return 0;
    return layouts[l][lane];
}
