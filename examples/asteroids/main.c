/*
 * pdot-asteroids: a small vector space-rocks game whose scores go on chain
 * through libpdot. The frame loop never waits on the network: chain work runs
 * in pdot_async (see chain.c) and the HUD shows how it is going.
 *
 *   asteroids [--rpc URL] [--bulletin URL|off] [--operator URL|off] [--player SEED] [--collection N]
 *             [--fund AMOUNT] [--session FILE] [--metadata-hash on|off|SYM:DEC]
 *             [--relay URL --checkpoint FILE [--para-id N]] [--offline]
 *             [--screenshot FILE]
 *
 * Defaults suit devnet.sh and asteroids-operator: local Asset Hub and
 * Bulletin dev nodes (the leaderboard lives on Bulletin), the operator on
 * http://127.0.0.1:8090 and //Alice as the player. The game holds no
 * operator key: the operator checks each score on chain before it updates
 * the board or mints a trophy. Never pass a real key on the command line.
 *
 * The game itself is sim.c, stepped at a fixed 60 ticks a second; each
 * tick's input is recorded, and the replay goes with the score so the
 * operator can re-run the game (replay.h).
 *
 * On the title screen a row's number (0 for the tenth) plays that board
 * entry's replay, fetched from Bulletin: the same sim.c re-runs the game
 * on screen, and the end shows whether it reaches the board's score.
 *
 * Keys: arrows turn and thrust, space fires. At game over, type your initials
 * (or pick them with the arrows) and press enter to record the score; escape
 * goes back to the title and its top 10. On the title, escape (or closing
 * the window, anywhere) quits, closing the session on chain first.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "chain.h"
#include "font.h"
#include "replay.h"
#include "sim.h"

#define W SIM_W
#define H SIM_H
#define PI_F SIM_PI

/* //Alice, the well-known dev key (`subkey inspect //Alice`). */
static const char *DEV_PLAYER = "e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a";

enum { S_TITLE, S_PLAY, S_OVER, S_QUIT, S_WATCH };

typedef struct {
    int      state;
    sim      s;             /* the game, or drifting rocks behind the menus */
    replay   rec;           /* this game's inputs */
    int      submitted;     /* this game's score was sent */
    Uint32   quit_at;       /* when closing started */
    char     name[4];       /* initials for the score, kept between games */
    int      cursor;        /* the initial being edited, 0..2 */
    SDL_FPoint stars[80];
    /* watching a replay */
    int         watch_pending;  /* asked the chain for one */
    board_entry watch_entry;    /* whose game it is */
    replay      watch;          /* its inputs */
    size_t      watch_at;       /* ticks played */
    uint8_t     watch_input;    /* the current tick's, for the thrust flame */
    char        watch_error[96];
} game;

/* Rendering only: the game's own randomness is sim.c's. */
static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

/* Any seed will do: the replay carries it. */
static uint32_t new_seed(void)
{
    return (uint32_t)SDL_GetPerformanceCounter() ^ ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

static void new_game(game *g)
{
    uint32_t seed = new_seed();
    sim_new(&g->s, seed);
    replay_begin(&g->rec, seed);
    g->submitted = 0;
    g->state = S_PLAY;
}

static uint8_t read_input(const Uint8 *keys)
{
    return (uint8_t)((keys[SDL_SCANCODE_LEFT] ? SIM_LEFT : 0) |
                     (keys[SDL_SCANCODE_RIGHT] ? SIM_RIGHT : 0) |
                     (keys[SDL_SCANCODE_UP] ? SIM_THRUST : 0) |
                     (keys[SDL_SCANCODE_SPACE] ? SIM_FIRE : 0));
}

/* One fixed tick: the game while playing, drifting rocks otherwise. */
static void tick(game *g, const Uint8 *keys, chain *ch)
{
    uint8_t input;

    if (g->state == S_WATCH) {
        /* holding shift plays four ticks per tick */
        int n = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] ? 4 : 1;
        while (n-- && g->watch_at < g->watch.n && !g->s.over) {
            g->watch_input = g->watch.inputs[g->watch_at++];
            sim_step(&g->s, g->watch_input);
        }
        return;
    }
    if (g->state != S_PLAY) {
        sim_drift(&g->s);
        return;
    }
    input = read_input(keys);
    replay_add(&g->rec, input);
    sim_step(&g->s, input);
    if (g->s.over) {
        g->state = S_OVER;
        g->cursor = 0;
        if (ch)
            chain_set_replay(ch, &g->rec, g->s.score);
    }
}

