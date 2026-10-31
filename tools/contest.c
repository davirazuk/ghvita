// Streams a synthetic .mogg through rbcon's block reader, the way a package
// download does, and writes the decrypted Ogg out.
//   contest <raw blocks file> <out.ogg>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/rbcon.c"

bool shrink_mogg(const char * folder)
{
    (void)folder;
    return true;
}

void shrink_song(const char * folder)
{
    (void)folder;
}

int main(int argc, char ** argv)
{
    if (argc < 3)
        return 2;
    Con c;
    memset(&c, 0, sizeof(c));
    c.f = fopen(argv[1], "rb");
    if (!c.f)
        return 1;
    fseek(c.f, 0, SEEK_END);
    long total = ftell(c.f);
    fseek(c.f, 0, SEEK_SET);
    if (total <= 0x1000)
        return 1;
    Entry e;
    memset(&e, 0, sizeof(e));
    strcpy(e.name, "song.mogg");
    e.consecutive = true;
    e.first = 0;
    e.size = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : (uint32_t)(total - 0x1000);
    uint8_t head[20];
    if (!extract(&c, &e, NULL, head, 0, sizeof(head)))
        return 1;
    uint32_t version = head[0] | head[1] << 8 | head[2] << 16 | (uint32_t)head[3] << 24;
    uint32_t ogg = head[4] | head[5] << 8 | head[6] << 16 | (uint32_t)head[7] << 24;
    MoggCipher cipher;
    uint8_t * keys = NULL;
    char err[256] = "";
    if (version != 10) {
        uint32_t hmx = head[16] | head[17] << 8 | head[18] << 16 | (uint32_t)head[19] << 24;
        size_t need = 20 + (size_t)hmx * 8 + 80;
        keys = malloc(need);
        if (!keys || !extract(&c, &e, NULL, keys, 0, (uint32_t)need) ||
            !mogg_cipher_init(&cipher, keys, need, err, sizeof(err))) {
            fprintf(stderr, "init: %s\n", err);
            return 1;
        }
    }
    FILE * out = fopen(argv[2], "wb+");
    if (!out)
        return 1;
    bool ok = version != 10 ? extract_ex(&c, &e, out, NULL, ogg, 0, (XorFn)mogg_cipher_xor, &cipher)
                            : extract(&c, &e, out, NULL, ogg, 0);
    if (version != 10) {
        uint8_t first[64];
        ok = ok && fseek(out, 0, SEEK_SET) == 0 && fread(first, 1, sizeof(first), out) == sizeof(first) &&
             mogg_fix_ogg(&cipher, first, sizeof(first), err, sizeof(err));
        if (ok)
            ok = fseek(out, 0, SEEK_SET) == 0 && fwrite(first, 1, sizeof(first), out) == sizeof(first);
    }
    fclose(out);
    free(keys);
    if (!ok)
        fprintf(stderr, "stream: %s\n", err);
    return ok ? 0 : 1;
}
