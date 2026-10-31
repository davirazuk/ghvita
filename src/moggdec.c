// Decrypts Rock Band .mogg audio (versions 0x0B-0x11) to plain Ogg Vorbis.
// Port of DarkRTA/themethod3's key generation and AES-CTR, which is the
// published way the Rock Band community reads its own charts' audio.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "aes128.h"
#include "moggdec.h"
#include "moggkeys.h"

static uint32_t lcg(uint32_t x)
{
    return x * 0x19660Du + 0x3C6EF35Fu;
}

static uint32_t rd32(const uint8_t * p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t * p)
{
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static uint32_t rdbe32(const uint8_t * p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void wrbe32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void get_masher(uint8_t out[32])
{
    uint32_t word = 0xEB;
    for (int i = 0; i < 8; i++) {
        uint32_t reset = i == 0 ? 0xEB : 0;
        if (reset)
            word = reset;
        word = word * 0x19660Eu + 0x3C6EF35Fu;
        out[i * 4] = (uint8_t)word;
        out[i * 4 + 1] = (uint8_t)(word >> 8);
        out[i * 4 + 2] = (uint8_t)(word >> 16);
        out[i * 4 + 3] = (uint8_t)(word >> 24);
    }
}

static uint8_t rotl8(uint8_t x, int n)
{
    n &= 7;
    return (uint8_t)(x << n | x >> (8 - n));
}

static uint8_t rotr8(uint8_t x, int n)
{
    n &= 7;
    return (uint8_t)(x >> n | x << (8 - n));
}

static int not(int x)
{
    return x == 0 ? 1 : 0;
}

// the 64 little mixing steps themethod3 picked out of the original binary
static uint8_t o_funcs(uint8_t b1, uint8_t b2, uint8_t op)
{
    int32_t a1 = b1, a2 = b2, ret;
    uint8_t u1 = b1, u2 = b2;
    switch (op) {
    case 0: ret = a2 + rotr8(u1, not(a2)); break;
    case 1: ret = a2 + rotr8(u1, 3); break;
    case 2: ret = a2 + rotl8(u1, 1); break;
    case 3: ret = a2 ^ ((a1 >> (a2 & 7)) | (a1 << ((-a2) & 7))); break;
    case 4: ret = a2 ^ rotl8(u1, 4); break;
    case 5: ret = a2 + (a2 ^ rotr8(u1, 3)); break;
    case 6: ret = a2 + rotl8(u1, 2); break;
    case 7: ret = a2 + not(a1); break;
    case 8: ret = a2 ^ rotr8(u1, not(a2)); break;
    case 9: ret = a2 ^ (a2 + rotl8(u1, 3)); break;
    case 10: ret = a2 + rotl8(u1, 3); break;
    case 11: ret = a2 + rotl8(u1, 4); break;
    case 12: ret = a1 ^ a2; break;
    case 13: ret = a2 ^ not(a1); break;
    case 14: ret = a2 ^ (a2 + rotr8(u1, 3)); break;
    case 15: ret = a2 ^ rotl8(u1, 3); break;
    case 16: ret = a2 ^ rotl8(u1, 2); break;
    case 17: ret = a2 + (a2 ^ rotl8(u1, 3)); break;
    case 18: ret = a2 + (a1 ^ a2); break;
    case 19: ret = a1 + a2; break;
    case 20: ret = a2 ^ rotr8(u1, 3); break;
    case 21: ret = a2 ^ (a1 + a2); break;
    case 22: ret = rotr8(u1, not(a2)); break;
    case 23: ret = a2 + rotr8(u1, 1); break;
    case 24: ret = (a1 >> (a2 & 7)) | (a1 << ((-a2) & 7)); break;
    case 25:
        if (a1 == 0)
            ret = a2 == 0 ? 128 : 1;
        else
            ret = 0;
        break;
    case 26: ret = a2 + rotr8(u1, 2); break;
    case 27: ret = a2 ^ rotr8(u1, 1); break;
    case 28: ret = o_funcs((uint8_t)~a1, (uint8_t)a2, 24); break;
    case 29: ret = a2 ^ rotr8(u1, 2); break;
    case 30: ret = a2 + ((a1 >> (a2 & 7)) | (a1 << ((-a2) & 7))); break;
    case 31: ret = a2 ^ rotl8(u1, 1); break;
    case 32: ret = (((a1 << 8) | 0xAA | (a1 ^ 0xFF)) >> 4) ^ a2; break;
    case 33: ret = (((a1 ^ 0xFF) | a1 << 8) >> 3) ^ a2; break;
    case 34: ret = ((((a1 << 8) ^ 0xFF00) | a1) >> 2) ^ a2; break;
    case 35: ret = (((a1 ^ 0x5C) | a1 << 8) >> 5) ^ a2; break;
    case 36: ret = (((a1 << 8) | 0x65 | (a1 ^ 0x3C)) >> 2) ^ a2; break;
    case 37: ret = (((a1 ^ 0x36) | a1 << 8) >> 2) ^ a2; break;
    case 38: ret = (((a1 ^ 0x36) | a1 << 8) >> 4) ^ a2; break;
    case 39: ret = (((a1 ^ 0x5C) | a1 << 8 | 0x36) >> 1) ^ a2; break;
    case 40: ret = (((a1 ^ 0xFF) | a1 << 8) >> 5) ^ a2; break;
    case 41: ret = ((((((~a1) & 0xFF) << 8) | a1) >> 6) ^ a2); break;
    case 42: ret = (((a1 ^ 0x5C) | a1 << 8) >> 3) ^ a2; break;
    case 43: ret = (((a1 ^ 0x3C) | 0x65 | a1 << 8) >> 5) ^ a2; break;
    case 44: ret = (((a1 ^ 0x36) | a1 << 8) >> 1) ^ a2; break;
    case 45: ret = (((a1 ^ 0x65) | a1 << 8 | 0x3C) >> 6) ^ a2; break;
    case 46: ret = (((a1 ^ 0x5C) | a1 << 8) >> 2) ^ a2; break;
    case 47: ret = (((a2 ^ 0xAA) | a2 << 8 | 0xFF) >> 3) ^ a1; break;
    case 48: ret = (((a1 ^ 0x63) | a1 << 8 | 0x5C) >> 6) ^ a2; break;
    case 49: ret = (((a1 ^ 0x5C) | a1 << 8 | 0x36) >> 7) ^ a2; break;
    case 50: ret = (((a1 ^ 0x5C) | a1 << 8) >> 6) ^ a2; break;
    case 51: ret = ((((a1 << 8) ^ 0xFF00) | a1) >> 3) ^ a2; break;
    case 52: ret = (((a1 ^ 0xFF) | a1 << 8) >> 6) ^ a2; break;
    case 53: ret = ((((a1 << 8) ^ 0xFF00) | a1) >> 5) ^ a2; break;
    case 54: ret = (((a1 ^ 0x3C) | 0x65 | a1 << 8) >> 4) ^ a2; break;
    case 55: ret = (((a1 ^ 0x63) | a1 << 8 | 0x5C) >> 3) ^ a2; break;
    case 56: ret = (((a1 ^ 0x63) | a1 << 8 | 0x5C) >> 5) ^ a2; break;
    case 57: ret = (((a1 ^ 0xAF) | a1 << 8 | 0xFA) >> 5) ^ a2; break;
    case 58: ret = (((a1 ^ 0x5C) | a1 << 8 | 0x36) >> 5) ^ a2; break;
    case 59: ret = (((a1 ^ 0x5C) | a1 << 8 | 0x36) >> 3) ^ a2; break;
    case 60: ret = (((a1 ^ 0x36) | a1 << 8) >> 3) ^ a2; break;
    case 61: ret = (((a1 ^ 0x63) | a1 << 8 | 0x5C) >> 4) ^ a2; break;
    case 62: ret = (((a1 ^ 0xFF) | a1 << 8 | 0xAF) >> 6) ^ a2; break;
    default: ret = (((a1 ^ 0xFF) | a1 << 8) >> 2) ^ a2; break;
    }
    (void)u2;
    return (uint8_t)ret;
}

static uint32_t roll(uint32_t x)
{
    return (x + 0x13) % 0x20;
}

static void shuffle1(uint8_t k[32])
{
    for (int i = 0; i < 8; i++) {
        uint32_t o = roll(i << 2);
        uint8_t t = k[o];
        k[o] = k[i * 4 + 2];
        k[i * 4 + 2] = t;
        o = roll(i * 4 + 3);
        t = k[o];
        k[o] = k[i * 4 + 1];
        k[i * 4 + 1] = t;
    }
}

static void shuffle2(uint8_t k[32])
{
    for (int i = 0; i < 8; i++) {
        int a = (7 - i) * 4 + 1, b = i * 4 + 2;
        uint8_t t = k[a];
        k[a] = k[b];
        k[b] = t;
        a = (7 - i) * 4, b = i * 4 + 3;
        t = k[a];
        k[a] = k[b];
        k[b] = t;
    }
}

static void shuffle3(uint8_t k[32])
{
    for (int i = 0; i < 8; i++) {
        uint32_t o = roll((7 - i) * 4 + 1);
        uint8_t t = k[o];
        k[o] = k[i * 4 + 2];
        k[i * 4 + 2] = t;
        int a = (7 - i) * 4, b = i * 4 + 3;
        t = k[a];
        k[a] = k[b];
        k[b] = t;
    }
}

static void shuffle4(uint8_t k[32])
{
    for (int i = 0; i < 8; i++) {
        int a = (7 - i) * 4 + 1, b = i * 4 + 2;
        uint8_t t = k[a];
        k[a] = k[b];
        k[b] = t;
        uint32_t o = roll((7 - i) * 4);
        t = k[o];
        k[o] = k[i * 4 + 3];
        k[i * 4 + 3] = t;
    }
}

static void shuffle5(uint8_t k[32])
{
    for (int i = 0; i < 8; i++) {
        uint32_t o = roll(i * 4 + 2);
        int a = (7 - i) * 4 + 1;
        uint8_t t = k[a];
        k[a] = k[o];
        k[o] = t;
        a = (7 - i) * 4;
        t = k[a];
        k[a] = k[i * 4 + 3];
        k[i * 4 + 3] = t;
    }
}

static void shuffle6(uint8_t k[32])
{
    for (int i = 0; i < 8; i++) {
        int a = (7 - i) * 4 + 1, b = i * 4 + 2;
        uint8_t t = k[a];
        k[a] = k[b];
        k[b] = t;
        uint32_t o = roll(i * 4 + 3);
        a = (7 - i) * 4;
        t = k[a];
        k[a] = k[o];
        k[o] = t;
    }
}

static void shuffle_all(uint8_t k[32])
{
    shuffle1(k);
    shuffle2(k);
    shuffle3(k);
    shuffle4(k);
    shuffle5(k);
    shuffle6(k);
}

static uint8_t hex_digit(uint8_t c)
{
    if (c >= 'a' && c <= 'f')
        return (uint8_t)(c - 87);
    if (c >= 'A' && c <= 'F')
        return (uint8_t)(c - 0x37);
    return (uint8_t)(c - 0x30);
}

static void grind(uint32_t magic_a, uint32_t magic_b, uint8_t key[16], uint32_t version)
{
    uint8_t used[64] = {0}, order[64];
    uint32_t num1 = magic_a, num2 = magic_b;
    int table2[256], table4[256], num;

    for (int i = 0; i < 256; i++) {
        table2[i] = (magic_a & 0xFF) >> 3;
        magic_a = lcg(magic_a);
    }
    if (!magic_b)
        magic_b = 0x303F;
    for (int i = 0; i < 0x20; i++) {
        for (;;) {
            magic_b = lcg(magic_b);
            num = (int)((magic_b >> 2) & 0x1F);
            if (!used[num])
                break;
        }
        order[i] = (uint8_t)num;
        used[num] = 1;
    }
    magic_a = num2;
    for (int i = 0; i < 256; i++) {
        table4[i] = (int)((magic_a & 0xFF) >> 2 & 0x3F);
        magic_a = lcg(magic_a);
    }
    if (version > 13) {
        for (int i = 0x20; i < 0x40; i++) {
            for (;;) {
                num1 = lcg(num1);
                num = (int)(((num1 >> 2) & 0x1F) + 0x20);
                if (!used[num])
                    break;
            }
            order[i] = (uint8_t)num;
            used[num] = 1;
        }
    }
    for (int j = 0; j < 16; j++) {
        uint8_t v = key[j];
        for (int k = 0; k < 16; k += 2) {
            int idx = (version > 13 ? table4 : table2)[key[k]];
            v = o_funcs(v, key[k + 1], order[idx]);
        }
        key[j] = v;
    }
}

static bool derive_key(const uint8_t * h, uint32_t version, uint8_t out[16])
{
    static const uint8_t * const hv[6] = {HV_KEY_0C, HV_KEY_0C, HV_KEY_0E, HV_KEY_0F, HV_KEY_10, HV_KEY_11};
    if (version < 12 || version > 17)
        return false;
    size_t base = 20 + (size_t)rd32(h + 16) * 8;
    uint8_t masked[16], key[16], revealed[32], hex[16];
    Aes128 aes;
    memcpy(masked, h + base + 16 + 32, 16);
    aes128_key(&aes, hv[version - 12]);
    aes128_decrypt(&aes, masked, key);
    uint32_t magic_a = rd32(h + base + 16), magic_b = rd32(h + base + 24);
    size_t pos = base + 16 + 48;
    uint64_t which = 0;
    if (version == 17) {
        which = rd64(h + pos);
        pos += 8;
    }
    int index = (int)(rd64(h + pos) % 6) + 6;
    switch (version == 17 ? (int)which : 0) {
    case 1: memcpy(revealed, HIDDEN_KEYS_17_1[index], 32); break;
    case 4: memcpy(revealed, HIDDEN_KEYS_17_4[index], 32); break;
    case 6: memcpy(revealed, HIDDEN_KEYS_17_6[index], 32); break;
    case 8: memcpy(revealed, HIDDEN_KEYS_17_8[index], 32); break;
    case 10: memcpy(revealed, HIDDEN_KEYS_17_10[index], 32); break;
    default:
        if (version == 17)
            return false;
        memcpy(revealed, HIDDEN_KEYS[index], 32);
        break;
    }
    uint8_t masher[32];
    get_masher(masher);
    for (int r = 0; r < 14; r++)
        shuffle_all(revealed);
    for (int i = 0; i < 32; i++)
        revealed[i] ^= masher[i];
    for (int i = 0; i < 16; i++)
        hex[i] = (uint8_t)(hex_digit(revealed[i * 2 + 1]) + hex_digit(revealed[i * 2]) * 16);
    grind(magic_a, magic_b, hex, version);
    for (int i = 0; i < 16; i++)
        out[i] = hex[i] ^ key[i];
    return true;
}

static void ctr_nonce(const uint8_t nonce[16], uint64_t ctr, uint8_t out[16])
{
    uint64_t lo = rd64(nonce), hi = rd64(nonce + 8), old = lo;
    lo += ctr;
    if (lo < old)
        hi++;
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)(lo >> (i * 8));
    for (int i = 0; i < 8; i++)
        out[i + 8] = (uint8_t)(hi >> (i * 8));
}

bool mogg_cipher_init(MoggCipher * c, const uint8_t * head, size_t len, char * err, size_t err_size)
{
    uint32_t version = head[0];
    if (version <= 10 || version > 17) {
        snprintf(err, err_size, "unknown mogg audio version");
        return false;
    }
    size_t need = 20 + (size_t)rd32(head + 16) * 8 + 80;
    if (need > len || need > 512 * 1024) {
        snprintf(err, err_size, "its audio header is too big to read");
        return false;
    }
    memset(c, 0, sizeof(*c));
    uint8_t key[16];
    if (version == 11)
        memcpy(key, CTR_KEY_0B, 16);
    else if (!derive_key(head, version, key)) {
        snprintf(err, err_size, "its audio is encrypted with an unknown key");
        return false;
    }
    memcpy(c->nonce, head + 20 + (size_t)rd32(head + 16) * 8, 16);
    c->ogg = rd32(head + 4);
    size_t base = 20 + (size_t)rd32(head + 16) * 8;
    c->magic_a = rd32(head + base + 16);
    c->magic_b = rd32(head + base + 24);
    aes128_key(&c->aes, key);
    return true;
}

void mogg_cipher_xor(MoggCipher * c, uint8_t * buf, size_t n, uint32_t pos)
{
    if (pos < c->ogg) {
        size_t skip = (size_t)(c->ogg - pos);
        if (skip >= n)
            return;
        buf += skip;
        n -= skip;
        pos += (uint32_t)skip;
    }
    while (n) {
        uint32_t off = pos - c->ogg;
        uint64_t ctr = off >> 4;
        int at = (int)(off & 15);
        if (!c->cached || c->last_ctr != ctr) {
            uint8_t block[16];
            ctr_nonce(c->nonce, ctr, block);
            aes128_encrypt(&c->aes, block, c->mask);
            c->last_ctr = ctr;
            c->cached = true;
        }
        size_t take = (size_t)(16 - at);
        if (take > n)
            take = n;
        for (size_t i = 0; i < take; i++)
            buf[i] ^= c->mask[at + i];
        buf += take;
        pos += (uint32_t)take;
        n -= take;
    }
}

bool mogg_fix_ogg(MoggCipher * c, uint8_t * first, size_t n, char * err, size_t err_size)
{
    if (n < 24) {
        snprintf(err, err_size, "its audio is too short");
        return false;
    }
    if (!memcmp(first, "HMXA", 4)) {
        uint32_t a = lcg(lcg(c->magic_a ^ 0x5C5C5C5Cu));
        uint32_t b = lcg(c->magic_b ^ 0x36363636u);
        memcpy(first, "OggS", 4);
        wrbe32(first + 12, rdbe32(first + 12) ^ a);
        wrbe32(first + 20, rdbe32(first + 20) ^ b);
    }
    if (memcmp(first, "OggS", 4)) {
        snprintf(err, err_size, "its audio wouldn't decrypt");
        return false;
    }
    return true;
}