/* Reads back the frame being drawn (before SDL_RenderPresent). */
static void save_screenshot(SDL_Renderer *r, const char *path)
{
    int w, h;
    SDL_Surface *s;

    SDL_GetRendererOutputSize(r, &w, &h);
    s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s && SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0 &&
        SDL_SaveBMP(s, path) == 0)
        fprintf(stderr, "saved %s\n", path);
    else
        fprintf(stderr, "screenshot: %s\n", SDL_GetError());
    SDL_FreeSurface(s);
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_ship(SDL_Renderer *r, const sim *g, int thrust)
{
    float c = cosf(g->angle), s = sinf(g->angle);
    static const float pts[][2] = { { 16, 0 }, { -10, -10 }, { -5, 0 }, { -10, 10 }, { 16, 0 } };
    SDL_FPoint v[5];
    int i;

    if (g->invulnerable > 0 && ((int)(g->invulnerable * 8) & 1))
        return;  /* blink while invulnerable */
    for (i = 0; i < 5; i++) {
        v[i].x = g->p.x + pts[i][0] * c - pts[i][1] * s;
        v[i].y = g->p.y + pts[i][0] * s + pts[i][1] * c;
    }
    SDL_RenderDrawLinesF(r, v, 5);
    if (thrust && (SDL_GetTicks() / 60) % 2) {
        SDL_RenderDrawLineF(r, g->p.x - 6 * c + 4 * s, g->p.y - 6 * s - 4 * c,
                            g->p.x - 18 * c, g->p.y - 18 * s);
        SDL_RenderDrawLineF(r, g->p.x - 6 * c - 4 * s, g->p.y - 6 * s + 4 * c,
                            g->p.x - 18 * c, g->p.y - 18 * s);
    }
}

/* A small ship pointing up, for the lives counter: filled while the life is left. */
static void draw_life(SDL_Renderer *r, float x, float y, int filled)
{
    /* nose, right wing, notch, left wing: the ship's outline, turned up */
    const SDL_FPoint p[5] = { { x, y - 13 }, { x + 9, y + 9 }, { x, y + 4 }, { x - 9, y + 9 },
                              { x, y - 13 } };

    if (filled) {
        static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_Vertex v[4];
        Uint8 c[4];
        int k;
        SDL_GetRenderDrawColor(r, &c[0], &c[1], &c[2], &c[3]);
        for (k = 0; k < 4; k++) {
            v[k].position = p[k];
            v[k].color.r = c[0];
            v[k].color.g = c[1];
            v[k].color.b = c[2];
            v[k].color.a = c[3];
            v[k].tex_coord.x = v[k].tex_coord.y = 0;
        }
        SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
    }
    SDL_RenderDrawLinesF(r, p, 5);
}

static void draw_rock(SDL_Renderer *ren, const sim_rock *r)
{
    SDL_FPoint v[SIM_VERTS + 1];
    float rad = sim_rock_radius(r->size);
    int k;

    for (k = 0; k <= SIM_VERTS; k++) {
        float a = r->angle + 2 * PI_F * (float)k / SIM_VERTS;
        float d = rad * r->shape[k % SIM_VERTS];
        v[k].x = r->p.x + cosf(a) * d;
        v[k].y = r->p.y + sinf(a) * d;
    }
    SDL_RenderDrawLinesF(ren, v, SIM_VERTS + 1);
}

