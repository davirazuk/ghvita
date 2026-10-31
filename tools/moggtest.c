// Offline test bed for the mogg decryptor: builds synthetic encrypted .mogg
// files and decrypts real ones, outside the game.
//   moggtest enc <plain.ogg> <out.mogg> <version> <set> <keyindex>
//   moggtest dec <in.mogg> <out.ogg>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/moggdec.h"

static uint32_t rd32(const uint8_t * p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t * p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint8_t * read_all(const char * path, size_t * len)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t * p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

static bool write_all(const char * path, const uint8_t * p, size_t n)
{
    FILE * f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(p, 1, n, f) == n;
    fclose(f);
    return ok;
}

static int encrypt_file(const char * in, const char * out, uint32_t version, uint64_t set, uint64_t index)
{
    size_t n;
    uint8_t * plain = read_all(in, &n);
    if (!plain)
        return 1;
    const uint32_t ogg = 0x800, hmx = 16;
    size_t total = ogg + n;
    uint8_t * file = calloc(1, total);
    file[0] = (uint8_t)version;
    put32(file + 4, ogg);
    put32(file + 16, hmx);
    size_t base = 20 + hmx * 8;
    for (int i = 0; i < 16; i++)
        file[base + i] = (uint8_t)(0x40 + i);
    put32(file + base + 16, 0x12345678);
    put32(file + base + 24, 0x9ABCDEF0);
    for (int i = 0; i < 16; i++)
        file[base + 16 + 32 + i] = (uint8_t)(i * 7 + 3);
    if (version == 17) {
        put64(file + base + 16 + 48, set);
        put64(file + base + 16 + 56, index);
    } else
        put64(file + base + 16 + 48, index);
    memcpy(file + ogg, plain, n);
    if (version != 10) {
        MoggCipher c;
        char err[256] = "";
        size_t need = 20 + (size_t)rd32(file + 16) * 8 + 80;
        if (!mogg_cipher_init(&c, file, need, err, sizeof(err))) {
            fprintf(stderr, "init: %s\n", err);
            return 1;
        }
        for (size_t at = 0; at < n; at += 0x1000) {
            size_t k = n - at < 0x1000 ? n - at : 0x1000;
            mogg_cipher_xor(&c, file + ogg + at, k, ogg + (uint32_t)at);
        }
    }
    bool ok = write_all(out, file, total);
    free(file);
    free(plain);
    return ok ? 0 : 1;
}

static int decrypt_file(const char * in, const char * out)
{
    size_t n;
    uint8_t * file = read_all(in, &n);
    if (!file || n < 24)
        return 1;
    uint32_t version = file[0], ogg = rd32(file + 4);
    if (ogg >= n)
        return 1;
    size_t len = n - ogg;
    uint8_t * plain = malloc(len);
    memcpy(plain, file + ogg, len);
    if (version != 10) {
        MoggCipher c;
        char err[256] = "";
        size_t need = 20 + (size_t)rd32(file + 16) * 8 + 80;
        if (need > n || !mogg_cipher_init(&c, file, need, err, sizeof(err))) {
            fprintf(stderr, "init: %s\n", err);
            return 1;
        }
        for (size_t at = 0; at < len; at += 0x1000) {
            size_t k = len - at < 0x1000 ? len - at : 0x1000;
            mogg_cipher_xor(&c, plain + at, k, ogg + (uint32_t)at);
        }
        if (!mogg_fix_ogg(&c, plain, len, err, sizeof(err))) {
            fprintf(stderr, "fix: %s\n", err);
            return 1;
        }
    }
    bool ok = write_all(out, plain, len);
    free(plain);
    free(file);
    return ok ? 0 : 1;
}

int main(int argc, char ** argv)
{
    if (argc >= 7 && !strcmp(argv[1], "enc"))
        return encrypt_file(argv[2], argv[3], (uint32_t)atoi(argv[4]), strtoull(argv[5], NULL, 0), strtoull(argv[6], NULL, 0));
    if (argc >= 4 && !strcmp(argv[1], "dec"))
        return decrypt_file(argv[2], argv[3]);
    fprintf(stderr, "usage: moggtest enc <plain.ogg> <out.mogg> <version> <set> <keyindex>\n"
                    "       moggtest dec <in.mogg> <out.ogg>\n");
    return 2;
}
