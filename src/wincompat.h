// Windows (MinGW): the few POSIX calls the game uses that Windows spells differently. Force-included by CMake on
// Windows only, after the system's own headers for them so their declarations are already in
#ifndef GHV_WINCOMPAT_H
#define GHV_WINCOMPAT_H
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define mkdir(p, m) _mkdir(p)
#endif
#endif
