#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "songs.h"
#include "player.h"
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
// letters and digits: digit runs compare by value, so "Track 2" sorts before "Track 10"
static int natural_cmp(const char * a, const char * b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            size_t la = 0, lb = 0;
            while (isdigit((unsigned char)a[la])) la++;
            while (isdigit((unsigned char)b[lb])) lb++;
            int cmp;
            if (la != lb) cmp = (int)(la - lb);
            else cmp = memcmp(a, b, la < lb ? la : lb);
            if (cmp)
                return cmp;
            a += la;
            b += lb;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca - cb;
            a++;
            b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

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
                  : strcmp(which, "keys") == 0 ? PART_KEYS : strcmp(which, "drums") == 0 ? PART_DRUMS
                  : strcmp(which, "vocals") == 0 ? PART_VOCALS : -1;
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

// ------------------------------------------------------------------------------------------------ the library file
// What every folder said last time, so a library of thousands doesn't mean opening thousands of files at each start.
// A folder whose time has changed is read afresh. New songs, gone songs and a new Song layout all just rewrite it

#define LIB_MAGIC 0x3142494cu

typedef struct {
    uint32_t magic, size;
} LibHead;

static Song * lib;
static int lib_n, lib_mask;
static int * lib_slots;
static bool lib_changed;

static uint32_t folder_hash(const char * s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void lib_path(char * path, size_t size)
{
    snprintf(path, size, "%slibrary.bin", plat_data_dir());
}

static void lib_free(void)
{
    free(lib);
    free(lib_slots);
    lib = NULL;
    lib_slots = NULL;
    lib_n = 0;
}

static void lib_load(void)
{
    char path[400];
    lib_path(path, sizeof(path));
    FILE * f = fopen(path, "rb");
    if (!f)
        return;
    LibHead h;
    long size = 0;
    if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == LIB_MAGIC && h.size == sizeof(Song)) {
        fseek(f, 0, SEEK_END);
        size = ftell(f) - (long)sizeof(h);
        fseek(f, sizeof(h), SEEK_SET);
    }
    int n = size > 0 ? (int)(size / sizeof(Song)) : 0;
    int slots = 64;
    while (slots < n * 2)
        slots *= 2;
    lib = n ? malloc(n * sizeof(Song)) : NULL;
    lib_slots = n ? malloc(slots * sizeof(int)) : NULL;
    if (lib && lib_slots && fread(lib, sizeof(Song), n, f) == (size_t)n) {
        lib_n = n;
        lib_mask = slots - 1;
        memset(lib_slots, -1, slots * sizeof(int));
        for (int i = 0; i < n; i++) {
            lib[i].folder[sizeof(lib[i].folder) - 1] = 0;
            uint32_t at = folder_hash(lib[i].folder) & lib_mask;
            while (lib_slots[at] >= 0)
                at = (at + 1) & lib_mask;
            lib_slots[at] = i;
        }
    } else
        lib_free();
    fclose(f);
}

static const Song * lib_find(const char * folder, long stamp)
{
    if (!lib_n)
        return NULL;
    for (uint32_t at = folder_hash(folder) & lib_mask; lib_slots[at] >= 0; at = (at + 1) & lib_mask) {
        const Song * s = &lib[lib_slots[at]];
        if (strcmp(s->folder, folder) == 0)
            return s->stamp == stamp ? s : NULL;
    }
    return NULL;
}

// scores, plays and favourites have their own files and come in after this
static void lib_save(void)
{
    char path[400], fresh[410];
    lib_path(path, sizeof(path));
    snprintf(fresh, sizeof(fresh), "%s.new", path);
    FILE * f = fopen(fresh, "wb");
    if (!f)
        return;
    LibHead h = {LIB_MAGIC, sizeof(Song)};
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(songs, sizeof(Song), count, f) == (size_t)count;
    ok &= fclose(f) == 0;
    remove(path);
    if (!ok || rename(fresh, path) != 0)
        remove(fresh);
}

void songs_forget(void)
{
    char path[400];
    lib_path(path, sizeof(path));
    remove(path);
}

static void scan_dir(const char * dir, int depth);

// a folder listed by scan_dir: what the library file has for it, or read it
static void scan_entry(const char * dir, const char * name, bool is_dir, long stamp, int depth)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    const Song * known = is_dir ? lib_find(path, stamp) : NULL;
    if (known) {
        if (count == cap) {
            cap = cap ? cap * 2 : 256;
            songs = realloc(songs, cap * sizeof(Song));
        }
        songs[count++] = *known;
        if (scan_progress && count % 250 == 0)
            scan_progress(count, NULL);
        return;
    }
    lib_changed = true;
    struct stat st;
    if (stat(path, &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        // a .sng copied onto the card: unpacked into a song folder of its own, then the package goes
        if (!has_ext(name, ".sng"))
            return;
        if (scan_progress)
            scan_progress(count, name);
        char out[400], err[160];
        if (songconv_install(path, dir, NULL, out, sizeof(out), err, sizeof(err))) {
            remove(path);
            if (load_song(out)) {
                songs[count - 1].added = (long)st.st_mtime;
                return;
            }
        }
        plat_log("songs: can't add %s: %s", path, err);
        return;
    }
    if (load_song(path)) {
        songs[count - 1].added = (long)st.st_mtime;
        songs[count - 1].stamp = stamp;
        if (scan_progress && count % 25 == 0)
            scan_progress(count, NULL);
    } else if (songconv_is_raw(path)) {
        // a Clone Hero song folder copied onto the card: converted where it is, once
        if (scan_progress)
            scan_progress(count, name);
        ConvOptions opt = {.in_place = true};
        char out[400], err[160];
        if (songconv_install(path, dir, &opt, out, sizeof(out), err, sizeof(err)) && load_song(path)) {
            songs[count - 1].added = (long)st.st_mtime;
            return;
        }
        plat_log("songs: can't add %s: %s", path, err);
    } else if (depth < 2)
        scan_dir(path, depth + 1);
}

static void scan_dir(const char * dir, int depth)
{
    DIR * d = opendir(dir);
    if (!d)
        return;
    // names first (a .sng unpacked into this folder mustn't upset readdir)
    typedef struct {
        char name[256];
        bool is_dir;
        long stamp;
    } Entry;
    Entry * names = NULL;
    int n = 0, cap_names = 0;
    struct dirent * e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (n == cap_names) {
            cap_names = cap_names ? cap_names * 2 : 64;
            names = realloc(names, cap_names * sizeof(*names));
        }
        Entry * en = &names[n++];
        snprintf(en->name, sizeof(en->name), "%s", e->d_name);
#ifdef __vita__
        // the Vita's listing carries the file's details: no stat for each of thousands of folders
        const SceDateTime * t = &e->d_stat.st_mtime;
        en->is_dir = SCE_S_ISDIR(e->d_stat.st_mode);
        en->stamp = (long)(((unsigned long)(t->year - 1980) << 25) | ((unsigned long)t->month << 21) |
                           ((unsigned long)t->day << 16) | (t->hour << 11) | (t->minute << 5) | (t->second >> 1));
#else
        char path[700];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        en->is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        en->stamp = en->is_dir ? (long)st.st_mtime : 0;
#endif
    }
    closedir(d);
    for (int i = 0; i < n; i++)
        scan_entry(dir, names[i].name, names[i].is_dir, names[i].stamp, depth);
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
    snprintf(path, sizeof(path), "%sscores.txt", player_dir());
    FILE * f = fopen(path, "r");
    if (!f)
        return;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        char * tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        int part, diff, score, stars, percent, fc, acc = 0;
        long when = 0;      // lines from before build 52 have no time, from before 227 no accuracy
        if (sscanf(tab + 1, "%d %d %d %d %d %d %ld %d", &part, &diff, &score, &stars, &percent, &fc, &when, &acc) < 6)
            continue;
        if (part < 0 || part >= PART_COUNT || diff < 0 || diff >= DIFF_COUNT)
            continue;
        for (int i = 0; i < count; i++) {
            if (strcmp(base_name(songs[i].folder), line) == 0) {
                Score * b = &songs[i].best[part][diff];
                songs[i].plays++;
                if (when > songs[i].last_played)
                    songs[i].last_played = when;
                if (score > b->score) {
                    b->score = score;
                    b->stars = stars;
                    b->percent = percent;
                }
                b->fc |= fc;
                if (acc > b->acc && acc <= 1000)
                    b->acc = (uint16_t)acc;
                break;
            }
        }
    }
    fclose(f);
}

