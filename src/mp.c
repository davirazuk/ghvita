// Playing together and sharing songs (mp.h), on p2p.h's connection. Main thread only: mp_poll runs every frame.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "dl.h"
#include "mp.h"
#include "p2p.h"
#include "plat.h"

void p2p_set_songs(int n);

enum { MT_LOBBY = 10, MT_PICK, MT_READY, MT_START, MT_STATE, MT_OVER,
       MT_LIST_REQ = 20, MT_LIST, MT_LIST_END, MT_GET, MT_SONG_BEGIN, MT_FILE_BEGIN, MT_FILE_DATA, MT_FILE_END, MT_SONG_END,
       MT_SONG_FAIL };
#define CHUNK 1180              // a file's bytes a message
#define OUT_AHEAD 300           // messages queued ahead before more are made (the rest wait for room)

static struct {
    int was;                    // p2p's state last frame
    // the lobby
    char key[96], name[96], artist[96];
    char my_folder[256];        // this device's copy of the pick ("" none)
    int my_part, my_diff, their_part, their_diff;
    bool me_ready, they_ready, they_have, asked;
    double start_at;
    MpState theirs;
    bool theirs_in;
    // the other's list
    MpSong * list;
    int list_n, list_cap;
    bool list_done;
    // songs going out
    char want[64][96];
    int want_head, want_n;
    bool sending;
    char out_key[96], out_dir[300];
    char files[64][128];
    long sizes[64];
    int nfiles, file_at;
    FILE * out_f;
    long out_total, out_sent;
    int list_out;               // -1: not sending the list; else the next song's index
    // songs coming in
    bool receiving;
    char in_key[96], in_name[96], in_tmp[320];
    FILE * in_f;
    long in_total, in_got;
    int got;
} M = {.list_out = -1};

static size_t slen(const char * s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}
#define strnlen slen

static void base_name(const char * path, char * out, size_t size)
{
    const char * s = strrchr(path, '/');
    snprintf(out, size, "%s", s ? s + 1 : path);
}

// "AC/DC" and "ac-dc" alike (as songs.c matches downloads)
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

static int find_local(const char * artist, const char * name)
{
    char a[96], n[96], x[96];
    match_key(a, sizeof(a), artist);
    match_key(n, sizeof(n), name);
    for (int i = 0; i < songs_count(); i++) {
        Song * s = songs_get(i);
        match_key(x, sizeof(x), s->name);
        if (strcmp(x, n))
            continue;
        match_key(x, sizeof(x), s->artist);
        if (!strcmp(x, a))
            return i;
    }
    return -1;
}

static int find_key(const char * key)
{
    char b[256];
    for (int i = 0; i < songs_count(); i++) {
        base_name(songs_get(i)->folder, b, sizeof(b));
        if (!strcmp(b, key))
            return i;
    }
    return -1;
}

// a name sent over that's fit to be a file's or folder's here
static bool safe_name(const char * s)
{
    return s[0] && s[0] != '.' && !strchr(s, '/') && !strchr(s, '\\') && !strstr(s, "..") && strlen(s) < 120;
}

static void send_ready(void)
{
    uint8_t b[2] = {M.me_ready, M.my_folder[0] != 0};
    p2p_send(MT_READY, b, 2);
}

static void send_pick(void)
{
    uint8_t b[2] = {(uint8_t)M.my_part, (uint8_t)M.my_diff};
    p2p_send(MT_PICK, b, 2);
}

static void lobby_reset(void)
{
    M.key[0] = M.name[0] = M.artist[0] = M.my_folder[0] = 0;
    M.me_ready = M.they_ready = M.they_have = M.asked = false;
    M.start_at = 0;
    M.theirs_in = false;
    M.their_part = M.their_diff = -1;
}

static void link_reset(void)
{
    lobby_reset();
    M.list_n = 0;
    M.list_done = false;
    M.want_n = M.want_head = 0;
    if (M.out_f)
        fclose(M.out_f);
    M.out_f = NULL;
    M.sending = false;
    M.list_out = -1;
    if (M.in_f)
        fclose(M.in_f);
    M.in_f = NULL;
    if (M.receiving)
        dl_remove_tree(M.in_tmp);
    M.receiving = false;
    M.got = 0;
}

