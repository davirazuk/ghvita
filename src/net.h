// HTTPS for the song downloader (libcurl; certificates from the app's own cacert.pem)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char * url;
    const char * post;                  // a POST body (NULL: GET)
    const char * const * headers;       // "Name: value" lines, NULL-terminated (or NULL)
    const char * to_file;               // the body goes into this file rather than memory
    volatile bool * cancel;             // set to stop
    volatile int64_t * done, * total;   // progress in bytes (either may be NULL; total 0 when unknown)
    int stall_s;                        // give up after this long without data (default 30)
    // runs over a file's bytes as they arrive, before they're written; a retry starts again from 0
    void (*filter)(void * user, uint64_t offset, unsigned char * data, size_t n);
    void * filter_user;
} HttpReq;

// the HTTP status (0 when there was no answer, and err says why); a body in memory is NUL-terminated (free() it)
int http_fetch(const HttpReq * r, char ** body, size_t * len, char * err, size_t err_size);
// where a short link leads, without going there: true with the next address in out
bool http_location(const char * url, char * out, size_t size, volatile bool * cancel);
// "a b&c" -> "a%20b%26c"
void url_escape(char * out, size_t size, const char * in);