static void load_favs(void);

void songs_reload_player(void)
{
    for (int i = 0; i < count; i++) {
        memset(songs[i].best, 0, sizeof(songs[i].best));
        songs[i].plays = 0;
        songs[i].last_played = 0;
        songs[i].fav = false;
    }
    load_scores();
    load_favs();
}

// favourites: a line for each song's folder
static void load_favs(void)
{
    char path[400], line[300];
    snprintf(path, sizeof(path), "%sfavourites.txt", player_dir());
    FILE * f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        for (int i = 0; i < count; i++)
            if (strcmp(base_name(songs[i].folder), line) == 0)
                songs[i].fav = true;
    }
    fclose(f);
}

void songs_set_fav(Song * s, bool fav)
{
    s->fav = fav;
    char path[400];
    snprintf(path, sizeof(path), "%sfavourites.txt", player_dir());
    FILE * f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < count; i++)
        if (songs[i].fav)
            fprintf(f, "%s\n", base_name(songs[i].folder));
    fclose(f);
}

int songs_scan(void (*progress)(int found, const char * converting))
{
    scan_progress = progress;
    count = 0;
    char dir[400];
    snprintf(dir, sizeof(dir), "%ssongs", plat_data_dir());
    lib_load();
    lib_changed = false;
    int known = lib_n;
    scan_dir(dir, 0);
    lib_free();
    for (int i = 0; i < count; i++) {
        memset(songs[i].best, 0, sizeof(songs[i].best));
        songs[i].plays = 0;
        songs[i].last_played = 0;
        songs[i].fav = false;
    }
    if (lib_changed || known != count)
        lib_save();
    load_scores();
    load_favs();
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
    songs[count - 1].added = (long)time(NULL);
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
    int c = natural_cmp(x->sort_name, y->sort_name);
    return c ? c : natural_cmp(x->sort_artist, y->sort_artist);
}