/* The bottom bar: chain state for everyone to see. */
static void draw_hud(SDL_Renderer *r, const chain_view *v, int offline)
{
    char line[200];
    static const char spin[] = "-/|\\";

    SDL_SetRenderDrawColor(r, 40, 60, 80, 255);
    SDL_RenderDrawLine(r, 0, H - 44, W, H - 44);
    if (offline) {
        SDL_SetRenderDrawColor(r, 120, 140, 160, 255);
        font_draw(r, 12, H - 34, 1.0f, "OFFLINE: NO CHAIN");
        return;
    }
    SDL_SetRenderDrawColor(r, 120, 200, 255, 255);
    snprintf(line, sizeof line, "BLOCK %u%s   SESSION KEY %s   %c", v->head,
             v->head_verified ? " PROVEN" : "", v->session_balance,
             v->busy ? spin[(SDL_GetTicks() / 120) % 4] : ' ');
    font_draw(r, 12, H - 38, 1.0f, line);
    if (v->status_error)
        SDL_SetRenderDrawColor(r, 255, 110, 110, 255);
    else
        SDL_SetRenderDrawColor(r, 160, 255, 160, 255);
    snprintf(line, sizeof line, "%.110s", v->status);
    font_draw(r, 12, H - 20, 0.9f, line);
}

static void short_addr(const char *a, char *out, size_t cap)
{
    if (strlen(a) > 12)
        snprintf(out, cap, "%.6s..%s", a, a + strlen(a) - 4);
    else
        snprintf(out, cap, "%s", a);
}

static void draw_record(SDL_Renderer *r, const chain_view *v, float y)
{
    char line[160], who[24];

    if (v->has_record) {
        short_addr(v->record_holder, who, sizeof who);
        if (v->record_name[0])
            snprintf(line, sizeof line, "RECORD %u BY %s (%s)%s", v->record, v->record_name,
                     who, v->record_verified ? " PROVEN" : "");
        else
            snprintf(line, sizeof line, "RECORD %u BY %s%s", v->record, who,
                     v->record_verified ? " (PROVEN)" : "");
    } else {
        snprintf(line, sizeof line, v->state == CHAIN_READY ? "NO RECORD YET" : "RECORD: ...");
    }
    font_draw_centered(r, W / 2.0f, y, 1.4f, line);
    if (v->trophies >= 0) {
        snprintf(line, sizeof line, "YOUR TROPHIES: %d", v->trophies);
        font_draw_centered(r, W / 2.0f, y + 26, 1.2f, line);
    }
}

/* The title screen's top 10, read from the Bulletin chain; the player's own in yellow. */
static void draw_board(SDL_Renderer *r, const chain_view *v)
{
    char line[160], who[24];
    size_t i;
    float y = 150;

    short_addr(v->player, who, sizeof who);
    if (v->trophies >= 0)
        snprintf(line, sizeof line, "PLAYER %s   TROPHIES %d", who, v->trophies);
    else
        snprintf(line, sizeof line, "PLAYER %s", who);
    font_draw_centered(r, W / 2.0f, y, 1.2f, line);
    y += 44;
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    font_draw_centered(r, W / 2.0f, y, 2.0f, "TOP 10");
    y += 40;
    SDL_SetRenderDrawColor(r, 120, 200, 255, 255);
    if (v->board_state == 0 || (v->board_state == 1 && v->board.n == 0)) {
        font_draw_centered(r, W / 2.0f, y + 60, 1.4f,
                           v->board_state ? "NO SCORES YET: BE THE FIRST" : "READING THE LEADERBOARD...");
        return;
    }
    if (v->board_state < 0) {
        SDL_SetRenderDrawColor(r, 255, 110, 110, 255);
        font_draw_centered(r, W / 2.0f, y + 60, 1.4f, "LEADERBOARD UNAVAILABLE");
        return;
    }
    for (i = 0; i < v->board.n; i++, y += 26) {
        const board_entry *e = &v->board.e[i];
        short_addr(e->account, who, sizeof who);
        snprintf(line, sizeof line, "%2u. %s %8u   %-14s BLOCK %-8u", (unsigned)(i + 1),
                 e->name, e->score, who, e->block);
        if (!e->replay[0])  /* from before replays: nothing to watch */
            SDL_SetRenderDrawColor(r, 90, 100, 120, 255);
        else if (strcmp(e->account, v->player) == 0)
            SDL_SetRenderDrawColor(r, 255, 230, 120, 255);
        else
            SDL_SetRenderDrawColor(r, 120, 200, 255, 255);
        /* Every row has the same width (fixed fields), so they line up as columns. */
        font_draw(r, (W - font_width(1.3f, line)) / 2.0f, y, 1.3f, line);
    }
}

