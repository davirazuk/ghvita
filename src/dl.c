#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <archive.h>
#include <archive_entry.h>
#ifdef __vita__
#include <un7zip.h>
#endif
#include "dl.h"
#include "json.h"
#include "mega.h"
#include "net.h"
#include "rar.h"
#include "songconv.h"
#include "plat.h"
#include "rbcon.h"
#include "shrink.h"

#define PAGE 40
#define MAX_JOBS 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t install_lock = PTHREAD_MUTEX_INITIALIZER;    // the unpacking and converting, one at a time
static bool started;

// ---------------------------------------------------------------------------------------------------- the search

static DlHit * hits;
static int hit_count, hit_cap, version;
static char status_text[200];
static volatile bool searching;
static int cur_site, cur_page, cur_total;
static char cur_query[128];

// later in this file, needed here
static void copy_str(char * out, size_t size, const Json * j);

// the search filters: words the two sites use, one spelling each (RhythmVerse's first, Chorus's second)
typedef struct {
    const char * rv, * chorus, * label;
} DlWord;

static const DlWord word_diff[] = {{"x", "expert", "Expert"}, {"h", "hard", "Hard"}, {"m", "medium", "Medium"},
                                   {"e", "easy", "Easy"}};
static const DlWord word_instr[] = {{"guitar", "guitar", "Guitar"}, {"guitar_coop", "guitarcoop", "Co-Op Guitar"},
                                    {"rhythm", "rhythm", "Rhythm Guitar"}, {"bass", "bass", "Bass"},
                                    {"guitarghl", "guitarghl", "Guitar 6-Fret"}, {"keys", "keys", "Keys"},
                                    {"drums", "drums", "Drums"}};
// the sorts: 0 the sites' own best match; the rest are ordered here after the fetch, except the last two, which the
// sites know (the ones fetched before the rest, so the pages hold the right charts to begin with)
typedef struct {
    const char * rv, * order, * chorus, * cdir, * label;
} DlSort;
static const DlSort word_sort[] = {
    {"", "", "", "", "Best match"},
    {"", "", "", "", "Title A to Z"},
    {"", "", "", "", "Title Z to A"},
    {"", "", "", "", "Artist A to Z"},
    {"", "", "", "", "Newest first"},
    {"", "", "", "", "Oldest first"},
    {"", "", "", "", "Longest first"},
    {"author", "ASC", "charter", "asc", "Charter A to Z"},
    {"update_date", "DESC", "modifiedTime", "desc", "Recently added"},
};
// each site's own filters: what the screen holds (dl_get/dl_set_filter) and what the request builder reads
static DlFilter filt[DL_SITES];
static pthread_mutex_t filt_mutex = PTHREAD_MUTEX_INITIALIZER;

// the genres, asked of RhythmVerse's dictionary the first time the panel opens
typedef struct { char slug[48], label[64]; } GenrePair;
static GenrePair genres[96];
static volatile int genre_n = -2;         // -2 not asked, -1 being asked, 0 the site had nothing, N got them

void dl_get_filter(int site, DlFilter * f)
{
    pthread_mutex_lock(&filt_mutex);
    *f = filt[site];
    pthread_mutex_unlock(&filt_mutex);
}

void dl_set_filter(int site, const DlFilter * f)
{
    pthread_mutex_lock(&filt_mutex);
    filt[site] = *f;
    pthread_mutex_unlock(&filt_mutex);
}

bool dl_quit_filters(void)
{
    // is anything on at either site? the display names don't decide: what's filtered does
    for (int s = 0; s < DL_SITES; s++) {
        DlFilter f;
        dl_get_filter(s, &f);
        if (f.charter[0] || f.artist[0])
            return true;
        if (f.genre >= 0 || f.instrument >= 0 || f.difficulty >= 0)
            return true;
        if (f.decade || f.year || f.stems)
            return true;
    }
    return false;
}

int dl_sort_count(void)
{
    return sizeof(word_sort) / sizeof(word_sort[0]);
}

const char * dl_sort_label(int i)
{
    return i >= 0 && i < (int)(sizeof(word_sort) / sizeof(word_sort[0])) ? word_sort[i].label : "?";
}

// the filters and the sort as one dim line for the bottom of the screen ("" when nothing is on)
void dl_filter_text(int site, char * buf, size_t size)
{
    DlFilter f;
    dl_get_filter(site, &f);
    buf[0] = 0;
    if (f.charter[0])
        snprintf(buf + strlen(buf), size - strlen(buf), "%sCharter %s", buf[0] ? "  " : "",
                 f.charter_name[0] ? f.charter_name : f.charter);
    if (f.artist[0])
        snprintf(buf + strlen(buf), size - strlen(buf), "%sArtist %s", buf[0] ? "  " : "",
                 f.artist_name[0] ? f.artist_name : f.artist);
    if (f.genre >= 0)
        snprintf(buf + strlen(buf), size - strlen(buf), "%s%s", buf[0] ? "  " : "", dl_genre_name(f.genre));
    if (f.instrument >= 0)
        snprintf(buf + strlen(buf), size - strlen(buf), "%s%s", buf[0] ? "  " : "", word_instr[f.instrument].label);
    if (f.difficulty >= 0)
        snprintf(buf + strlen(buf), size - strlen(buf), "%s%s", buf[0] ? "  " : "", word_diff[f.difficulty].label);
    if (f.decade)
        snprintf(buf + strlen(buf), size - strlen(buf), "%s%d0s", buf[0] ? "  " : "", f.decade / 10);
    else if (f.year)
        snprintf(buf + strlen(buf), size - strlen(buf), "%s%d", buf[0] ? "  " : "", f.year);
    if (f.stems)
        snprintf(buf + strlen(buf), size - strlen(buf), "%sstems only", buf[0] ? "  " : "");
    if (f.sort > 0 && f.sort < (int)(sizeof(word_sort) / sizeof(word_sort[0])))
        snprintf(buf + strlen(buf), size - strlen(buf), "%sSort: %s", buf[0] ? "  " : "", word_sort[f.sort].label);
}

int dl_instru_count(void)
{
    return sizeof(word_instr) / sizeof(word_instr[0]);
}

int dl_diff_count(void)
{
    return sizeof(word_diff) / sizeof(word_diff[0]);
}

void dl_instru(int i, const char ** rv, const char ** chorus, const char ** label)
{
    *rv = word_instr[i].rv, *chorus = word_instr[i].chorus, *label = word_instr[i].label;
}

static void * dict_thread(void * arg)
{
    (void)arg;
    char err[128] = "";
    char * body = NULL;
    size_t len = 0;
    // the lists are the same across the systems: the CH one is the shape the search takes
    const HttpReq r = {.url = "https://rhythmverse.co/api/ch/dictionary/list", .stall_s = 20};
    int st = http_fetch(&r, &body, &len, err, sizeof(err));
    int n = 0;
    if (st >= 200 && st < 300) {
        Json * root = json_parse(body, len);
        // the genre pairs sit in the dictionary as slug -> label ("rock": "Rock", ...)
        const Json * list = json_get(json_get(json_get(root, "data"), "filters"), "genre");
        const Json * pair = list ? list->child : NULL;
        for (; pair && n < (int)(sizeof(genres) / sizeof(genres[0])) && pair->key; pair = pair->next) {
            snprintf(genres[n].slug, sizeof(genres[0].slug), "%s", pair->key);
            copy_str(genres[n].label, sizeof(genres[0].label), pair);
            n++;
        }
        json_free(root);
    }
    free(body);
    genre_n = n;
    return NULL;
}

int dl_genre_count(void)
{
    if (genre_n == -2) {
        genre_n = -1;
        pthread_t t;
        if (pthread_create(&t, NULL, dict_thread, NULL) == 0)
            pthread_detach(t);
        else
            genre_n = 0;
        return -1;
    }
    return genre_n;
}

const char * dl_genre_slug(int i)
{
    static const char * empty = "";
    return i >= 0 && i < genre_n ? genres[i].slug : empty;
}

const char * dl_genre_name(int i)
{
    static const char * empty = "";
    return i >= 0 && i < genre_n ? genres[i].label : empty;
}

// the pickers' lists: "who are you looking at?" 0 charter, 1 artist. Both are live lookups on the site, and the words
// typed come back as the site's own id (the search filter carries that)
int dl_lookup(int kind, const char * text, DlLookup * out, int max)
{
    char err[128] = "";
    char * body = NULL;
    size_t len = 0;
    char q[400], post[700];
    url_escape(q, sizeof(q), text);
    snprintf(post, sizeof(post), "text=%s&page=1&records=%d", q, max);
    char url[200];
    snprintf(url, sizeof(url), "https://rhythmverse.co/api/all/%s/search/live", kind ? "artists" : "authors");
    const char * headers[] = {"X-Requested-With: XMLHttpRequest", NULL};
    const HttpReq r = {.url = url, .post = post, .headers = headers, .stall_s = 20};
    int st = http_fetch(&r, &body, &len, err, sizeof(err));
    int n = -1;
    if (st >= 200 && st < 300) {
        Json * root = json_parse(body, len);
        const Json * list = json_get(json_get(root, "data"), kind ? "artists" : "authors");
        n = 0;
        for (const Json * a = list && list->child ? list->child : NULL; a && n < max; a = a->next) {
            copy_str(out[n].id, sizeof(out[0].id), json_get(a, kind ? "artist_s" : "shortname"));
            copy_str(out[n].name, sizeof(out[0].name), json_get(a, kind ? "artist" : "name"));
            int rel = (int)json_num(json_get(a, kind ? "songs" : "releases"), 0);
            if (!out[n].id[0])
                continue;
            out[n].releases = rel;
            n++;
        }
        json_free(root);
    }
    free(body);
    return n;
}

static struct {
    volatile bool pending;
    int site, page;
    char query[128];
} want;

// covers: the one the screen asked for last
static char cover_url[400], cover_path[400];
static volatile bool cover_pending;
static char cover_done_url[400];

// ---------------------------------------------------------------------------------------------------- the queue

