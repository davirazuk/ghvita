#include <stdlib.h>
#include <string.h>
#include "ppmd7.h"

// the model after Dmitry Shkarin's variant H and Igor Pavlov's public-domain Ppmd7: memory is one block cut into 12-byte
// units, with contexts growing down from the top and text up from the bottom, all linked by offsets

#define INT_BITS 7
#define PERIOD_BITS 7
#define BIN_BITS (INT_BITS + PERIOD_BITS)
#define BIN_SCALE (1 << BIN_BITS)
#define MAX_FREQ 124
#define UNIT_SIZE 12
#define NUM_INDEXES 38
#define MAX_ORDER 64

typedef uint32_t Ref;

typedef struct {
    uint16_t summ;
    uint8_t shift, count;
} See;

typedef struct {
    uint8_t symbol, freq;
    uint16_t succ_low, succ_high;
} State;

typedef struct {
    uint16_t num_stats, summ_freq;
    Ref stats, suffix;
} Context;

typedef struct {
    uint16_t stamp, nu;
    Ref next, prev;
} Node;

struct Ppmd7 {
    Context * min_context, * max_context;
    State * found;
    unsigned order_fall, init_esc, prev_success, max_order, hi_bits_flag;
    int32_t run_length, init_rl;
    uint32_t size, glue_count;
    uint8_t * base, * lo_unit, * hi_unit, * text, * units_start;
    uint32_t align_offset;
    uint8_t indx2units[NUM_INDEXES], units2indx[128];
    Ref free_list[NUM_INDEXES];
    uint8_t ns2indx[256], ns2bs_indx[256], hb2flag[256];
    See dummy_see, see[25][16];
    uint16_t bin_summ[128][64];
};

#define REF(ptr) ((Ref)((uint8_t *)(ptr) - p->base))
#define AT(type, ref) ((type *)(p->base + (ref)))
#define CTX(ref) AT(Context, ref)
#define STATS(c) AT(State, (c)->stats)
#define NODE(ref) AT(Node, ref)
#define SUFFIX(c) CTX((c)->suffix)
#define ONE_STATE(c) ((State *)&(c)->summ_freq)
#define U2B(nu) ((uint32_t)(nu) * UNIT_SIZE)
#define U2I(nu) (p->units2indx[(nu) - 1])
#define I2U(i) (p->indx2units[i])

static Ref successor(const State * s)
{
    return s->succ_low | (Ref)s->succ_high << 16;
}

static void set_successor(State * s, Ref v)
{
    s->succ_low = (uint16_t)v;
    s->succ_high = (uint16_t)(v >> 16);
}

Ppmd7 * ppmd7_new(void)
{
    Ppmd7 * p = calloc(1, sizeof(Ppmd7));
    if (!p)
        return NULL;
    unsigned i, k, m;
    for (i = 0, k = 0; i < NUM_INDEXES; i++) {
        unsigned step = i >= 12 ? 4 : (i >> 2) + 1;
        do
            p->units2indx[k++] = (uint8_t)i;
        while (--step);
        p->indx2units[i] = (uint8_t)k;
    }
    p->ns2bs_indx[0] = 0 << 1;
    p->ns2bs_indx[1] = 1 << 1;
    memset(p->ns2bs_indx + 2, 2 << 1, 9);
    memset(p->ns2bs_indx + 11, 3 << 1, 256 - 11);
    for (i = 0; i < 3; i++)
        p->ns2indx[i] = (uint8_t)i;
    for (m = i, k = 1; i < 256; i++) {
        p->ns2indx[i] = (uint8_t)m;
        if (--k == 0)
            k = ++m - 2;
    }
    memset(p->hb2flag, 0, 0x40);
    memset(p->hb2flag + 0x40, 8, 0x100 - 0x40);
    return p;
}

void ppmd7_free(Ppmd7 * p)
{
    if (p) {
        free(p->base);
        free(p);
    }
}

bool ppmd7_alloc(Ppmd7 * p, uint32_t size)
{
    if (p->base && p->size == size)
        return true;
    free(p->base);
    p->align_offset = 4 - (size & 3);
    p->base = malloc((size_t)p->align_offset + size + UNIT_SIZE);
    p->size = p->base ? size : 0;
    return p->base != NULL;
}

