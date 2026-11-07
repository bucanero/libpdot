#include "replay.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"

#define HEADER_V1 13  /* magic, version, seed, ticks */
#define HEADER    17  /* magic, version, block, seed, ticks */

uint32_t replay_seed(const uint8_t block_hash[32])
{
    const uint8_t *p = block_hash;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void replay_begin(replay *r, uint32_t block, uint32_t seed)
{
    free(r->inputs);
    memset(r, 0, sizeof *r);
    r->block = block;
    r->seed = seed;
}

void replay_add(replay *r, uint8_t input)
{
    if (r->full)
        return;
    if (r->n == REPLAY_MAX_TICKS) {
        r->full = 1;
        return;
    }
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 4096;
        uint8_t *p = realloc(r->inputs, cap);
        if (!p) {
            r->full = 1;
            return;
        }
        r->inputs = p;
        r->cap = cap;
    }
    r->inputs[r->n++] = input & SIM_INPUTS;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint8_t *replay_encode(const replay *r, size_t *len)
{
    /* worst case: a new run every tick, a byte for the input and one for the count */
    size_t cap = HEADER + 2 * r->n, at = HEADER, i = 0;
    uint8_t *out;

    if (r->full || !(out = malloc(cap)))
        return NULL;
    memcpy(out, "PDAR", 4);
    out[4] = REPLAY_VERSION;
    put32(out + 5, r->block);
    put32(out + 9, r->seed);
    put32(out + 13, (uint32_t)r->n);
    while (i < r->n) {
        uint32_t run = 1;
        while (i + run < r->n && r->inputs[i + run] == r->inputs[i])
            run++;
        if (at + 6 > cap) {  /* long runs take more count bytes but fewer runs */
            uint8_t *p = realloc(out, cap += 64);
            if (!p) {
                free(out);
                return NULL;
            }
            out = p;
        }
        out[at++] = r->inputs[i];
        i += run;
        do {
            out[at++] = (uint8_t)((run & 0x7f) | (run > 0x7f ? 0x80 : 0));
            run >>= 7;
        } while (run);
    }
    if (at > REPLAY_MAX_BYTES) {
        free(out);
        return NULL;
    }
    *len = at;
    return out;
}

void replay_free(replay *r)
{
    free(r->inputs);
    memset(r, 0, sizeof *r);
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static int bad(char *msg, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, len, fmt, ap);
    va_end(ap);
    return -1;
}

int replay_decode(const uint8_t *data, size_t len, replay *r, char *msg, size_t msg_len)
{
    uint32_t total;
    size_t at;

    memset(r, 0, sizeof *r);
    if (len < HEADER_V1 || len > REPLAY_MAX_BYTES || memcmp(data, "PDAR", 4) != 0)
        return bad(msg, msg_len, "not a replay");
    if (data[4] == 1) {  /* no block: an unanchored seed */
        at = HEADER_V1;
        r->seed = get32(data + 5);
    } else if (data[4] == REPLAY_VERSION && len >= HEADER) {
        at = HEADER;
        r->block = get32(data + 5);
        r->seed = get32(data + 9);
    } else {
        return bad(msg, msg_len, "replay version %u, expected %u", data[4], REPLAY_VERSION);
    }
    total = get32(data + at - 4);
    if (total == 0 || total > REPLAY_MAX_TICKS)
        return bad(msg, msg_len, "%u ticks: more than an hour, or none", total);
    if (!(r->inputs = malloc(total)))
        return bad(msg, msg_len, "out of memory");
    r->cap = total;
    while (at < len) {
        uint8_t input = data[at++];
        uint32_t run = 0;
        int shift = 0;

        if (input & ~SIM_INPUTS) {
            bad(msg, msg_len, "input 0x%02x at tick %zu", input, r->n);
            goto fail;
        }
        for (;;) {
            uint8_t b;
            if (at == len || shift > 28) {
                bad(msg, msg_len, "truncated run at tick %zu", r->n);
                goto fail;
            }
            b = data[at++];
            run |= (uint32_t)(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        if (run == 0 || run > total - r->n) {
            bad(msg, msg_len, "runs do not add up to %u ticks", total);
            goto fail;
        }
        memset(r->inputs + r->n, input, run);
        r->n += run;
    }
    if (r->n != total) {
        bad(msg, msg_len, "runs cover %zu of %u ticks", r->n, total);
        goto fail;
    }
    return 0;
fail:
    replay_free(r);
    return -1;
}

int replay_check(const uint8_t *data, size_t len, uint32_t *score, uint32_t *ticks,
                 uint32_t *block, uint32_t *seed, char *msg, size_t msg_len)
{
    replay r;
    sim s;
    size_t i;
    uint32_t from, seeded;

    if (replay_decode(data, len, &r, msg, msg_len))
        return -1;
    from = r.block;  /* replay_free clears r */
    seeded = r.seed;
    sim_new(&s, r.seed);
    for (i = 0; i < r.n; i++) {
        if (s.over) {
            replay_free(&r);
            return bad(msg, msg_len, "inputs after the game ended at tick %zu", i);
        }
        sim_step(&s, r.inputs[i]);
    }
    replay_free(&r);
    if (!s.over)
        return bad(msg, msg_len, "the game has not ended after %zu ticks", i);
    *score = s.score;
    *ticks = (uint32_t)i;
    if (block)
        *block = from;
    if (seed)
        *seed = seeded;
    return 0;
}
