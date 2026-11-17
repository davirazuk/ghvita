// no game code in here: the GH Vita Updater app is this and a main
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "update_apply.h"

#define MAX_FILES 4096      // was 1024: an update with more stopped short, the program itself never moved in

void update_mkdirs(const char * dir)
{
    char p[512];
    snprintf(p, sizeof(p), "%s", dir);
    for (char * c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0777);
            *c = '/';
        }
    mkdir(p, 0777);
}

static void make_parent(const char * path)
{
    char p[512];
    snprintf(p, sizeof(p), "%s", path);
    char * slash = strrchr(p, '/');
    if (slash) {
        *slash = 0;
        update_mkdirs(p);
    }
}

static bool exists(const char * path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

bool update_path_ok(const char * path)
{
    if (!path[0] || path[0] == '/' || strstr(path, "..") || strchr(path, ':') || strchr(path, '\\'))
        return false;
    return !strcmp(path, "eboot.bin") || !strcmp(path, "updater.bin") || !strncmp(path, "assets/", 7);
}

// the old file aside, the new one in. A restart finds the new one gone and leaves it be
static bool move_in(const char * staged, const char * app, const char * old, const char * path)
{
    char from[512], to[512], aside[512];
    snprintf(from, sizeof(from), "%s/%s", staged, path);
    snprintf(to, sizeof(to), "%s/%s", app, path);
    snprintf(aside, sizeof(aside), "%s/%s", old, path);
    if (!exists(from))
        return exists(to);
    make_parent(to);
    if (exists(to)) {
        make_parent(aside);
        remove(aside);
        if (rename(to, aside) != 0)
            return false;
    }
    if (rename(from, to) != 0) {
        rename(aside, to);
        return false;
    }
    return true;
}

bool update_apply_one(const char * staged, const char * app, const char * old, const char * path)
{
    return move_in(staged, app, old, path);
}

static void put_back(const char * app, const char * old, const char * path)
{
    char to[512], aside[512];
    snprintf(to, sizeof(to), "%s/%s", app, path);
    snprintf(aside, sizeof(aside), "%s/%s", old, path);
    if (exists(aside)) {
        remove(to);
        rename(aside, to);
    }
}

int update_apply(const char * staged, const char * app, const char * old, char * err, size_t err_size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/manifest.txt", staged);
    FILE * f = fopen(path, "r");
    if (!f) {
        snprintf(err, err_size, "no manifest");
        return -1;
    }
    static char names[MAX_FILES][200];
    int n = 0;
    char line[400];
    while (fgets(line, sizeof(line), f) && n < MAX_FILES) {
        long long size;
        unsigned crc;
        int at = 0;
        if (sscanf(line, "%lld %x %n", &size, &crc, &at) < 2 || !at)
            continue;
        line[strcspn(line, "\r\n")] = 0;
        if (!update_path_ok(line + at) || strlen(line + at) >= sizeof(names[0])) {
            fclose(f);
            snprintf(err, err_size, "bad path in the manifest");
            return -1;
        }
        snprintf(names[n++], sizeof(names[0]), "%s", line + at);
    }
    fclose(f);
    // the program goes last: until it's in, the old one still starts
    int done = 0;
    bool ok = true;
    for (int pass = 0; pass < 2 && ok; pass++)
        for (int i = 0; i < n && ok; i++) {
            if ((strcmp(names[i], "eboot.bin") == 0) != (pass == 1))
                continue;
            if (move_in(staged, app, old, names[i]))
                done++;
            else {
                ok = false;
                snprintf(err, err_size, "can't replace %s (%s)", names[i], strerror(errno));
            }
        }
    if (!ok) {
        for (int i = 0; i < n; i++)
            put_back(app, old, names[i]);
        return -1;
    }
    return done;
}
