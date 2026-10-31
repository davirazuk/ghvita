// Mega's public links: the key is in the link, after the #, so a file is fetched and unlocked here as it arrives
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    volatile bool * cancel;
    volatile int64_t * done, * total;           // of the file on its way
    int (*wanted)(const char * name);           // a folder's files: 2 needed, 1 nice to have, 0 left out
    void (*on_file)(void * user, const char * name);    // a folder's file is starting
    void * user;
} MegaJob;

// the link names a file or folder and has its key
bool mega_link_ok(const char * url);
// a file link: the file comes as `file`; a folder link: *folder is set and its files come under `dir`, in their folders
bool mega_fetch(const char * url, const char * file, const char * dir, const MegaJob * job, bool * folder, char * err,
                size_t err_size);