bool mp_active(void)
{
    return p2p_state() == P2P_CONNECTED;
}

// the pick found here: by its artist and name (the folder's name differs from device to device)
static void look_for_pick(void)
{
    M.my_folder[0] = 0;
    int i = find_local(M.artist, M.name);
    if (i >= 0)
        snprintf(M.my_folder, sizeof(M.my_folder), "%s", songs_get(i)->folder);
}

// ------------------------------------------------------------------------------------------------- songs going out
static void list_files(const char * dir)
{
    M.nfiles = 0;
    M.out_total = 0;
    DIR * d = opendir(dir);
    if (!d)
        return;
    struct dirent * e;
    while ((e = readdir(d)) && M.nfiles < 64) {
        if (!safe_name(e->d_name))
            continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        snprintf(M.files[M.nfiles], sizeof(M.files[0]), "%s", e->d_name);
        M.sizes[M.nfiles++] = (long)st.st_size;
        M.out_total += (long)st.st_size;
    }
    closedir(d);
}

static void feed_out(void)
{
    // the list asked for, a song a message
    while (M.list_out >= 0 && p2p_backlog() < OUT_AHEAD) {
        if (M.list_out >= songs_count()) {
            p2p_send(MT_LIST_END, NULL, 0);
            M.list_out = -1;
            break;
        }
        Song * s = songs_get(M.list_out++);
        uint8_t b[P2P_MAX_MSG];
        char key[96];
        base_name(s->folder, key, sizeof(key));
        int n = snprintf((char *)b, 300, "%s", key) + 1;
        n += snprintf((char *)b + n, 100, "%s", s->name) + 1;
        n += snprintf((char *)b + n, 100, "%s", s->artist) + 1;
        p2p_send(MT_LIST, b, n);
    }
    // songs asked for, a file's bytes at a time, as far as there's room in the queue
    while (p2p_backlog() < OUT_AHEAD) {
        if (!M.sending) {
            if (!M.want_n)
                return;
            snprintf(M.out_key, sizeof(M.out_key), "%s", M.want[M.want_head]);
            M.want_head = (M.want_head + 1) % 64;
            M.want_n--;
            int i = find_key(M.out_key);
            if (i < 0) {
                p2p_send(MT_SONG_FAIL, M.out_key, (int)strlen(M.out_key) + 1);
                continue;
            }
            Song * s = songs_get(i);
            snprintf(M.out_dir, sizeof(M.out_dir), "%s", s->folder);
            list_files(M.out_dir);
            uint8_t b[400];
            int n = snprintf((char *)b, 100, "%s", M.out_key) + 1;
            n += snprintf((char *)b + n, 100, "%s", s->name) + 1;
            n += snprintf((char *)b + n, 100, "%s", s->artist) + 1;
            uint32_t tot = (uint32_t)M.out_total;
            memcpy(b + n, &tot, 4);
            p2p_send(MT_SONG_BEGIN, b, n + 4);
            M.sending = true;
            M.file_at = 0;
            M.out_sent = 0;
            M.out_f = NULL;
            plat_log("mp: sending %s (%ld KB)", M.out_key, M.out_total / 1024);
        }
        if (!M.out_f) {
            if (M.file_at >= M.nfiles) {
                p2p_send(MT_SONG_END, NULL, 0);
                M.sending = false;
                continue;
            }
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", M.out_dir, M.files[M.file_at]);
            M.out_f = fopen(p, "rb");
            uint8_t b[200];
            uint32_t sz = (uint32_t)M.sizes[M.file_at];
            memcpy(b, &sz, 4);
            int n = snprintf((char *)b + 4, 130, "%s", M.files[M.file_at]) + 1;
            p2p_send(MT_FILE_BEGIN, b, 4 + n);
            if (!M.out_f) {
                p2p_send(MT_FILE_END, NULL, 0);
                M.file_at++;
            }
            continue;
        }
        uint8_t b[CHUNK];
        size_t n = fread(b, 1, sizeof(b), M.out_f);
        if (n > 0) {
            p2p_send(MT_FILE_DATA, b, (int)n);
            M.out_sent += (long)n;
        }
        if (n < sizeof(b)) {
            fclose(M.out_f);
            M.out_f = NULL;
            p2p_send(MT_FILE_END, NULL, 0);
            M.file_at++;
        }
    }
}

