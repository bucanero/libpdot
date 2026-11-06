/*
 * The leaderboard document stored on the Bulletin chain: JSON, highest score
 * first, at most BOARD_MAX entries.
 *
 *   {"game": "pdot-asteroids", "version": 1, "entries": [
 *     {"score": 5000, "name": "DAM", "account": "5Grw...", "block": 342}, ...]}
 *
 * "block" is the Asset Hub block holding the score's remark, so each entry
 * can be checked against the chain.
 */
#ifndef ASTEROIDS_BOARD_H
#define ASTEROIDS_BOARD_H

#include <stddef.h>
#include <stdint.h>

#include <pdot.h>

#define BOARD_MAX 10

typedef struct {
    uint32_t score;
    char     name[4];  /* 3 letters A-Z */
    char     account[PDOT_SS58_MAX_LEN];
    uint32_t block;
} board_entry;

typedef struct {
    size_t      n;
    board_entry e[BOARD_MAX];
} board;

/* Parses and checks a board; entries that do not fit the format are an error. */
int board_parse(const uint8_t *data, size_t len, board *b);
/* The board as JSON, freed with free(); NULL when out of memory. */
char *board_json(const board *b);
/* The place (1..BOARD_MAX) score would take, below equal scores; 0 if none. */
int board_rank(const board *b, uint32_t score);
/* Inserts e where board_rank puts it; returns that place, or 0 (unchanged). */
int board_insert(board *b, const board_entry *e);

#endif
