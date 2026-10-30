// PC test tool: converts Clone Hero songs (folders or .sng) the way the Vita does (src/songconv.c)
// usage: ghvconv OUT_DIR SOURCE... (a SOURCE "inplace:<folder>" converts that folder where it is)
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "ext/stb_image.h"
#include "songconv.h"
#include "plat.h"

void plat_log(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int main(int argc, char ** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ghvconv OUT_DIR SOURCE...\n");
        return 2;
    }
    mkdir(argv[1], 0777);
    int bad = 0;
    for (int i = 2; i < argc; i++) {
        char out[800], err[200];
        ConvOptions opt = {0};
        const char * src = argv[i];
        if (strncmp(src, "inplace:", 8) == 0) {
            src += 8;
            opt.in_place = true;
        }
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        bool ok = songconv_install(src, argv[1], &opt, out, sizeof(out), err, sizeof(err));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (ok)
            printf("ok %.0f ms: %s -> %s\n", ms, src, out);
        else {
            printf("FAILED: %s: %s\n", src, err);
            bad++;
        }
    }
    return bad != 0;
}
