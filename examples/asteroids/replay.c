#include "replay.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"

#define HEADER 13  /* magic, version, seed, ticks */

void replay_begin(replay *r, uint32_t seed)
{
    free(r->inputs);
    memset(r, 0, sizeof *r);
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
    put32(out + 5, r->seed);
    put32(out + 9, (uint32_t)r->n);
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

int replay_check(const uint8_t *data, size_t len, uint32_t *score, uint32_t *ticks,
                 char *msg, size_t msg_len)
{
    sim s;
    uint32_t total, done = 0;
    size_t at = HEADER;

    if (len < HEADER || len > REPLAY_MAX_BYTES || memcmp(data, "PDAR", 4) != 0)
        return bad(msg, msg_len, "not a replay");
    if (data[4] != REPLAY_VERSION)
        return bad(msg, msg_len, "replay version %u, expected %u", data[4], REPLAY_VERSION);
    total = get32(data + 9);
    if (total == 0 || total > REPLAY_MAX_TICKS)
        return bad(msg, msg_len, "%u ticks: more than an hour, or none", total);
    sim_new(&s, get32(data + 5));
    while (at < len) {
        uint8_t input = data[at++];
        uint32_t run = 0;
        int shift = 0;

        if (input & ~SIM_INPUTS)
            return bad(msg, msg_len, "input 0x%02x at tick %u", input, done);
        for (;;) {
            uint8_t b;
            if (at == len || shift > 28)
                return bad(msg, msg_len, "truncated run at tick %u", done);
            b = data[at++];
            run |= (uint32_t)(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        if (run == 0 || run > total - done)
            return bad(msg, msg_len, "runs do not add up to %u ticks", total);
        while (run--) {
            if (s.over)
                return bad(msg, msg_len, "inputs after the game ended at tick %u", done);
            sim_step(&s, input);
            done++;
        }
    }
    if (done != total)
        return bad(msg, msg_len, "runs cover %u of %u ticks", done, total);
    if (!s.over)
        return bad(msg, msg_len, "the game has not ended after %u ticks", total);
    *score = s.score;
    *ticks = total;
    return 0;
}

