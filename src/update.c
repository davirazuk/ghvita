// Asks GitHub for the latest release, and if it's newer than this build fetches update-from-<this build>.zip, or update-full.zip
// from further back. Every file is checked against the zip's manifest before anything's touched; the GH Vita Updater app
// (GHVUPD001) then moves them in with this game closed, because a running game can't write its own app directory
// (update_apply.c). The repository is private, so a read-only token lives in update.txt on the card. It only ever goes to
// GitHub's API, and it's never logged.
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
static double checked_at = -1e9;        // when the last check started, in seconds
static volatile bool told_token;        // the token's trouble is said once a run, not every check
#define RECHECK_S (10 * 60)             // a Vita mostly sleeps instead of quitting, so a run can last days
static char auth[260];
static const char * const json_headers[] = {auth, "Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28", NULL};
static const char * const file_headers[] = {auth, "Accept: application/octet-stream", NULL};

// a PC's own release and what's in it: Windows a zip of the game, Linux the AppImage (only when run as one)
#if defined(_WIN32)
#define PC_TAG "windows"
#define PC_ASSET "GHVita-windows.zip"
#elif defined(__linux__) && !defined(__ANDROID__)
#define PC_TAG "linux"
#define PC_ASSET "GHVita-x86_64.AppImage"
#include <sys/stat.h>
#include <unistd.h>
#endif

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

static bool skip_file_token;     // update.txt's token was refused: the built-in one instead