// --------------------------------------------------------------------------------------------------- the allocator

static void insert_node(Ppmd7 * p, void * node, unsigned i)
{
    *(Ref *)node = p->free_list[i];
    p->free_list[i] = REF(node);
}

static void * remove_node(Ppmd7 * p, unsigned i)
{
    Ref * node = AT(Ref, p->free_list[i]);
    p->free_list[i] = *node;
    return node;
}

static void split_block(Ppmd7 * p, void * ptr, unsigned old_i, unsigned new_i)
{
    unsigned i, nu = I2U(old_i) - I2U(new_i);
    ptr = (uint8_t *)ptr + U2B(I2U(new_i));
    if (I2U(i = U2I(nu)) != nu) {
        unsigned k = I2U(--i);
        insert_node(p, (uint8_t *)ptr + U2B(k), nu - k - 1);
    }
    insert_node(p, ptr, i);
}

static void glue_free_blocks(Ppmd7 * p)
{
    Ref head = p->align_offset + p->size, n = head;
    p->glue_count = 255;
    // the free blocks as one doubly linked list
    for (unsigned i = 0; i < NUM_INDEXES; i++) {
        uint16_t nu = I2U(i);
        Ref next = p->free_list[i];
        p->free_list[i] = 0;
        while (next) {
            Node * node = NODE(next);
            node->next = n;
            n = NODE(n)->prev = next;
            next = *(const Ref *)node;
            node->stamp = 0;
            node->nu = nu;
        }
    }
    NODE(head)->stamp = 1;
    NODE(head)->next = n;
    NODE(n)->prev = head;
    if (p->lo_unit != p->hi_unit)
        ((Node *)p->lo_unit)->stamp = 1;
    // neighbours join up
    while (n != head) {
        Node * node = NODE(n);
        uint32_t nu = node->nu;
        for (;;) {
            Node * node2 = NODE(n) + nu;
            nu += node2->nu;
            if (node2->stamp || nu >= 0x10000)
                break;
            NODE(node2->prev)->next = node2->next;
            NODE(node2->next)->prev = node2->prev;
            node->nu = (uint16_t)nu;
        }
        n = node->next;
    }
    // back into the lists by size
    for (n = NODE(head)->next; n != head;) {
        Node * node = NODE(n);
        unsigned nu, i;
        Ref next = node->next;
        for (nu = node->nu; nu > 128; nu -= 128, node += 128)
            insert_node(p, node, NUM_INDEXES - 1);
        if (I2U(i = U2I(nu)) != nu) {
            unsigned k = I2U(--i);
            insert_node(p, node + k, nu - k - 1);
        }
        insert_node(p, node, i);
        n = next;
    }
}

static void * alloc_units_rare(Ppmd7 * p, unsigned indx)
{
    if (!p->glue_count) {
        glue_free_blocks(p);
        if (p->free_list[indx])
            return remove_node(p, indx);
    }
    unsigned i = indx;
    do {
        if (++i == NUM_INDEXES) {
            uint32_t bytes = U2B(I2U(indx));
            p->glue_count--;
            return (uint32_t)(p->units_start - p->text) > bytes ? (p->units_start -= bytes) : NULL;
        }
    } while (!p->free_list[i]);
    void * block = remove_node(p, i);
    split_block(p, block, i, indx);
    return block;
}

static void * alloc_units(Ppmd7 * p, unsigned indx)
{
    if (p->free_list[indx])
        return remove_node(p, indx);
    uint32_t bytes = U2B(I2U(indx));
    if (bytes <= (uint32_t)(p->hi_unit - p->lo_unit)) {
        void * block = p->lo_unit;
        p->lo_unit += bytes;
        return block;
    }
    return alloc_units_rare(p, indx);
}

