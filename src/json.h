// A small JSON reader for the song sites' search results
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JsonType;

typedef struct Json {
    JsonType type;
    double num;
    bool b;
    char * str;                     // a string's text (UTF-8)
    char * key;                     // an object member's name
    struct Json * child, * next;    // an array's or object's first item, and the item after this one
} Json;

Json * json_parse(const char * text, size_t len);
void json_free(Json * j);
const Json * json_get(const Json * obj, const char * key);
// a.b.c through objects
const Json * json_path(const Json * j, const char * path);
// strings as they are, numbers and booleans as text; def for anything else
const char * json_str(const Json * j, const char * def, char * buf, size_t size);
// numbers, or strings holding one
double json_num(const Json * j, double def);
