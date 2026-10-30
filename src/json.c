#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

typedef struct {
    const char * p, * end;
    int depth;
    bool bad;
} Parser;

static void skip_ws(Parser * ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hex4(const char * p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
        else
            return -1;
    }
    return v;
}

static char * parse_string(Parser * ps)
{
    // the opening quote is at p
    const char * p = ps->p + 1;
    // sized to this string alone (escapes only shorten it): a size from the rest of the text ran a big search answer
    // out of memory, one such block a string
    const char * q = p;
    while (q < ps->end && *q != '"')
        q += *q == '\\' ? 2 : 1;
    if (q >= ps->end) {
        ps->bad = true;
        return NULL;
    }
    char * out = malloc(q - p + 1), * o = out;
    if (!out) {
        ps->bad = true;
        return NULL;
    }
    while (p < ps->end && *p != '"') {
        if (*p != '\\') {
            *o++ = *p++;
            continue;
        }
        if (++p >= ps->end)
            break;
        char c = *p++;
        switch (c) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            if (ps->end - p < 4)
                goto bad;
            int cp = hex4(p);
            p += 4;
            if (cp < 0)
                goto bad;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                int lo = hex4(p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (cp < 0x80)
                *o++ = (char)cp;
            else if (cp < 0x800) {
                *o++ = (char)(0xC0 | cp >> 6);
                *o++ = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                *o++ = (char)(0xE0 | cp >> 12);
                *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *o++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *o++ = (char)(0xF0 | cp >> 18);
                *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
                *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *o++ = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: *o++ = c; break;
        }
    }
    if (p >= ps->end)
        goto bad;
    *o = 0;
    ps->p = p + 1;
    return out;
bad:
    free(out);
    ps->bad = true;
    return NULL;
}

static Json * parse_value(Parser * ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end || ++ps->depth > 64) {
        ps->bad = true;
        return NULL;
    }
    Json * j = calloc(1, sizeof(Json));
    if (!j) {
        ps->bad = true;
        return NULL;
    }
    char c = *ps->p;
    if (c == '{' || c == '[') {
        j->type = c == '{' ? J_OBJ : J_ARR;
        ps->p++;
        Json ** tail = &j->child;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == (c == '{' ? '}' : ']'))
            ps->p++;
        else
            for (;;) {
                char * key = NULL;
                if (j->type == J_OBJ) {
                    skip_ws(ps);
                    if (ps->p >= ps->end || *ps->p != '"' || !(key = parse_string(ps)))
                        break;
                    skip_ws(ps);
                    if (ps->p >= ps->end || *ps->p != ':') {
                        free(key);
                        break;
                    }
                    ps->p++;
                }
                Json * item = parse_value(ps);
                if (!item) {
                    free(key);
                    break;
                }
                item->key = key;
                *tail = item;
                tail = &item->next;
                skip_ws(ps);
                if (ps->p < ps->end && *ps->p == ',') {
                    ps->p++;
                    continue;
                }
                if (ps->p < ps->end && *ps->p == (c == '{' ? '}' : ']'))
                    ps->p++;
                else
                    ps->bad = true;
                break;
            }
    } else if (c == '"') {
        j->type = J_STR;
        j->str = parse_string(ps);
    } else if (c == 't' && ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) {
        j->type = J_BOOL;
        j->b = true;
        ps->p += 4;
    } else if (c == 'f' && ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) {
        j->type = J_BOOL;
        ps->p += 5;
    } else if (c == 'n' && ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) {
        j->type = J_NULL;
        ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        size_t n = 0;
        while (ps->p < ps->end && n + 1 < sizeof(buf) && strchr("+-0123456789.eE", *ps->p))
            buf[n++] = *ps->p++;
        buf[n] = 0;
        j->type = J_NUM;
        j->num = strtod(buf, NULL);
    } else
        ps->bad = true;
    ps->depth--;
    if (ps->bad) {
        json_free(j);
        return NULL;
    }
    return j;
}

Json * json_parse(const char * text, size_t len)
{
    Parser ps = {text, text + len, 0, false};
    return parse_value(&ps);
}

void json_free(Json * j)
{
    while (j) {
        Json * next = j->next;
        json_free(j->child);
        free(j->str);
        free(j->key);
        free(j);
        j = next;
    }
}

const Json * json_get(const Json * obj, const char * key)
{
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (const Json * c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

const Json * json_path(const Json * j, const char * path)
{
    char part[64];
    while (j && *path) {
        size_t n = strcspn(path, ".");
        if (n >= sizeof(part))
            return NULL;
        memcpy(part, path, n);
        part[n] = 0;
        j = json_get(j, part);
        path += n + (path[n] == '.');
    }
    return j;
}

const char * json_str(const Json * j, const char * def, char * buf, size_t size)
{
    if (!j)
        return def;
    if (j->type == J_STR)
        return j->str;
    if (j->type == J_NUM) {
        if (j->num == (long long)j->num)
            snprintf(buf, size, "%lld", (long long)j->num);
        else
            snprintf(buf, size, "%g", j->num);
        return buf;
    }
    if (j->type == J_BOOL)
        return j->b ? "true" : "false";
    return def;
}

double json_num(const Json * j, double def)
{
    if (!j)
        return def;
    if (j->type == J_NUM)
        return j->num;
    if (j->type == J_STR && j->str[0]) {
        char * end;
        double v = strtod(j->str, &end);
        return end != j->str ? v : def;
    }
    return def;
}
