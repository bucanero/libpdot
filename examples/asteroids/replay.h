/*
 * A replay: the seed and every tick's input of one whole game, enough to
 * re-run it with sim.h and get the same score.
 *
 *   "PDAR" | version (1) | seed u32 LE | ticks u32 LE | runs
 *   run:   input (one byte, SIM_* bits) | count (LEB128, at least 1)
 *
 * The runs cover exactly `ticks` ticks, and the game ends (no lives left)
 * on the last one: a replay is a finished game, nothing more. A typical
 * game is a few KB. The score remark names the replay's CID, and the
 * operator stores the replay on Bulletin, so anyone can re-run a score on
 * the board.
 */
#ifndef ASTEROIDS_REPLAY_H
#define ASTEROIDS_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#define REPLAY_VERSION   1
#define REPLAY_MAX_TICKS (60u * 60u * 60u)  /* an hour of play */
#define REPLAY_MAX_BYTES (256u * 1024u)

typedef struct {
    uint32_t seed;
    uint8_t *inputs;  /* one per tick */
    size_t   n, cap;
    int      full;    /* past REPLAY_MAX_TICKS: this game cannot be claimed */
} replay;

void replay_begin(replay *r, uint32_t seed);
void replay_add(replay *r, uint8_t input);
/* The encoded replay, freed with free(); NULL when full or out of memory. */
uint8_t *replay_encode(const replay *r, size_t *len);
void replay_free(replay *r);

/* Decodes a replay into r (seed and one input per tick), to play it back. */
int replay_decode(const uint8_t *data, size_t len, replay *r, char *msg, size_t msg_len);

/*
 * Decodes and re-runs a replay. 0 with the final score and tick count when
 * it is one finished game; -1 otherwise, with the reason in msg.
 */
int replay_check(const uint8_t *data, size_t len, uint32_t *score, uint32_t *ticks,
                 char *msg, size_t msg_len);

#endif