static int by_artist(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = natural_cmp(x->sort_artist, y->sort_artist);
    return c ? c : natural_cmp(x->sort_name, y->sort_name);
}

static int by_album(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = natural_cmp(x->album, y->album);
    return c ? c : by_artist(a, b);
}

static int by_year(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = atoi(x->year) - atoi(y->year);
    return c ? c : by_artist(a, b);
}

static int by_genre(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = natural_cmp(x->genre, y->genre);
    return c ? c : by_artist(a, b);
}

static int by_charter(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = natural_cmp(x->charter, y->charter);
    return c ? c : by_artist(a, b);
}

// how hard the part you last played is, easiest first
static int by_hard(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = x->diff[cfg.last_part] - y->diff[cfg.last_part];
    return c ? c : by_artist(a, b);
}

static int by_length(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = x->length_ms - y->length_ms;
    return c ? c : by_artist(a, b);
}

// the newest first
static int by_new(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    return x->added != y->added ? (x->added < y->added ? 1 : -1) : by_artist(a, b);
}

// the most played first
static int by_plays(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    int c = y->plays - x->plays;
    return c ? c : by_artist(a, b);
}

// the latest played first; never played at the end, by artist
static int by_recent(const void * a, const void * b)
{
    const Song * x = a, * y = b;
    if (x->last_played != y->last_played)
        return x->last_played < y->last_played ? 1 : -1;
    return by_artist(a, b);
}

