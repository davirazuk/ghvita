// GH Vita Updater: a tiny app of its own, installed once. The game stages an update and starts this, because a running
// game may not write into its own ux0:app directory (the Vita mounts it read-only while the title runs). With the game
// closed, this moves the staged files in and starts the game again. It has to be a separate title, not a second
// executable in the game: only a different title gets a writable path to the game's app directory.
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "update_apply.h"

#define DIR_UP "ux0:data/ghvita/update"
#define GAME "GHVT00001"

static void logf(const char * fmt, ...)
{
    FILE * f = fopen("ux0:data/ghvita/log.txt", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void start_game(void)
{
    int r = sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" GAME);
    if (r < 0)
        logf("updater: the game wouldn't start: %08x", r);
    sceKernelExitProcess(0);
}

int main(void)
{
    // The game writes pending before it hands over. Launched by hand, with no update waiting, just start the game.
    FILE * f = fopen(DIR_UP "/pending", "r");
    if (!f)
        start_game();
    fclose(f);

    logf("updater: starting");
    // The game has already exited by the time the Vita starts us, but make sure: its app directory stays locked while
    // any part of the title runs. The wait gives the system time to let go before the files are moved.
    sceAppMgrDestroyOtherApp();
    sceKernelDelayThread(2 * 1000 * 1000);

    char err[200] = "";
    int n = update_apply(DIR_UP "/new", "ux0:app/" GAME, DIR_UP "/old", err, sizeof(err));
    f = fopen(DIR_UP "/result.txt", "w");
    if (f) {
        if (n >= 0) {
            fprintf(f, "ok %d\n", n);
            remove(DIR_UP "/pending");
        } else
            fprintf(f, "failed %s\n", err);
        fclose(f);
    }
    logf("updater: %s", n >= 0 ? "done" : err);
    start_game();
    return 0;
}