static void dl_save_locked(void);       // the queue's file, called with the lock held
static void dl_load(void);              // what the last run left, once at the start

typedef struct {
    DlHit hit;
    volatile JobState state;
    volatile int64_t done, total;
    volatile bool cancel;
    volatile long long bumps;       // "downloads next" marks theirs: the smallest mark goes first
    char msg[120];
} Job;

static Job jobs[MAX_JOBS];
static int job_count, jobs_added;
static char installed[32][400];
static int installed_count;

void dl_lock(void)
{
    pthread_mutex_lock(&lock);
}

void dl_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

static const char * tmp_dir(void)
{
    static char p[300];
    snprintf(p, sizeof(p), "%stmp", plat_data_dir());
    return p;
}

void dl_remove_tree(const char * path)
{
    DIR * d = opendir(path);
    if (!d) {
        remove(path);
        return;
    }
    struct dirent * e;
    char (*names)[256] = NULL;
    int n = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        names = realloc(names, (n + 1) * sizeof(*names));
        snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", path, names[i]);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
            dl_remove_tree(p);
        else
            remove(p);
    }
    free(names);
    rmdir(path);
}

void dl_mkdirs(const char * path)
{
    char p[700];
    snprintf(p, sizeof(p), "%s", path);
    for (char * s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (s[-1] != ':')       // not "ux0:"
                mkdir(p, 0777);
            *s = '/';
        }
    mkdir(p, 0777);
}

// a name from an archive or a web page as a file name the memory card takes
void dl_safe_part(char * s)
{
    for (; *s; s++)
        if (strchr(":*?\"<>|\\", *s) || (unsigned char)*s < 0x20)
            *s = '_';
}

// "AC/DC" and "ac-dc" alike
static void key_of(char * out, size_t size, const char * in)
{
    size_t k = 0;
    for (; in && *in && k + 1 < size; in++)
        if (isalnum((unsigned char)*in) || (unsigned char)*in >= 0x80)
            out[k++] = (char)tolower((unsigned char)*in);
    out[k] = 0;
}

static void lower(char * out, size_t size, const char * in)
{
    size_t k = 0;
    for (; in && *in && k + 1 < size; in++)
        out[k++] = (char)tolower((unsigned char)*in);
    out[k] = 0;
}

// the YARG overlay's order: the artist asked for first, then the title, covers and remixes last
static int score(const DlHit * h, const char * q)
{
    char qk[128], a[128], n[128], all[256], ql[128];
    key_of(qk, sizeof(qk), q);
    key_of(a, sizeof(a), h->artist);
    key_of(n, sizeof(n), h->name);
    char both[200];
    snprintf(both, sizeof(both), "%s %s", h->artist, h->name);
    lower(all, sizeof(all), both);
    lower(ql, sizeof(ql), q);
    int s = 0;
    if (!strcmp(a, qk))
        s += 100;
    else if (strstr(a, qk) || (strstr(qk, a) && strlen(a) > 2))
        s += 60;
    if (!strcmp(n, qk))
        s += 80;
    else if (strstr(n, qk))
        s += 30;
    bool every = true;
    for (char * w = ql; *w;) {
        while (*w == ' ')
            w++;
        size_t l = strcspn(w, " ");
        char word[64];
        snprintf(word, sizeof(word), "%.*s", (int)l, w);
        if (l && !strstr(all, word))
            every = false;
        w += l;
    }
    if (every)
        s += 20;
    if (strstr(all, "cover") || strstr(all, "style") || strstr(all, "remix"))
        s -= 25;
    return s;
}

// ------------------------------------------------------------------------------------------- the sites' answers

