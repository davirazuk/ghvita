#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ppmd7.h"
#include "rar.h"

// the most RAR 3 can reach back; the window's a ring of this size
#define WIN_SIZE (4u << 20)
#define WIN_MASK (WIN_SIZE - 1)
// the writer's called this far short of being lapped
#define MAX_MATCH 0x2000

enum { NC = 299, DC = 60, LDC = 17, RC = 28, BC = 20, TABLE_SIZE = NC + DC + LDC + RC, QUICK_BITS = 10 };

typedef struct {
    uint32_t limit[16];         // left-aligned, the first code past each length
    uint32_t pos[16];           // where each length's symbols start in num
    uint16_t quick[1 << QUICK_BITS];    // length << 12 | symbol, for the short codes
    uint16_t num[NC];
    uint32_t size;
} Huff;

// filters come as little programs for RAR's virtual machine, but WinRAR only ever sends these six: they're known by
// their length and CRC and done here
enum { F_NONE, F_E8, F_E8E9, F_ITANIUM, F_DELTA, F_RGB, F_AUDIO };

#define MAX_FILTERS 1024
#define VM_MEM 0x40000u

typedef struct {
    uint32_t start, length, parent, r[7];
    bool next_window;
} Filter;

typedef struct {
    FILE * f, * out;
    uint8_t in[0x10000 + 64];
    uint32_t in_len, in_pos, in_bit;
    uint64_t pack_left;         // of this file, not yet in the buffer
    bool in_ended;

    uint8_t * window;
    uint32_t ptr, wr;           // where it's writing to, and what's reached the file
    uint64_t file_size, written;
    uint32_t crc;
    bool bad, full;

    Huff ld, dd, ldd, rd, bd;
    uint8_t old_table[TABLE_SIZE];
    bool tables_read, ppm_block;
    uint32_t old_dist[4], last_len, low_rep, prev_low;
    int esc_char;

    Ppmd7 * ppm;
    bool ppm_ready;
    uint32_t ppm_low, ppm_code, ppm_range;

    uint8_t prog_type[MAX_FILTERS];
    uint32_t old_lengths[MAX_FILTERS];
    uint32_t prog_count, last_filter;
    Filter stack[MAX_FILTERS];
    bool used[MAX_FILTERS];
    uint32_t stack_size;
    uint8_t * mem;
    bool unknown_filter;
} Rar;

static uint32_t crc_table[256];

static uint32_t crc_update(uint32_t c, const uint8_t * p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t x = i;
            for (int k = 0; k < 8; k++)
                x = x & 1 ? 0xEDB88320u ^ x >> 1 : x >> 1;
            crc_table[i] = x;
        }
    while (n--)
        c = crc_table[(c ^ *p++) & 255] ^ c >> 8;
    return c;
}

// -------------------------------------------------------------------------------------------------------- the bits

static void refill(Rar * r)
{
    uint32_t left = r->in_len - r->in_pos;
    memmove(r->in, r->in + r->in_pos, left);
    r->in_pos = 0;
    uint32_t room = 0x10000 - left;
    if (room > r->pack_left)
        room = (uint32_t)r->pack_left;
    size_t got = room ? fread(r->in + left, 1, room, r->f) : 0;
    r->pack_left -= got;
    r->in_len = left + (uint32_t)got;
    if (got < room)
        r->pack_left = 0;
    memset(r->in + r->in_len, 0, 64);
    r->in_ended = r->pack_left == 0;
}

static uint32_t getbits(const Rar * r)
{
    // past the end there's nothing but zeros
    if (r->in_pos > r->in_len)
        return 0;
    const uint8_t * p = r->in + r->in_pos;
    uint32_t v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    return v >> (8 - r->in_bit) & 0xFFFF;
}

static void addbits(Rar * r, uint32_t n)
{
    n += r->in_bit;
    r->in_pos += n >> 3;
    r->in_bit = n & 7;
}

static bool in_low(const Rar * r)
{
    return r->in_pos + 40 > r->in_len && !r->in_ended;
}

static bool out_of_data(const Rar * r)
{
    return r->in_ended && r->in_pos >= r->in_len;
}

static uint32_t next_byte(Rar * r)
{
    if (in_low(r))
        refill(r);
    return r->in_pos < r->in_len ? r->in[r->in_pos++] : (r->in_pos++, 0);
}

// ----------------------------------------------------------------------------------------------------------- Huffman

static void make_table(Huff * h, const uint8_t * lengths, uint32_t size)
{
    uint32_t count[16] = {0}, next[16], upper = 0;
    for (uint32_t i = 0; i < size; i++)
        count[lengths[i] & 15]++;
    count[0] = 0;
    h->limit[0] = h->pos[0] = 0;
    for (int i = 1; i < 16; i++) {
        upper += count[i];
        h->limit[i] = upper << (16 - i);
        upper *= 2;
        h->pos[i] = h->pos[i - 1] + count[i - 1];
    }
    memcpy(next, h->pos, sizeof(next));
    memset(h->num, 0, size * sizeof(h->num[0]));
    for (uint32_t i = 0; i < size; i++) {
        int len = lengths[i] & 15;
        if (len)
            h->num[next[len]++] = (uint16_t)i;
    }
    h->size = size;
    int len = 1;
    for (uint32_t code = 0; code < 1u << QUICK_BITS; code++) {
        uint32_t field = code << (16 - QUICK_BITS);
        while (len < 16 && field >= h->limit[len])
            len++;
        if (len > QUICK_BITS) {
            h->quick[code] = 0;
            continue;
        }
        uint32_t pos = h->pos[len] + ((field - h->limit[len - 1]) >> (16 - len));
        h->quick[code] = (uint16_t)(len << 12 | (pos < size ? h->num[pos] : 0));
    }
}

