#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dl.h"
#include "json.h"
#include "mega.h"
#include "net.h"
#include "plat.h"
#include "aes128.h"

typedef Aes128 Aes;
#define aes_key aes128_key
#define aes_encrypt aes128_encrypt
#define aes_decrypt aes128_decrypt

static void put32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// AES lives in aes128.c, shared with the mogg decryptor

// ------------------------------------------------------------------------------------------------------------ the link

typedef struct {
    char id[16], inside[16];        // the file or folder, and what a folder link points at within it
    uint8_t key[32];
    bool folder, inside_file;
} Link;

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '-' || c == '+' ? 62 : c == '_' || c == '/' ? 63 : -1;
}

// Mega's base64: - and _, no padding
static size_t b64_decode(const char * in, size_t in_len, uint8_t * out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        int v = b64_value(in[i]);
        if (v < 0)
            break;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap)
                out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

// /file/ID#KEY, #!ID!KEY, /folder/ID#KEY with /file/X or /folder/X after it, #F!ID!KEY with !X (a file) or ?X (a folder)
static bool parse_link(const char * url, Link * l)
{
    char u[300], key[64] = "", rest[64] = "";
    size_t k = 0;
    memset(l, 0, sizeof(*l));
    for (const char * p = url; *p && k + 1 < sizeof(u); p++) {
        if (p[0] == '%' && p[1] == '2' && (p[2] == '3' || p[2] == '1')) {
            u[k++] = p[2] == '3' ? '#' : '!';
            p += 2;
        } else if (!strncmp(p, "&#33;", 5)) {
            u[k++] = '!';
            p += 4;
        } else
            u[k++] = *p;
    }
    u[k] = 0;
    const char * p;
    if ((p = strstr(u, "/folder/"))) {
        l->folder = true;
        sscanf(p + 8, "%15[A-Za-z0-9_-]#%63[A-Za-z0-9_-]%63s", l->id, key, rest);
        if (!strncmp(rest, "/file/", 6) || !strncmp(rest, "/folder/", 8)) {
            l->inside_file = rest[2] == 'i';
            sscanf(rest + (l->inside_file ? 6 : 8), "%15[A-Za-z0-9_-]", l->inside);
        }
    } else if ((p = strstr(u, "/file/")))
        sscanf(p + 6, "%15[A-Za-z0-9_-]#%63[A-Za-z0-9_-]", l->id, key);
    else if ((p = strstr(u, "#F!"))) {
        l->folder = true;
        sscanf(p + 3, "%15[A-Za-z0-9_-]!%63[A-Za-z0-9_-]%63s", l->id, key, rest);
        if (rest[0] == '!' || rest[0] == '?') {
            l->inside_file = rest[0] == '!';
            sscanf(rest + 1, "%15[A-Za-z0-9_-]", l->inside);
        }
    } else if ((p = strstr(u, "#!")))
        sscanf(p + 2, "%15[A-Za-z0-9_-]!%63[A-Za-z0-9_-]", l->id, key);
    size_t got = b64_decode(key, strlen(key), l->key, sizeof(l->key));
    return l->id[0] && got == (l->folder ? 16 : 32);
}

bool mega_link_ok(const char * url)
{
    Link l;
    return parse_link(url, &l);
}

// ------------------------------------------------------------------------------------------------------------- the API

static void say_error(int code, char * err, size_t es)
{
    switch (code) {
    case -9: snprintf(err, es, "it's gone from Mega"); break;
    case -11: snprintf(err, es, "Mega won't give it: private or taken down"); break;
    case -16: snprintf(err, es, "Mega has taken this file down"); break;
    case -4:
    case -6: snprintf(err, es, "Mega is limiting downloads of this: try again later"); break;
    case -17: snprintf(err, es, "Mega's free download limit is used up: try again later"); break;
    case -18: snprintf(err, es, "Mega is busy: try again later"); break;
    default: snprintf(err, es, "Mega said no (%d)", code); break;
    }
}

