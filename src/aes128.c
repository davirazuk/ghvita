#include <stdint.h>
#include <string.h>
#include "aes128.h"

static uint8_t sbox[256], unbox[256];
static uint32_t te[256];

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)(x << 1) ^ (x & 0x80 ? 0x1B : 0);
}

static uint8_t rol8(uint8_t x, int n)
{
    return (uint8_t)(x << n) | (uint8_t)(x >> (8 - n));
}

static uint32_t ror32(uint32_t x, int n)
{
    return x >> n | x << (32 - n);
}

static uint32_t be32(const uint8_t * p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// the S-box from the field's inverses: p runs through it by threes, q undoes the same steps
static void aes_tables(void)
{
    if (sbox[0])
        return;
    uint8_t p = 1, q = 1;
    do {
        p = p ^ xtime(p);
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80)
            q ^= 0x09;
        sbox[p] = q ^ rol8(q, 1) ^ rol8(q, 2) ^ rol8(q, 3) ^ rol8(q, 4) ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) {
        uint8_t s = sbox[i], s2 = xtime(s);
        unbox[s] = (uint8_t)i;
        te[i] = (uint32_t)s2 << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | (uint8_t)(s2 ^ s);
    }
}

void aes128_key(Aes128 * a, const uint8_t key[16])
{
    aes_tables();
    uint32_t * k = a->rk;
    for (int i = 0; i < 4; i++)
        k[i] = be32(key + i * 4);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = k[i - 1];
        if (i % 4 == 0) {
            t = (uint32_t)sbox[t >> 16 & 255] << 24 | (uint32_t)sbox[t >> 8 & 255] << 16 |
                (uint32_t)sbox[t & 255] << 8 | sbox[t >> 24];
            t ^= (uint32_t)rcon << 24;
            rcon = xtime(rcon);
        }
        k[i] = k[i - 4] ^ t;
    }
}

void aes128_encrypt(const Aes128 * a, const uint8_t in[16], uint8_t out[16])
{
    const uint32_t * k = a->rk;
    uint32_t s0 = be32(in) ^ k[0], s1 = be32(in + 4) ^ k[1], s2 = be32(in + 8) ^ k[2], s3 = be32(in + 12) ^ k[3];
    for (int r = 1; r < 10; r++) {
        k += 4;
        uint32_t t0 = te[s0 >> 24] ^ ror32(te[s1 >> 16 & 255], 8) ^ ror32(te[s2 >> 8 & 255], 16) ^
                      ror32(te[s3 & 255], 24) ^ k[0];
        uint32_t t1 = te[s1 >> 24] ^ ror32(te[s2 >> 16 & 255], 8) ^ ror32(te[s3 >> 8 & 255], 16) ^
                      ror32(te[s0 & 255], 24) ^ k[1];
        uint32_t t2 = te[s2 >> 24] ^ ror32(te[s3 >> 16 & 255], 8) ^ ror32(te[s0 >> 8 & 255], 16) ^
                      ror32(te[s1 & 255], 24) ^ k[2];
        uint32_t t3 = te[s3 >> 24] ^ ror32(te[s0 >> 16 & 255], 8) ^ ror32(te[s1 >> 8 & 255], 16) ^
                      ror32(te[s2 & 255], 24) ^ k[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    k += 4;
    put32(out, ((uint32_t)sbox[s0 >> 24] << 24 | (uint32_t)sbox[s1 >> 16 & 255] << 16 |
                (uint32_t)sbox[s2 >> 8 & 255] << 8 | sbox[s3 & 255]) ^ k[0]);
    put32(out + 4, ((uint32_t)sbox[s1 >> 24] << 24 | (uint32_t)sbox[s2 >> 16 & 255] << 16 |
                    (uint32_t)sbox[s3 >> 8 & 255] << 8 | sbox[s0 & 255]) ^ k[1]);
    put32(out + 8, ((uint32_t)sbox[s2 >> 24] << 24 | (uint32_t)sbox[s3 >> 16 & 255] << 16 |
                    (uint32_t)sbox[s0 >> 8 & 255] << 8 | sbox[s1 & 255]) ^ k[2]);
    put32(out + 12, ((uint32_t)sbox[s3 >> 24] << 24 | (uint32_t)sbox[s0 >> 16 & 255] << 16 |
                     (uint32_t)sbox[s1 >> 8 & 255] << 8 | sbox[s2 & 255]) ^ k[3]);
}

// only keys and names are decrypted, so this one's written for size: the rounds backwards, a byte at a time
void aes128_decrypt(const Aes128 * a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    const uint32_t * k = a->rk + 40;
    for (int i = 0; i < 4; i++)
        put32(s + i * 4, be32(in + i * 4) ^ k[i]);
    for (int r = 9; r >= 0; r--) {
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++)
                t[((c + row) & 3) * 4 + row] = unbox[s[c * 4 + row]];
        k -= 4;
        for (int i = 0; i < 4; i++)
            put32(t + i * 4, be32(t + i * 4) ^ k[i]);
        if (r == 0) {
            memcpy(s, t, 16);
            break;
        }
        for (int c = 0; c < 4; c++) {
            const uint8_t * m = t + c * 4;
            for (int row = 0; row < 4; row++) {
                uint8_t x = m[row], x2 = xtime(x), x4 = xtime(x2), x8 = xtime(x4);
                uint8_t y = m[(row + 1) & 3], y2 = xtime(y), y4 = xtime(y2), y8 = xtime(y4);
                uint8_t z = m[(row + 2) & 3], z2 = xtime(z), z4 = xtime(z2), z8 = xtime(z4);
                uint8_t w = m[(row + 3) & 3], w2 = xtime(w), w4 = xtime(w2), w8 = xtime(w4);
                s[c * 4 + row] = (x8 ^ x4 ^ x2) ^ (y8 ^ y2 ^ y) ^ (z8 ^ z4 ^ z) ^ (w8 ^ w);
            }
        }
    }
    memcpy(out, s, 16);
}