static uint32_t decode_symbol(Rar * r, const Huff * h)
{
    uint32_t field = getbits(r) & 0xFFFE, q = h->quick[field >> (16 - QUICK_BITS)];
    if (q) {
        addbits(r, q >> 12);
        return q & 0xFFF;
    }
    uint32_t bits = 15;
    for (uint32_t i = QUICK_BITS + 1; i < 15; i++)
        if (field < h->limit[i]) {
            bits = i;
            break;
        }
    addbits(r, bits);
    uint32_t pos = h->pos[bits] + ((field - h->limit[bits - 1]) >> (16 - bits));
    return pos < h->size ? h->num[pos] : 0;
}

static bool ppm_start(Rar * r);

static bool read_tables(Rar * r)
{
    uint8_t bit_len[BC], table[TABLE_SIZE];
    if (in_low(r))
        refill(r);
    addbits(r, (8 - r->in_bit) & 7);
    uint32_t field = getbits(r);
    if (field & 0x8000) {
#ifdef RAR_TEST
        fprintf(stderr, "PPMd block\n");
#endif
        r->ppm_block = true;
        return ppm_start(r);
    }
    r->ppm_block = false;
    r->prev_low = r->low_rep = 0;
    if (!(field & 0x4000))
        memset(r->old_table, 0, sizeof(r->old_table));
    addbits(r, 2);
    for (int i = 0; i < BC; i++) {
        uint32_t len = getbits(r) >> 12;
        addbits(r, 4);
        if (len == 15) {
            uint32_t zeros = getbits(r) >> 12;
            addbits(r, 4);
            if (!zeros)
                bit_len[i] = 15;
            else {
                zeros += 2;
                while (zeros-- > 0 && i < BC)
                    bit_len[i++] = 0;
                i--;
            }
        } else
            bit_len[i] = (uint8_t)len;
    }
    make_table(&r->bd, bit_len, BC);
    for (int i = 0; i < TABLE_SIZE;) {
        if (in_low(r))
            refill(r);
        uint32_t n = decode_symbol(r, &r->bd);
        if (n < 16) {
            table[i] = (uint8_t)((n + r->old_table[i]) & 15);
            i++;
        } else {
            uint32_t count;
            if (n & 1) {
                count = (getbits(r) >> 9) + 11;
                addbits(r, 7);
            } else {
                count = (getbits(r) >> 13) + 3;
                addbits(r, 3);
            }
            if (n < 18) {
                if (!i)
                    return false;
                while (count-- > 0 && i < TABLE_SIZE) {
                    table[i] = table[i - 1];
                    i++;
                }
            } else
                while (count-- > 0 && i < TABLE_SIZE)
                    table[i++] = 0;
        }
    }
    r->tables_read = true;
    make_table(&r->ld, table, NC);
    make_table(&r->dd, table + NC, DC);
    make_table(&r->ldd, table + NC + DC, LDC);
    make_table(&r->rd, table + NC + DC + LDC, RC);
    memcpy(r->old_table, table, sizeof(table));
    return true;
}

// ---------------------------------------------------------------------------------------------------------- filters