void songs_sort(int mode)
{
    static int (* const cmp[SORT_COUNT])(const void *, const void *) = {
        by_title, by_artist, by_album, by_year, by_genre, by_charter, by_hard, by_length, by_new, by_plays, by_recent,
    };
    if (count)
        qsort(songs, count, sizeof(Song), cmp[mode >= 0 && mode < SORT_COUNT ? mode : SORT_ARTIST]);
}

const char * sort_name(int mode)
{
    static const char * const names[SORT_COUNT] = {"Title", "Artist", "Album", "Year", "Genre", "Charter", "How hard", "Length",
                                                   "Newest", "Most played", "Recently played"};
    return names[mode >= 0 && mode < SORT_COUNT ? mode : SORT_ARTIST];
}

void songs_group(const Song * s, int mode, char * out, size_t size)
{
    const char * key = mode == SORT_TITLE ? s->sort_name : mode == SORT_ARTIST ? s->sort_artist : NULL;
    int n;
    switch (mode) {
    case SORT_ALBUM: snprintf(out, size, "%s", s->album[0] ? s->album : "No album"); return;
    case SORT_GENRE: snprintf(out, size, "%s", s->genre[0] ? s->genre : "No genre"); return;
    case SORT_CHARTER: snprintf(out, size, "%s", s->charter[0] ? s->charter : "Unknown charter"); return;
    case SORT_YEAR:
        n = atoi(s->year);
        if (n)
            snprintf(out, size, "%d0s", n / 10);
        else
            snprintf(out, size, "No year");
        return;
    case SORT_HARD:
        n = s->diff[cfg.last_part];
        if (n >= 0)
            snprintf(out, size, "Tier %d", n);
        else
            snprintf(out, size, "Untiered");
        return;
    case SORT_LENGTH: snprintf(out, size, "%d min", s->length_ms / 60000); return;
    case SORT_NEW: snprintf(out, size, "%s", ""); return;
    case SORT_PLAYS: snprintf(out, size, "%d plays", s->plays); return;
    case SORT_RECENT: {
        // headings by how long ago
        long ago = s->last_played ? (long)time(NULL) - s->last_played : -1;
        snprintf(out, size, "%s", ago < 0 ? "Not played yet" : ago < 86400 ? "Today" : ago < 7 * 86400 ? "This week"
                                  : ago < 31 * 86400 ? "This month" : "Earlier");
        return;
    }
    }
    unsigned char c = key && key[0] ? (unsigned char)key[0] : '#';
    if (c >= 'a' && c <= 'z')
        snprintf(out, size, "%c", c - 32);
    else
        snprintf(out, size, c >= 0x80 ? "Other" : "#");
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
    if (score.acc > b->acc)
        b->acc = score.acc;
    char path[400];
    snprintf(path, sizeof(path), "%sscores.txt", player_dir());
    FILE * f = fopen(path, "a");
    if (!f)
        return;
    long now = (long)time(NULL);
    fprintf(f, "%s\t%d %d %d %d %d %d %ld %d\n", base_name(s->folder), part, diff, score.score, score.stars, score.percent,
            score.fc, now, score.acc);
    fclose(f);
    s->last_played = now;
}

int songs_history(const Song * s, int part, int diff, Played * out, int max)
{
    char path[400];
    snprintf(path, sizeof(path), "%sscores.txt", player_dir());
    FILE * f = fopen(path, "r");
    if (!f || max <= 0) {
        if (f)
            fclose(f);
        return 0;
    }
    const char * name = base_name(s->folder);
    char line[600];
    int n = 0;          // a ring of the newest max, unrolled at the end
    while (fgets(line, sizeof(line), f)) {
        char * tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        if (strcmp(line, name))
            continue;
        int p, d, score, stars, percent, fc;
        long when = 0;
        if (sscanf(tab + 1, "%d %d %d %d %d %d %ld", &p, &d, &score, &stars, &percent, &fc, &when) < 6 || p != part ||
            d != diff)
            continue;
        Played * e = &out[n % max];
        e->score.score = score;
        e->score.stars = (uint8_t)stars;
        e->score.percent = (uint8_t)percent;
        e->score.fc = (uint8_t)fc;
        e->when = when;
        n++;
    }
    fclose(f);
    if (n > max) {
        Played tmp[64];
        int k = max > 64 ? 64 : max;
        for (int i = 0; i < k; i++)
            tmp[i] = out[(n + i) % max];
        memcpy(out, tmp, sizeof(Played) * k);
        n = k;
    }
    return n;
}

