/*
 * Everything the game does on chain, through libpdot's pdot_async so the
 * frame loop never waits on the network. The game reads chain_view each
 * frame; it changes only inside chain_poll, on the game thread.
 */
#ifndef ASTEROIDS_CHAIN_H
#define ASTEROIDS_CHAIN_H

#include <stdint.h>

#include <pdot.h>

#include "board.h"
#include "replay.h"

#define CHAIN_CID_STR_MAX BOARD_CID_MAX

typedef struct {
    const char *rpc;               /* Asset Hub */
    const char *bulletin_rpc;      /* Bulletin, for the leaderboard; NULL for none */
    const char *operator_url;      /* asteroids-operator: claims scores; NULL for none */
    uint8_t     player_seed[32];   /* the player's account: signs to open and close the session */
    uint32_t    collection;        /* the trophy collection, owned by the operator */
    const char *fund;              /* native token sent to the session key, e.g. "1" */
    const char *session_file;      /* where the session key is kept between runs */
    const char *metadata_hash;     /* "on", "off" or "SYMBOL:DECIMALS" */
    const char *relay;             /* with checkpoint: prove results from relay-chain finality */
    const char *checkpoint;
    uint32_t    para_id;
} chain_config;

enum { CHAIN_OPENING, CHAIN_READY, CHAIN_FAILED, CHAIN_CLOSING, CHAIN_CLOSED };

typedef struct {
    int      state;             /* CHAIN_* */
    char     symbol[16];
    uint8_t  decimals;
    char     player[PDOT_SS58_MAX_LEN];
    char     session[PDOT_SS58_MAX_LEN];
    uint32_t head;              /* newest final block */
    int      head_verified;
    char     session_balance[40];
    int      trophies;          /* the player's, -1 until read */
    int      has_record;        /* the collection's "best" attribute */
    uint32_t record;
    char     record_holder[PDOT_SS58_MAX_LEN];
    char     record_name[4];    /* the holder's initials; "" on records from before them */
    int      record_verified;
    char     fee[48];           /* quote for submitting a score; "" until known */
    char     status[160];       /* newest event, for the HUD */
    int      status_error;
    int      busy;              /* jobs in flight */
    int      proven;            /* results are proven (a verifier is set) */
    int      board_state;       /* 0 not read yet, 1 read (n may be 0), -1 failed */
    board    board;             /* the top 10, from the Bulletin chain */
    char     board_cid[CHAIN_CID_STR_MAX];
    int      has_bulletin;      /* a Bulletin node is configured */
    int      has_operator;      /* an operator is configured */
    int      watch_state;       /* replay fetch: 0 none, 1 fetching, 2 ready, -1 failed */
} chain_view;

typedef struct chain chain;

/* Starts opening the session: the player adds a funded session key as a proxy. */
chain *chain_open(const chain_config *cfg, char *err, size_t err_len);
/* Runs finished callbacks; call once a frame. */
void chain_poll(chain *ch);
const chain_view *chain_view_get(const chain *ch);

/*
 * The game just ended with this score: keeps its replay (encoded, with its
 * CID) for chain_submit and quotes the fee of recording it (view.fee).
 */
void chain_set_replay(chain *ch, const replay *r, uint32_t score);
/*
 * Records that score, its replay's CID and the player's initials (3 letters
 * A-Z) on chain, then claims it with the operator, replay attached: a
 * top-10 score goes on the board, a new record wins a trophy.
 */
void chain_submit(chain *ch, const char *name);

/* Fetches a replay from Bulletin by CID (a board entry's), checked against it. */
void chain_watch(chain *ch, const char *cid);
/* Once watch_state is 2: the replay, freed with free(); resets watch_state. */
int chain_take_watch(chain *ch, uint8_t **data, size_t *len);

/* Starts closing: leftover funds back to the player, proxy removed. */
void chain_close(chain *ch);
/* Waits for running jobs (poll until state is CLOSED), then frees. */
void chain_free(chain *ch);

#endif