static uint32_t rd32(const uint8_t * p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void filter_e8(uint8_t * mem, uint32_t size, uint32_t file_pos, bool e9)
{
    if (size < 4 || size > VM_MEM)
        return;
    for (uint32_t cur = 0; cur < size - 4;) {
        uint8_t c = mem[cur++];
        if (c == 0xE8 || (e9 && c == 0xE9)) {
            uint32_t offset = cur + file_pos, addr = rd32(mem + cur);
            if (addr & 0x80000000u) {
                if (!((addr + offset) & 0x80000000u))
                    wr32(mem + cur, addr + 0x1000000);
            } else if ((addr - 0x1000000) & 0x80000000u)
                wr32(mem + cur, addr - offset);
            cur += 4;
        }
    }
}

// the rest leave their result after the input
static void filter_delta(uint8_t * mem, uint32_t size, uint32_t channels)
{
    uint32_t src = 0;
    for (uint32_t c = 0; c < channels; c++) {
        uint8_t prev = 0;
        for (uint32_t d = size + c; d < size * 2; d += channels)
            mem[d] = prev = (uint8_t)(prev - mem[src++]);
    }
}

static void filter_rgb(uint8_t * mem, uint32_t size, uint32_t width, uint32_t pos_r)
{
    uint8_t * src = mem, * dst = mem + size;
    for (uint32_t c = 0; c < 3; c++) {
        uint32_t prev = 0;
        for (uint32_t i = c; i < size; i += 3) {
            uint32_t predicted = prev;
            if (i >= width + 3) {
                const uint8_t * up = dst + i - width;
                uint32_t upper = *up, upper_left = *(up - 3);
                predicted = prev + upper - upper_left;
                int pa = abs((int)(predicted - prev)), pb = abs((int)(predicted - upper)),
                    pc = abs((int)(predicted - upper_left));
                predicted = pa <= pb && pa <= pc ? prev : pb <= pc ? upper : upper_left;
            }
            prev = dst[i] = (uint8_t)(predicted - *src++);
        }
    }
    for (uint32_t i = pos_r; i + 2 < size; i += 3) {
        uint8_t g = dst[i + 1];
        dst[i] = (uint8_t)(dst[i] + g);
        dst[i + 2] = (uint8_t)(dst[i + 2] + g);
    }
}

static void filter_audio(uint8_t * mem, uint32_t size, uint32_t channels)
{
    uint8_t * src = mem, * dst = mem + size;
    for (uint32_t c = 0; c < channels; c++) {
        uint32_t prev_byte = 0, prev_delta = 0, dif[7] = {0}, count = 0;
        int d1 = 0, d2 = 0, d3, k1 = 0, k2 = 0, k3 = 0;
        for (uint32_t i = c; i < size; i += channels, count++) {
            d3 = d2;
            d2 = (int)prev_delta - d1;
            d1 = (int)prev_delta;
            uint32_t predicted = 8 * prev_byte + (uint32_t)(k1 * d1 + k2 * d2 + k3 * d3);
            predicted = predicted >> 3 & 255;
            uint32_t cur = *src++;
            predicted -= cur;
            dst[i] = (uint8_t)predicted;
            prev_delta = (uint32_t)(int)(signed char)(predicted - prev_byte);
            prev_byte = predicted;
            int d = (int)(signed char)cur * 8;
            dif[0] += (uint32_t)abs(d);
            dif[1] += (uint32_t)abs(d - d1);
            dif[2] += (uint32_t)abs(d + d1);
            dif[3] += (uint32_t)abs(d - d2);
            dif[4] += (uint32_t)abs(d + d2);
            dif[5] += (uint32_t)abs(d - d3);
            dif[6] += (uint32_t)abs(d + d3);
            if (!(count & 31)) {
                uint32_t least = dif[0], which = 0;
                dif[0] = 0;
                for (uint32_t j = 1; j < 7; j++) {
                    if (dif[j] < least) {
                        least = dif[j];
                        which = j;
                    }
                    dif[j] = 0;
                }
                switch (which) {
                case 1: if (k1 >= -16) k1--; break;
                case 2: if (k1 < 16) k1++; break;
                case 3: if (k2 >= -16) k2--; break;
                case 4: if (k2 < 16) k2++; break;
                case 5: if (k3 >= -16) k3--; break;
                case 6: if (k3 < 16) k3++; break;
                }
            }
        }
    }
}

// Runs a filter over mem; what it made is at *data
static bool run_filter(Rar * r, const Filter * f, const uint8_t ** data, uint32_t * size)
{
    uint32_t n = f->length;
    *data = r->mem;
    *size = n;
    switch (r->prog_type[f->parent]) {
    case F_E8:
    case F_E8E9:
        filter_e8(r->mem, n, (uint32_t)r->written, r->prog_type[f->parent] == F_E8E9);
        return true;
    case F_DELTA:
        if (n > VM_MEM / 2 || !f->r[0] || f->r[0] > 32)
            return false;
        filter_delta(r->mem, n, f->r[0]);
        break;
    case F_RGB:
        if (n > VM_MEM / 2 || n < 3 || f->r[0] < 3 || f->r[0] - 3 > n || f->r[1] > 2)
            return false;
        filter_rgb(r->mem, n, f->r[0] - 3, f->r[1]);
        break;
    case F_AUDIO:
        if (n > VM_MEM / 2 || !f->r[0] || f->r[0] > 128)
            return false;
        filter_audio(r->mem, n, f->r[0]);
        break;
    default:
        r->unknown_filter = true;
        return false;
    }
    *data = r->mem + n;
    return true;
}

static void put_out(Rar * r, const uint8_t * p, uint32_t n)
{
    if (r->written + n > r->file_size) {
        r->bad = true;
        n = r->written >= r->file_size ? 0 : (uint32_t)(r->file_size - r->written);
    }
    if (!n)
        return;
    r->crc = crc_update(r->crc, p, n);
    r->written += n;
    if (r->out && fwrite(p, 1, n, r->out) != n)
        r->bad = r->full = true;
}

static void write_area(Rar * r, uint32_t from, uint32_t to)
{
    if (to < from) {
        put_out(r, r->window + from, WIN_SIZE - from);
        from = 0;
    }
    if (to > from)
        put_out(r, r->window + from, to - from);
}

static void to_mem(Rar * r, uint32_t start, uint32_t length)
{
    uint32_t end = (start + length) & WIN_MASK;
    if (start < end || !end)
        memcpy(r->mem, r->window + start, length);
    else {
        memcpy(r->mem, r->window + start, WIN_SIZE - start);
        memcpy(r->mem + (WIN_SIZE - start), r->window, end);
    }
}

// writes what's finished; a filter's block has to be whole in the window first
static void write_buffer(Rar * r)
{
    uint32_t border = r->wr, size = (r->ptr - border) & WIN_MASK;
    for (uint32_t i = 0; i < r->stack_size; i++) {
        Filter * f = &r->stack[i];
        if (!r->used[i])
            continue;
        if (f->next_window) {
            f->next_window = false;
            continue;
        }
        uint32_t start = f->start, length = f->length;
        if (((start - border) & WIN_MASK) >= size)
            continue;
        if (border != start) {
            write_area(r, border, start);
            border = start;
            size = (r->ptr - border) & WIN_MASK;
        }
        if (length > size) {
            // it runs past what's decoded: next time, then
            for (uint32_t j = i; j < r->stack_size; j++)
                if (r->used[j])
                    r->stack[j].next_window = false;
            r->wr = border;
            return;
        }
        uint32_t end = (start + length) & WIN_MASK;
        const uint8_t * data;
        uint32_t made;
        if (length > VM_MEM) {
            r->bad = true;
            return;
        }
        to_mem(r, start, length);
        if (!run_filter(r, f, &data, &made)) {
            r->bad = true;
            return;
        }
        r->used[i] = false;
        // several filters can work on the same block, each on what the last made
        while (i + 1 < r->stack_size) {
            Filter * next = &r->stack[i + 1];
            if (!r->used[i + 1] || next->start != end || next->length != made || next->next_window)
                break;
            memmove(r->mem, data, made);
            if (!run_filter(r, next, &data, &made)) {
                r->bad = true;
                return;
            }
            i++;
            r->used[i] = false;
        }
        put_out(r, data, made);
        border = end;
        size = (r->ptr - border) & WIN_MASK;
    }
    write_area(r, border, r->ptr);
    r->wr = r->ptr;
}

typedef struct {
    const uint8_t * p;
    uint32_t size, pos, bit;
} Bits;

static uint32_t vm_bits(const Bits * b)
{
    uint32_t at = b->pos, v = 0;
    for (int i = 0; i < 3; i++, at++)
        v = v << 8 | (at < b->size ? b->p[at] : 0);
    return v >> (8 - b->bit) & 0xFFFF;
}

static void vm_add(Bits * b, uint32_t n)
{
    n += b->bit;
    b->pos += n >> 3;
    b->bit = n & 7;
}

static uint32_t vm_data(Bits * b)
{
    uint32_t data = vm_bits(b);
    switch (data & 0xC000) {
    case 0:
        vm_add(b, 6);
        return data >> 10 & 15;
    case 0x4000:
        if (!(data & 0x3C00)) {
            data = 0xFFFFFF00u | (data >> 2 & 255);
            vm_add(b, 14);
        } else {
            data = data >> 6 & 255;
            vm_add(b, 10);
        }
        return data;
    case 0x8000:
        vm_add(b, 2);
        data = vm_bits(b);
        vm_add(b, 16);
        return data;
    default:
        vm_add(b, 2);
        data = vm_bits(b) << 16;
        vm_add(b, 16);
        data |= vm_bits(b);
        vm_add(b, 16);
        return data;
    }
}

static uint8_t filter_type(const uint8_t * code, uint32_t size)
{
    static const struct { uint32_t length, crc; uint8_t type; } known[] = {
        {53, 0xAD576887u, F_E8}, {57, 0x3CD7E57Eu, F_E8E9}, {120, 0x3769893Fu, F_ITANIUM},
        {29, 0x0E06077Du, F_DELTA}, {149, 0x1C2C5DC8u, F_RGB}, {216, 0xBC85E701u, F_AUDIO},
    };
    uint32_t crc = crc_update(0xFFFFFFFFu, code, size) ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (known[i].length == size && known[i].crc == crc)
            return known[i].type;
    return F_NONE;
}

static void reset_filters(Rar * r, bool all)
{
    if (all) {
        r->prog_count = r->last_filter = 0;
        memset(r->old_lengths, 0, sizeof(r->old_lengths));
    }
    r->stack_size = 0;
    memset(r->used, 0, sizeof(r->used));
}

static bool add_filter(Rar * r, uint32_t first, const uint8_t * code, uint32_t size)
{
    Bits b = {code, size, 0, 0};
    uint32_t pos;
    if (first & 0x80) {
        pos = vm_data(&b);
        if (!pos)
            reset_filters(r, true);
        else
            pos--;
    } else
        pos = r->last_filter;
    if (pos > r->prog_count || pos >= MAX_FILTERS)
        return false;
    r->last_filter = pos;
    bool is_new = pos == r->prog_count;
    if (is_new) {
        r->prog_type[pos] = F_NONE;
        r->old_lengths[pos] = 0;
        r->prog_count++;
    }
    // the first free place, keeping their order
    uint32_t k = 0;
    for (uint32_t i = 0; i < r->stack_size; i++)
        if (r->used[i]) {
            if (k != i) {
                r->stack[k] = r->stack[i];
                r->used[k] = true;
                r->used[i] = false;
            }
            k++;
        }
    if (k >= MAX_FILTERS)
        return false;
    r->stack_size = k + 1;
    Filter * f = &r->stack[k];
    r->used[k] = true;
    memset(f, 0, sizeof(*f));
    f->parent = pos;
    uint32_t start = vm_data(&b);
    if (first & 0x40)
        start += 258;
    f->start = (start + r->ptr) & WIN_MASK;
    if (first & 0x20) {
        f->length = vm_data(&b);
        r->old_lengths[pos] = f->length;
    } else
        f->length = r->old_lengths[pos];
    f->next_window = r->wr != r->ptr && ((r->wr - r->ptr) & WIN_MASK) <= start;
    f->r[4] = f->length;
    if (first & 0x10) {
        uint32_t mask = vm_bits(&b) >> 9;
        vm_add(&b, 7);
        for (uint32_t i = 0; i < 7; i++)
            if (mask & 1u << i)
                f->r[i] = vm_data(&b);
    }
    if (is_new) {
        uint32_t n = vm_data(&b);
        if (n >= 0x10000 || !n || b.pos + n > size)
            return false;
        uint8_t * prog = malloc(n);
        if (!prog)
            return false;
        for (uint32_t i = 0; i < n; i++) {
            prog[i] = (uint8_t)(vm_bits(&b) >> 8);
            vm_add(&b, 8);
        }
        r->prog_type[pos] = filter_type(prog, n);
#ifdef RAR_TEST
        fprintf(stderr, "filter type %d\n", r->prog_type[pos]);
#endif
        free(prog);
    }
    return true;
}

static bool read_filter(Rar * r)
{
    if (in_low(r))
        refill(r);
    uint32_t first = getbits(r) >> 8, length;
    addbits(r, 8);
    length = (first & 7) + 1;
    if (length == 7) {
        length = (getbits(r) >> 8) + 7;
        addbits(r, 8);
    } else if (length == 8) {
        length = getbits(r);
        addbits(r, 16);
    }
    if (!length)
        return false;
    uint8_t * code = malloc(length);
    if (!code)
        return false;
    for (uint32_t i = 0; i < length; i++) {
        if (in_low(r))
            refill(r);
        code[i] = (uint8_t)(getbits(r) >> 8);
        addbits(r, 8);
    }
    bool ok = add_filter(r, first, code, length);
    free(code);
    return ok;
}

// ------------------------------------------------------------------------------------------------------------- PPMd

// RAR's own range coder over the shared model
static uint32_t ppm_threshold(void * user, uint32_t total)
{
    Rar * r = user;
    return (r->ppm_code - r->ppm_low) / (r->ppm_range /= total);
}

static void ppm_normalize(Rar * r)
{
    for (;;) {
        if (((r->ppm_low ^ (r->ppm_low + r->ppm_range)) >= 1u << 24)) {
            if (r->ppm_range >= 1u << 15)
                return;
            r->ppm_range = (0u - r->ppm_low) & ((1u << 15) - 1);
        }
        r->ppm_code = r->ppm_code << 8 | next_byte(r);
        r->ppm_range <<= 8;
        r->ppm_low <<= 8;
    }
}

static void ppm_decode(void * user, uint32_t start, uint32_t size)
{
    Rar * r = user;
    r->ppm_low += start * r->ppm_range;
    r->ppm_range *= size;
    ppm_normalize(r);
}

static uint32_t ppm_bit(void * user, uint32_t size0, uint32_t total_bits)
{
    Rar * r = user;
    uint32_t value = (r->ppm_code - r->ppm_low) / (r->ppm_range >>= total_bits);
    uint32_t bit;
    if (value < size0) {
        bit = 0;
        r->ppm_range *= size0;
    } else {
        bit = 1;
        r->ppm_low += size0 * r->ppm_range;
        r->ppm_range *= (1u << total_bits) - size0;
    }
    ppm_normalize(r);
    return bit;
}

static bool ppm_start(Rar * r)
{
    uint32_t flags = next_byte(r);
    bool reset = (flags & 0x20) != 0;
    uint32_t mb = 0;
    if (reset)
        mb = next_byte(r);
    else if (!r->ppm_ready)
        return false;
    if (flags & 0x40)
        r->esc_char = (int)next_byte(r);
    r->ppm_low = r->ppm_code = 0;
    r->ppm_range = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++)
        r->ppm_code = r->ppm_code << 8 | next_byte(r);
    if (reset) {
        uint32_t order = (flags & 0x1F) + 1;
        if (order > 16)
            order = 16 + (order - 16) * 3;
        r->ppm_ready = false;
        if (order == 1)
            return false;
        if (!r->ppm && !(r->ppm = ppmd7_new()))
            return false;
        if (!ppmd7_alloc(r->ppm, (mb + 1) << 20))
            return false;
        ppmd7_init(r->ppm, order);
        r->ppm_ready = true;
    }
    return r->ppm_ready;
}