// ------------------------------------------------------------------------------------------------------- settings

void config_defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.speed = 5;
    cfg.song_speed = 100;
    cfg.frame_rate = 60;
    cfg.hit_ratings = 1;
    cfg.hw_length = 1;
    snprintf(cfg.highway, sizeof(cfg.highway), "judy");
    snprintf(cfg.venue, sizeof(cfg.venue), "song");
    cfg.tilt = true;
    cfg.easy_sustains = true;
    cfg.highway_alpha = 10;
    cfg.venue_light = 10;
    cfg.music_vol = 10;
    cfg.sfx_vol = 8;
    cfg.crowd_vol = 5;
    cfg.flat_notes = false;
    cfg.rival = 1;     // notes facing you: they read better than lying on the neck
    cfg.note_size = 1;
    cfg.last_diff = DIFF_MEDIUM;
    cfg.sing_diff = DIFF_MEDIUM;    // singing along judges the voice at Medium, not at the instrument's own
}

void config_load(void)
{
    config_defaults();
    int look = 0, notes_look = 0;
    bool flat = false;   // notes_look: settings saved since notes face you by default
    char path[400];
    snprintf(path, sizeof(path), "%ssettings.txt", player_dir());
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
        else if (strcmp(k, "board_look") == 0)
            look = n;
        else if (strcmp(k, "notes_look") == 0)
            notes_look = n;
        else if (strcmp(k, "highway") == 0)
            snprintf(cfg.highway, sizeof(cfg.highway), "%s", v);
        else if (strcmp(k, "venue") == 0)
            snprintf(cfg.venue, sizeof(cfg.venue), "%s", v);
        else if (strcmp(k, "camera") == 0)
            cfg.camera = n < 0 ? 0 : n > 2 ? 2 : n;
        else if (strcmp(k, "highway_alpha") == 0)
            cfg.highway_alpha = n < 0 ? 0 : n > 10 ? 10 : n;
        else if (strcmp(k, "venue_light") == 0)
            cfg.venue_light = n < 0 ? 0 : n > 10 ? 10 : n;
        else if (strcmp(k, "flashes") == 0)
            cfg.flashes = n < 0 ? 0 : n > 2 ? 2 : n;
        else if (strcmp(k, "no_popups") == 0)
            cfg.no_popups = n != 0;
        else if (strcmp(k, "timing_bar") == 0)
            cfg.timing_bar = n != 0;
        else if (strcmp(k, "live_stats") == 0)
            cfg.live_stats = n != 0;
        else if (strcmp(k, "no_ghost") == 0)
            cfg.no_ghost = n != 0;
        else if (strcmp(k, "light_fx") == 0)
            cfg.light_fx = n != 0;
        else if (strcmp(k, "still_camera") == 0)
            cfg.still_camera = n != 0;
        else if (strcmp(k, "frame_rate") == 0)
            cfg.frame_rate = n == 30 ? 30 : 60;
        else if (strcmp(k, "gem_style") == 0)
            cfg.gem_style = n == 1 ? 1 : 0;
        else if (strcmp(k, "hud_style") == 0)
            cfg.hud_style = n == 1 ? 1 : 0;
        else if (strcmp(k, "backdrop") == 0)
            cfg.backdrop = n >= 0 && n <= 2 ? n : 0;
        else if (strcmp(k, "hit_ratings") == 0)
            cfg.hit_ratings = n >= 0 && n <= 2 ? n : 1;
        else if (strcmp(k, "hw_length") == 0)
            cfg.hw_length = n >= 0 && n <= 2 ? n : 1;
        else if (strcmp(k, "sudden_death") == 0)
            cfg.sudden_death = n != 0;
        else if (strcmp(k, "hard_rock") == 0)
            cfg.hard_rock = n != 0;
        else if (strcmp(k, "metronome") == 0)
            cfg.metronome = n != 0;
        else if (strcmp(k, "song_speed") == 0)
            cfg.song_speed = n < 50 ? 50 : n > 150 ? 150 : n / 5 * 5;
        else if (strcmp(k, "hit_window") == 0)
            cfg.hit_window = n < 0 ? 0 : n > 3 ? 3 : n;
        else if (strcmp(k, "rival") == 0)
            cfg.rival = n < 0 ? 0 : n > 3 ? 3 : n;
        else if (strcmp(k, "vanish") == 0)
            cfg.vanish = n != 0;
        else if (strcmp(k, "no_miss_sound") == 0)
            cfg.no_miss_sound = n != 0;
        else if (strcmp(k, "no_rear_whammy") == 0)
            cfg.no_rear_whammy = n != 0;
        else if (strcmp(k, "no_bend") == 0)
            cfg.no_bend = n != 0;
        else if (strcmp(k, "no_countdown") == 0)
            cfg.no_countdown = n != 0;
        else if (strcmp(k, "stage_screens") == 0)
            cfg.stage_screens = n != 0;
        else if (strcmp(k, "cup_skin") == 0)
            cfg.cup_skin = n ? 1 : 0;
        else if (strcmp(k, "no_lyrics") == 0)
            cfg.no_lyrics = n != 0;
        else if (strcmp(k, "auto_repress") == 0)
            cfg.auto_repress = n != 0;
        else if (strcmp(k, "flat_notes") == 0)
            flat = n != 0;
        else if (strcmp(k, "no_wobble") == 0)
            cfg.no_wobble = n != 0;
        else if (strcmp(k, "fire") == 0)
            cfg.fire = n < 0 ? 0 : n > 2 ? 2 : n;
        else if (strcmp(k, "note_size") == 0)
            cfg.note_size = n < 0 ? 0 : n > 2 ? 2 : n;
        else if (strcmp(k, "no_crowd") == 0)
            cfg.no_crowd = n != 0;
        else if (strcmp(k, "no_vignette") == 0)
            cfg.no_vignette = n != 0;
        else if (strcmp(k, "no_menu_music") == 0)
            cfg.no_menu_music = n != 0;
        else if (strcmp(k, "no_progress") == 0)
            cfg.no_progress = n != 0;
        else if (strcmp(k, "no_count_in") == 0)
            cfg.no_count_in = n != 0;
        else if (strcmp(k, "easy_sustains") == 0)
            cfg.easy_sustains = n != 0;
        else if (strcmp(k, "double_kick") == 0)
            cfg.double_kick = n != 0;
        else if (strcmp(k, "five_pads") == 0)
            cfg.five_pads = n != 0;
        else if (strcmp(k, "no_band") == 0)
            cfg.no_band = atoi(v) != 0;
        else if (strcmp(k, "sing_along") == 0)
            cfg.sing_along = atoi(v) != 0;
        else if (strcmp(k, "sing_diff") == 0) {
            int d = atoi(v);
            cfg.sing_diff = d < 0 ? DIFF_MEDIUM : d > DIFF_EXPERT ? DIFF_EXPERT : d;
        }
        else if (strcmp(k, "offset_ms") == 0)
            cfg.offset_ms = n;
        else if (strcmp(k, "layout") == 0)
            cfg.layout = n;
        else if (strcmp(k, "drum_layout") == 0)
            cfg.drum_layout = n < 0 || n > 2 ? 0 : n;
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
    // notes lying on the neck were the default before: a file from then gets them facing you, once; after that it's
    // whatever the player picked
    cfg.flat_notes = notes_look ? flat : false;
    // a settings file from before the board matched World Tour still names the old default: move it on, once
    if (!look && strcmp(cfg.highway, "classic") == 0)
        snprintf(cfg.highway, sizeof(cfg.highway), "judy");
    if (!look)
        cfg.sing_along = false;     // lyrics over the top of the picture when you aren't singing: only if asked for
}