/* Game over: the initials to record with the score, arcade style. */
/* The submitted score: pending in a block first, then final and claimed. */
static void draw_score_step(SDL_Renderer *r, const chain_view *v)
{
    static const char spin[] = "-/|\\";
    char line[128];
    int waiting = v->score_step == CHAIN_STEP_SENT || v->score_step == CHAIN_STEP_INCLUDED ||
                  v->score_step == CHAIN_STEP_CLAIMING;

    if (v->score_step == CHAIN_STEP_FAILED)
        SDL_SetRenderDrawColor(r, 255, 110, 110, 255);
    else if (v->score_step == CHAIN_STEP_INCLUDED)
        SDL_SetRenderDrawColor(r, 255, 230, 120, 255);  /* pending: a reorg could drop it */
    else if (v->score_step == CHAIN_STEP_DONE)
        SDL_SetRenderDrawColor(r, 160, 255, 160, 255);
    else
        SDL_SetRenderDrawColor(r, 200, 200, 210, 255);
    font_draw_centered(r, W / 2.0f, 384, 1.4f, "YOUR SCORE");
    snprintf(line, sizeof line, "%s%s%c", v->score_text, waiting ? " " : "",
             waiting ? spin[(SDL_GetTicks() / 120) % 4] : ' ');
    font_draw_centered(r, W / 2.0f, 414, 1.4f, line);
}

static void draw_initials(SDL_Renderer *r, const game *g, const chain_view *v)
{
    const float scale = 4.0f, step = 64.0f, x0 = W / 2.0f - step;
    char line[160], letter[2] = { 0, 0 };
    int k;

    SDL_SetRenderDrawColor(r, 255, 230, 120, 255);
    font_draw_centered(r, W / 2.0f, 374, 1.4f, "ENTER YOUR INITIALS");
    for (k = 0; k < 3; k++) {
        float cx = x0 + step * (float)k;
        letter[0] = g->name[k];
        font_draw_centered(r, cx, 404, scale, letter);
        if (k == g->cursor && (SDL_GetTicks() / 300) % 2 == 0)
            SDL_RenderDrawLineF(r, cx - 14, 456, cx + 14, 456);  /* blinking cursor */
    }
    snprintf(line, sizeof line, "ENTER: RECORD IT ON CHAIN (FEE %s)", v->fee[0] ? v->fee : "...");
    font_draw_centered(r, W / 2.0f, 488, 1.5f, line);
    {
        int rank = v->board_state == 1 ? board_rank(&v->board, g->s.score) : 0;
        int record = !v->has_record || g->s.score > v->record;
        if (rank)
            snprintf(line, sizeof line, "NUMBER %d IN THE TOP 10%s", rank,
                     record ? ", AND A NEW RECORD: A TROPHY NFT" : "");
        else
            snprintf(line, sizeof line, "%s", record ? "A NEW RECORD EARNS A TROPHY NFT" : "");
        font_draw_centered(r, W / 2.0f, 516, 1.2f, line);
    }
    SDL_SetRenderDrawColor(r, 150, 150, 160, 255);
    font_draw_centered(r, W / 2.0f, 544, 1.0f, "TYPE A-Z, OR ARROWS: UP/DOWN CHANGE, LEFT/RIGHT MOVE");
}