static void copy_str(char * out, size_t size, const Json * j)
{
    char buf[64];
    snprintf(out, size, "%s", json_str(j, "", buf, sizeof(buf)));
    // no line breaks in the list
    for (char * p = out; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
}

// a part this game plays, as the sites name them
// the sites' diff_guitar, diff_drums...: what's in the chart before it's downloaded
static void read_parts(DlHit * h, const Json * o)
{
    static const char * const keys[PART_COUNT] = {"diff_guitar", "diff_bass", "diff_rhythm", "diff_guitar_coop",
                                                  "diff_keys", "diff_drums", "diff_vocals"};
    for (int p = 0; p < PART_COUNT; p++) {
        double v = json_num(json_get(o, keys[p]), -1);
        h->parts[p] = v < 0 ? -1 : v > 6 ? 6 : (int8_t)v;
        h->any_parts |= v >= 0;
    }
}

static bool five_fret(const char * part)
{
    return !strcmp(part, "guitar") || !strcmp(part, "guitarcoop") || !strcmp(part, "guitar_coop") ||
           !strcmp(part, "rhythm") || !strcmp(part, "bass") || !strcmp(part, "keys") || !strcmp(part, "drums") ||
           !strcmp(part, "vocals");
}

static int parse_chorus(const char * body, size_t len, DlHit ** out, int * total)
{
    Json * root = json_parse(body, len);
    const Json * data = json_get(root, "data");
    *total = (int)json_num(json_get(root, "found"), 0);
    int n = 0;
    for (const Json * c = data ? data->child : NULL; c; c = c->next)
        n++;
    *out = calloc(n ? n : 1, sizeof(DlHit));
    if (!*out) {
        json_free(root);
        return 0;
    }
    int k = 0;
    for (const Json * c = data ? data->child : NULL; c; c = c->next) {
        DlHit * h = &(*out)[k];
        char md5[64], art[64];
        copy_str(md5, sizeof(md5), json_get(c, "md5"));
        if (!md5[0])
            continue;
        h->site = DL_CHORUS;
        snprintf(h->id, sizeof(h->id), "chorus:%s", md5);
        copy_str(h->name, sizeof(h->name), json_get(c, "name"));
        copy_str(h->artist, sizeof(h->artist), json_get(c, "artist"));
        copy_str(h->album, sizeof(h->album), json_get(c, "album"));
        copy_str(h->year, sizeof(h->year), json_get(c, "year"));
        copy_str(h->genre, sizeof(h->genre), json_get(c, "genre"));
        copy_str(h->charter, sizeof(h->charter), json_get(c, "charter"));
        copy_str(h->comment, sizeof(h->comment), json_get(c, "loading_phrase"));
        h->length_ms = (int)json_num(json_get(c, "song_length"), 0);
        snprintf(h->url, sizeof(h->url), "https://files.enchor.us/%s.sng", md5);
        copy_str(art, sizeof(art), json_get(c, "albumArtMd5"));
        if (art[0])
            snprintf(h->art, sizeof(h->art), "https://files.enchor.us/%s.jpg", art);
        // the parts it has: one this game plays, or it can't be had
        bool playable = false, any = false, ghl = false;
        const Json * inst = json_path(c, "notesData.instruments");
        for (const Json * i = inst ? inst->child : NULL; i; i = i->next) {
            if (i->type != J_STR)
                continue;
            any = true;
            playable |= five_fret(i->str);
            ghl |= strstr(i->str, "ghl") != NULL;
        }
        playable |= json_num(json_get(c, "diff_vocals"), -1) >= 0;      // not among its instruments
        read_parts(h, c);
        if (any && !playable)
            snprintf(h->unplayable, sizeof(h->unplayable), "%s",
                     ghl ? "a 6-fret (Guitar Hero Live) chart" : "no part the game can play");
        k++;
    }
    json_free(root);
    return k;
}

static void html_unescape(char * s)
{
    static const struct { const char * from; char to; } ents[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&#34;", '"'}, {"&#33;", '!'},
    };
    char * o = s;
    for (char * p = s; *p;) {
        bool done = false;
        for (size_t k = 0; k < sizeof(ents) / sizeof(ents[0]) && !done; k++) {
            size_t l = strlen(ents[k].from);
            if (!strncmp(p, ents[k].from, l)) {
                *o++ = ents[k].to;
                p += l;
                done = true;
            }
        }
        if (!done)
            *o++ = *p++;
    }
    *o = 0;
}

// a RhythmVerse download address as something this can fetch
static void resolve_rv(DlHit * h, const char * address)
{
    const char * p;
    char u[600];
    snprintf(u, sizeof(u), "%s", address);
    html_unescape(u);
    h->url[0] = 0;
    if (!u[0] || !strcmp(u, "none")) {
        snprintf(h->unplayable, sizeof(h->unplayable), "no download link");
        return;
    }
    if (u[0] == '/') {
        // a file on RhythmVerse itself, its name as it is ("/download_file/x/Metallica - Am I Savage.zip")
        static const char hex[] = "0123456789ABCDEF";
        char esc[500];
        size_t k = 0;
        for (const unsigned char * s = (const unsigned char *)u; *s && k + 4 < sizeof(esc); s++) {
            bool keep = isalnum(*s) || strchr("/-._~!$&'()*+,;=:@", *s) ||
                        (*s == '%' && isxdigit(s[1]) && isxdigit(s[2]));
            if (keep)
                esc[k++] = (char)*s;
            else {
                esc[k++] = '%';
                esc[k++] = hex[*s >> 4];
                esc[k++] = hex[*s & 15];
            }
        }
        esc[k] = 0;
        snprintf(h->url, sizeof(h->url), "https://rhythmverse.co%s", esc);
        return;
    }
    if (strstr(u, "drive.google.com") || strstr(u, "docs.google.com")) {
        char id[128] = "";
        if ((p = strstr(u, "/folders/"))) {
            sscanf(p + 9, "%127[A-Za-z0-9_-]", id);
            if (id[0])
                snprintf(h->url, sizeof(h->url), "gdrive-folder:%s", id);
        } else if ((p = strstr(u, "/file/d/")) || (p = strstr(u, "id="))) {
            sscanf(p + (p[0] == '/' ? 8 : 3), "%127[A-Za-z0-9_-]", id);
            if (id[0])
                snprintf(h->url, sizeof(h->url),
                         "https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t", id);
        }
    } else if (strstr(u, "dropbox.com")) {
        snprintf(h->url, sizeof(h->url), "%s", u);
        char * dl0 = strstr(h->url, "dl=0");
        if (dl0)
            dl0[3] = '1';
        else if (!strstr(h->url, "dl=1"))
            strncat(h->url, strchr(h->url, '?') ? "&dl=1" : "?dl=1", sizeof(h->url) - strlen(h->url) - 1);
    } else if (strstr(u, "mediafire.com"))
        snprintf(h->url, sizeof(h->url), "mediafire:%s", u);
    else if (strstr(u, "mega.nz") || strstr(u, "mega.co.nz")) {
        if (mega_link_ok(u))
            snprintf(h->url, sizeof(h->url), "mega:%s", u);
        else
            snprintf(h->unplayable, sizeof(h->unplayable), "its Mega link has no key");
    } else if (!strncmp(u, "http", 4)) {
        char low[600];
        lower(low, sizeof(low), u);
        if (strstr(low, "bit.ly/") || strstr(low, "tinyurl.com/"))
            snprintf(h->url, sizeof(h->url), "short:%s", u);
        else if (strstr(low, ".zip") || strstr(low, ".rar") || strstr(low, ".7z") || strstr(low, ".sng") ||
                 strstr(low, "onyxite.org/") || strstr(low, "keepitfishy.com/"))
            snprintf(h->url, sizeof(h->url), "%s", u);
        else if (strstr(low, "xbox.com/"))
            snprintf(h->unplayable, sizeof(h->unplayable), "official Rock Band DLC");
    }
    if (!h->url[0] && !h->unplayable[0])
        snprintf(h->unplayable, sizeof(h->unplayable), "its file host isn't supported");
}

// -1 when the site said no (err says why)
static int parse_rv(const char * body, size_t len, DlHit ** out, int * total, char * err, size_t err_size)
{
    Json * root = json_parse(body, len);
    char buf[16];
    *out = NULL;
    if (strcmp(json_str(json_get(root, "status"), "", buf, sizeof(buf)), "success") != 0) {
        const char * why = json_str(json_path(root, "error.message"), NULL, buf, sizeof(buf));
        snprintf(err, err_size, "%s", why ? why : "RhythmVerse sent something odd");
        json_free(root);
        return -1;
    }
    const Json * songs = json_path(root, "data.songs");
    *total = (int)json_num(json_path(root, "data.records.total_filtered"), 0);
    int n = 0;
    for (const Json * c = songs ? songs->child : NULL; c; c = c->next)
        n++;
    *out = calloc(n ? n : 1, sizeof(DlHit));
    if (!*out) {
        json_free(root);
        return 0;
    }
    int k = 0;
    for (const Json * c = songs ? songs->child : NULL; c; c = c->next) {
        const Json * d = json_get(c, "data"), * f = json_get(c, "file");
        if (!f)
            continue;
        DlHit * h = &(*out)[k];
        char id[64], url[600], art[300];
        copy_str(id, sizeof(id), json_get(f, "file_id"));
        if (!id[0])
            continue;
        h->site = DL_RHYTHMVERSE;
        snprintf(h->id, sizeof(h->id), "rv:%s", id);
        copy_str(h->name, sizeof(h->name), json_get(d, "title"));
        if (!h->name[0])
            copy_str(h->name, sizeof(h->name), json_get(f, "file_title"));
        copy_str(h->artist, sizeof(h->artist), json_get(d, "artist"));
        if (!h->artist[0])
            copy_str(h->artist, sizeof(h->artist), json_get(f, "file_artist"));
        copy_str(h->album, sizeof(h->album), json_get(d, "album"));
        if (!h->album[0])
            copy_str(h->album, sizeof(h->album), json_get(f, "file_album"));
        copy_str(h->year, sizeof(h->year), json_get(d, "year"));
        copy_str(h->genre, sizeof(h->genre), json_get(d, "genre"));
        copy_str(h->comment, sizeof(h->comment), json_get(d, "loading_phrase"));
        if (!h->comment[0])
            copy_str(h->comment, sizeof(h->comment), json_get(f, "loading_phrase"));
        if (!strcmp(h->year, "0"))
            h->year[0] = 0;
        copy_str(h->charter, sizeof(h->charter), json_path(f, "author.name"));
        if (!h->charter[0])
            copy_str(h->charter, sizeof(h->charter), json_get(f, "user"));
        double secs = json_num(json_get(f, "file_song_length"), 0);
        if (secs <= 0)
            secs = json_num(json_get(d, "song_length"), 0);
        h->length_ms = (int)(secs * 1000);
        h->size = (int64_t)json_num(json_get(f, "size"), 0);
        copy_str(url, sizeof(url), json_get(f, "download_url"));
        resolve_rv(h, url);
        read_parts(h, f);
        if (!h->any_parts)
            read_parts(h, d);
        char format[24];
        copy_str(format, sizeof(format), json_get(f, "gameformat"));
        h->rock_band = !strncmp(format, "rb", 2);
        // the parts, when RhythmVerse knows them (diff_guitar: 3 ...)
        bool known = false, five = false, ghl = false;
        for (const Json * x = f->child; x; x = x->next) {
            if (!x->key || strncmp(x->key, "diff_", 5) != 0 || json_num(x, -1) < 0)
                continue;
            known = true;
            five |= five_fret(x->key + 5);
            ghl |= strstr(x->key, "ghl") != NULL;
        }
        if (known && !five && !h->unplayable[0])
            snprintf(h->unplayable, sizeof(h->unplayable), "%s",
                     ghl ? "a 6-fret (Guitar Hero Live) chart" : "no part the game can play");
        copy_str(art, sizeof(art), json_get(f, "album_art"));
        if (!art[0])
            copy_str(art, sizeof(art), json_get(d, "album_art"));
        if (art[0] == '/')
            snprintf(h->art, sizeof(h->art), "https://rhythmverse.co%s", art);
        k++;
    }
    json_free(root);
    return k;
}

// what the filters ask for, as RhythmVerse wants them ("author=<id>&genre[0]=rock..."). Slugs are escaped; kept,
// because RhythmVerse itself takes its ids like that
static void rv_filters(char * out, size_t size, const DlFilter * f)
{
    char piece[256];
    out[0] = 0;
    if (f->charter[0]) {
        url_escape(piece, sizeof(piece), f->charter);
        snprintf(out, size, "&author=%s", piece);
    }
    if (f->artist[0]) {
        char head[220];
        url_escape(piece, sizeof(piece), f->artist);
        snprintf(head, sizeof(head), "%s&artist=%s", out, piece);
        snprintf(out, size, "%s", head);
    }
    if (f->genre >= 0) {
        char head[220];
        url_escape(piece, sizeof(piece), dl_genre_slug(f->genre));
        snprintf(head, sizeof(head), "%s&genre%%5B0%%5D=%s", out, piece);
        snprintf(out, size, "%s", head);
    }
    if (f->instrument >= 0) {
        char head[220];
        url_escape(piece, sizeof(piece), word_instr[f->instrument].rv);
        snprintf(head, sizeof(head), "%s&instrument%%5B0%%5D=%s", out, piece);
        snprintf(out, size, "%s", head);
    }
    if (f->difficulty >= 0)
        // one difficulty: the lowest of the ones asked? the site takes one, and the charts it lists also have the
        // harder ones beside it
        snprintf(out + strlen(out), size - strlen(out), "&difficulties%%5B0%%5D=%s", word_diff[f->difficulty].rv);
    if (f->decade && !f->year)
        snprintf(out + strlen(out), size - strlen(out), "&decade=%d", f->decade);
    if (f->year)
        snprintf(out + strlen(out), size - strlen(out), "&year=%d", f->year);
    if (f->stems)
        snprintf(out + strlen(out), size - strlen(out), "&audio=full");
}

static bool filter_set(const DlFilter * f)
{
    return f->charter[0] || f->artist[0] || f->genre >= 0 || f->instrument >= 0 || f->difficulty >= 0 || f->decade ||
           f->year || f->stems;
}

// one page of a RhythmVerse search (nothing typed: the newest), Clone Hero and Phase Shift uploads (the same song
// folders) together, and asked for by name its Rock Band customs; false when none answered
static bool rv_fetch(const char * text, int page, int records, DlHit ** out, int * n, int * total, char * err,
                     size_t err_size)
{
    bool any = false;
    static const char * const systems[3] = {"ch", "ps", "rb3xbox"};
    for (int sys = 0; sys < (text[0] ? 3 : 2); sys++) {
        char q[400], post[1100], url[200], tail[300];
        url_escape(q, sizeof(q), text);
        DlFilter flat;
        dl_get_filter(DL_RHYTHMVERSE, &flat);
        if (text[0])
            snprintf(post, sizeof(post), "text=%s&data_type=full&records=%d&page=%d", q, records, page);
        else
            snprintf(post, sizeof(post), "data_type=full&records=%d&page=%d", records, page);
        rv_filters(tail, sizeof(tail), &flat);
        snprintf(post + strlen(post), sizeof(post) - strlen(post), "%s", tail);
        // two of the orders are the sites' own (the pages arrive already in them), the rest are sorted here
        const char * rv_key = flat.sort >= 0 && flat.sort < (int)(sizeof(word_sort) / sizeof(word_sort[0])) ?
                              word_sort[flat.sort].rv : "";
        if (rv_key[0])
            snprintf(post + strlen(post), sizeof(post) - strlen(post),
                     "&sort%%5B0%%5D%%5Bsort_by%%5D=%s&sort%%5B0%%5D%%5Bsort_order%%5D=%s", rv_key, word_sort[flat.sort].order);
        else if (!text[0])
            snprintf(post + strlen(post), sizeof(post) - strlen(post),
                     "&sort%%5B0%%5D%%5Bsort_by%%5D=update_date&sort%%5B0%%5D%%5Bsort_order%%5D=DESC");
        snprintf(url, sizeof(url), "https://rhythmverse.co/api/%s/songfiles/%s", systems[sys], text[0] ? "search/live" : "list");
        const char * headers[] = {"X-Requested-With: XMLHttpRequest", NULL};
        HttpReq r = {.url = url, .post = post, .headers = headers, .stall_s = 20};
        char * body = NULL;
        size_t len = 0;
        int st = http_fetch(&r, &body, &len, err, err_size);
        if (st >= 200 && st < 300) {
            DlHit * part;
            int t, m = parse_rv(body, len, &part, &t, err, err_size);
            if (m >= 0) {
                any = true;
                DlHit * more = realloc(*out, (*n + m + 1) * sizeof(DlHit));
                if (more)
                    *out = more;
                else
                    m = 0;
                for (int i = 0; i < m; i++) {
                    bool dup = false;
                    for (int k = 0; k < *n && !dup; k++)
                        dup = !strcmp((*out)[k].id, part[i].id);
                    if (!dup)
                        (*out)[(*n)++] = part[i];
                }
                *total += t;
                free(part);
            }
        } else if (st && !err[0])
            snprintf(err, err_size, "RhythmVerse answered %d", st);
        free(body);
    }
    return any;
}

static bool has_words(const DlHit * h, char words[][64], int nw)
{
    char all[300], both[300];
    snprintf(both, sizeof(both), "%s %s %s", h->artist, h->name, h->album);
    lower(all, sizeof(all), both);
    for (int i = 0; i < nw; i++)
        if (!strstr(all, words[i]))
            return false;
    return true;
}

static bool rv_page(const char * query, int page, DlHit ** out, int * n, int * total, char * err, size_t err_size)
{
    bool ok = rv_fetch(query, page, PAGE / 2, out, n, total, err, err_size);
    // RhythmVerse looks for the words as they're typed, together: "nirvana very ape" finds nothing. Then each of the
    // two longest words is looked for too, and what has all of them is kept (all of it, if none has)
    char words[8][64], low[128];
    int nw = 0;
    lower(low, sizeof(low), query);
    for (char * w = low; *w && nw < 8;) {
        while (*w == ' ')
            w++;
        size_t l = strcspn(w, " ");
        if (l)
            snprintf(words[nw++], sizeof(words[0]), "%.*s", (int)l, w);
        w += l;
    }
    if (nw < 2 || *n >= 5 || page > 1)
        return ok;
    int pick[2] = {-1, -1};
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < nw; i++)
            if (strlen(words[i]) >= 3 && i != pick[0] && (pick[k] < 0 || strlen(words[i]) > strlen(words[pick[k]])))
                pick[k] = i;
    for (int k = 0; k < 2; k++) {
        int t = 0;
        if (pick[k] >= 0 && rv_fetch(words[pick[k]], 1, 60, out, n, &t, err, err_size))
            ok = true;
    }
    int kept = 0;
    for (int i = 0; i < *n; i++)
        kept += has_words(&(*out)[i], words, nw);
    if (kept) {
        int k = 0;
        for (int i = 0; i < *n; i++)
            if (has_words(&(*out)[i], words, nw))
                (*out)[k++] = (*out)[i];
        *n = k;
    }
    *total = *n;                    // no more pages of these
    return ok;
}

