// rar_test <archive> <outdir>: the RAR code alone
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "rar.h"

static const char * dir;

static FILE * open_out(void * user, const char * name)
{
    char path[900];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    for (char * p = path + strlen(dir) + 1; *p; p++)
        if (*p == '\\' || *p == '/') {
            *p = 0;
            mkdir(path, 0777);
            *p = '/';
        }
    return fopen(path, "wb");
}

static void close_out(void * user, FILE * f)
{
    fclose(f);
}

int main(int c, char ** v)
{
    char err[200] = "";
    dir = v[2];
    mkdir(dir, 0777);
    bool ok = rar_extract(v[1], open_out, close_out, NULL, err, sizeof(err));
    printf("%s %s\n", ok ? "OK" : "FAILED", err);
    return !ok;
}
