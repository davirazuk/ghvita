// Solid RAR 3/4 archives, the one kind libarchive won't unpack: each file is packed on the ones before it, so they're
// decoded in order, from the first, with the state carried across
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// open_out gives the file an entry's data goes to (the callback's to close, after), or NULL to leave it out; it's called
// for every file in turn. False: err says why
bool rar_extract(const char * file, FILE * (*open_out)(void * user, const char * name),
                 void (*close_out)(void * user, FILE * f), void * user, char * err, size_t err_size);