void config_save(void)
{
    char path[400], tmp[410];
    snprintf(path, sizeof(path), "%ssettings.txt", player_dir());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "speed = %d\nhighway = %s\noffset_ms = %d\nlayout = %d\ntilt = %d\nnofail = %d\nmusic_vol = %d\n"
               "sfx_vol = %d\ncrowd_vol = %d\nlefty = %d\nshow_fps = %d\nlast_part = %d\nlast_diff = %d\nsort = %d\n"
               "last_song = %s\nvenue = %s\nno_band = %d\nsing_along = %d\ndrum_layout = %d\ndrum_hits = %d\n",
            cfg.speed, cfg.highway, cfg.offset_ms, cfg.layout, cfg.tilt, cfg.nofail, cfg.music_vol, cfg.sfx_vol,
            cfg.crowd_vol, cfg.lefty, cfg.show_fps, cfg.last_part, cfg.last_diff, cfg.sort, cfg.last_song, cfg.venue, cfg.no_band, cfg.sing_along, cfg.drum_layout, cfg.drum_hits);
    fprintf(f, "board_look = 1\nno_lyrics = %d\nauto_repress = %d\nflat_notes = %d\nno_wobble = %d\nfire = %d\nnote_size = %d\nno_crowd = %d\nno_vignette = %d\nno_menu_music = %d\nsing_diff = %d\n",
            cfg.no_lyrics, cfg.auto_repress, cfg.flat_notes, cfg.no_wobble, cfg.fire, cfg.note_size, cfg.no_crowd, cfg.no_vignette, cfg.no_menu_music, cfg.sing_diff);
    fprintf(f, "camera = %d\nhighway_alpha = %d\nvenue_light = %d\nflashes = %d\n",
            cfg.camera, cfg.highway_alpha, cfg.venue_light, cfg.flashes);
    fprintf(f, "no_popups = %d\ntiming_bar = %d\nlive_stats = %d\nvanish = %d\n"
               "no_miss_sound = %d\nno_rear_whammy = %d\nno_bend = %d\nno_countdown = %d\n",
            cfg.no_popups, cfg.timing_bar, cfg.live_stats, cfg.vanish, cfg.no_miss_sound, cfg.no_rear_whammy, cfg.no_bend, cfg.no_countdown);
    fprintf(f, "double_kick = %d\nfive_pads = %d\neasy_sustains = %d\nno_count_in = %d\nstage_screens = %d\nno_progress = %d\ncup_skin = %d\n", cfg.double_kick, cfg.five_pads, cfg.easy_sustains,
            cfg.no_count_in, cfg.stage_screens, cfg.no_progress, cfg.cup_skin);
    fprintf(f, "no_ghost = %d\nnotes_look = 1\nrival = %d\nhit_window = %d\nlight_fx = %d\nmetronome = %d\nsong_speed = %d\n", cfg.no_ghost, cfg.rival, cfg.hit_window,
            cfg.light_fx, cfg.metronome, cfg.song_speed);
    fprintf(f, "still_camera = %d\nframe_rate = %d\ngem_style = %d\n", cfg.still_camera, cfg.frame_rate, cfg.gem_style);
    fprintf(f, "hud_style = %d\nbackdrop = %d\nhit_ratings = %d\nhw_length = %d\nsudden_death = %d\nhard_rock = %d\n", cfg.hud_style,
            cfg.backdrop, cfg.hit_ratings, cfg.hw_length, cfg.sudden_death, cfg.hard_rock);
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