static const Ppmd7Coder ppm_coder = {ppm_threshold, ppm_decode, ppm_bit};

static int ppm_char(Rar * r)
{
    return ppmd7_decode(r->ppm, &ppm_coder, r);
}

// ---------------------------------------------------------------------------------------------------------- the LZ

static void copy_string(Rar * r, uint32_t length, uint32_t distance)
{
    uint32_t src = r->ptr - distance, dst = r->ptr;
    uint8_t * w = r->window;
    if (src < WIN_SIZE && src + length <= WIN_SIZE && dst + length <= WIN_SIZE) {
        for (uint32_t i = 0; i < length; i++)
            w[dst + i] = w[src + i];
    } else
        for (uint32_t i = 0; i < length; i++)
            w[(dst + i) & WIN_MASK] = w[(src + i) & WIN_MASK];
    r->ptr = (dst + length) & WIN_MASK;
}

static void old_dist(Rar * r, uint32_t d)
{
    r->old_dist[3] = r->old_dist[2];
    r->old_dist[2] = r->old_dist[1];
    r->old_dist[1] = r->old_dist[0];
    r->old_dist[0] = d;
}

// true: on with the next file
static bool end_of_block(Rar * r, bool * more)
{
    uint32_t field = getbits(r);
    bool new_table, new_file = false;
    if (field & 0x8000) {
        new_table = true;
        addbits(r, 1);
    } else {
        new_file = true;
        new_table = (field & 0x4000) != 0;
        addbits(r, 2);
    }
    r->tables_read = !new_table;
    if (new_file) {
        *more = false;
        return true;
    }
    return read_tables(r);
}

