// Runs the game's CON unpacker on a desktop, without the Opus shrinking.
//   conunpack <package.con> <dest dir>
#include <stdio.h>
#include <string.h>
#include "../src/rbcon.c"

bool shrink_mogg(const char * folder)
{
    (void)folder;
    return true;
}

void shrink_song(const char * folder)
{
    (void)folder;
}

int main(int argc, char ** argv)
{
    if (argc < 3)
        return 2;
    char err[512] = "";
    int n = rbcon_unpack(argv[1], argv[2], err, sizeof(err));
    printf("songs=%d err=%s\n", n, err);
    return n > 0 ? 0 : 1;
}