static void * shrink_units(Ppmd7 * p, void * old, unsigned old_nu, unsigned new_nu)
{
    unsigned i0 = U2I(old_nu), i1 = U2I(new_nu);
    if (i0 == i1)
        return old;
    if (p->free_list[i1]) {
        void * block = remove_node(p, i1);
        memcpy(block, old, U2B(new_nu));
        insert_node(p, old, i0);
        return block;
    }
    split_block(p, old, i0, i1);
    return old;
}

// --------------------------------------------------------------------------------------------------------- the model

static void restart_model(Ppmd7 * p)
{
    static const uint16_t init_bin_esc[] = {0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051};
    memset(p->free_list, 0, sizeof(p->free_list));
    p->text = p->base + p->align_offset;
    p->hi_unit = p->text + p->size;
    p->lo_unit = p->units_start = p->hi_unit - p->size / 8 / UNIT_SIZE * 7 * UNIT_SIZE;
    p->glue_count = 0;
    p->order_fall = p->max_order;
    p->run_length = p->init_rl = -(int32_t)(p->max_order < 12 ? p->max_order : 12) - 1;
    p->prev_success = 0;
    p->min_context = p->max_context = (Context *)(p->hi_unit -= UNIT_SIZE);
    p->min_context->suffix = 0;
    p->min_context->num_stats = 256;
    p->min_context->summ_freq = 256 + 1;
    p->found = (State *)p->lo_unit;
    p->lo_unit += U2B(256 / 2);
    p->min_context->stats = REF(p->found);
    for (unsigned i = 0; i < 256; i++) {
        State * s = &p->found[i];
        s->symbol = (uint8_t)i;
        s->freq = 1;
        set_successor(s, 0);
    }
    for (unsigned i = 0; i < 128; i++)
        for (unsigned k = 0; k < 8; k++) {
            uint16_t val = (uint16_t)(BIN_SCALE - init_bin_esc[k] / (i + 2));
            for (unsigned m = 0; m < 64; m += 8)
                p->bin_summ[i][k + m] = val;
        }
    for (unsigned i = 0; i < 25; i++)
        for (unsigned k = 0; k < 16; k++) {
            See * s = &p->see[i][k];
            s->shift = PERIOD_BITS - 4;
            s->summ = (uint16_t)((5 * i + 10) << s->shift);
            s->count = 4;
        }
}

void ppmd7_init(Ppmd7 * p, unsigned max_order)
{
    p->max_order = max_order;
    restart_model(p);
    p->dummy_see.shift = PERIOD_BITS;
    p->dummy_see.summ = 0;
    p->dummy_see.count = 64;
}

static Context * create_successors(Ppmd7 * p, bool skip)
{
    State up;
    Context * c = p->min_context;
    Ref up_branch = successor(p->found);
    State * ps[MAX_ORDER];
    unsigned num = 0;
    if (!skip)
        ps[num++] = p->found;
    while (c->suffix) {
        State * s;
        c = SUFFIX(c);
        if (c->num_stats != 1) {
            for (s = STATS(c); s->symbol != p->found->symbol; s++)
                ;
        } else
            s = ONE_STATE(c);
        Ref succ = successor(s);
        if (succ != up_branch) {
            c = CTX(succ);
            if (!num)
                return c;
            break;
        }
        ps[num++] = s;
    }
    up.symbol = *AT(const uint8_t, up_branch);
    set_successor(&up, up_branch + 1);
    if (c->num_stats == 1)
        up.freq = ONE_STATE(c)->freq;
    else {
        State * s;
        for (s = STATS(c); s->symbol != up.symbol; s++)
            ;
        uint32_t cf = s->freq - 1u, s0 = c->summ_freq - c->num_stats - cf;
        up.freq = (uint8_t)(1 + (2 * cf <= s0 ? 5 * cf > s0 : (2 * cf + 3 * s0 - 1) / (2 * s0)));
    }
    do {
        Context * c1;
        if (p->hi_unit != p->lo_unit)
            c1 = (Context *)(p->hi_unit -= UNIT_SIZE);
        else if (p->free_list[0])
            c1 = remove_node(p, 0);
        else if (!(c1 = alloc_units_rare(p, 0)))
            return NULL;
        c1->num_stats = 1;
        *ONE_STATE(c1) = up;
        c1->suffix = REF(c);
        set_successor(ps[--num], REF(c1));
        c = c1;
    } while (num);
    return c;
}