static uint32_t ddecode[DC];
static uint8_t dbits[DC];

static void unpack_file(Rar * r, bool solid)
{
    static const uint8_t ldecode[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112,
                                      128, 160, 192, 224};
    static const uint8_t lbits[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};
    static const uint8_t counts[] = {4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 14, 0, 12};
    static const uint8_t sddecode[] = {0, 4, 8, 16, 32, 64, 128, 192};
    static const uint8_t sdbits[] = {2, 2, 3, 4, 5, 6, 6, 6};
    if (!ddecode[1]) {
        uint32_t dist = 0;
        int slot = 0;
        for (int len = 0; len < (int)sizeof(counts); len++)
            for (int j = 0; j < counts[len]; j++, slot++, dist += 1u << len) {
                ddecode[slot] = dist;
                dbits[slot] = (uint8_t)len;
            }
    }
    r->in_pos = r->in_len = r->in_bit = 0;
    r->written = 0;
    r->crc = 0xFFFFFFFFu;
    r->wr = r->ptr;
    if (!solid) {
        memset(r->old_dist, 0, sizeof(r->old_dist));
        r->last_len = 0;
        r->tables_read = false;
        memset(r->old_table, 0, sizeof(r->old_table));
        r->esc_char = 2;
        r->ppm_block = false;
    }
    reset_filters(r, !solid);
    refill(r);
    if ((!solid || !r->tables_read) && !read_tables(r)) {
        r->bad = true;
        return;
    }
    bool more = true;
    while (more && !r->bad) {
        if (in_low(r))
            refill(r);
        if (out_of_data(r) && r->in_pos > r->in_len + 8)
            break;
        uint32_t left = (r->wr - r->ptr) & WIN_MASK;
        if (left <= MAX_MATCH && r->wr != r->ptr)
            write_buffer(r);
        if (r->ppm_block) {
            int ch = ppm_char(r);
            if (ch < 0) {
                r->bad = true;
                break;
            }
            if (ch == r->esc_char) {
                int next = ppm_char(r);
                if (next < 0)
                    break;
                if (next == 0) {
                    if (!read_tables(r))
                        r->bad = true;
                    continue;
                }
                if (next == 2)
                    break;
                if (next == 3) {
                    // a filter's code, as bytes
                    int f = ppm_char(r);
                    if (f < 0)
                        break;
                    uint32_t first = (uint32_t)f, length = (first & 7) + 1;
                    if (length == 7)
                        length = (uint32_t)ppm_char(r) + 7;
                    else if (length == 8) {
                        length = (uint32_t)ppm_char(r) * 256;
                        length += (uint32_t)ppm_char(r);
                    }
                    uint8_t * code = length ? malloc(length) : NULL;
                    if (!code) {
                        r->bad = true;
                        break;
                    }
                    for (uint32_t i = 0; i < length; i++)
                        code[i] = (uint8_t)ppm_char(r);
                    if (!add_filter(r, first, code, length))
                        r->bad = true;
                    free(code);
                    continue;
                }
                if (next == 4) {
                    uint32_t distance = 0;
                    for (int i = 0; i < 3; i++)
                        distance = distance << 8 | (uint8_t)ppm_char(r);
                    uint32_t length = (uint8_t)ppm_char(r);
                    copy_string(r, length + 32, distance + 2);
                    continue;
                }
                if (next == 5) {
                    uint32_t length = (uint8_t)ppm_char(r);
                    copy_string(r, length + 4, 1);
                    continue;
                }
            }
            r->window[r->ptr] = (uint8_t)ch;
            r->ptr = (r->ptr + 1) & WIN_MASK;
            continue;
        }
        uint32_t n = decode_symbol(r, &r->ld), bits;
        if (n < 256) {
            r->window[r->ptr] = (uint8_t)n;
            r->ptr = (r->ptr + 1) & WIN_MASK;
            continue;
        }
        if (n >= 271) {
            n -= 271;
            uint32_t length = ldecode[n] + 3u;
            if ((bits = lbits[n]) > 0) {
                length += getbits(r) >> (16 - bits);
                addbits(r, bits);
            }
            uint32_t slot = decode_symbol(r, &r->dd);
            uint32_t distance = ddecode[slot] + 1;
            if ((bits = dbits[slot]) > 0) {
                if (slot > 9) {
                    if (bits > 4) {
                        distance += (getbits(r) >> (20 - bits)) << 4;
                        addbits(r, bits - 4);
                    }
                    if (r->low_rep > 0) {
                        r->low_rep--;
                        distance += r->prev_low;
                    } else {
                        uint32_t low = decode_symbol(r, &r->ldd);
                        if (low == 16) {
                            r->low_rep = 15;
                            distance += r->prev_low;
                        } else {
                            distance += low;
                            r->prev_low = low;
                        }
                    }
                } else {
                    distance += getbits(r) >> (16 - bits);
                    addbits(r, bits);
                }
            }
            if (distance >= 0x2000) {
                length++;
                if (distance >= 0x40000)
                    length++;
            }
            old_dist(r, distance);
            r->last_len = length;
            copy_string(r, length, distance);
            continue;
        }
        if (n == 256) {
            if (!end_of_block(r, &more))
                r->bad = true;
            continue;
        }
        if (n == 257) {
            if (!read_filter(r))
                r->bad = true;
            continue;
        }
        if (n == 258) {
            if (r->last_len)
                copy_string(r, r->last_len, r->old_dist[0]);
            continue;
        }
        if (n < 263) {
            uint32_t which = n - 259, distance = r->old_dist[which];
            for (uint32_t i = which; i > 0; i--)
                r->old_dist[i] = r->old_dist[i - 1];
            r->old_dist[0] = distance;
            uint32_t len_n = decode_symbol(r, &r->rd), length = ldecode[len_n] + 2u;
            if ((bits = lbits[len_n]) > 0) {
                length += getbits(r) >> (16 - bits);
                addbits(r, bits);
            }
            r->last_len = length;
            copy_string(r, length, distance);
            continue;
        }
        n -= 263;
        uint32_t distance = sddecode[n] + 1u;
        if ((bits = sdbits[n]) > 0) {
            distance += getbits(r) >> (16 - bits);
            addbits(r, bits);
        }
        old_dist(r, distance);
        r->last_len = 2;
        copy_string(r, 2, distance);
    }
    write_buffer(r);
}