/* Keys on the game-over screen that edit the initials; 1 if k was one. */
static int edit_initials(game *g, SDL_Keycode k)
{
    char *c = &g->name[g->cursor];

    if (k >= SDLK_a && k <= SDLK_z) {
        *c = (char)('A' + (k - SDLK_a));
        if (g->cursor < 2)
            g->cursor++;
    } else if (k == SDLK_UP) {
        *c = *c == 'Z' ? 'A' : (char)(*c + 1);
    } else if (k == SDLK_DOWN) {
        *c = *c == 'A' ? 'Z' : (char)(*c - 1);
    } else if (k == SDLK_LEFT || k == SDLK_BACKSPACE) {
        if (g->cursor > 0)
            g->cursor--;
    } else if (k == SDLK_RIGHT) {
        if (g->cursor < 2)
            g->cursor++;
    } else {
        return 0;
    }
    return 1;
}

static void draw(SDL_Renderer *r, const game *g, const chain_view *v, int offline,
                 const Uint8 *keys, const char *screenshot)
{
    char line[160], who[24];
    int i;

    SDL_SetRenderDrawColor(r, 4, 6, 12, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 70, 80, 100, 255);
    SDL_RenderDrawPointsF(r, g->stars, 80);

    SDL_SetRenderDrawColor(r, 220, 220, 230, 255);
    for (i = 0; i < SIM_ROCKS; i++)
        if (g->s.rocks[i].size)
            draw_rock(r, &g->s.rocks[i]);

    switch (g->state) {
    case S_TITLE:
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_draw_centered(r, W / 2.0f, v->has_bulletin && !offline ? 50 : 150, 5.0f,
                           "PDOT ASTEROIDS");
        SDL_SetRenderDrawColor(r, 120, 200, 255, 255);
        if (!offline && v->has_bulletin) {
            draw_board(r, v);
        } else if (!offline) {
            short_addr(v->player, who, sizeof who);
            snprintf(line, sizeof line, "PLAYER %s", who);
            font_draw_centered(r, W / 2.0f, 260, 1.4f, line);
            draw_record(r, v, 300);
        }
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_draw_centered(r, W / 2.0f, 560, 2.0f, "PRESS SPACE TO PLAY");
        font_draw_centered(r, W / 2.0f, 600, 1.0f, "ARROWS TURN AND THRUST, SPACE FIRES, ESC QUITS");
        if (!offline && v->board_state == 1 && v->board.n) {
            SDL_SetRenderDrawColor(r, 120, 200, 255, 255);
            if (g->watch_pending)
                font_draw_centered(r, W / 2.0f, 626, 1.0f, "FETCHING THE REPLAY FROM BULLETIN...");
            else if (g->watch_error[0])
                font_draw_centered(r, W / 2.0f, 626, 1.0f, g->watch_error);
            else
                font_draw_centered(r, W / 2.0f, 626, 1.0f,
                                   "A ROW'S NUMBER WATCHES ITS REPLAY (0 FOR 10)");
        }
        break;
    case S_WATCH:
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (i = 0; i < SIM_SHOTS; i++)
            if (g->s.shots[i].life > 0)
                SDL_RenderDrawPointF(r, g->s.shots[i].p.x, g->s.shots[i].p.y),
                SDL_RenderDrawPointF(r, g->s.shots[i].p.x + 1, g->s.shots[i].p.y);
        if (!g->s.over)
            draw_ship(r, &g->s, g->watch_input & SIM_THRUST);
        snprintf(line, sizeof line, "%06u", g->s.score);
        font_draw(r, 16, 14, 2.0f, line);
        for (i = 0; i < 3; i++)
            draw_life(r, (float)(W - 24 - i * 26), 30, i < g->s.lives);
        SDL_SetRenderDrawColor(r, 255, 230, 120, 255);
        snprintf(line, sizeof line, "REPLAY: %s %u (BLOCK %u)", g->watch_entry.name,
                 g->watch_entry.score, g->watch_entry.block);
        font_draw_centered(r, W / 2.0f, 20, 1.4f, line);
        snprintf(line, sizeof line, "%u:%02u   HOLD SHIFT: FAST   ESC: BACK",
                 (unsigned)(g->watch_at / SIM_HZ / 60), (unsigned)(g->watch_at / SIM_HZ % 60));
        font_draw_centered(r, W / 2.0f, 46, 1.0f, line);
        if (g->s.over || g->watch_at == g->watch.n) {
            int ok = g->s.over && g->watch_at == g->watch.n && g->s.score == g->watch_entry.score;
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            snprintf(line, sizeof line, "REPLAY ENDED: SCORE %u", g->s.score);
            font_draw_centered(r, W / 2.0f, 260, 2.5f, line);
            if (ok)
                SDL_SetRenderDrawColor(r, 160, 255, 160, 255);
            else
                SDL_SetRenderDrawColor(r, 255, 110, 110, 255);
            font_draw_centered(r, W / 2.0f, 320, 1.5f,
                               ok ? "IT MATCHES THE BOARD: THE GAME WAS PLAYED"
                                  : "IT DOES NOT MATCH THE BOARD");
        }
        break;
    case S_PLAY:
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (i = 0; i < SIM_SHOTS; i++)
            if (g->s.shots[i].life > 0)
                SDL_RenderDrawPointF(r, g->s.shots[i].p.x, g->s.shots[i].p.y),
                SDL_RenderDrawPointF(r, g->s.shots[i].p.x + 1, g->s.shots[i].p.y);
        draw_ship(r, &g->s, keys[SDL_SCANCODE_UP]);
        snprintf(line, sizeof line, "%06u", g->s.score);
        font_draw(r, 16, 14, 2.0f, line);
        for (i = 0; i < 3; i++)  /* right to left: lost lives go hollow from the left */
            draw_life(r, (float)(W - 24 - i * 26), 30, i < g->s.lives);
        break;
    case S_OVER:
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_draw_centered(r, W / 2.0f, 160, 4.0f, "GAME OVER");
        snprintf(line, sizeof line, "SCORE %u", g->s.score);
        font_draw_centered(r, W / 2.0f, 240, 2.5f, line);
        SDL_SetRenderDrawColor(r, 120, 200, 255, 255);
        if (!offline) {
            draw_record(r, v, 300);
            if (!g->submitted && v->state == CHAIN_READY)
                draw_initials(r, g, v);
            else if (g->submitted && v->score_step != CHAIN_STEP_NONE)
                draw_score_step(r, v);
        }
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_draw_centered(r, W / 2.0f, 590, 1.5f, "SPACE: PLAY AGAIN   ESC: TOP 10");
        break;
    case S_QUIT:
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_draw_centered(r, W / 2.0f, 260, 3.0f, "CLOSING THE SESSION");
        font_draw_centered(r, W / 2.0f, 320, 1.2f,
                           "LEFTOVER FUNDS GO BACK TO THE PLAYER, THEN THE PROXY IS REMOVED");
        if (SDL_GetTicks() - g->quit_at > 2000)
            font_draw_centered(r, W / 2.0f, 350, 1.0f,
                               "ESC AGAIN TO QUIT NOW: THE NEXT RUN FINISHES THE CLEANUP");
        break;
    }
    draw_hud(r, v, offline);
    if (screenshot)
        save_screenshot(r, screenshot);
    SDL_RenderPresent(r);
}