// while a search is typed, the results come in the site's order; sort by how well each one fits the words instead.
// an empty query keeps the site's order (newest first)
static char sort_query[128];

static int hit_rank(const DlHit * h)
{
    char q[128], n[96], a[96];
    for (int i = 0; sort_query[i] && i < 127; i++)
        q[i] = (char)tolower((unsigned char)sort_query[i]);
    q[127] = 0;
    for (int i = 0; h->name[i] && i < 95; i++)
        n[i] = (char)tolower((unsigned char)h->name[i]);
    n[95] = 0;
    for (int i = 0; h->artist[i] && i < 95; i++)
        a[i] = (char)tolower((unsigned char)h->artist[i]);
    a[95] = 0;
    if (!strcmp(n, q))
        return 0;
    if (strstr(n, q) == n)
        return 1;
    if (strstr(a, q) == a)
        return 2;
    if (strstr(n, q))
        return 3;
    if (strstr(a, q))
        return 4;
    return 5;
}

static void sort_hits(DlHit * h, int n, const char * query)
{
    if (!query || !query[0] || n < 2)
        return;
    snprintf(sort_query, sizeof(sort_query), "%s", query);
    // order[] keeps it stable: equal ranks keep the site's order
    static int order[256];
    static DlHit tmp[256];
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 1; i < n; i++) {
        int j = i;
        while (j > 0 && hit_rank(&h[order[j - 1]]) > hit_rank(&h[order[j]])) {
            int t = order[j - 1];
            order[j - 1] = order[j];
            order[j] = t;
            j--;
        }
    }
    for (int i = 0; i < n; i++)
        tmp[i] = h[order[i]];
    for (int i = 0; i < n; i++)
        h[i] = tmp[i];
}

// one page of a search: false and err when the site couldn't be asked
// do_search's orders over the whole loaded set (the last two aren't here: the sites already ordered them)
static int hit_sort_key;

static int hit_sort_cmp(const void * a, const void * b)
{
    const DlHit * x = a, * y = b;
    int c = 0;
    switch (hit_sort_key) {
    case 1:     // Title A to Z
        c = strcasecmp(x->name, y->name);
        break;
    case 2:     // Title Z to A
        c = strcasecmp(y->name, x->name);
        break;
    case 3:     // Artist A to Z
        c = strcasecmp(x->artist, y->artist);
        break;
    case 4: {   // Newest first; a year the site didn't say sits at the end, both ways
        int xa = atoi(x->year), ya = atoi(y->year);
        if (!xa)
            xa = -99999;
        if (!ya)
            ya = -99999;
        c = ya - xa;
        break;
    }
    case 5: {   // Oldest first
        int xa = atoi(x->year), ya = atoi(y->year);
        if (!xa)
            xa = -99999;
        if (!ya)
            ya = -99999;
        c = xa - ya;
        break;
    }
    case 6:     // Longest first
        c = y->length_ms - x->length_ms;
        break;
    case 7:     // Charter A to Z
        c = strcasecmp(x->charter, y->charter);
        break;
    }
    return c != 0 ? c : (x->order < y->order ? -1 : x->order > y->order ? 1 : 0);
}

static bool fetch_page(int site, const char * query, int page, DlHit ** out, int * n, int * total, char * err,
                       size_t err_size)
{
    char * body = NULL;
    size_t len = 0;
    int status;
    *out = NULL;
    *n = 0;
    *total = 0;
    if (site == DL_CHORUS) {
        DlFilter flat;
        dl_get_filter(DL_CHORUS, &flat);
        bool deep = filter_set(&flat);
        const char * words2 = deep && !query[0] ? "*" : query;
        char q[400];
        size_t k = 0;
        for (const char * s = words2; *s && k + 3 < sizeof(q); s++) {
            if (*s == '"' || *s == '\\')
                q[k++] = '\\';
            if ((unsigned char)*s >= 0x20)
                q[k++] = *s;
        }
        q[k] = 0;
        char extra[500];
        extra[0] = 0;
        if (deep) {
            // Chorus asks for the words each in a {value, exact, exclude} object, its own instrument spellings,
            // and the year as min and max (a decade spans the ten years it starts on)
            const struct { const char * key, * value; } words[] = {
                {"charter", flat.charter}, {"artist", flat.artist}};
            for (int i = 0; i < 2; i++) {
                if (!words[i].value[0])
                    continue;
                char e[140];
                size_t ek = 0;
                for (const char * s = words[i].value; *s && ek + 2 < sizeof(e); s++) {
                    if (*s == '"' || *s == '\\')
                        e[ek++] = '\\';
                    e[ek++] = *s;
                }
                e[ek] = 0;
                snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                         ",\"%s\":{\"value\":\"%s\",\"exact\":false,\"exclude\":false}", words[i].key, e);
            }
            if (flat.genre >= 0)
                snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                         ",\"genre\":{\"value\":\"%s\",\"exact\":false,\"exclude\":false}", dl_genre_slug(flat.genre));
            if (flat.instrument >= 0)
                snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"instrument\":\"%s\"",
                         word_instr[flat.instrument].chorus);
            if (flat.difficulty >= 0)
                snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"difficulty\":\"%s\"",
                         word_diff[flat.difficulty].chorus);
            if (flat.year) {
                snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"minYear\":%d,\"maxYear\":%d",
                         flat.year, flat.year);
            } else if (flat.decade) {
                snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                         ",\"minYear\":%d,\"maxYear\":%d", flat.decade, flat.decade + 9);
            }
        }
        char post[1400];
        const char * c_key = flat.sort >= 0 && flat.sort < (int)(sizeof(word_sort) / sizeof(word_sort[0])) ?
                             word_sort[flat.sort].chorus : "";
        if (c_key[0])
            snprintf(post, sizeof(post), "{\"search\":\"%s\",\"page\":%d,\"per_page\":%d%s"
                     ",\"sort\":{\"type\":\"%s\",\"direction\":\"%s\"}}", q, page, PAGE, extra,
                     c_key, word_sort[flat.sort].cdir);
        else
            snprintf(post, sizeof(post), "{\"search\":\"%s\",\"page\":%d,\"per_page\":%d%s}", q, page, PAGE, extra);
        const char * headers[] = {"Content-Type: application/json", NULL};
        HttpReq r = {.url = deep ? "https://api.enchor.us/search/advanced" : "https://api.enchor.us/search",
                     .post = post, .headers = headers, .stall_s = 20};
        status = http_fetch(&r, &body, &len, err, err_size);
        if (status >= 200 && status < 300)
            *n = parse_chorus(body, len, out, total);
    } else {
        if (query[0] && strlen(query) < 3) {
            snprintf(err, err_size, "RhythmVerse needs 3 letters or more");
            return false;
        }
        status = rv_page(query, page, out, n, total, err, err_size) ? 200 : 0;
    }
    if (*out && *n > 1 && query[0])
        sort_hits(*out, *n, query);
    free(body);
    if (status < 200 || status >= 300) {
        if (status && !err[0])
            snprintf(err, err_size, "the site answered %d", status);
        free(*out);
        *out = NULL;
        *n = 0;
        return false;
    }
    return true;
}

