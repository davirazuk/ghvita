// Asks GitHub for the latest release, and if it's newer than this build fetches update-from-<this build>.zip, or update-full.zip
// from further back. Every file is checked against the zip's manifest before anything's touched; updater.bin then moves
// them in with the game closed (update_apply.c). The repository is private, so a read-only token lives in update.txt on the
// card. It only ever goes to GitHub's API, and it's never logged.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "dl.h"
#include "json.h"
#include "net.h"
#include "plat.h"
#include "update.h"
#include "update_apply.h"

#ifndef GHV_BUILD
#define GHV_BUILD 0
#endif
#define API "https://api.github.com/repos/davirazuk/ghvita"

static volatile UpState state = UP_OFF;
static volatile bool cancelling;
static volatile int64_t got, total;
static int offered;
static int64_t offered_size;
static char asset_url[300], error[160], token[200], news[160];
static char auth[260];
static const char * const json_headers[] = {auth, "Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28", NULL};
static const char * const file_headers[] = {auth, "Accept: application/octet-stream", NULL};

static const char * api_base(void)
{
#ifndef __vita__
    if (getenv("GHV_UPDATE_API"))
        return getenv("GHV_UPDATE_API");
#endif
    return API;
}

static void path_of(char * out, size_t size, const char * name)
{
    snprintf(out, size, "%supdate/%s", plat_data_dir(), name);
}

static void fail(const char * why)
{
    snprintf(error, sizeof(error), "%s", why);
    plat_log("update: %s", why);
    state = UP_FAILED;
}

static bool read_token(void)
{
    char path[300];
    snprintf(path, sizeof(path), "%supdate.txt", plat_data_dir());
    FILE * f = fopen(path, "r");
    if (f) {
        if (!fgets(token, sizeof(token), f))
            token[0] = 0;
        fclose(f);
    }
#ifndef __vita__
    if (getenv("GHV_UPDATE_TOKEN"))
        snprintf(token, sizeof(token), "%s", getenv("GHV_UPDATE_TOKEN"));
#endif
    token[strcspn(token, " \t\r\n")] = 0;
    if (!token[0])
        return false;
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    return true;
}

static int check_thread(void * arg)
{
    (void)arg;
    char err[160] = "", url[300];
    if (!plat_net_up(err, sizeof(err))) {
        state = UP_NONE;                // no Wi-Fi is no news, not an error
        return 0;
    }
    snprintf(url, sizeof(url), "%s/releases/latest", api_base());
    char * body = NULL;
    size_t len = 0;
    HttpReq r = {.url = url, .headers = json_headers, .stall_s = 15};
    int status = http_fetch(&r, &body, &len, err, sizeof(err));
    Json * j = status == 200 && body ? json_parse(body, len) : NULL;
    free(body);
    if (!j) {
        plat_log("update: the check got %d", status);
        state = UP_NONE;
        return 0;
    }
    char tag[32], want[48], name[64], link[300];
    const char * t = json_str(json_get(j, "tag_name"), "", tag, sizeof(tag));
    int version = t[0] == 'v' ? atoi(t + 1) : 0;
    plat_log("update: this is build %d, the latest release is %s", GHV_BUILD, t);
    if (version > GHV_BUILD) {
        snprintf(want, sizeof(want), "update-from-%d.zip", GHV_BUILD);
        const Json * assets = json_get(j, "assets");
        for (const Json * a = assets ? assets->child : NULL; a; a = a->next) {
            const char * n = json_str(json_get(a, "name"), "", name, sizeof(name));
            bool exact = !strcmp(n, want);
            if (exact || (!strcmp(n, "update-full.zip") && !asset_url[0])) {
                snprintf(asset_url, sizeof(asset_url), "%s", json_str(json_get(a, "url"), "", link, sizeof(link)));
                offered_size = (int64_t)json_num(json_get(a, "size"), 0);
            }
            if (exact)
                break;
        }
        if (asset_url[0]) {
            offered = version;
            state = UP_AVAILABLE;
        } else
            state = UP_NONE;
    } else
        state = UP_NONE;
    json_free(j);
    return 0;
}

void update_check(void)
{
    if (state != UP_OFF || !read_token())
        return;
    state = UP_CHECKING;
    plat_thread("update", check_thread, NULL, PLAT_BACKGROUND);
}