/* ---- main --------------------------------------------------------------- */


static int parse_seed(const char *hex, uint8_t out[32])
{
    int i;
    unsigned b;

    if (hex[0] == '0' && hex[1] == 'x')
        hex += 2;
    if (strlen(hex) != 64)
        return -1;
    for (i = 0; i < 32; i++) {
        if (sscanf(hex + 2 * i, "%2x", &b) != 1)
            return -1;
        out[i] = (uint8_t)b;
    }
    return 0;
}

static int usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--rpc URL] [--bulletin URL|off] [--operator URL|off] [--player SEED] [--collection N]\n"
            "          [--fund AMOUNT] [--session FILE] [--metadata-hash on|off|SYM:DEC]\n"
            "          [--relay URL --checkpoint FILE [--para-id N]] [--offline]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv)
{
    chain_config cfg;
    const char *player = DEV_PLAYER;
    static game g;
    chain_view offline_view;
    chain *ch = NULL;
    const char *screenshot = NULL;  /* saves the title screen as BMP, then quits */
    int offline = 0, running = 1, i;
    SDL_Window *win;
    SDL_Renderer *ren;
    Uint64 last;
    double behind = 0;  /* simulated time owed to the clock, in seconds */
    char err[256];

    memset(&cfg, 0, sizeof cfg);
    cfg.rpc = "http://127.0.0.1:9944";
    cfg.bulletin_rpc = "http://127.0.0.1:9945";
    cfg.operator_url = "http://127.0.0.1:8090";
    cfg.fund = "1";
    cfg.session_file = "asteroids-session.key";
    cfg.metadata_hash = "on";
    cfg.para_id = 1000;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--offline") == 0) {
            offline = 1;
            continue;
        }
        if (!val)
            return usage(argv[0]);
        if (strcmp(a, "--rpc") == 0) cfg.rpc = val;
        else if (strcmp(a, "--bulletin") == 0) cfg.bulletin_rpc = strcmp(val, "off") ? val : NULL;
        else if (strcmp(a, "--player") == 0) player = val;
        else if (strcmp(a, "--operator") == 0) cfg.operator_url = strcmp(val, "off") ? val : NULL;
        else if (strcmp(a, "--collection") == 0) cfg.collection = (uint32_t)strtoul(val, NULL, 10);
        else if (strcmp(a, "--fund") == 0) cfg.fund = val;
        else if (strcmp(a, "--session") == 0) cfg.session_file = val;
        else if (strcmp(a, "--metadata-hash") == 0) cfg.metadata_hash = val;
        else if (strcmp(a, "--relay") == 0) cfg.relay = val;
        else if (strcmp(a, "--checkpoint") == 0) cfg.checkpoint = val;
        else if (strcmp(a, "--para-id") == 0) cfg.para_id = (uint32_t)strtoul(val, NULL, 10);
        else if (strcmp(a, "--screenshot") == 0) screenshot = val;
        else return usage(argv[0]);
        i++;
    }
    if (parse_seed(player, cfg.player_seed)) {
        fprintf(stderr, "error: seeds are 32 bytes of hex\n");
        return 2;
    }
    if (!offline && !(ch = chain_open(&cfg, err, sizeof err))) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    memset(&offline_view, 0, sizeof offline_view);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        chain_free(ch);
        return 1;
    }
    win = SDL_CreateWindow("pdot asteroids", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                           SDL_WINDOW_ALLOW_HIGHDPI);
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
              : NULL;
    if (!ren) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        chain_free(ch);
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, W, H);

    srand((unsigned)SDL_GetTicks64() ^ 0x5eed);
    for (i = 0; i < 80; i++) {
        g.stars[i].x = frand(0, W);
        g.stars[i].y = frand(0, H - 46);
    }
    g.state = S_TITLE;
    memcpy(g.name, "AAA", 4);
    sim_new(&g.s, new_seed());  /* drifting rocks behind the title */

    last = SDL_GetPerformanceCounter();
    while (running) {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        const chain_view *v = ch ? chain_view_get(ch) : &offline_view;
        Uint64 now = SDL_GetPerformanceCounter();
        SDL_Event e;

        behind += (double)(now - last) / (double)SDL_GetPerformanceFrequency();
        last = now;
        if (behind > 0.25)
            behind = 0.25;  /* after a stall: slow down rather than jump */
        if (ch)
            chain_poll(ch);  /* chain callbacks run here, on the game thread */

        while (SDL_PollEvent(&e)) {
            /* Closing the window is like escape: close the session first. */
            int quit = e.type == SDL_QUIT ||
                       (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE);
            if (e.type == SDL_KEYDOWN && e.key.repeat) {
                continue;
            } else if (quit && e.type == SDL_KEYDOWN && (g.state == S_WATCH || g.state == S_OVER)) {
                /* escape from a replay or the game over: back to the title and its top 10 */
                g.state = S_TITLE;
                sim_new(&g.s, new_seed());
            } else if (quit) {
                if (!ch) {
                    running = 0;
                } else if (g.state != S_QUIT) {
                    g.state = S_QUIT;
                    g.quit_at = SDL_GetTicks();
                    chain_close(ch);
                } else if (SDL_GetTicks() - g.quit_at > 2000) {
                    running = 0;  /* the next run resumes or cleans up the session */
                }
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                int can_submit = g.state == S_OVER && ch && !g.submitted &&
                                 v->state == CHAIN_READY;
                if (k == SDLK_SPACE && (g.state == S_TITLE || g.state == S_OVER)) {
                    new_game(&g);
                } else if (g.state == S_TITLE && ch && k >= SDLK_0 && k <= SDLK_9 &&
                           !g.watch_pending && v->board_state == 1) {
                    size_t row = k == SDLK_0 ? 9 : (size_t)(k - SDLK_1);
                    g.watch_error[0] = '\0';
                    if (row >= v->board.n)
                        ;
                    else if (!v->board.e[row].replay[0])
                        snprintf(g.watch_error, sizeof g.watch_error,
                                 "ROW %u HAS NO REPLAY: IT IS FROM BEFORE REPLAYS",
                                 (unsigned)row + 1);
                    else {
                        g.watch_entry = v->board.e[row];
                        g.watch_pending = 1;
                        chain_watch(ch, g.watch_entry.replay);
                    }
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && can_submit) {
                    g.submitted = 1;
                    chain_submit(ch, g.name);
                } else if (can_submit) {
                    edit_initials(&g, k);
                }
            }
        }

        if (g.watch_pending && v->watch_state != 1) {
            uint8_t *data;
            size_t len;
            g.watch_pending = 0;
            if (v->watch_state == -1 || chain_take_watch(ch, &data, &len)) {
                snprintf(g.watch_error, sizeof g.watch_error, "THE REPLAY COULD NOT BE FETCHED");
            } else {
                char why[96];
                replay_free(&g.watch);
                if (replay_decode(data, len, &g.watch, why, sizeof why)) {
                    snprintf(g.watch_error, sizeof g.watch_error, "NOT A REPLAY");
                } else if (g.state == S_TITLE) {
                    sim_new(&g.s, g.watch.seed);
                    g.watch_at = 0;
                    g.watch_input = 0;
                    g.state = S_WATCH;
                }
                free(data);
            }
        }
        /* Fixed ticks, as many as the clock asks for: never the frame time. */
        while (behind >= 1.0 / SIM_HZ) {
            tick(&g, keys, ch);
            behind -= 1.0 / SIM_HZ;
        }
        if (g.state == S_QUIT && v->state == CHAIN_CLOSED && !v->busy)
            running = 0;
        if (screenshot && g.state == S_TITLE && SDL_GetTicks() > 1500 &&
            (offline || (v->state != CHAIN_OPENING && !v->busy && (!v->has_bulletin || v->board_state)))) {
            draw(ren, &g, v, offline, keys, screenshot);
            screenshot = NULL;
            g.state = S_QUIT;
            g.quit_at = SDL_GetTicks();
            if (ch)
                chain_close(ch);
            else
                running = 0;
            continue;
        }
        draw(ren, &g, v, offline, keys, NULL);
    }

    chain_free(ch);
    replay_free(&g.rec);
    replay_free(&g.watch);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