// ------------------------------------------------------------------------------------------------ songs coming in
static void song_in(int type, const uint8_t * b, int n)
{
    char songs_dir[300];
    snprintf(songs_dir, sizeof(songs_dir), "%ssongs", plat_data_dir());
    if (type == MT_SONG_BEGIN) {
        const char * key = (const char *)b;
        size_t kl = strnlen(key, (size_t)n);
        if (kl >= (size_t)n || !safe_name(key))
            return;
        const char * name = key + kl + 1;
        size_t nl = strnlen(name, (size_t)n - kl - 1);
        const char * artist = name + nl + 1;
        size_t al = strnlen(artist, (size_t)(n - (int)(artist - key)));
        uint32_t tot = 0;
        if (artist + al + 1 + 4 <= (const char *)b + n)
            memcpy(&tot, artist + al + 1, 4);
        if (M.in_f)
            fclose(M.in_f);
        M.in_f = NULL;
        if (M.receiving)
            dl_remove_tree(M.in_tmp);
        snprintf(M.in_key, sizeof(M.in_key), "%s", key);
        snprintf(M.in_name, sizeof(M.in_name), "%.95s", name);
        snprintf(M.in_tmp, sizeof(M.in_tmp), "%s/.in_%s", songs_dir, key);
        dl_remove_tree(M.in_tmp);
        dl_mkdirs(M.in_tmp);
        M.in_total = (long)tot;
        M.in_got = 0;
        M.receiving = true;
    } else if (type == MT_FILE_BEGIN && M.receiving && n > 4) {
        const char * fname = (const char *)b + 4;
        if (M.in_f)
            fclose(M.in_f);
        M.in_f = NULL;
        if (strnlen(fname, (size_t)n - 4) < (size_t)n - 4 && safe_name(fname)) {
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", M.in_tmp, fname);
            M.in_f = fopen(p, "wb");
        }
    } else if (type == MT_FILE_DATA && M.receiving) {
        if (M.in_f)
            fwrite(b, 1, (size_t)n, M.in_f);
        M.in_got += n;
    } else if (type == MT_FILE_END && M.receiving) {
        if (M.in_f)
            fclose(M.in_f);
        M.in_f = NULL;
    } else if (type == MT_SONG_END && M.receiving) {
        if (M.in_f)
            fclose(M.in_f);
        M.in_f = NULL;
        M.receiving = false;
        // its own folder name, or another beside it when that's taken
        char dst[400];
        snprintf(dst, sizeof(dst), "%s/%s", songs_dir, M.in_key);
        struct stat st;
        for (int k = 2; stat(dst, &st) == 0 && k < 50; k++)
            snprintf(dst, sizeof(dst), "%s/%s (%d)", songs_dir, M.in_key, k);
        if (rename(M.in_tmp, dst) == 0 && songs_add(dst) >= 0) {
            M.got++;
            plat_log("mp: got %s", dst);
            for (int i = 0; i < M.list_n; i++)
                if (!strcmp(M.list[i].key, M.in_key))
                    M.list[i].have = true;
        } else {
            plat_log("mp: couldn't keep %s", M.in_key);
            dl_remove_tree(M.in_tmp);
        }
        if (M.key[0] && !M.my_folder[0]) {
            look_for_pick();
            send_ready();
        }
    } else if (type == MT_SONG_FAIL) {
        plat_log("mp: they don't have %s", (const char *)b);
    }
}