static void do_search(void)
{
    pthread_mutex_lock(&lock);
    int site = want.site, page = want.page;
    char query[128];
    snprintf(query, sizeof(query), "%s", want.query);
    want.pending = false;
    pthread_mutex_unlock(&lock);

    DlHit * got = NULL;      // a failed fetch leaves it as it was, and a stale result frees it below
    int n = 0, total = 0;
    char err[200] = "";
    plat_log("search: site %d page %d \"%s\"", site, page, query);
    bool ok = fetch_page(site, query, page, &got, &n, &total, err, sizeof(err));
    plat_log("search: fetched ok=%d n=%d total=%d %s", ok, n, total, err);
    // a newer search asked for meanwhile: this one is stale
    pthread_mutex_lock(&lock);
    if (want.pending) {
        pthread_mutex_unlock(&lock);
        free(got);
        return;
    }
    if (!ok) {
        snprintf(status_text, sizeof(status_text), "Couldn't search: %s", err);
        searching = false;
        version++;
        pthread_mutex_unlock(&lock);
        return;
    }
    // the best matches first (within the page), the same chart once
    if (query[0])
        for (int i = 1; i < n; i++) {
            DlHit x = got[i];
            int sx = score(&x, query), j = i - 1;
            while (j >= 0 && score(&got[j], query) < sx) {
                got[j + 1] = got[j];
                j--;
            }
            got[j + 1] = x;
        }
    plat_log("search: sorted");
    if (page == 1)
        hit_count = 0;
    int before = hit_count;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int k = 0; k < hit_count && !dup; k++)
            dup = !strcmp(hits[k].id, got[i].id) ||
                  (!strcasecmp(hits[k].artist, got[i].artist) && !strcasecmp(hits[k].name, got[i].name) &&
                   !strcasecmp(hits[k].charter, got[i].charter) && hits[k].site == got[i].site);
        if (dup)
            continue;
        if (hit_count == hit_cap) {
            hit_cap = hit_cap ? hit_cap * 2 : 64;
            DlHit * more = realloc(hits, hit_cap * sizeof(DlHit));
            if (!more)
                break;
            hits = more;
        }
        hits[hit_count++] = got[i];
    }
    free(got);
    plat_log("search: %d hits kept", hit_count);
    // the order chosen in the panel, over everything loaded so far: the sites' systems come in mixed streams,
    // and the results re-tie themselves as more pages are asked for
    DlFilter flat;
    dl_get_filter(site, &flat);
    if (hit_count > 1 && flat.sort > 0 && flat.sort < (int)(sizeof(word_sort) / sizeof(word_sort[0])) &&
        flat.sort < 8) {
        hit_sort_key = flat.sort;
        for (int i = 0; i < hit_count; i++)
            hits[i].order = i;
        qsort(hits, hit_count, sizeof(DlHit), hit_sort_cmp);
        plat_log("search: ordered by %s", word_sort[flat.sort].label);
    }
    cur_site = site;
    cur_page = page;
    cur_total = page > 1 && hit_count == before ? hit_count : total;
    snprintf(cur_query, sizeof(cur_query), "%s", query);
    if (!hit_count)
        snprintf(status_text, sizeof(status_text), "Nothing found");
    else if (cur_total > hit_count)
        snprintf(status_text, sizeof(status_text), "%d of %d charts", hit_count, cur_total);
    else
        snprintf(status_text, sizeof(status_text), "%d chart%s", hit_count, hit_count == 1 ? "" : "s");
    searching = false;
    version++;
    pthread_mutex_unlock(&lock);
}

static void do_cover(void)
{
    pthread_mutex_lock(&lock);
    char url[400], path[400];
    snprintf(url, sizeof(url), "%s", cover_url);
    snprintf(path, sizeof(path), "%s", cover_path);
    cover_pending = false;
    pthread_mutex_unlock(&lock);
    char * body = NULL;
    size_t len = 0;
    char err[160];
    HttpReq r = {.url = url, .stall_s = 15};
    int status = http_fetch(&r, &body, &len, err, sizeof(err));
    // under its name only when whole: the screen is looking for it
    char tmp[420];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    bool ok = status == 200 && body && songconv_cover((const unsigned char *)body, len, tmp) && !rename(tmp, path);
    free(body);
    pthread_mutex_lock(&lock);
    if (ok || status)
        snprintf(cover_done_url, sizeof(cover_done_url), "%s", url);     // done (or no picture: don't ask again)
    pthread_mutex_unlock(&lock);
}

static int search_thread(void * arg)
{
    (void)arg;
    for (;;) {
        if (want.pending)
            do_search();
        else if (cover_pending)
            do_cover();
        else
            plat_sleep_us(30000);
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------- the downloads

static int job_thread(void * arg);

static void start(void)
{
    if (started)
        return;
    started = true;
    dl_mkdirs(tmp_dir());
    char p[400];
    snprintf(p, sizeof(p), "%s/covers", tmp_dir());
    mkdir(p, 0777);
    dl_load();
    plat_thread("dl_search", search_thread, NULL, PLAT_BACKGROUND);
    // three of them: the downloads share the line, the converting takes its turn (the slow host of one song no
    // longer holds the others back)
    plat_thread("dl_job 1", job_thread, NULL, PLAT_BACKGROUND);
    plat_thread("dl_job 2", job_thread, NULL, PLAT_BACKGROUND);
    plat_thread("dl_job 3", job_thread, NULL, PLAT_BACKGROUND);
}

void dl_search(int site, const char * query)
{
    start();
    pthread_mutex_lock(&lock);
    want.site = site;
    want.page = 1;
    snprintf(want.query, sizeof(want.query), "%s", query);
    want.pending = true;
    searching = true;
    hit_count = 0;
    version++;
    snprintf(status_text, sizeof(status_text), "Searching %s...", site == DL_CHORUS ? "Chorus Encore" : "RhythmVerse");
    pthread_mutex_unlock(&lock);
}

void dl_more(void)
{
    pthread_mutex_lock(&lock);
    if (!searching && hit_count < cur_total) {
        want.site = cur_site;
        want.page = cur_page + 1;
        snprintf(want.query, sizeof(want.query), "%s", cur_query);
        want.pending = true;
        searching = true;
        snprintf(status_text, sizeof(status_text), "Getting more...");
        version++;
    }
    pthread_mutex_unlock(&lock);
}

bool dl_searching(void)
{
    return searching;
}

bool dl_can_more(void)
{
    return !searching && hit_count && hit_count < cur_total;
}

int dl_count(void)
{
    return hit_count;
}

DlHit * dl_hit(int i)
{
    return i >= 0 && i < hit_count ? &hits[i] : NULL;
}

int dl_version(void)
{
    return version;
}

void dl_status(char * buf, size_t size)
{
    pthread_mutex_lock(&lock);
    snprintf(buf, size, "%s", status_text);
    pthread_mutex_unlock(&lock);
}

const char * dl_cover(const DlHit * h)
{
    if (!h->art[0])
        return NULL;
    start();
    // the file is named after the address
    unsigned hash = 5381;
    for (const char * p = h->art; *p; p++)
        hash = hash * 33 + (unsigned char)*p;
    static char path[400];
    snprintf(path, sizeof(path), "%s/covers/%08x.jpg", tmp_dir(), hash);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0)
        return path;
    pthread_mutex_lock(&lock);
    if (strcmp(cover_done_url, h->art) != 0 && strcmp(cover_url, h->art) != 0) {
        snprintf(cover_url, sizeof(cover_url), "%s", h->art);
        snprintf(cover_path, sizeof(cover_path), "%s", path);
        cover_pending = true;
    } else if (strcmp(cover_url, h->art) == 0 && !cover_pending && strcmp(cover_done_url, h->art) != 0)
        cover_pending = true;
    pthread_mutex_unlock(&lock);
    return NULL;
}

// one line of a saved job: tabs would break the columns
static void one_line(const char * s, char * out, size_t size)
{
    size_t n = 0;
    for (; *s && n + 1 < size; s++)
        out[n++] = *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s;
    out[n] = 0;
}

// the queue survives the game: what's waiting and what failed, so the next run carries on and can retry
static void dl_save_locked(void)
{
    char path[400], id[80], name[96], artist[96], url[600], art[400], msg[96];
    snprintf(path, sizeof(path), "%sdl.txt", plat_data_dir());
    FILE * f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < job_count; i++) {
        const Job * j = &jobs[i];
        int st = j->state == JOB_DOWNLOADING || j->state == JOB_INSTALLING ? JOB_QUEUED : j->state;
        if (st != JOB_QUEUED && st != JOB_FAILED)
            continue;
        one_line(j->hit.id, id, sizeof(id));
        one_line(j->hit.name, name, sizeof(name));
        one_line(j->hit.artist, artist, sizeof(artist));
        one_line(j->hit.url, url, sizeof(url));
        one_line(j->hit.art, art, sizeof(art));
        one_line(j->msg, msg, sizeof(msg));
        fprintf(f, "%d\t%s\t%s\t%s\t%s\t%s\t%lld\t%s\n", st, id, name, artist, url, art, (long long)j->hit.size, msg);
    }
    fclose(f);
}

static void dl_load(void)
{
    char path[400];
    snprintf(path, sizeof(path), "%sdl.txt", plat_data_dir());
    FILE * f = fopen(path, "r");
    if (!f)
        return;
    char line[1700];
    int carried = 0;
    while (fgets(line, sizeof(line), f) && job_count < MAX_JOBS) {
        Job * j = &jobs[job_count];
        memset(j, 0, sizeof(*j));
        char size[32] = "", msg[96] = "";
        int st;
        int got = sscanf(line, "%d\t%79[^\t]\t%95[^\t]\t%95[^\t]\t%599[^\t]\t%399[^\t]\t%31[^\t]\t%95[^\n]",
                         &st, j->hit.id, j->hit.name, j->hit.artist, j->hit.url, j->hit.art, size, msg);
        if (got < 6 || (st != JOB_QUEUED && st != JOB_FAILED) || !j->hit.id[0] || !j->hit.url[0])
            continue;
        j->hit.size = atoll(size);
        snprintf(j->msg, sizeof(j->msg), "%s", msg);
        j->state = st;
        job_count++;
        carried++;
    }
    fclose(f);
    if (carried)
        plat_log("dl: %d jobs carried over from the last run", carried);
}

