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
    uint64_t file_pos;
    bool write_failed;
} Sink;

static size_t on_data(char * data, size_t size, size_t n, void * user)
{
    Sink * s = user;
    size_t bytes = size * n;
    if (s->file) {
        if (s->r->filter)
            s->r->filter(s->r->filter_user, s->file_pos, (unsigned char *)data, bytes);
        s->file_pos += bytes;
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

// a song site's link can't reach anything but the web
static void web_only(CURL * c)
{
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
}

static int fetch_once(const HttpReq * r, char ** body, size_t * len, char * err, size_t err_size, bool * again)
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
    Sink sink = {r, NULL, 0, 0, NULL, 0, false};
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
    web_only(c);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "GHVita/1.0 (PS Vita)");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    // rhythmverse (an RSA chain on its own nginx box) and the Vita's mbedTLS ring: enchor's ECDSA handshakes fine
    // from the game yet RV does not, so its requests alone take the old plain roads the server itself offers -
    // TLS 1.2 and RSA-key-exchange suites (no ECDHE curves, no TLS 1.3), which every mbedtls build ships
    if (strstr(r->url, "rhythmverse.co/")) {
        curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(c, CURLOPT_SSL_CIPHER_LIST, "AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA");
    }
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
        *again = !sink.write_failed &&
                 (rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR || rc == CURLE_PARTIAL_FILE || rc == CURLE_GOT_NOTHING ||
                  rc == CURLE_COULDNT_CONNECT || rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_SSL_CONNECT_ERROR);
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

// a host dropping the connection now and then is no reason to give up on a song
int http_fetch(const HttpReq * r, char ** body, size_t * len, char * err, size_t err_size)
{
    for (int attempt = 1;; attempt++) {
        bool again = false;
        int status = fetch_once(r, body, len, err, err_size, &again);
        if (!again || attempt == 3 || (r->cancel && *r->cancel))
            return status;
        plat_log("http: trying again (%d)", attempt);
        if (r->done)
            *r->done = 0;
        plat_sleep_us(attempt * 1000000);
    }
}

bool http_location(const char * url, char * out, size_t size, volatile bool * cancel)
{
    char err[160], ca[300];
    if (!plat_net_up(err, sizeof(err)) || (cancel && *cancel))
        return false;
    CURL * c = curl_easy_init();
    if (!c)
        return false;
    snprintf(ca, sizeof(ca), "%scacert.pem", plat_asset_dir());
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    web_only(c);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "GHVita/1.0 (PS Vita)");
    curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    // the same TLS roads for rhythmverse's short-link hops (net.c's note in fetch_once)
    if (strstr(url, "rhythmverse.co/")) {
        curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(c, CURLOPT_SSL_CIPHER_LIST, "AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA");
    }
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    char * to = NULL;
    bool ok = curl_easy_perform(c) == CURLE_OK && curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &to) == CURLE_OK && to;
    if (ok)
        snprintf(out, size, "%s", to);
    curl_easy_cleanup(c);
    return ok;
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
