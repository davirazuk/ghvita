#ifndef MOGGDEC_H
#define MOGGDEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aes128.h"

// A .mogg past version 10 has its Ogg pages XORed with an AES-CTR keystream:
// the key comes from the file's own header (themethod3 by DarkRTA et al.)
typedef struct {
    Aes128 aes;
    uint8_t nonce[16];
    uint32_t ogg;
    uint32_t magic_a, magic_b;
    uint64_t last_ctr;
    uint8_t mask[16];
    bool cached;
} MoggCipher;

// head is the mogg's first bytes, at least 20 + hmx * 8 + 80 of them
bool mogg_cipher_init(MoggCipher * c, const uint8_t * head, size_t len, char * err, size_t err_size);

// XORs an absolute run of the file in place; bytes before the Ogg offset are left alone
void mogg_cipher_xor(MoggCipher * c, uint8_t * buf, size_t n, uint32_t pos);

// the decrypted first page can still say HMXA: turn it back into a readable OggS page
bool mogg_fix_ogg(MoggCipher * c, uint8_t * first, size_t n, char * err, size_t err_size);

#endif