bool dl_queue(const DlHit * h)
{
    start();
    pthread_mutex_lock(&lock);
    Job * j = NULL;
    for (int i = 0; i < job_count; i++)
        if (!strcmp(jobs[i].hit.id, h->id))
            j = &jobs[i];
    if (j && (j->state == JOB_QUEUED || j->state == JOB_DOWNLOADING || j->state == JOB_INSTALLING)) {
        pthread_mutex_unlock(&lock);
        return true;
    }
    if (!j) {
        // a finished, failed or emptied slot makes room, in place: nothing in the array ever shifts, so the job
        // thread's pointer to the one running stays good
        for (int i = 0; i < job_count && !j; i++)
            if (jobs[i].state == JOB_DONE || jobs[i].state == JOB_FAILED || jobs[i].state == JOB_NONE)
                j = &jobs[i];
        if (!j && job_count < MAX_JOBS)
            j = &jobs[job_count++];
        if (!j) {
            pthread_mutex_unlock(&lock);
            return false;
        }
    }
    memset(j, 0, sizeof(*j));
    j->hit = *h;
    j->state = JOB_QUEUED;
    dl_save_locked();
    pthread_mutex_unlock(&lock);
    return true;
}

void dl_forget(const char * id)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < job_count; i++)
        if (!strcmp(jobs[i].hit.id, id) && jobs[i].state != JOB_DOWNLOADING && jobs[i].state != JOB_INSTALLING) {
            jobs[i].state = JOB_NONE;
            dl_save_locked();
        }
    pthread_mutex_unlock(&lock);
}

int dl_jobs_list(DlJob * out, int max)
{
    int n = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < job_count && n < max; i++)
        if (jobs[i].state != JOB_NONE) {
            out[n].hit = jobs[i].hit;
            out[n].state = jobs[i].state;
            out[n].percent = jobs[i].total > 0 ? (int)(jobs[i].done * 100 / jobs[i].total) : -1;
            out[n].done = jobs[i].done;
            out[n].total = jobs[i].total;
            snprintf(out[n].msg, sizeof(out[n].msg), "%s", jobs[i].msg);
            n++;
        }
    pthread_mutex_unlock(&lock);
    return n;
}

void dl_cancel(const char * id)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < job_count; i++)
        if (!strcmp(jobs[i].hit.id, id)) {
            if (jobs[i].state == JOB_QUEUED) {
                jobs[i].state = JOB_FAILED;
                snprintf(jobs[i].msg, sizeof(jobs[i].msg), "cancelled");
            } else if (jobs[i].state == JOB_DOWNLOADING)
                jobs[i].cancel = true;
        }
    pthread_mutex_unlock(&lock);
}

// one of the waiting ones goes before the others: it gets the smallest mark there is
void dl_prioritize(const char * id)
{
    static long long last_mark;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < job_count; i++)
        if (!strcmp(jobs[i].hit.id, id) && jobs[i].state == JOB_QUEUED) {
            jobs[i].bumps = --last_mark;
            break;
        }
    pthread_mutex_unlock(&lock);
}

void dl_clear_done(void)
{
    bool any = false;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < job_count; i++)
        if (jobs[i].state == JOB_DONE) {
            jobs[i].state = JOB_NONE;
            any = true;
        }
    if (any)
        dl_save_locked();
    pthread_mutex_unlock(&lock);
}

JobState dl_job_state(const char * id, int * percent, char * msg, size_t msg_size)
{
    JobState st = JOB_NONE;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < job_count; i++)
        if (!strcmp(jobs[i].hit.id, id)) {
            st = jobs[i].state;
            if (percent)
                *percent = jobs[i].total > 0 ? (int)(jobs[i].done * 100 / jobs[i].total) : -1;
            if (msg)
                snprintf(msg, msg_size, "%s", jobs[i].msg);
        }
    pthread_mutex_unlock(&lock);
    return st;
}

int dl_jobs_active(void)
{
    int n = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < job_count; i++)
        n += jobs[i].state == JOB_QUEUED || jobs[i].state == JOB_DOWNLOADING || jobs[i].state == JOB_INSTALLING;
    pthread_mutex_unlock(&lock);
    return n;
}

int dl_jobs_done(void)
{
    return jobs_added;
}

void dl_now(char * buf, size_t size)
{
    pthread_mutex_lock(&lock);
    const Job * cur = NULL;
    int queued = 0;
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].state == JOB_DOWNLOADING || jobs[i].state == JOB_INSTALLING)
            cur = &jobs[i];
        else if (jobs[i].state == JOB_QUEUED)
            queued++;
    }
    buf[0] = 0;
    if (cur) {
        char how[40] = "";
        if (cur->state == JOB_INSTALLING)
            snprintf(how, sizeof(how), "adding, making it smaller");
        else if (cur->total > 0)
            snprintf(how, sizeof(how), "%d%%", (int)(cur->done * 100 / cur->total));
        else if (cur->done > 0)
            snprintf(how, sizeof(how), "%.1f MB", cur->done / 1048576.0);
        snprintf(buf, size, "%s  %s", cur->hit.name, how);
        if (queued) {
            size_t l = strlen(buf);
            snprintf(buf + l, size - l, "   (%d more)", queued);
        }
    } else if (queued)
        snprintf(buf, size, "%d waiting", queued);
    pthread_mutex_unlock(&lock);
}

bool dl_take_installed(char * folder, size_t size)
{
    bool got = false;
    pthread_mutex_lock(&lock);
    if (installed_count) {
        snprintf(folder, size, "%s", installed[0]);
        memmove(installed[0], installed[1], (installed_count - 1) * sizeof(installed[0]));
        installed_count--;
        got = true;
    }
    pthread_mutex_unlock(&lock);
    return got;
}

static void job_msg(Job * j, const char * fmt, const char * a)
{
    pthread_mutex_lock(&lock);
    snprintf(j->msg, sizeof(j->msg), fmt, a);
    pthread_mutex_unlock(&lock);
}

// -------------------------------------------------------------------------------------------- unpacking a download

// where an archive's file goes in dir, its folders made; false: skip it (no climbing out of the folder)
static bool entry_path(const char * name, const char * dir, char * path, size_t size)
{
    char rel[500];
    snprintf(rel, sizeof(rel), "%s", name);
    for (char * p = rel; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (rel[0] == '/' || strstr(rel, "../") || !strcmp(rel, ".."))
        return false;
    dl_safe_part(rel);
    snprintf(path, size, "%s/%s", dir, rel);
    return true;
}

// a solid RAR, each file packed on the ones before: libarchive won't, so these go through rar.c
typedef struct {
    const char * dir;
    char * err;
    size_t err_size;
    bool failed;
} SolidOut;

static FILE * solid_open(void * user, const char * name)
{
    SolidOut * o = user;
    char path[800];
    if (o->failed || !entry_path(name, o->dir, path, sizeof(path)))
        return NULL;
    char * slash = strrchr(path, '/');
    *slash = 0;
    dl_mkdirs(path);
    *slash = '/';
    FILE * f = fopen(path, "wb");
    if (!f) {
        snprintf(o->err, o->err_size, "can't write to the memory card");
        o->failed = true;
    }
    return f;
}

static void solid_close(void * user, FILE * f)
{
    SolidOut * o = user;
    if (fclose(f) != 0 && !o->failed) {
        snprintf(o->err, o->err_size, "the memory card is full");
        o->failed = true;
    }
}

bool dl_extract_archive(const char * file, const char * dir, char * err, size_t err_size)
{
    dl_mkdirs(dir);
#ifdef __vita__
    // the Vita's libarchive has no LZMA: 7-Zip packages go through un7zip
    FILE * f = fopen(file, "rb");
    unsigned char magic[6] = {0};
    if (f) {
        size_t got = fread(magic, 1, 6, f);
        (void)got;
        fclose(f);
    }
    if (!memcmp(magic, "7z\xBC\xAF\x27\x1C", 6)) {
        int r = un7z_ExtractArchive(file, dir, NULL, UN7Z_IN_BUF_SIZE);
        if (r != SZ_OK) {
            snprintf(err, err_size, "couldn't unpack the 7-Zip file (%d)", r);
            return false;
        }
        return true;
    }
#endif
    struct archive * a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_support_format_7zip(a);
    if (archive_read_open_filename(a, file, 64 * 1024) != ARCHIVE_OK) {
        snprintf(err, err_size, "couldn't open the archive: %s", archive_error_string(a));
        archive_read_free(a);
        return false;
    }
    struct archive_entry * e;
    bool ok = true;
    int files = 0;
    static char buf[64 * 1024];
    while (ok) {
        int r = archive_read_next_header(a, &e);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_WARN) {
            const char * why = archive_error_string(a);
            if (why && strstr(why, "solid")) {
                SolidOut o = {dir, err, err_size, false};
                archive_read_free(a);
                return rar_extract(file, solid_open, solid_close, &o, err, err_size) && !o.failed;
            }
            snprintf(err, err_size, "the archive is damaged: %s", why);
            ok = false;
            break;
        }
        const char * name = archive_entry_pathname_utf8(e);
        if (!name)
            name = archive_entry_pathname(e);
        if (!name)
            continue;
        char path[800];
        if (!entry_path(name, dir, path, sizeof(path)))
            continue;
        if (archive_entry_filetype(e) == AE_IFDIR) {
            dl_mkdirs(path);
            continue;
        }
        if (archive_entry_filetype(e) != AE_IFREG)
            continue;
        char * slash = strrchr(path, '/');
        *slash = 0;
        dl_mkdirs(path);
        *slash = '/';
        FILE * out = fopen(path, "wb");
        if (!out) {
            snprintf(err, err_size, "can't write to the memory card");
            ok = false;
            break;
        }
        la_ssize_t n;
        while ((n = archive_read_data(a, buf, sizeof(buf))) > 0)
            if (fwrite(buf, 1, n, out) != (size_t)n) {
                snprintf(err, err_size, "the memory card is full");
                ok = false;
                break;
            }
        if (n < 0 && ok) {
            snprintf(err, err_size, "couldn't unpack %s: %s", name, archive_error_string(a));
            ok = false;
        }
        if (fclose(out) != 0 && ok) {
            snprintf(err, err_size, "the memory card is full");
            ok = false;
        }
        files++;
    }
    archive_read_free(a);
    if (ok && !files) {
        snprintf(err, err_size, "the archive is empty");
        ok = false;
    }
    return ok;
}

