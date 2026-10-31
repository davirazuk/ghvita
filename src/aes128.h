#ifndef AES128_H
#define AES128_H

#include <stdint.h>

// AES-128, one block at a time: MEGA's links and the mogg decryptor's keys
typedef struct {
    uint32_t rk[44];
} Aes128;

void aes128_key(Aes128 * a, const uint8_t key[16]);
void aes128_encrypt(const Aes128 * a, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const Aes128 * a, const uint8_t in[16], uint8_t out[16]);

#endif