// every file there, at its size, with its CRC
static bool verify(const char * dir, char * why, size_t size)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/manifest.txt", dir);
    FILE * f = fopen(path, "r");
    if (!f) {
        snprintf(why, size, "the update has no list of files");
        return false;
    }
    static unsigned char buf[64 * 1024];
    char line[400];
    int files = 0;
    bool ok = true;
    while (ok && fgets(line, sizeof(line), f)) {
        long long want;
        unsigned crc;
        int at = 0;
        if (sscanf(line, "%lld %x %n", &want, &crc, &at) < 2 || !at)
            continue;
        line[strcspn(line, "\r\n")] = 0;
        const char * name = line + at;
        if (!update_path_ok(name)) {
            snprintf(why, size, "the update names a file it shouldn't");
            ok = false;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        FILE * in = fopen(path, "rb");
        if (!in) {
            snprintf(why, size, "the update is missing %s", name);
            ok = false;
            break;
        }
        uLong c = crc32(0, NULL, 0);
        long long have = 0;
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            c = crc32(c, buf, (uInt)n);
            have += n;
        }
        fclose(in);
        if (have != want || (unsigned)c != crc) {
            snprintf(why, size, "%s arrived damaged", name);
            ok = false;
        }
        files++;
    }
    fclose(f);
    if (ok && !files) {
        snprintf(why, size, "the update is empty");
        ok = false;
    }
    return ok;
}

static void wipe(const char * dir)
{
    dl_remove_tree(dir);
}

static int get_thread(void * arg)
{
    (void)arg;
    char zip[300], dir[300], old[300], err[160] = "";
    path_of(dir, sizeof(dir), "");
    update_mkdirs(dir);
    path_of(zip, sizeof(zip), "update.zip");
    path_of(dir, sizeof(dir), "new");
    path_of(old, sizeof(old), "old");
    wipe(dir);
    HttpReq r = {.url = asset_url, .headers = file_headers, .to_file = zip, .cancel = &cancelling, .done = &got, .total = &total, .stall_s = 30};
    int status = http_fetch(&r, NULL, NULL, err, sizeof(err));
    if (cancelling) {
        remove(zip);
        state = UP_AVAILABLE;
        return 0;
    }
    if (status != 200) {
        remove(zip);
        if (status == 401 || status == 403 || status == 404)
            fail("GitHub refused: check the token in update.txt");
        else
            fail(err[0] ? err : "the download stopped");
        return 0;
    }
    state = UP_UNPACKING;
    char why[160] = "";
    bool ok = dl_extract_archive(zip, dir, why, sizeof(why)) && verify(dir, why, sizeof(why));
    remove(zip);
    if (!ok) {
        wipe(dir);
        fail(why[0] ? why : "the update wouldn't unpack");
        return 0;
    }
    wipe(old);
    state = UP_READY;
    return 0;
}

void update_get(void)
{
    if (state != UP_AVAILABLE && state != UP_FAILED)
        return;
    cancelling = false;
    got = total = 0;
    error[0] = 0;
    state = UP_DOWNLOADING;
    plat_thread("update", get_thread, NULL, PLAT_BACKGROUND);
}

void update_cancel(void)
{
    cancelling = true;
}

static void app_dir(char * out, size_t size)
{
    snprintf(out, size, "%s", plat_app_dir());
    size_t n = strlen(out);
    if (n && out[n - 1] == '/')
        out[n - 1] = 0;
}

void update_install(void)
{
    char dir[300], old[300], app[300], flag[300];
    path_of(dir, sizeof(dir), "new");
    path_of(old, sizeof(old), "old");
    path_of(flag, sizeof(flag), "pending");
    app_dir(app, sizeof(app));
    FILE * f = fopen(flag, "w");
    if (f) {
        fprintf(f, "%d\n", offered);
        fclose(f);
    }
    update_apply_one(dir, app, old, "updater.bin");
    plat_log("update: handing over for build %d", offered);
    plat_run_updater(dir, app, old);
    remove(flag);
    fail("the updater wouldn't start");
}

void update_boot(void)
{
    char flag[300], path[300], line[200] = "";
    path_of(flag, sizeof(flag), "pending");
    FILE * f = fopen(flag, "r");
    if (f) {                            // power went while the updater ran
        bool tried = fgets(line, sizeof(line), f) && line[0] == 'r';
        fclose(f);
        if (tried) {
            remove(flag);
            snprintf(news, sizeof(news), "The update didn't finish");
            plat_log("update: %s", news);
        } else if ((f = fopen(flag, "w"))) {
            fputs("r\n", f);
            fclose(f);
            char dir[300], old[300], app[300];
            path_of(dir, sizeof(dir), "new");
            path_of(old, sizeof(old), "old");
            app_dir(app, sizeof(app));
            plat_run_updater(dir, app, old);
            remove(flag);
        }
    }
    path_of(path, sizeof(path), "result.txt");
    if ((f = fopen(path, "r"))) {
        if (!fgets(line, sizeof(line), f))
            line[0] = 0;
        fclose(f);
        remove(path);
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "ok", 2))
            snprintf(news, sizeof(news), "Updated to build %d", GHV_BUILD);
        else
            snprintf(news, sizeof(news), "The update didn't go in: %s", line);
        plat_log("update: %s", news);
    }
}

const char * update_news(void)
{
    static bool told;
    if (told || !news[0])
        return NULL;
    told = true;
    return news;
}

UpState update_state(void) { return state; }
int update_version(void) { return offered; }
int64_t update_size(void) { return offered_size; }
const char * update_error(void) { return error; }
void update_progress(int64_t * done, int64_t * all) { *done = got; *all = total; }
