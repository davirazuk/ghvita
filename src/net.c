#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "net.h"
#include "plat.h"

typedef struct {
    const HttpReq * r;
    char * mem;
    size_t len, cap;
    FILE * file;
    bool write_failed;
} Sink;

static size_t on_data(char * data, size_t size, size_t n, void * user)
{
    Sink * s = user;
    size_t bytes = size * n;
    if (s->file) {
        if (fwrite(data, 1, bytes, s->file) != bytes) {
            s->write_failed = true;
            return 0;
        }
    } else {
        if (s->len + bytes + 1 > s->cap) {
            s->cap = (s->len + bytes + 1) * 2;
            char * m = realloc(s->mem, s->cap);
            if (!m)
                return 0;
            s->mem = m;
        }
        memcpy(s->mem + s->len, data, bytes);
        s->len += bytes;
        s->mem[s->len] = 0;
    }
    return bytes;
}

static int on_progress(void * user, curl_off_t total, curl_off_t done, curl_off_t ut, curl_off_t ud)
{
    (void)ut;
    (void)ud;
    const HttpReq * r = ((Sink *)user)->r;
    if (r->done)
        *r->done = done;
    if (r->total)
        *r->total = total;
    return r->cancel && *r->cancel;         // non-zero stops the transfer
}

int http_fetch(const HttpReq * r, char ** body, size_t * len, char * err, size_t err_size)
{
    static bool global;
    if (!global) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        global = true;
    }
    if (body)
        *body = NULL;
    if (len)
        *len = 0;
    if (!plat_net_up(err, err_size))
        return 0;
    CURL * c = curl_easy_init();
    if (!c) {
        snprintf(err, err_size, "no network library");
        return 0;
    }
    Sink sink = {r, NULL, 0, 0, NULL, false};
    if (r->to_file && !(sink.file = fopen(r->to_file, "wb"))) {
        snprintf(err, err_size, "can't write to the memory card");
        curl_easy_cleanup(c);
        return 0;
    }
    char ca[300];
    snprintf(ca, sizeof(ca), "%scacert.pem", plat_asset_dir());
    char curl_err[CURL_ERROR_SIZE] = "";
    struct curl_slist * hl = NULL;
    for (int i = 0; r->headers && r->headers[i]; i++)
        hl = curl_slist_append(hl, r->headers[i]);
    curl_easy_setopt(c, CURLOPT_URL, r->url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "GHVita/1.0 (PS Vita)");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)(r->stall_s > 0 ? r->stall_s : 30));
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, curl_err);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (hl)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (r->post)
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->post);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    curl_slist_free_all(hl);
    if (sink.file && fclose(sink.file) != 0)
        sink.write_failed = true;
    if (rc != CURLE_OK || sink.write_failed) {
        if (sink.write_failed)
            snprintf(err, err_size, "the memory card is full");
        else if (rc == CURLE_ABORTED_BY_CALLBACK)
            snprintf(err, err_size, "cancelled");
        else
            snprintf(err, err_size, "%s", curl_err[0] ? curl_err : curl_easy_strerror(rc));
        plat_log("http: %s: %s", r->url, err);
        free(sink.mem);
        if (r->to_file)
            remove(r->to_file);
        return 0;
    }
    if (body)
        *body = sink.mem;
    else
        free(sink.mem);
    if (len)
        *len = sink.len;
    return (int)status;
}

void url_escape(char * out, size_t size, const char * in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for (const unsigned char * p = (const unsigned char *)in; *p && k + 4 < size; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p))
            out[k++] = (char)*p;
        else {
            out[k++] = '%';
            out[k++] = hex[*p >> 4];
            out[k++] = hex[*p & 15];
        }
    }
    out[k] = 0;
}