// the first item of Mega's answer, the whole of it kept in *root (json_free it); it says -3 when it wants another go
static const Json * api(const char * node, const char * cmd, const MegaJob * job, Json ** root, char * err, size_t es)
{
    static unsigned seq;
    const char * headers[] = {"Content-Type: application/json", NULL};
    *root = NULL;
    for (int attempt = 1; attempt <= 6; attempt++) {
        char url[120], * body = NULL;
        size_t len = 0;
        snprintf(url, sizeof(url), "https://g.api.mega.co.nz/cs?id=%u%s%s", ++seq, node ? "&n=" : "", node ? node : "");
        HttpReq r = {.url = url, .post = cmd, .headers = headers, .cancel = job->cancel, .stall_s = 30};
        int status = http_fetch(&r, &body, &len, err, es);
        if (status != 200 || !body) {
            free(body);
            if (status)
                snprintf(err, es, "Mega answered %d", status);
            return NULL;
        }
        Json * j = json_parse(body, len);
        free(body);
        const Json * first = j && j->type == J_ARR ? j->child : j;
        if (first && first->type == J_OBJ) {
            *root = j;
            return first;
        }
        int code = first && first->type == J_NUM ? (int)first->num : -1;
        json_free(j);
        if (code != -3) {
            say_error(code, err, es);
            return NULL;
        }
        plat_sleep_us(attempt * 1000000);
    }
    say_error(-18, err, es);
    return NULL;
}

// a node's attributes: "MEGA{...}" under AES-CBC with zeros on the end
static bool read_name(const uint8_t key[16], const char * at, char * name, size_t size)
{
    static uint8_t raw[2048];               // (the job thread's alone)
    static char text[2049];
    size_t n = b64_decode(at, strlen(at), raw, sizeof(raw)) & ~(size_t)15;
    if (!n)
        return false;
    Aes a;
    aes_key(&a, key);
    for (size_t i = 0; i < n; i += 16) {
        aes_decrypt(&a, raw + i, (uint8_t *)text + i);
        for (int b = 0; b < 16 && i; b++)
            text[i + b] ^= (char)raw[i + b - 16];
    }
    text[n] = 0;
    if (strncmp(text, "MEGA{", 5))
        return false;
    Json * j = json_parse(text + 4, strlen(text + 4));
    char buf[16];
    const Json * nm = j ? json_get(j, "n") : NULL;
    snprintf(name, size, "%s", nm ? json_str(nm, "", buf, sizeof(buf)) : "");
    json_free(j);
    return name[0] != 0;
}

static void fold_key(const uint8_t key[32], uint8_t out[16])
{
    for (int i = 0; i < 16; i++)
        out[i] = key[i] ^ key[i + 16];
}

typedef struct {
    Aes aes;
    uint8_t nonce[8];
} Unlock;

// what comes off Mega's servers is AES-CTR: the counter is the position in the file, so a retry from 0 lines up
static void unlock_bytes(void * user, uint64_t off, unsigned char * data, size_t n)
{
    Unlock * u = user;
    uint8_t block[16], pad[16];
    memcpy(block, u->nonce, 8);
    for (size_t i = 0; i < n;) {
        size_t from = (size_t)(off % 16), m = 16 - from;
        uint64_t at = off / 16;
        if (m > n - i)
            m = n - i;
        put32(block + 8, (uint32_t)(at >> 32));
        put32(block + 12, (uint32_t)at);
        aes_encrypt(&u->aes, block, pad);
        for (size_t k = 0; k < m; k++)
            data[i + k] ^= pad[from + k];
        i += m;
        off += m;
    }
}