// ------------------------------------------------------------------------------------------------------ the lobby
static void got(int type, const uint8_t * b, int n)
{
    switch (type) {
    case MT_LOBBY: {
        if (n < 3)
            break;
        const char * key = (const char *)b;
        size_t kl = strnlen(key, (size_t)n);
        const char * name = key + kl + 1;
        size_t nl = strnlen(name, (size_t)n - kl - 1);
        const char * artist = name + nl + 1;
        if (artist >= (const char *)b + n)
            break;
        lobby_reset();
        snprintf(M.key, sizeof(M.key), "%.95s", key);
        snprintf(M.name, sizeof(M.name), "%.95s", name);
        snprintf(M.artist, sizeof(M.artist), "%.95s", artist);
        look_for_pick();
        if (!M.my_folder[0] && safe_name(M.key)) {
            mp_get(M.key);              // not here: the host sends it
            M.asked = true;
        }
        send_ready();
        send_pick();
        break;
    }
    case MT_PICK:
        if (n >= 2) {
            M.their_part = b[0];
            M.their_diff = b[1];
        }
        break;
    case MT_READY:
        if (n >= 2) {
            M.they_ready = b[0] != 0;
            M.they_have = b[1] != 0;
        }
        break;
    case MT_START:
        if (n >= 8) {
            double theirs;
            memcpy(&theirs, b, 8);
            // the host's clock's time as this one's
            M.start_at = theirs - (p2p_clock() - plat_now_us() / 1e6);
        }
        break;
    case MT_STATE:
        if (n >= 12) {
            int32_t sc, st;
            memcpy(&sc, b, 4);
            memcpy(&st, b + 4, 4);
            M.theirs.score = sc;
            M.theirs.streak = st;
            M.theirs.mult = b[8];
            M.theirs.pct = b[9];
            M.theirs.rock = b[10] / 255.0f;
            M.theirs.sp = b[11] & 1;
            M.theirs.done = b[11] & 2;
            M.theirs.failed = b[11] & 4;
            M.theirs.at = plat_now_us() / 1e6;
            M.theirs_in = true;
        }
        break;
    case MT_OVER:
        M.me_ready = M.they_ready = false;
        M.start_at = 0;
        break;
    case MT_LIST_REQ:
        M.list_out = 0;
        break;
    case MT_LIST: {
        if (M.list_n >= M.list_cap) {
            int cap = M.list_cap ? M.list_cap * 2 : 256;
            MpSong * l = realloc(M.list, (size_t)cap * sizeof(MpSong));
            if (!l)
                break;
            M.list = l;
            M.list_cap = cap;
        }
        const char * key = (const char *)b;
        size_t kl = strnlen(key, (size_t)n);
        if (kl >= (size_t)n)
            break;
        const char * name = key + kl + 1;
        size_t nl = strnlen(name, (size_t)n - kl - 1);
        const char * artist = name + nl + 1;
        if (artist >= (const char *)b + n)
            break;
        MpSong * e = &M.list[M.list_n++];
        snprintf(e->key, sizeof(e->key), "%.95s", key);
        snprintf(e->name, sizeof(e->name), "%.95s", name);
        snprintf(e->artist, sizeof(e->artist), "%.95s", artist);
        e->size_kb = 0;
        e->have = find_local(e->artist, e->name) >= 0;
        break;
    }
    case MT_LIST_END:
        M.list_done = true;
        break;
    case MT_GET:
        if (n > 0 && M.want_n < 64) {
            char key[96];
            snprintf(key, sizeof(key), "%.*s", n < 95 ? n : 95, (const char *)b);
            for (int i = 0; i < M.want_n; i++)
                if (!strcmp(M.want[(M.want_head + i) % 64], key))
                    return;             // asked for already
            if (M.sending && !strcmp(M.out_key, key))
                return;
            snprintf(M.want[(M.want_head + M.want_n) % 64], 96, "%s", key);
            M.want_n++;
        }
        break;
    default:
        song_in(type, b, n);
    }
}

void mp_poll(void)
{
    if (!p2p_is_open())
        return;
    p2p_set_songs(songs_count());
    p2p_poll();
    int st = p2p_state();
    if (st != M.was) {
        if (st == P2P_CONNECTED || M.was == P2P_CONNECTED)
            link_reset();
        if (st == P2P_CONNECTED) {
            M.my_part = M.my_part < 0 ? 0 : M.my_part;
            send_pick();
        }
        M.was = st;
    }
    if (st != P2P_CONNECTED)
        return;
    uint8_t b[P2P_MAX_MSG];
    int type, n;
    for (int k = 0; k < 800 && (n = p2p_recv(&type, b, sizeof(b))) >= 0; k++)
        got(type, b, n);
    feed_out();
    // the host starts the song when both are ready and both have it: three and a half seconds on, on its clock
    if (p2p_hosting() && M.me_ready && M.they_ready && M.they_have && M.my_folder[0] && M.start_at == 0) {
        double at = plat_now_us() / 1e6 + 3.5;    // on this clock: the other one turns it into its own
        if (p2p_send(MT_START, &at, 8))
            M.start_at = at;
    }
}