static void swap_states(State * a, State * b)
{
    State t = *a;
    *a = *b;
    *b = t;
}

static void update_model(Ppmd7 * p)
{
    Ref succ, f_succ = successor(p->found);
    Context * c;
    unsigned s0, ns;
    if (p->found->freq < MAX_FREQ / 4 && p->min_context->suffix) {
        c = SUFFIX(p->min_context);
        if (c->num_stats == 1) {
            State * s = ONE_STATE(c);
            if (s->freq < 32)
                s->freq++;
        } else {
            State * s = STATS(c);
            if (s->symbol != p->found->symbol) {
                do
                    s++;
                while (s->symbol != p->found->symbol);
                if (s[0].freq >= s[-1].freq) {
                    swap_states(&s[0], &s[-1]);
                    s--;
                }
            }
            if (s->freq < MAX_FREQ - 9) {
                s->freq += 2;
                c->summ_freq += 2;
            }
        }
    }
    if (!p->order_fall) {
        p->min_context = p->max_context = create_successors(p, true);
        if (!p->min_context) {
            restart_model(p);
            return;
        }
        set_successor(p->found, REF(p->min_context));
        return;
    }
    *p->text++ = p->found->symbol;
    succ = REF(p->text);
    if (p->text >= p->units_start) {
        restart_model(p);
        return;
    }
    if (f_succ) {
        if (f_succ <= succ) {
            Context * cs = create_successors(p, false);
            if (!cs) {
                restart_model(p);
                return;
            }
            f_succ = REF(cs);
        }
        if (--p->order_fall == 0) {
            succ = f_succ;
            p->text -= p->max_context != p->min_context;
        }
    } else {
        set_successor(p->found, succ);
        f_succ = REF(p->min_context);
    }
    ns = p->min_context->num_stats;
    s0 = p->min_context->summ_freq - ns - (p->found->freq - 1u);
    for (c = p->max_context; c != p->min_context; c = SUFFIX(c)) {
        unsigned ns1 = c->num_stats;
        uint32_t cf, sf;
        if (ns1 != 1) {
            if (!(ns1 & 1)) {
                // one unit more
                unsigned old_nu = ns1 >> 1, i = U2I(old_nu);
                if (i != U2I(old_nu + 1)) {
                    void * block = alloc_units(p, i + 1);
                    if (!block) {
                        restart_model(p);
                        return;
                    }
                    void * old = STATS(c);
                    memcpy(block, old, U2B(old_nu));
                    insert_node(p, old, i);
                    c->stats = REF(block);
                }
            }
            c->summ_freq = (uint16_t)(c->summ_freq + (2 * ns1 < ns) + 2 * ((4 * ns1 <= ns) & (c->summ_freq <= 8 * ns1)));
        } else {
            State * s = alloc_units(p, 0);
            if (!s) {
                restart_model(p);
                return;
            }
            *s = *ONE_STATE(c);
            c->stats = REF(s);
            if (s->freq < MAX_FREQ / 4 - 1)
                s->freq <<= 1;
            else
                s->freq = MAX_FREQ - 4;
            c->summ_freq = (uint16_t)(s->freq + p->init_esc + (ns > 3));
        }
        cf = 2 * (uint32_t)p->found->freq * (c->summ_freq + 6u);
        sf = (uint32_t)s0 + c->summ_freq;
        if (cf < 6 * sf) {
            cf = 1 + (cf > sf) + (cf >= 4 * sf);
            c->summ_freq = (uint16_t)(c->summ_freq + 3);
        } else {
            cf = 4 + (cf >= 9 * sf) + (cf >= 12 * sf) + (cf >= 15 * sf);
            c->summ_freq = (uint16_t)(c->summ_freq + cf);
        }
        State * s = STATS(c) + ns1;
        set_successor(s, succ);
        s->symbol = p->found->symbol;
        s->freq = (uint8_t)cf;
        c->num_stats = (uint16_t)(ns1 + 1);
    }
    p->max_context = p->min_context = CTX(f_succ);
}

