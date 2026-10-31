// dl_test DATA_DIR SITE QUERY NAME_PART: finds a song with the real downloader's search, then fetches, unpacks and
// installs it (SITE 0 Chorus, 1 RhythmVerse); or dl_test DATA_DIR url <what to fetch> to fetch an address as it is
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "ext/stb_image.h"
#include "dl.h"
#include "plat.h"

static char data_dir[300];

const char * plat_data_dir(void) { return data_dir; }
const char * plat_asset_dir(void) { return "assets/"; }
bool plat_net_up(char * err, int size) { (void)err; (void)size; return true; }
void plat_sleep_us(int us) { usleep(us); }
int64_t plat_now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
bool audio_song_active(void) { return false; }
void plat_thread(const char * name, int (*fn)(void * arg), void * arg, int core)
{
    pthread_t t;
    pthread_create(&t, NULL, (void * (*)(void *))fn, arg);
    pthread_detach(t);
}
void plat_log(const char * fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}

int main(int c, char ** v)
{
    snprintf(data_dir, sizeof(data_dir), "%s/", v[1]);
    mkdir(v[1], 0777);
    DlHit one;
    memset(&one, 0, sizeof(one));
    if (!strcmp(v[2], "url")) {
        snprintf(one.id, sizeof(one.id), "test:1");
        snprintf(one.name, sizeof(one.name), "test");
        snprintf(one.url, sizeof(one.url), "%s", v[3]);
    } else {
        dl_search(atoi(v[2]), v[3]);
        usleep(200000);
        while (dl_searching())
            usleep(100000);
        char status[200];
        dl_status(status, sizeof(status));
        printf("search: %s\n", status);
        dl_lock();
        bool found = false;
        for (int i = 0; i < dl_count() && !found; i++) {
            DlHit * h = dl_hit(i);
            if (strcasestr(h->name, v[4]) && !h->unplayable[0] && (c < 6 || strstr(h->url, v[5]))) {
                one = *h;
                found = true;
            }
        }
        dl_unlock();
        if (!found) {
            printf("no such hit\n");
            return 2;
        }
        printf("hit: %s - %s (%s) from %s\n", one.artist, one.name, one.id, one.url);
    }
    dl_queue(&one);
    usleep(300000);
    char msg[160] = "";
    int percent = 0;
    JobState st = JOB_QUEUED;
    for (int t = 0; t < 2400; t++) {
        st = dl_job_state(one.id, &percent, msg, sizeof(msg));
        if (st == JOB_DONE || st == JOB_FAILED || st == JOB_NONE)
            break;
        usleep(250000);
    }
    printf("state %d, %d%%, %s\n", (int)st, percent, msg);
    char folder[400];
    while (dl_take_installed(folder, sizeof(folder)))
        printf("installed %s\n", folder);
    return st == JOB_DONE ? 0 : 1;
}
