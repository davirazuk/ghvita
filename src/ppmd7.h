// PPMd variant H, the text model RAR 3 packs some blocks with: decoding only. The range coder is the caller's (RAR's
// differs from 7-Zip's)
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Ppmd7 Ppmd7;

typedef struct {
    uint32_t (*threshold)(void * user, uint32_t total);
    void (*decode)(void * user, uint32_t start, uint32_t size);
    uint32_t (*bit)(void * user, uint32_t size0, uint32_t total_bits);      // 0 or 1, with the chance of a 0 being size0
} Ppmd7Coder;

Ppmd7 * ppmd7_new(void);
void ppmd7_free(Ppmd7 * p);
bool ppmd7_alloc(Ppmd7 * p, uint32_t size);
void ppmd7_init(Ppmd7 * p, unsigned max_order);
// the next byte, or below 0 at an error or the end of the data
int ppmd7_decode(Ppmd7 * p, const Ppmd7Coder * c, void * user);