void mp_choose(Song * s)
{
    if (!s)
        return;
    lobby_reset();
    base_name(s->folder, M.key, sizeof(M.key));
    snprintf(M.name, sizeof(M.name), "%s", s->name);
    snprintf(M.artist, sizeof(M.artist), "%s", s->artist);
    snprintf(M.my_folder, sizeof(M.my_folder), "%s", s->folder);
    uint8_t b[400];
    int n = snprintf((char *)b, 100, "%s", M.key) + 1;
    n += snprintf((char *)b + n, 100, "%s", M.name) + 1;
    n += snprintf((char *)b + n, 100, "%s", M.artist) + 1;
    p2p_send(MT_LOBBY, b, n);
    send_ready();
}

const char * mp_song_name(void)
{
    return M.name;
}

const char * mp_song_artist(void)
{
    return M.artist;
}

Song * mp_song(void)
{
    if (!M.my_folder[0])
        return NULL;
    int i = songs_find(M.my_folder);
    return i >= 0 ? songs_get(i) : NULL;
}

void mp_pick(int part, int diff)
{
    if (part == M.my_part && diff == M.my_diff)
        return;
    M.my_part = part;
    M.my_diff = diff;
    send_pick();
}

void mp_their_pick(int * part, int * diff)
{
    *part = M.their_part;
    *diff = M.their_diff;
}

void mp_ready(bool on)
{
    if (on && !M.my_folder[0])
        on = false;
    M.me_ready = on;
    send_ready();
}

bool mp_am_ready(void)
{
    return M.me_ready;
}

bool mp_they_ready(void)
{
    return M.they_ready;
}

bool mp_they_have(void)
{
    return M.they_have;
}

double mp_start_at(void)
{
    return M.start_at;
}

void mp_started(void)
{
    M.start_at = -1;                // used: no start again till the next round
    M.theirs_in = false;
}

void mp_send_state(const MpState * s)
{
    uint8_t b[12];
    int32_t sc = s->score, st = s->streak;
    memcpy(b, &sc, 4);
    memcpy(b + 4, &st, 4);
    b[8] = (uint8_t)s->mult;
    b[9] = (uint8_t)(s->pct < 0 ? 0 : s->pct > 100 ? 100 : s->pct);
    b[10] = (uint8_t)(s->rock < 0 ? 0 : s->rock > 1 ? 255 : s->rock * 255);
    b[11] = (uint8_t)(s->sp | s->done << 1 | s->failed << 2);
    if (s->done)
        p2p_send(MT_STATE, b, 12);      // the last word: it mustn't be lost
    else
        p2p_send_fast(MT_STATE, b, 12);
}

const MpState * mp_their_state(void)
{
    return M.theirs_in ? &M.theirs : NULL;
}

void mp_song_over(void)
{
    M.me_ready = false;
    M.start_at = 0;
    p2p_send(MT_OVER, NULL, 0);
    send_ready();
}

void mp_ask_list(void)
{
    M.list_n = 0;
    M.list_done = false;
    p2p_send(MT_LIST_REQ, NULL, 0);
}

int mp_list(MpSong * out, int max)
{
    int n = M.list_n < max ? M.list_n : max;
    if (out && n)
        memcpy(out, M.list, (size_t)n * sizeof(MpSong));
    return M.list_n;
}

bool mp_list_done(void)
{
    return M.list_done;
}

void mp_get(const char * key)
{
    p2p_send(MT_GET, key, (int)strlen(key) + 1);
}

bool mp_transfer(char * in, int in_size, int * in_pct, char * out, int out_size, int * out_pct, int * queued)
{
    in[0] = out[0] = 0;
    *in_pct = *out_pct = 0;
    *queued = M.want_n;
    if (M.receiving) {
        snprintf(in, (size_t)in_size, "%s", M.in_name);
        *in_pct = M.in_total > 0 ? (int)(M.in_got * 100 / M.in_total) : 0;
    }
    if (M.sending) {
        snprintf(out, (size_t)out_size, "%s", M.out_key);
        *out_pct = M.out_total > 0 ? (int)(M.out_sent * 100 / M.out_total) : 0;
    }
    return M.receiving || M.sending;
}

int mp_got(void)
{
    return M.got;
}