// the file's address from Mega, then the file. 32 bytes of key: 16 of AES (folded), 8 of counter, 8 of checksum
static bool get_file(const char * node, const char * cmd, const uint8_t key[32], const char * path, const MegaJob * job,
                     char * err, size_t es)
{
    Json * root;
    const Json * a = api(node, cmd, job, &root, err, es);
    if (!a)
        return false;
    uint8_t ak[16];
    char url[600] = "", name[200] = "", buf[16];
    fold_key(key, ak);
    const Json * g = json_get(a, "g"), * at = json_get(a, "at");
    int code = (int)json_num(json_get(a, "e"), 0);
    if (g)
        snprintf(url, sizeof(url), "%s", json_str(g, "", buf, sizeof(buf)));
    bool key_good = !at || read_name(ak, json_str(at, "", buf, sizeof(buf)), name, sizeof(name));
    json_free(root);
    if (!url[0] || code) {
        say_error(code ? code : -17, err, es);
        return false;
    }
    if (!key_good) {
        snprintf(err, es, "the Mega link's key is wrong");
        return false;
    }
    Unlock u;
    aes_key(&u.aes, ak);
    memcpy(u.nonce, key + 16, 8);
    HttpReq r = {.url = url, .to_file = path, .cancel = job->cancel, .done = job->done, .total = job->total,
                 .filter = unlock_bytes, .filter_user = &u};
    int status = http_fetch(&r, NULL, NULL, err, es);
    if (status == 200)
        return true;
    remove(path);
    if (status == 509)
        say_error(-17, err, es);
    else if (status)
        snprintf(err, es, "Mega answered %d", status);
    return false;
}

// ---------------------------------------------------------------------------------------------------------- a folder

typedef struct {
    char h[16], parent[16], name[200];
    uint8_t key[32];
    bool dir;
} Node;

// a node's key comes locked with the folder link's key, as "owner:key" pairs: the right one opens its attributes
static bool open_node(const Aes * share, const char * k, const char * at, Node * n)
{
    size_t want = n->dir ? 16 : 32;
    for (const char * p = k; p && *p;) {
        const char * colon = strchr(p, ':'), * end = strchr(p, '/');
        if (!colon)
            break;
        uint8_t raw[32], ak[16];
        size_t len = end ? (size_t)(end - colon - 1) : strlen(colon + 1);
        if (b64_decode(colon + 1, len, raw, sizeof(raw)) == want) {
            for (size_t i = 0; i < want; i += 16)
                aes_decrypt(share, raw + i, n->key + i);
            if (n->dir)
                memcpy(ak, n->key, 16);
            else
                fold_key(n->key, ak);
            if (read_name(ak, at, n->name, sizeof(n->name)))
                return true;
        }
        p = end ? end + 1 : NULL;
    }
    return false;
}

static int find_node(const Node * nodes, int count, const char * h)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(nodes[i].h, h))
            return i;
    return -1;
}

// where a node goes under dir: its folders below the link's own; false when it isn't under `inside`
static bool node_path(const Node * nodes, int count, int i, const char * inside, const char * dir, char * path, size_t size)
{
    int chain[16], depth = 0;
    bool under = !inside[0];
    for (int at = i; at >= 0 && depth < 16;) {
        if (at != i && inside[0] && !strcmp(nodes[at].h, inside)) {
            under = true;
            break;
        }
        chain[depth++] = at;
        at = find_node(nodes, count, nodes[at].parent);
    }
    if (!under)
        return false;
    // the top of the chain is the shared folder itself
    if (!inside[0] && depth > 1)
        depth--;
    size_t k = (size_t)snprintf(path, size, "%s", dir);
    while (depth-- > 0 && k < size) {
        char part[200];
        snprintf(part, sizeof(part), "%s", nodes[chain[depth]].name);
        dl_safe_part(part);
        for (char * c = part; *c; c++)
            if (*c == '/')
                *c = '_';
        k += (size_t)snprintf(path + k, size - k, "/%s", part);
    }
    return true;
}

