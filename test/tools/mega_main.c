// mega_test <link> <outdir>: the Mega code alone, and the FIPS-197 check of its AES through a known file key
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dl.h"
#include "mega.h"
#include "plat.h"

const char * plat_asset_dir(void) { return "assets/"; }
bool plat_net_up(char * err, int size) { (void)err; (void)size; return true; }
void plat_sleep_us(int us) { usleep(us); }
void plat_log(const char * fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}
void dl_mkdirs(const char * path)
{
    char p[700];
    snprintf(p, sizeof(p), "%s", path);
    for (char * s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0777);
            *s = '/';
        }
    mkdir(p, 0777);
}
void dl_safe_part(char * s)
{
    for (; *s; s++)
        if (strchr(":*?\"<>|\\", *s) || (unsigned char)*s < 0x20)
            *s = '_';
}
static int wanted(const char * name) { return strstr(name, ".mp4") ? 0 : 2; }
static void on_file(void * user, const char * name) { printf("file %s\n", name); }

int main(int c, char ** v)
{
    static volatile bool cancel;
    static volatile int64_t done, total;
    char file[400], dir[400], err[200] = "";
    bool folder = false;
    snprintf(file, sizeof(file), "%s/file.bin", v[2]);
    snprintf(dir, sizeof(dir), "%s/folder", v[2]);
    dl_mkdirs(v[2]);
    MegaJob j = {&cancel, &done, &total, wanted, on_file, NULL};
    printf("link ok: %d\n", mega_link_ok(v[1]));
    bool ok = mega_fetch(v[1], file, dir, &j, &folder, err, sizeof(err));
    printf("%s folder=%d %lld bytes: %s\n", ok ? "OK" : "FAILED", folder, (long long)total, err);
    return !ok;
}
