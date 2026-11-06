/*
 * A game-style frame loop that never blocks on the network: it shows a
 * player's balance and whether they own an NFT, updated as the chain
 * finalizes blocks, while a one-off asset lookup runs in the background.
 *
 *   ./game_loop ADDRESS [COLLECTION ITEM [RPC [RELAY CHECKPOINT]]]
 *
 * With RELAY and a CHECKPOINT file (pdot-bulletin checkpoint creates one),
 * every value is proven from relay-chain finality instead of trusted.
 * Defaults: Polkadot Asset Hub, NFT 752/56.
 *
 *   ./game_loop 15wD8upZxRZijKkF7JZDdaaFXuRZJhFyYpFQC5j6FaZL5Pza
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include <pdot.h>

/* What the "game" displays; written only by callbacks, on the game thread. */
struct game {
    uint8_t  player[32];
    char     balance[64];
    char     usdt[64];
    int      owns_sword;
    uint32_t block;
    int      verified;
    int      changes;
};

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { 0, 0 };
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static void on_balance(void *ud, int rc, const char *error, const void *result)
{
    struct game *g = ud;
    const pdot_balance *b = result;

    if (rc == PDOT_E_CANCELLED)
        return;
    if (rc) {
        fprintf(stderr, "balance: %s\n", error);
        return;
    }
    pdot_u128_format(b->free, 10, g->balance, sizeof g->balance);
    g->block = b->at.number;
    g->verified = b->at.verified;
    g->changes++;
}

static void on_sword(void *ud, int rc, const char *error, const void *result)
{
    struct game *g = ud;
    const pdot_nft_item *item = result;

    if (rc == PDOT_E_CANCELLED)
        return;
    if (rc) {
        fprintf(stderr, "nft: %s\n", error);
        return;
    }
    g->owns_sword = item->exists && memcmp(item->owner, g->player, 32) == 0;
    g->changes++;
}

static void on_usdt(void *ud, int rc, const char *error, const void *result)
{
    struct game *g = ud;
    const pdot_asset_info *info = result;

    if (rc) {
        snprintf(g->usdt, sizeof g->usdt, "unavailable (%s)", error);
        return;
    }
    snprintf(g->usdt, sizeof g->usdt, "%s, %u decimals", info->symbol, info->decimals);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;

    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) ||
        !(p = malloc((size_t)n)) || fread(p, 1, (size_t)n, f) != (size_t)n) {
        if (f)
            fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

int main(int argc, char **argv)
{
    const char *rpc = argc > 4 ? argv[4] : "https://polkadot-asset-hub-rpc.polkadot.io";
    uint32_t collection = argc > 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 752;
    uint32_t item = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 56;
    struct game g;
    pdot_client *c;
    pdot_async *a;
    unsigned frame;
    int shown = -1;

    memset(&g, 0, sizeof g);
    strcpy(g.balance, "...");
    strcpy(g.usdt, "...");
    if (argc < 2 || pdot_ss58_decode(argv[1], g.player, NULL)) {
        fprintf(stderr, "usage: %s ADDRESS [COLLECTION ITEM [RPC [RELAY CHECKPOINT]]]\n", argv[0]);
        return 2;
    }
    if (!(c = pdot_client_new(rpc, NULL)))
        return 1;
    if (argc > 6) {
        size_t len;
        uint8_t *ck = read_file(argv[6], &len);
        if (!ck || pdot_client_set_verifier(c, argv[5], 1000, ck, len)) {
            fprintf(stderr, "cannot load checkpoint %s\n", argv[6]);
            pdot_client_free(c);
            free(ck);
            return 1;
        }
        free(ck);
    }

    /* From here the client belongs to pdot_async. */
    a = pdot_async_new(c);
    pdot_async_watch_balance(a, g.player, on_balance, &g);
    pdot_async_watch_nft_item(a, collection, item, on_sword, &g);
    pdot_async_asset_info(a, 1984, on_usdt, &g);

    /* 60 frames a second for 30 seconds. */
    for (frame = 0; frame < 60 * 30; frame++) {
        pdot_async_poll(a);  /* callbacks run here, on this thread */
        if (g.changes != shown) {
            printf("frame %4u  block #%u%s  balance %s DOT  owns NFT %u/%u: %s  USDt: %s\n",
                   frame, g.block, g.verified ? " (proven)" : "", g.balance, collection, item,
                   g.owns_sword ? "yes" : "no", g.usdt);
            fflush(stdout);
            shown = g.changes;
        }
        sleep_ms(16);  /* render, simulate, ... */
    }
    pdot_async_free(a);
    return 0;
}