static bool get_folder(const Link * l, const char * file, const char * dir, const MegaJob * job, char * err, size_t es)
{
    Json * root;
    const Json * a = api(l->id, "[{\"a\":\"f\",\"c\":1,\"ca\":1,\"r\":1}]", job, &root, err, es);
    if (!a)
        return false;
    int count = 0, n = 0;
    const Json * f = json_get(a, "f");
    for (const Json * c = f ? f->child : NULL; c; c = c->next)
        count++;
    Node * nodes = calloc((size_t)count + 1, sizeof(Node));
    Aes share;
    aes_key(&share, l->key);
    for (const Json * c = f ? f->child : NULL; c && nodes; c = c->next) {
        char buf[16], at[1500], k[400];
        int type = (int)json_num(json_get(c, "t"), -1);
        if (type != 0 && type != 1)
            continue;
        Node * x = &nodes[n];
        x->dir = type == 1;
        snprintf(x->h, sizeof(x->h), "%s", json_str(json_get(c, "h"), "", buf, sizeof(buf)));
        snprintf(x->parent, sizeof(x->parent), "%s", json_str(json_get(c, "p"), "", buf, sizeof(buf)));
        snprintf(at, sizeof(at), "%s", json_str(json_get(c, "a"), "", buf, sizeof(buf)));
        snprintf(k, sizeof(k), "%s", json_str(json_get(c, "k"), "", buf, sizeof(buf)));
        if (open_node(&share, k, at, x))
            n++;
    }
    json_free(root);
    if (!n) {
        free(nodes);
        snprintf(err, es, "the Mega folder is empty or its key is wrong");
        return false;
    }
    bool ok = true;
    int files = 0;
    char cmd[100], path[800];
    if (l->inside_file) {
        int i = find_node(nodes, n, l->inside);
        if (i < 0 || nodes[i].dir) {
            say_error(-9, err, es);
            ok = false;
        } else {
            snprintf(cmd, sizeof(cmd), "[{\"a\":\"g\",\"g\":1,\"ssl\":2,\"n\":\"%s\"}]", nodes[i].h);
            ok = get_file(l->id, cmd, nodes[i].key, file, job, err, es);
        }
        free(nodes);
        return ok;
    }
    dl_mkdirs(dir);
    for (int i = 0; ok && i < n; i++) {
        if (nodes[i].dir || !node_path(nodes, n, i, l->inside, dir, path, sizeof(path)))
            continue;
        int want = job->wanted(nodes[i].name);
        if (!want)
            continue;
        if (job->cancel && *job->cancel) {
            snprintf(err, es, "cancelled");
            ok = false;
            break;
        }
        files++;
        job->on_file(job->user, nodes[i].name);
        char * slash = strrchr(path, '/');
        *slash = 0;
        dl_mkdirs(path);
        *slash = '/';
        snprintf(cmd, sizeof(cmd), "[{\"a\":\"g\",\"g\":1,\"ssl\":2,\"n\":\"%s\"}]", nodes[i].h);
        // a cover that won't come is no reason to give up the song
        if (!get_file(l->id, cmd, nodes[i].key, path, job, err, es) && want == 2)
            ok = false;
    }
    free(nodes);
    if (ok && !files) {
        snprintf(err, es, "no song files in the Mega folder");
        ok = false;
    }
    return ok;
}

bool mega_fetch(const char * url, const char * file, const char * dir, const MegaJob * job, bool * folder, char * err,
                size_t err_size)
{
    Link l;
    if (!parse_link(url, &l)) {
        snprintf(err, err_size, "the Mega link has no key");
        return false;
    }
    *folder = l.folder && !l.inside_file;
    if (l.folder)
        return get_folder(&l, file, dir, job, err, err_size);
    char cmd[100];
    snprintf(cmd, sizeof(cmd), "[{\"a\":\"g\",\"g\":1,\"ssl\":2,\"p\":\"%s\"}]", l.id);
    return get_file(NULL, cmd, l.key, file, job, err, err_size);
}