static const char * sniff(const char * file)
{
    FILE * f = fopen(file, "rb");
    if (!f)
        return "missing";
    unsigned char m[16] = {0};
    size_t n = fread(m, 1, sizeof(m), f);
    fclose(f);
    if (n >= 6 && !memcmp(m, "SNGPKG", 6))
        return "sng";
    if (n >= 4 && !memcmp(m, "PK\x03\x04", 4))
        return "archive";
    if (n >= 4 && !memcmp(m, "Rar!", 4))
        return "archive";
    if (n >= 6 && !memcmp(m, "7z\xBC\xAF\x27\x1C", 6))
        return "archive";
    if (n >= 4 && (!memcmp(m, "CON ", 4) || !memcmp(m, "LIVE", 4)))
        return "rockband";
    size_t i = 0;
    while (i < n && isspace(m[i]))
        i++;
    if (i < n && m[i] == '<')
        return "html";
    return "unknown";
}

static bool has_ext(const char * name, const char * ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

typedef struct {
    Job * job;
    int added;
    char last_err[200];
} Install;

// a Rock Band package keeps its cover in a console's format: the site's picture does instead
static void fetch_cover(const char * url, const char * folder)
{
    char path[440], err[160];
    snprintf(path, sizeof(path), "%s/album.jpg", folder);
    struct stat st;
    if (!url[0] || stat(path, &st) == 0)
        return;
    char * body = NULL;
    size_t len = 0;
    HttpReq r = {.url = url, .stall_s = 15};
    if (http_fetch(&r, &body, &len, err, sizeof(err)) == 200 && body)
        songconv_cover((const unsigned char *)body, len, path);
    free(body);
}

static void install_one(Install * in, const char * src, bool move)
{
    char songs[300], out[400], err[200];
    snprintf(songs, sizeof(songs), "%ssongs", plat_data_dir());
    mkdir(songs, 0777);
    ConvOptions opt = {.source_id = in->job->hit.id, .move = move};
    if (songconv_install(src, songs, &opt, out, sizeof(out), err, sizeof(err))) {
        pthread_mutex_lock(&lock);
        if (installed_count < 32)
            snprintf(installed[installed_count++], sizeof(installed[0]), "%s", out);
        pthread_mutex_unlock(&lock);
        in->added++;
        plat_log("dl: added %s", out);
        fetch_cover(in->job->hit.art, out);
        shrink_song(out);
    } else {
        snprintf(in->last_err, sizeof(in->last_err), "%s", err);
        plat_log("dl: %s: %s", src, err);
    }
}

// the songs in an unpacked download: Clone Hero folders, .sng packages, and archives inside archives
static void install_tree(Install * in, const char * dir, int depth)
{
    if (songconv_is_raw(dir)) {
        install_one(in, dir, true);
        return;
    }
    DIR * d = opendir(dir);
    if (!d)
        return;
    char (*names)[256] = NULL;
    int n = 0;
    struct dirent * e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "__MACOSX"))
            continue;
        names = realloc(names, (n + 1) * sizeof(*names));
        snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        struct stat st;
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 5)
                install_tree(in, p, depth + 1);
        } else if (has_ext(names[i], ".sng"))
            install_one(in, p, true);
        else if (depth < 4 && (has_ext(names[i], ".zip") || has_ext(names[i], ".rar") || has_ext(names[i], ".7z"))) {
            char sub[720], err[200];
            snprintf(sub, sizeof(sub), "%s.x", p);
            if (dl_extract_archive(p, sub, err, sizeof(err)))
                install_tree(in, sub, depth + 1);
            else
                snprintf(in->last_err, sizeof(in->last_err), "%s", err);
        } else if (depth < 4 && !strcmp(sniff(p), "rockband")) {
            // a pack of Rock Band songs: each package has a name of its own and no extension
            char sub[720];
            snprintf(sub, sizeof(sub), "%s.x", p);
            if (rbcon_unpack(p, sub, in->last_err, sizeof(in->last_err)))
                install_tree(in, sub, depth + 1);
        }
    }
    free(names);
}

// what a web page in place of a file means
static void web_page_instead(const char * file, const char * url, const char * name, char * err, size_t es)
{
    char head[4096] = "";
    FILE * f = fopen(file, "rb");
    if (f) {
        size_t n = fread(head, 1, sizeof(head) - 1, f);
        head[n] = 0;
        fclose(f);
    }
    bool drive = strstr(url, "google.com") != NULL;
    if (drive && (strstr(head, "Quota exceeded") || strstr(head, "Too many users")))
        snprintf(err, es, "Google Drive won't give it now: downloaded too much lately (try again later)");
    else if (drive && (strstr(head, "accounts.google.com") || strstr(head, "ServiceLogin")))
        snprintf(err, es, "the Google Drive file is private");
    else
        snprintf(err, es, "%s sent a web page, not %s (gone, private or over its limit)", drive ? "Google Drive"
                                                                                             : "the host", name);
}

// what of a song folder is worth fetching: 2 needed (the chart, song.ini, the audio, packed songs), 1 nice to have
// (the cover), 0 not used here (videos, backgrounds, the editors' own files)
static int wanted(const char * name)
{
    static const char * const needed[] = {".chart", ".mid", ".midi", ".ini", ".ogg", ".opus", ".mp3", ".wav",
                                          ".mix", ".sng", ".zip", ".rar", ".7z"};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); i++)
        if (has_ext(name, needed[i]))
            return 2;
    if (!strncasecmp(name, "album.", 6) && (has_ext(name, ".png") || has_ext(name, ".jpg") || has_ext(name, ".jpeg")))
        return 1;
    // a Rock Band package has no extension, though it may have a dot ("Mr. Brightside_rb3con")
    const char * dot = strrchr(name, '.');
    return !dot || strlen(dot) > 6 || strchr(dot, ' ') || strchr(dot, '_') ? 2 : 0;
}

static void mega_file(void * user, const char * name)
{
    job_msg(user, "file %s", name);
}

// a shared Google Drive folder: its listing page names the files, each then comes on its own
static bool drive_folder(Job * j, const char * id, const char * dir, int depth, int * files, char * err, size_t es)
{
    char url[300];
    snprintf(url, sizeof(url), "https://drive.google.com/embeddedfolderview?id=%s", id);
    char * body = NULL;
    size_t len;
    HttpReq r = {.url = url, .cancel = &j->cancel, .stall_s = 30};
    int status = http_fetch(&r, &body, &len, err, es);
    if (status != 200 || !body) {
        if (status)
            snprintf(err, es, "the Google Drive folder can't be opened (%d): private or gone", status);
        free(body);
        return false;
    }
    dl_mkdirs(dir);
    bool ok = true;
    int found = 0;
    for (char * p = body; ok && (p = strstr(p, "id=\"entry-"));) {
        p += 10;
        char eid[128] = "", href[400] = "", name[256] = "";
        sscanf(p, "%127[A-Za-z0-9_-]", eid);
        char * h = strstr(p, "href=\"");
        char * t = strstr(p, "flip-entry-title\">");
        if (!eid[0] || !h || !t)
            continue;
        sscanf(h + 6, "%399[^\"]", href);
        sscanf(t + 18, "%255[^<]", name);
        html_unescape(name);
        dl_safe_part(name);
        for (char * q = name; *q; q++)
            if (*q == '/')
                *q = '_';
        if (!name[0])
            continue;
        found++;
        char path[800];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (strstr(href, "/folders/")) {
            if (depth < 3)
                ok = drive_folder(j, eid, path, depth + 1, files, err, es);
            continue;
        }
        int want = wanted(name);
        if (!want)
            continue;
        (*files)++;
        job_msg(j, "file %s", name);
        snprintf(url, sizeof(url), "https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t",
                 eid);
        HttpReq fr = {.url = url, .to_file = path, .cancel = &j->cancel, .done = &j->done, .total = &j->total};
        status = http_fetch(&fr, NULL, NULL, err, es);
        if (status == 403 || status == 429)
            snprintf(err, es, "Google Drive won't give it now: downloaded too much lately (try again later)");
        else if (status && status != 200)
            snprintf(err, es, "Google Drive refused %s (%d)", name, status);
        else if (status == 200 && !strcmp(sniff(path), "html") && !has_ext(name, ".htm") && !has_ext(name, ".html")) {
            web_page_instead(path, url, name, err, es);
            status = 0;
        }
        if (status != 200) {
            remove(path);
            if (want == 2)
                ok = false;         // a cover that won't come is no reason to give up the song
        }
    }
    free(body);
    if (ok && !found && depth == 0) {
        snprintf(err, es, "the Google Drive folder is empty or private");
        ok = false;
    }
    return ok;
}