static void rescale(Ppmd7 * p)
{
    unsigned i, adder, sum_freq, esc_freq;
    State * stats = STATS(p->min_context), * s = p->found;
    // the found one to the front
    if (s != stats) {
        State t = *s;
        do
            s[0] = s[-1];
        while (--s != stats);
        *s = t;
    }
    esc_freq = p->min_context->summ_freq - s->freq;
    s->freq += 4;
    adder = p->order_fall != 0;
    s->freq = (uint8_t)((s->freq + adder) >> 1);
    sum_freq = s->freq;
    i = p->min_context->num_stats - 1u;
    do {
        esc_freq -= (++s)->freq;
        s->freq = (uint8_t)((s->freq + adder) >> 1);
        sum_freq += s->freq;
        if (s[0].freq > s[-1].freq) {
            State * s1 = s, t = *s1;
            do
                s1[0] = s1[-1];
            while (--s1 != stats && t.freq > s1[-1].freq);
            *s1 = t;
        }
    } while (--i);
    if (!s->freq) {
        unsigned num_stats = p->min_context->num_stats, n0, n1;
        do
            i++;
        while (!(--s)->freq);
        esc_freq += i;
        p->min_context->num_stats = (uint16_t)(p->min_context->num_stats - i);
        if (p->min_context->num_stats == 1) {
            State t = *stats;
            do {
                t.freq = (uint8_t)(t.freq - (t.freq >> 1));
                esc_freq >>= 1;
            } while (esc_freq > 1);
            insert_node(p, stats, U2I((num_stats + 1) >> 1));
            *(p->found = ONE_STATE(p->min_context)) = t;
            return;
        }
        n0 = (num_stats + 1) >> 1;
        n1 = (p->min_context->num_stats + 1u) >> 1;
        if (n0 != n1)
            p->min_context->stats = REF(shrink_units(p, stats, n0, n1));
    }
    p->min_context->summ_freq = (uint16_t)(sum_freq + esc_freq - (esc_freq >> 1));
    p->found = STATS(p->min_context);
}

static void next_context(Ppmd7 * p)
{
    Context * c = CTX(successor(p->found));
    if (!p->order_fall && (uint8_t *)c > p->text)
        p->min_context = p->max_context = c;
    else
        update_model(p);
}

static void update1(Ppmd7 * p)
{
    State * s = p->found;
    s->freq += 4;
    p->min_context->summ_freq += 4;
    if (s[0].freq > s[-1].freq) {
        swap_states(&s[0], &s[-1]);
        p->found = --s;
        if (s->freq > MAX_FREQ)
            rescale(p);
    }
    next_context(p);
}

static void update1_0(Ppmd7 * p)
{
    p->prev_success = 2u * p->found->freq > p->min_context->summ_freq;
    p->run_length += (int32_t)p->prev_success;
    p->min_context->summ_freq += 4;
    if ((p->found->freq += 4) > MAX_FREQ)
        rescale(p);
    next_context(p);
}

static void update2(Ppmd7 * p)
{
    p->min_context->summ_freq += 4;
    if ((p->found->freq += 4) > MAX_FREQ)
        rescale(p);
    p->run_length = p->init_rl;
    update_model(p);
}

static void update_bin(Ppmd7 * p)
{
    p->found->freq = (uint8_t)(p->found->freq + (p->found->freq < 128));
    p->prev_success = 1;
    p->run_length++;
    next_context(p);
}

static See * make_esc_freq(Ppmd7 * p, unsigned masked, uint32_t * esc_freq)
{
    See * see;
    unsigned non_masked = p->min_context->num_stats - masked;
    if (p->min_context->num_stats != 256) {
        see = p->see[p->ns2indx[non_masked - 1]] +
              (non_masked < (unsigned)SUFFIX(p->min_context)->num_stats - p->min_context->num_stats) +
              2 * (p->min_context->summ_freq < 11u * p->min_context->num_stats) + 4 * (masked > non_masked) + p->hi_bits_flag;
        unsigned r = see->summ >> see->shift;
        see->summ = (uint16_t)(see->summ - r);
        *esc_freq = r + (r == 0);
    } else {
        see = &p->dummy_see;
        *esc_freq = 1;
    }
    return see;
}

