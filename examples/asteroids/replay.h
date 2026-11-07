/*
 * A replay: the seed and every tick's input of one whole game, enough to
 * re-run it with sim.h and get the same score.
 *
 *   "PDAR" | version (2) | block u32 LE | seed u32 LE | ticks u32 LE | runs
 *   run:   input (one byte, SIM_* bits) | count (LEB128, at least 1)
 *
 * The seed comes from the hash of Asset Hub block `block`, the newest final
 * block when the game started (replay_seed): nobody knew the rocks before
 * that block, and the operator checks it. Block 0 marks a game played
 * without a chain, which cannot be claimed. Version 1 replays (no block)
 * still decode, as block 0.
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

#define REPLAY_VERSION   2
#define REPLAY_MAX_TICKS (60u * 60u * 60u)  /* an hour of play */
#define REPLAY_MAX_BYTES (256u * 1024u)

typedef struct {
    uint32_t block;   /* the block the seed came from; 0 for none */
    uint32_t seed;
    uint8_t *inputs;  /* one per tick */
    size_t   n, cap;
    int      full;    /* past REPLAY_MAX_TICKS: this game cannot be claimed */
} replay;

/* The seed a game started at a block with this hash plays. */
uint32_t replay_seed(const uint8_t block_hash[32]);

void replay_begin(replay *r, uint32_t block, uint32_t seed);
void replay_add(replay *r, uint8_t input);
/* The encoded replay, freed with free(); NULL when full or out of memory. */
uint8_t *replay_encode(const replay *r, size_t *len);
void replay_free(replay *r);

/* Decodes a replay into r (seed and one input per tick), to play it back. */
int replay_decode(const uint8_t *data, size_t len, replay *r, char *msg, size_t msg_len);

/*
 * Decodes and re-runs a replay. 0 with the final score and tick count, and
 * the seed and its block (either may be NULL), when it is one finished
 * game; -1 otherwise, with the reason in msg. Whether the seed matches the
 * block is for the caller to check, against the chain.
 */
int replay_check(const uint8_t *data, size_t len, uint32_t *score, uint32_t *ticks,
                 uint32_t *block, uint32_t *seed, char *msg, size_t msg_len);

#endif