static bool read_token(void)
{
    char path[300];
    snprintf(path, sizeof(path), "%supdate.txt", plat_data_dir());
    FILE * f = skip_file_token ? NULL : fopen(path, "r");
    token[0] = 0;
    if (!f) {
        // none on the card: the one the release build put in the app (the owner's read-only token)
        snprintf(path, sizeof(path), "%supdate_token", plat_asset_dir());
        f = fopen(path, "r");
    }
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
#ifdef PC_TAG
#ifndef _WIN32
    if (!getenv("APPIMAGE")) {
        state = UP_NONE;            // a build run from its folder (development): nothing to update
        return 0;
    }
#endif
    snprintf(url, sizeof(url), "%s/releases/tags/" PC_TAG, api_base());     // the PC's own release
#else
    snprintf(url, sizeof(url), "%s/releases/latest", api_base());
#endif
    char * body = NULL;
    size_t len = 0;
    HttpReq r = {.url = url, .headers = json_headers, .stall_s = 15};
    int status = http_fetch(&r, &body, &len, err, sizeof(err));
    Json * j = status == 200 && body ? json_parse(body, len) : NULL;
    free(body);
    if (!j && (status == 401 || status == 403 || status == 404) && !skip_file_token) {
        // an old token in update.txt (a card or a phone keeps it for ever): try again with the game's own
        char path[300];
        snprintf(path, sizeof(path), "%supdate.txt", plat_data_dir());
        FILE * uf = fopen(path, "r");
        if (uf) {
            fclose(uf);
            skip_file_token = true;
            plat_log("update: update.txt's token got %d, trying the built-in one", status);
            if (read_token()) {
                body = NULL;
                len = 0;
                HttpReq r2 = {.url = url, .headers = json_headers, .stall_s = 15};
                status = http_fetch(&r2, &body, &len, err, sizeof(err));
                j = status == 200 && body ? json_parse(body, len) : NULL;
                free(body);
            }
        }
    }
    if (!j) {
        plat_log("update: the check got %d", status);
        // a fine-grained token expires, and one without this repository reads it as missing (404): say so, or the game just
        // quietly stops updating
        if ((status == 401 || status == 403) && !told_token) {
            told_token = true;
            snprintf(news, sizeof(news), status == 401 ? "Updates: GitHub refused the update token (expired?) [%d]"
                                                       : "Updates: the update token can't read the repository [%d]", status);
        }
        state = UP_NONE;
        return 0;
    }
    char tag[32], want[48], name[64], link[300];
    const char * t = json_str(json_get(j, "tag_name"), "", tag, sizeof(tag));
    int version = t[0] == 'v' ? atoi(t + 1) : 0;
#ifdef PC_TAG
    {
        // the PC's release: its version.txt says the build, PC_ASSET is the whole game
        char vurl[300] = "";
        const Json * assets = json_get(j, "assets");
        for (const Json * a = assets ? assets->child : NULL; a; a = a->next) {
            const char * n = json_str(json_get(a, "name"), "", name, sizeof(name));
            if (!strcmp(n, "version.txt"))
                snprintf(vurl, sizeof(vurl), "%s", json_str(json_get(a, "url"), "", link, sizeof(link)));
            if (!strcmp(n, PC_ASSET)) {
                snprintf(asset_url, sizeof(asset_url), "%s", json_str(json_get(a, "url"), "", link, sizeof(link)));
                offered_size = (int64_t)json_num(json_get(a, "size"), 0);
            }
        }
        json_free(j);
        version = 0;
        if (vurl[0] && asset_url[0]) {
            char * vb = NULL;
            size_t vl = 0;
            HttpReq vr = {.url = vurl, .headers = file_headers, .stall_s = 15};
            if (http_fetch(&vr, &vb, &vl, err, sizeof(err)) == 200 && vb)
                version = atoi(vb);
            free(vb);
        }
        plat_log("update: this is build %d, the PC's latest is %d", GHV_BUILD, version);
        if (version > GHV_BUILD) {
            offered = version;
            state = UP_AVAILABLE;
        } else
            state = UP_NONE;
        return 0;
    }
#endif
    plat_log("update: this is build %d, the latest release is %s", GHV_BUILD, t);
    asset_url[0] = 0;
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

void update_check(bool now)
{
#if defined(__ANDROID__)
    (void)now;
    return;                     // a phone updates through its own app (GHVitaActivity)
#endif
    // at the first menu, then again every RECHECK_S or on waking (now) while nothing's on offer: before, it asked once a
    // launch, and a Vita that only ever sleeps never asked again
    double t = plat_now_us() / 1e6;
    bool again = (state == UP_NONE || state == UP_FAILED) && (now || t - checked_at >= RECHECK_S);
    if ((state != UP_OFF && !again) || (state == UP_OFF && t - checked_at < 5))
        return;
    checked_at = t;                     // also spaces out looking for update.txt when there isn't one
    if (!read_token())
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
        if (status == 404)
            fail("the new build was still being uploaded: try again in a few minutes");
        else if (status == 401 || status == 403)
            fail("GitHub refused the update token (expired?)");
        else
            fail(err[0] ? err : "the download stopped");
        return 0;
    }
    state = UP_UNPACKING;
    char why[160] = "";
#if defined(PC_TAG) && !defined(_WIN32)
    // the AppImage is the whole game: kept as it came, made runnable
    struct stat lst;
    bool kept = stat(zip, &lst) == 0 && lst.st_size > 1000000;
    if (kept) {
        char img[320];
        path_of(img, sizeof(img), "GHVita.AppImage");
        kept = rename(zip, img) == 0;
        chmod(img, 0755);
    } else
        snprintf(why, sizeof(why), "the download came out too small");
    if (!kept) {
        remove(zip);
        fail(why[0] ? why : "couldn't keep the download");
        return 0;
    }
    state = UP_READY;
    return 0;
#endif
#ifdef _WIN32
    char exe[400];
    snprintf(exe, sizeof(exe), "%s/GHVita/GHVita.exe", dir);
    FILE * ef = NULL;
    bool ok = dl_extract_archive(zip, dir, why, sizeof(why)) && (ef = fopen(exe, "rb")) != NULL;
    if (ef)
        fclose(ef);
    else if (!why[0])
        snprintf(why, sizeof(why), "the download has no GHVita.exe");
#else
    bool ok = dl_extract_archive(zip, dir, why, sizeof(why)) && verify(dir, why, sizeof(why));
#endif
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

#ifdef _WIN32
#include <dirent.h>
#include <windows.h>
#include <sys/stat.h>
// the new files over the game's own (it's running: Windows lets a running exe or DLL be renamed, not overwritten, so
// each old file steps aside as name.old, cleared at the next start); data/ (songs, saves) is left alone
static int win_move_tree(const char * from, const char * to, char * why, size_t size)
{
    DIR * d = opendir(from);
    if (!d)
        return 0;
    int n = 0;
    struct dirent * e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char src[700], dst[700], aside[720];
        snprintf(src, sizeof(src), "%s/%s", from, e->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", to, e->d_name);
        struct stat st;
        if (stat(src, &st))
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (!strcmp(to, ".") && !strcmp(e->d_name, "data"))
                continue;
            _mkdir(dst);
            int k = win_move_tree(src, dst, why, size);
            if (k < 0) {
                closedir(d);
                return k;
            }
            n += k;
            continue;
        }
        snprintf(aside, sizeof(aside), "%s.old", dst);
        DeleteFileA(aside);
        MoveFileExA(dst, aside, MOVEFILE_REPLACE_EXISTING);
        if (!MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            snprintf(why, size, "couldn't put %s in place", e->d_name);
            MoveFileExA(aside, dst, MOVEFILE_REPLACE_EXISTING);
            closedir(d);
            return -1;
        }
        n++;
    }
    closedir(d);
    return n;
}

