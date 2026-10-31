// updater.bin: the game hands over to this, it moves the staged update into ux0:app with the game closed, and hands back.
// Whatever happens here the game must still start, so every way out is the same one.
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#include "update_apply.h"

#define DIR_UP "ux0:data/ghvita/update"

int main(void)
{
    char err[200] = "";
    int n = update_apply(DIR_UP "/new", "ux0:app/GHVT00001", DIR_UP "/old", err, sizeof(err));
    FILE * log = fopen(DIR_UP "/result.txt", "w");
    if (log) {
        if (n >= 0)
            fprintf(log, "ok %d\n", n);
        else
            fprintf(log, "failed %s\n", err);
        fclose(log);
    }
    remove(DIR_UP "/pending");
    sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    sceKernelExitProcess(0);
    return 0;
}
