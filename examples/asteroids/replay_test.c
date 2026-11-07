/*
 * Replays: a scripted player plays whole games, and each replay must re-run
 * to the same score, while edited replays must not check. It also prints a
 * digest of every game's final state: builds with other compilers, flags or
 * CPUs must print the same digests (`replay_test --digest`), which is what
 * lets the operator re-run games played anywhere.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay.h"
#include "sim.h"

static int failures;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                                           __FILE__, __LINE__, #x); failures++; } } while (0)

/* FNV-1a over the bytes of every float and integer that matters. */
static uint64_t mix(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 0x100000001b3ull;
    return h;
}

static uint64_t digest(const sim *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    int i;

    h = mix(h, &s->score, sizeof s->score);
    h = mix(h, &s->ticks, sizeof s->ticks);
    h = mix(h, &s->level, sizeof s->level);
    h = mix(h, &s->p, sizeof s->p);
    h = mix(h, &s->v, sizeof s->v);
    h = mix(h, &s->angle, sizeof s->angle);
    for (i = 0; i < SIM_ROCKS; i++)
        if (s->rocks[i].size)
            h = mix(h, &s->rocks[i].p, sizeof s->rocks[i].p);
    return h;
}

/* A player that turns, thrusts in bursts and fires a lot: inputs from an LCG. */
static void play(uint32_t seed, replay *r, sim *s)
{
    uint32_t x = seed * 2654435761u + 1;
    uint8_t input = 0;

    sim_new(s, seed);
    replay_begin(r, seed ^ 0x5a5a, seed);  /* any block number: only carried */
    while (!s->over && r->n < REPLAY_MAX_TICKS) {
        if (s->ticks % 9 == 0) {  /* a human changes input a few times a second */
            x = x * 1664525u + 1013904223u;
            input = (uint8_t)((x >> 24) & (SIM_LEFT | SIM_RIGHT | SIM_THRUST)) | SIM_FIRE;
            if ((input & (SIM_LEFT | SIM_RIGHT)) == (SIM_LEFT | SIM_RIGHT))
                input &= (uint8_t)~SIM_RIGHT;
        }
        replay_add(r, input);
        sim_step(s, input);
    }
}

static void test_trig(void)
{
    float x;
    for (x = -50.0f; x < 50.0f; x += 0.01f) {
        float e = sim_sin(x) * sim_sin(x) + sim_cos(x) * sim_cos(x) - 1.0f;
        CHECK(e < 1e-4f && e > -1e-4f);
    }
    CHECK(sim_sin(0) == 0);
    CHECK(sim_sin(SIM_PI / 2) > 0.99999f && sim_sin(SIM_PI / 2) < 1.00001f);
}

static void test_games(int print)
{
    static const uint32_t seeds[] = { 1, 42, 0xdeadbeef, 123456789, 0 };
    size_t k;

    for (k = 0; k < sizeof seeds / sizeof seeds[0]; k++) {
        replay r;
        sim s;
        uint8_t *enc, *bad;
        size_t len, i;
        uint32_t score, ticks, block;
        char msg[160];

        memset(&r, 0, sizeof r);
        play(seeds[k], &r, &s);
        CHECK(s.over);
        enc = replay_encode(&r, &len);
        CHECK(enc != NULL);
        if (!enc) {
            replay_free(&r);
            continue;
        }
        if (print)
            printf("seed %10u: score %6u after %6u ticks, %5zu bytes, state %016llx\n",
                   seeds[k], s.score, s.ticks, len, (unsigned long long)digest(&s));

        /* the replay re-runs to the same game, and carries its seed's block */
        CHECK(replay_check(enc, len, &score, &ticks, &block, NULL, msg, sizeof msg) == 0);
        CHECK(score == s.score && ticks == s.ticks && block == (seeds[k] ^ 0x5a5a));

        /* a version 1 replay (no block) still re-runs, as block 0 */
        bad = malloc(len);
        memcpy(bad, enc, 4);
        bad[4] = 1;
        memcpy(bad + 5, enc + 9, len - 9);
        CHECK(replay_check(bad, len - 4, &score, &ticks, &block, NULL, msg, sizeof msg) == 0);
        CHECK(score == s.score && ticks == s.ticks && block == 0);
        free(bad);

        /* any cut fails */
        for (i = 0; i < len; i++)
            CHECK(replay_check(enc, i, &score, &ticks, NULL, NULL, msg, sizeof msg) != 0);

        bad = malloc(len + 2);
        /* one more tick after the end */
        memcpy(bad, enc, len);
        bad[len] = 0;
        bad[len + 1] = 1;
        bad[13]++;  /* ticks + 1 */
        CHECK(replay_check(bad, len + 2, &score, &ticks, NULL, NULL, msg, sizeof msg) != 0);
        /* a different seed is a different game */
        memcpy(bad, enc, len);
        bad[9] ^= 1;  /* the seed */
        CHECK(replay_check(bad, len, &score, &ticks, NULL, NULL, msg, sizeof msg) != 0 || score != s.score ||
              ticks != s.ticks);
        /* every changed input changes the game (or breaks the replay) */
        for (i = 17; i < len; i += 7) {
            memcpy(bad, enc, len);
            bad[i] ^= 0x04;
            if (replay_check(bad, len, &score, &ticks, NULL, NULL, msg, sizeof msg) == 0)
                CHECK(score != s.score || ticks != s.ticks || bad[i] == enc[i]);
        }
        /* bad header */
        memcpy(bad, enc, len);
        bad[4] = 3;
        CHECK(replay_check(bad, len, &score, &ticks, NULL, NULL, msg, sizeof msg) != 0);
        free(bad);
        free(enc);
        replay_free(&r);
    }
}

int main(int argc, char **argv)
{
    int print = argc > 1 && strcmp(argv[1], "--digest") == 0;

    test_trig();
    test_games(print);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures != 0;
}