// --------------------------------------------------------------------------------------------------------- the archive

static uint32_t rd16(const uint8_t * p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

// a RAR 3 name: plain bytes, or plain bytes and a zero and then how the wide characters differ from them
static void read_name(const uint8_t * raw, uint32_t n, bool unicode, char * out, size_t size)
{
    const uint8_t * zero = unicode ? memchr(raw, 0, n) : NULL;
    size_t k = 0;
    if (!zero) {
        // (with the flag and no zero it's UTF-8 already)
        for (uint32_t i = 0; i < n && k + 1 < size && raw[i]; i++)
            out[k++] = (char)raw[i];
        out[k] = 0;
        return;
    }
    uint32_t plain = (uint32_t)(zero - raw), at = plain + 1, count = 0, flags = 0, flag_bits = 0;
    uint16_t wide[512];
    uint32_t high = at < n ? raw[at++] : 0;
    while (at < n && count < 512) {
        if (!flag_bits) {
            flags = raw[at++];
            flag_bits = 8;
        }
        switch (flags >> 6 & 3) {
        case 0:
            if (at < n)
                wide[count++] = raw[at++];
            break;
        case 1:
            if (at < n)
                wide[count++] = (uint16_t)(raw[at++] | high << 8);
            break;
        case 2:
            if (at + 1 < n) {
                wide[count++] = (uint16_t)(raw[at] | raw[at + 1] << 8);
                at += 2;
            } else
                at = n;
            break;
        default: {
            uint32_t length = at < n ? raw[at++] : 0;
            if (length & 0x80) {
                uint32_t fix = at < n ? raw[at++] : 0;
                for (length = (length & 0x7F) + 2; length > 0 && count < 512; length--, count++)
                    wide[count] = (uint16_t)((((count < plain ? raw[count] : 0) + fix) & 255) | high << 8);
            } else
                for (length += 2; length > 0 && count < 512; length--, count++)
                    wide[count] = count < plain ? raw[count] : 0;
            break;
        }
        }
        flags = flags << 2 & 255;
        flag_bits -= 2;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = wide[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) {
            c = 0x10000 + ((c - 0xD800) << 10) + (wide[++i] & 0x3FF);
        }
        if (k + 5 > size)
            break;
        if (c < 0x80)
            out[k++] = (char)c;
        else if (c < 0x800) {
            out[k++] = (char)(0xC0 | c >> 6);
            out[k++] = (char)(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out[k++] = (char)(0xE0 | c >> 12);
            out[k++] = (char)(0x80 | (c >> 6 & 63));
            out[k++] = (char)(0x80 | (c & 63));
        } else {
            out[k++] = (char)(0xF0 | c >> 18);
            out[k++] = (char)(0x80 | (c >> 12 & 63));
            out[k++] = (char)(0x80 | (c >> 6 & 63));
            out[k++] = (char)(0x80 | (c & 63));
        }
    }
    out[k] = 0;
}

static bool store_file(Rar * r, uint64_t size)
{
    static uint8_t buf[0x8000];
    r->crc = 0xFFFFFFFFu;
    r->written = 0;
    r->file_size = size;
    while (size) {
        size_t n = size < sizeof(buf) ? (size_t)size : sizeof(buf);
        if (fread(buf, 1, n, r->f) != n)
            return false;
        put_out(r, buf, (uint32_t)n);
        size -= n;
    }
    return !r->bad;
}

bool rar_extract(const char * file, FILE * (*open_out)(void * user, const char * name),
                 void (*close_out)(void * user, FILE * f), void * user, char * err, size_t err_size)
{
    static const uint8_t marker[7] = {'R', 'a', 'r', '!', 0x1A, 7, 0};
    Rar * r = calloc(1, sizeof(Rar));
    uint8_t * head = malloc(0x10000);
    bool ok = false, went_on = false;
    if (r) {
        r->window = malloc(WIN_SIZE);
        r->mem = malloc(VM_MEM + 8);
        r->f = fopen(file, "rb");
    }
    if (!r || !head || !r->window || !r->mem) {
        snprintf(err, err_size, "not enough memory for the RAR");
        goto done;
    }
    uint8_t start[7];
    if (!r->f || fread(start, 1, 7, r->f) != 7 || memcmp(start, marker, 7)) {
        snprintf(err, err_size, "it's not a RAR 3 or 4 archive");
        goto done;
    }
    long at = 7;
    for (;;) {
        if (fseek(r->f, at, SEEK_SET) != 0 || fread(head, 1, 7, r->f) != 7)
            break;
        uint32_t type = head[2], flags = rd16(head + 3), size = rd16(head + 5);
        if (size < 7)
            break;
        if (fread(head + 7, 1, size - 7, r->f) != size - 7)
            break;
        if (type == 0x7B)
            break;
        if (type == 0x73 && (flags & 0x80)) {
            snprintf(err, err_size, "the RAR is locked with a password");
            goto done;
        }
        if (type != 0x74) {
            at += size;
            if ((flags & 0x8000) && size >= 11)
                at += (long)rd32(head + 7);
            continue;
        }
        uint64_t pack = rd32(head + 7), unp = rd32(head + 11);
        uint32_t crc = rd32(head + 16), name_size = rd16(head + 26), off = 32;
        uint8_t version = head[24], method = head[25];
        if (flags & 0x100) {
            pack |= (uint64_t)rd32(head + 32) << 32;
            unp |= (uint64_t)rd32(head + 36) << 32;
            off += 8;
        }
        if (off + name_size > size) {
            snprintf(err, err_size, "the archive is damaged");
            goto done;
        }
        long data = at + (long)size;
        at = data + (long)pack;
        if ((flags & 0xE0) == 0xE0)
            continue;               // a folder
        if (flags & 4) {
            snprintf(err, err_size, "the RAR is locked with a password");
            goto done;
        }
        if (flags & 3) {
            snprintf(err, err_size, "the RAR is split over several files");
            goto done;
        }
        char name[512];
        read_name(head + off, name_size, (flags & 0x200) != 0, name, sizeof(name));
        r->out = open_out(user, name);
        fseek(r->f, data, SEEK_SET);
        r->bad = false;
        if (method == 0x30)
            store_file(r, unp);
        else if (version == 29) {
            r->pack_left = pack;
            r->file_size = unp;
            r->in_ended = false;
            unpack_file(r, (flags & 0x10) && went_on);
            went_on = true;
        } else {
            if (r->out)
                close_out(user, r->out);
            snprintf(err, err_size, "this RAR is in an old format (%d)", version);
            goto done;
        }
        if (r->out)
            close_out(user, r->out);
        r->out = NULL;
        if (r->unknown_filter) {
            snprintf(err, err_size, "this RAR uses a filter that isn't known here");
            goto done;
        }
        if (r->full) {
            snprintf(err, err_size, "the memory card is full");
            goto done;
        }
        if (r->bad || r->written != unp || (r->crc ^ 0xFFFFFFFFu) != crc) {
            snprintf(err, err_size, "couldn't unpack %s: the archive is damaged", name);
            goto done;
        }
    }
    ok = true;
done:
    if (r) {
        if (r->f)
            fclose(r->f);
        ppmd7_free(r->ppm);
        free(r->window);
        free(r->mem);
        free(r);
    }
    free(head);
    return ok;
}
