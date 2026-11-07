/*
 * The leaderboard document stored on the Bulletin chain: JSON, highest score
 * first, at most BOARD_MAX entries.
 *
 *   {"game": "pdot-asteroids", "version": 1, "entries": [
 *     {"score": 5000, "name": "DAM", "account": "5Grw...", "block": 342,
 *      "replay": "bafk..."}, ...]}
 *
 * "block" is the Asset Hub block holding the score's remark, so each entry
 * can be checked against the chain. "replay" is the CID of the game's
 * replay on Bulletin (replay.h), which re-runs to the score; entries from
 * before replays have none.
 */
#ifndef ASTEROIDS_BOARD_H
#define ASTEROIDS_BOARD_H

#include <stddef.h>
#include <stdint.h>

#include <pdot.h>

#define BOARD_MAX 10
#define BOARD_CID_MAX 112   /* base32 text of any CID up to PDOT_CID_MAX bytes */

typedef struct {
    uint32_t score;
    char     name[4];  /* 3 letters A-Z */
    char     account[PDOT_SS58_MAX_LEN];
    uint32_t block;
    char     replay[BOARD_CID_MAX];  /* "" for none */
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
/* The place (1..) of the entry with this account and block, 0 if none. */
int board_find(const board *b, const char *account, uint32_t block);

/* ---- On chain: read by the game and the operator alike ---------------- */

#define BOARD_KEY  "board"  /* collection attribute: the board's CID on Bulletin */
#define RECORD_KEY "best"   /* collection attribute: "SCORE ADDRESS NAME" */

/*
 * The board behind the collection's "board" attribute on ah, fetched from
 * bul and checked against its CID: a node can withhold it, not change it.
 * An empty board, and cid "", when there is none yet. On failure msg says why.
 */
int board_read(pdot_client *ah, pdot_client *bul, uint32_t collection, board *b,
               char cid[BOARD_CID_MAX], char *msg, size_t msg_len);

typedef struct {
    int      exists;
    uint32_t score;
    char     holder[PDOT_SS58_MAX_LEN];
    char     name[4];   /* "" on records from before initials */
    int      verified;
} board_record;

/* The collection's "best" attribute: the score to beat. */
int board_record_read(pdot_client *ah, uint32_t collection, board_record *r);

#endif