// the files a last update set aside
static void win_clear_old(const char * dir)
{
    DIR * d = opendir(dir);
    if (!d)
        return;
    struct dirent * e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (!strcmp(dir, ".") && !strcmp(e->d_name, "data")))
            continue;
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st))
            continue;
        size_t l = strlen(e->d_name);
        if (S_ISDIR(st.st_mode))
            win_clear_old(p);
        else if (l > 4 && !strcmp(e->d_name + l - 4, ".old"))
            DeleteFileA(p);
    }
    closedir(d);
}
#endif

void update_install(void)
{
#if defined(PC_TAG) && !defined(_WIN32)
    {
        // the new AppImage in the old one's place (a running file can be replaced on Linux), then started instead
        const char * me = getenv("APPIMAGE");
        char img[320], res[320], tmp[600];
        path_of(img, sizeof(img), "GHVita.AppImage");
        path_of(res, sizeof(res), "result.txt");
        bool ok = me && me[0];
        if (ok) {
            snprintf(tmp, sizeof(tmp), "%s.new", me);
            if (rename(img, me) != 0) {
                // another drive: copied beside it, then swapped
                FILE * in = fopen(img, "rb"), * out = in ? fopen(tmp, "wb") : NULL;
                static char buf[1 << 16];
                size_t n;
                ok = in && out;
                while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
                    ok = fwrite(buf, 1, n, out) == n;
                if (in)
                    fclose(in);
                if (out)
                    ok = fclose(out) == 0 && ok;
                ok = ok && rename(tmp, me) == 0;
                remove(img);
            }
            chmod(me, 0755);
        }
        FILE * rf = fopen(res, "w");
        if (rf) {
            fprintf(rf, ok ? "ok 1\n" : "failed couldn't replace the AppImage\n");
            fclose(rf);
        }
        if (!ok) {
            fail("couldn't replace the AppImage");
            return;
        }
        execl(me, me, (char *)NULL);
        exit(0);
    }
#endif
#ifdef _WIN32
    {
        char from[320], why[200] = "", res[320];
        path_of(from, sizeof(from), "new/GHVita");
        int n = win_move_tree(from, ".", why, sizeof(why));
        path_of(res, sizeof(res), "result.txt");
        FILE * rf = fopen(res, "w");
        if (rf) {
            if (n > 0)
                fprintf(rf, "ok %d\n", n);
            else
                fprintf(rf, "failed %s\n", why[0] ? why : "nothing to put in");
            fclose(rf);
        }
        if (n <= 0) {
            fail(why[0] ? why : "the update had nothing in it");
            return;
        }
        // the new game, started in this one's place
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        char cmd[] = "GHVita.exe";
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        exit(0);
    }
#endif

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
    plat_log("update: handing over for build %d", offered);
    plat_run_updater(dir, app, old);
    remove(flag);
    fail("the GH Vita Updater app isn't installed");
}

void update_boot(void)
{
#if defined(__ANDROID__)
    return;
#endif
#ifdef PC_TAG
    {
#ifdef _WIN32
        win_clear_old(".");
#endif
        char res[320], line[200] = "", nd[320];
        path_of(nd, sizeof(nd), "new");
        wipe(nd);
        path_of(res, sizeof(res), "result.txt");
        FILE * rf = fopen(res, "r");
        if (rf) {
            if (!fgets(line, sizeof(line), rf))
                line[0] = 0;
            fclose(rf);
            remove(res);
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "ok", 2))
                snprintf(news, sizeof(news), "Updated to build %d", GHV_BUILD);
            else
                snprintf(news, sizeof(news), "The update didn't go in: %s", line);
        }
        return;
    }
#endif
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
    static char said[160];
    if (!news[0] || !strcmp(news, said))
        return NULL;
    snprintf(said, sizeof(said), "%s", news);
    return said;
}

UpState update_state(void) { return state; }
int update_version(void) { return offered; }
int64_t update_size(void) { return offered_size; }
const char * update_error(void) { return error; }
void update_progress(int64_t * done, int64_t * all) { *done = got; *all = total; }