static void run_job(Job * j)
{
    // each job its own corner of tmp/: more than one download can be running (the conversion is one at a time)
    char base[300], file[400], xdir[400], err[200] = "", tag[96];
    snprintf(tag, sizeof(tag), "%s", j->hit.id);
    dl_safe_part(tag);
    snprintf(base, sizeof(base), "%s/job-%s", tmp_dir(), tag);
    snprintf(file, sizeof(file), "%s/download.bin", base);
    snprintf(xdir, sizeof(xdir), "%s/unpacked", base);
    dl_remove_tree(xdir);
    remove(file);
    dl_mkdirs(base);
    pthread_mutex_lock(&lock);
    j->state = JOB_DOWNLOADING;
    j->done = j->total = 0;
    j->msg[0] = 0;
    pthread_mutex_unlock(&lock);
    plat_log("dl: %s - %s (%s) from %s", j->hit.artist, j->hit.name, j->hit.id, j->hit.url);

    const char * url = j->hit.url;
    char page_url[600];
    bool ok = true, folder = false;
    DlHit landed;               // the short link's landing: each job thread's own
    if (!strncmp(url, "short:", 6)) {
        // a short link: the same hosts as the rest, once it's been followed
        char to[600];
        snprintf(to, sizeof(to), "%s", url + 6);
        for (int hop = 0; hop < 5 && (strstr(to, "bit.ly/") || strstr(to, "tinyurl.com/")); hop++) {
            char next[600];
            if (!http_location(to, next, sizeof(next), &j->cancel)) {
                ok = false;
                snprintf(err, sizeof(err), "the short link doesn't lead anywhere");
                break;
            }
            snprintf(to, sizeof(to), "%s", next);
        }
        if (ok) {
            memset(&landed, 0, sizeof(landed));
            resolve_rv(&landed, to);
            url = landed.url[0] ? landed.url : to;
            plat_log("dl: the short link leads to %s", url);
        }
    }
    if (!ok) {
        // (a short link that went nowhere)
    } else if (!strncmp(url, "gdrive-folder:", 14)) {
        int files = 0;
        folder = true;
        ok = drive_folder(j, url + 14, xdir, 0, &files, err, sizeof(err));
    } else if (!strncmp(url, "mega:", 5)) {
        MegaJob m = {.cancel = &j->cancel, .done = &j->done, .total = &j->total, .wanted = wanted, .on_file = mega_file,
                     .user = j};
        ok = mega_fetch(url + 5, file, xdir, &m, &folder, err, sizeof(err));
    } else {
        if (!strncmp(url, "mediafire:", 10)) {
            // the download button's address is on the file's page
            char * body = NULL;
            size_t len;
            HttpReq r = {.url = url + 10, .cancel = &j->cancel, .stall_s = 30};
            int status = http_fetch(&r, &body, &len, err, sizeof(err));
            char * p = body ? strstr(body, "href=\"https://download") : NULL;
            if (status == 200 && p) {
                sscanf(p + 6, "%599[^\"]", page_url);
                url = page_url;
            } else {
                if (status)
                    snprintf(err, sizeof(err), "MediaFire didn't give the file (gone?)");
                ok = false;
            }
            free(body);
        }
        if (ok) {
            HttpReq r = {.url = url, .to_file = file, .cancel = &j->cancel, .done = &j->done, .total = &j->total};
            int status = http_fetch(&r, NULL, NULL, err, sizeof(err));
            if (status != 200) {
                if (status == 404 || status == 410)
                    snprintf(err, sizeof(err), "the file is gone from its host");
                else if (status)
                    snprintf(err, sizeof(err), "the host answered %d", status);
                ok = false;
            }
        }
    }
    Install in = {j, 0, ""};
    if (ok) {
        // the unpacking and converting is one at a time (its decoders were built for it); the downloads keep
        // moving beside it. A song being played doesn't hold this up either: what can't convert now stays as
        // it came, and the library's shrink gets it another time
        shrink_defer(true);
        pthread_mutex_lock(&install_lock);
        pthread_mutex_lock(&lock);
        j->state = JOB_INSTALLING;
        pthread_mutex_unlock(&lock);
        if (folder)
            install_tree(&in, xdir, 0);
        else {
            const char * kind = sniff(file);
            if (!strcmp(kind, "sng"))
                install_one(&in, file, true);
            else if (!strcmp(kind, "archive")) {
                if (dl_extract_archive(file, xdir, err, sizeof(err))) {
                    remove(file);
                    install_tree(&in, xdir, 0);
                } else
                    snprintf(in.last_err, sizeof(in.last_err), "%s", err);
            } else if (!strcmp(kind, "html"))
                web_page_instead(file, url, "the song", in.last_err, sizeof(in.last_err));
            else if (!strcmp(kind, "rockband")) {
                in.last_err[0] = 0;
                if (rbcon_unpack(file, xdir, in.last_err, sizeof(in.last_err))) {
                    remove(file);
                    install_tree(&in, xdir, 0);
                }
            }
            else
                snprintf(in.last_err, sizeof(in.last_err), "not a song package");
        }
        if (!in.added && !in.last_err[0])
            snprintf(in.last_err, sizeof(in.last_err), "no Clone Hero song in it");
        snprintf(err, sizeof(err), "%s", in.last_err);
    }
    dl_remove_tree(xdir);
    remove(file);
    pthread_mutex_unlock(&install_lock);
    shrink_defer(false);
    pthread_mutex_lock(&lock);
    if (in.added) {
        j->state = JOB_DONE;
        jobs_added += in.added;
        if (in.added > 1)
            snprintf(j->msg, sizeof(j->msg), "%d songs added", in.added);
        else
            j->msg[0] = 0;
    } else {
        j->state = JOB_FAILED;
        snprintf(j->msg, sizeof(j->msg), "%s", j->cancel ? "cancelled" : err);
    }
    pthread_mutex_unlock(&lock);
    plat_log("dl: %s: %s", j->hit.id, in.added ? "done" : j->msg);
}

static int job_thread(void * arg)
{
    (void)arg;
    for (;;) {
        Job * next = NULL;
        pthread_mutex_lock(&lock);
        // "downloads next" is the smallest mark; the others go in the order they were asked for
        for (int i = 0; i < job_count; i++)
            if (jobs[i].state == JOB_QUEUED && (!next || jobs[i].bumps < next->bumps))
                next = &jobs[i];
        pthread_mutex_unlock(&lock);
        if (next) {
            run_job(next);
            pthread_mutex_lock(&lock);
            dl_save_locked();               // what it became is what the next run picks up
            pthread_mutex_unlock(&lock);
        }
        else
            plat_sleep_us(50000);
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------- chart updates
// Songs from Chorus remember the chart's md5 ("chorus:<md5>"). A chart its charter has updated has a new md5 under the
// same name, artist and charter: a search for each finds them, on a worker, one request at a time.

#define UPD_MAX 64
static struct {
    DlUpdateAsk * ask;
    int n;
    volatile int done, found;
    volatile bool running;
    DlHit hits[UPD_MAX];
    char old[UPD_MAX][256];
} upd;
static pthread_mutex_t upd_lock = PTHREAD_MUTEX_INITIALIZER;

static void json_escape(char * out, size_t size, const char * in)
{
    size_t k = 0;
    for (; *in && k + 2 < size; in++) {
        if (*in == '"' || *in == '\\')
            out[k++] = '\\';
        out[k++] = (unsigned char)*in < 32 ? ' ' : *in;
    }
    out[k] = 0;
}

static void * upd_thread(void * arg)
{
    (void)arg;
    for (int i = 0; i < upd.n; i++) {
        const DlUpdateAsk * a = &upd.ask[i];
        char q[200], post[600], err[160];
        char both[200];
        snprintf(both, sizeof(both), "%s %s", a->name, a->artist);
        json_escape(q, sizeof(q), both);
        snprintf(post, sizeof(post), "{\"search\":\"%s\",\"page\":1,\"per_page\":25}", q);
        const char * headers[] = {"Content-Type: application/json", NULL};
        HttpReq r = {.url = "https://api.enchor.us/search", .post = post, .headers = headers, .stall_s = 20};
        char * body = NULL;
        size_t len = 0;
        int status = http_fetch(&r, &body, &len, err, sizeof(err));
        DlHit * hits = NULL;
        int total = 0, n = status >= 200 && status < 300 ? parse_chorus(body, len, &hits, &total) : 0;
        free(body);
        for (int k = 0; k < n; k++) {
            const DlHit * h = &hits[k];
            // the same chart: name, artist and charter alike; a newer one when its md5 isn't the one on the card
            if (strcasecmp(h->name, a->name) || strcasecmp(h->artist, a->artist) || strcasecmp(h->charter, a->charter))
                continue;
            if (!strcmp(h->id, a->source))
                break;                          // the one on the card is still the newest
            pthread_mutex_lock(&upd_lock);
            if (upd.found < UPD_MAX) {
                upd.hits[upd.found] = *h;
                snprintf(upd.old[upd.found], sizeof(upd.old[0]), "%s", a->folder);
                upd.found++;
            }
            pthread_mutex_unlock(&upd_lock);
            plat_log("update: %s - %s has a newer chart (%s)", a->artist, a->name, h->id);
            break;
        }
        free(hits);
        upd.done = i + 1;
    }
    free(upd.ask);
    upd.ask = NULL;
    upd.running = false;
    return NULL;
}

bool dl_updates_start(const DlUpdateAsk * ask, int n)
{
    if (upd.running || n <= 0)
        return false;
    upd.ask = malloc(n * sizeof(DlUpdateAsk));
    if (!upd.ask)
        return false;
    memcpy(upd.ask, ask, n * sizeof(DlUpdateAsk));
    upd.n = n;
    upd.done = upd.found = 0;
    upd.running = true;
    pthread_t t;
    if (pthread_create(&t, NULL, upd_thread, NULL) != 0) {
        free(upd.ask);
        upd.ask = NULL;
        upd.running = false;
        return false;
    }
    pthread_detach(t);
    return true;
}

bool dl_updates_progress(int * done, int * total, int * found)
{
    *done = upd.done;
    *total = upd.n;
    *found = upd.found;
    return upd.running;
}

int dl_updates_take(DlHit * out, char (*old)[256], int max)
{
    if (upd.running)
        return 0;
    pthread_mutex_lock(&upd_lock);
    int n = upd.found < max ? upd.found : max;
    memcpy(out, upd.hits, n * sizeof(DlHit));
    memcpy(old, upd.old, n * sizeof(old[0]));
    upd.found = 0;
    pthread_mutex_unlock(&upd_lock);
    return n;
}