int ppmd7_decode(Ppmd7 * p, const Ppmd7Coder * rc, void * user)
{
    static const uint8_t exp_escape[16] = {25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2};
    uint8_t mask[256];
    if (p->min_context->num_stats != 1) {
        State * s = STATS(p->min_context);
        unsigned i;
        uint32_t count = rc->threshold(user, p->min_context->summ_freq), hi = s->freq;
        if (count < hi) {
            rc->decode(user, 0, s->freq);
            p->found = s;
            int symbol = s->symbol;
            update1_0(p);
            return symbol;
        }
        p->prev_success = 0;
        i = p->min_context->num_stats - 1u;
        do {
            if ((hi += (++s)->freq) > count) {
                rc->decode(user, hi - s->freq, s->freq);
                p->found = s;
                int symbol = s->symbol;
                update1(p);
                return symbol;
            }
        } while (--i);
        if (count >= p->min_context->summ_freq)
            return -2;
        p->hi_bits_flag = p->hb2flag[p->found->symbol];
        rc->decode(user, hi, p->min_context->summ_freq - hi);
        memset(mask, 0xFF, sizeof(mask));
        mask[s->symbol] = 0;
        i = p->min_context->num_stats - 1u;
        do
            mask[(--s)->symbol] = 0;
        while (--i);
    } else {
        State * one = ONE_STATE(p->min_context);
        p->hi_bits_flag = p->hb2flag[p->found->symbol];
        uint16_t * prob = &p->bin_summ[one->freq - 1][p->prev_success + p->ns2bs_indx[SUFFIX(p->min_context)->num_stats - 1] +
                                                      p->hi_bits_flag + 2 * p->hb2flag[one->symbol] +
                                                      ((p->run_length >> 26) & 0x20)];
        unsigned mean = (*prob + (1u << (PERIOD_BITS - 2))) >> PERIOD_BITS;
        if (!rc->bit(user, *prob, BIN_BITS)) {
            *prob = (uint16_t)(*prob + (1 << INT_BITS) - mean);
            p->found = one;
            int symbol = one->symbol;
            update_bin(p);
            return symbol;
        }
        *prob = (uint16_t)(*prob - mean);
        p->init_esc = exp_escape[*prob >> 10];
        memset(mask, 0xFF, sizeof(mask));
        mask[one->symbol] = 0;
        p->prev_success = 0;
    }
    for (;;) {
        State * ps[256], * s;
        uint32_t freq_sum, count, hi;
        See * see;
        unsigned i, num, masked = p->min_context->num_stats;
        do {
            p->order_fall++;
            if (!p->min_context->suffix)
                return -1;
            p->min_context = SUFFIX(p->min_context);
        } while (p->min_context->num_stats == masked);
        hi = 0;
        s = STATS(p->min_context);
        i = 0;
        num = p->min_context->num_stats - masked;
        do {
            unsigned on = mask[s->symbol] != 0;
            if (on)
                hi += s->freq;
            ps[i] = s++;
            i += on;
        } while (i != num);
        see = make_esc_freq(p, masked, &freq_sum);
        freq_sum += hi;
        count = rc->threshold(user, freq_sum);
        if (count < hi) {
            State ** pps = ps;
            for (hi = 0; (hi += (*pps)->freq) <= count; pps++)
                ;
            s = *pps;
            rc->decode(user, hi - s->freq, s->freq);
            if (see->shift < PERIOD_BITS && --see->count == 0) {
                see->summ = (uint16_t)(see->summ << 1);
                see->count = (uint8_t)(3 << see->shift++);
            }
            p->found = s;
            int symbol = s->symbol;
            update2(p);
            return symbol;
        }
        if (count >= freq_sum)
            return -2;
        rc->decode(user, hi, freq_sum - hi);
        see->summ = (uint16_t)(see->summ + freq_sum);
        do
            mask[ps[--i]->symbol] = 0;
        while (i);
    }
}
