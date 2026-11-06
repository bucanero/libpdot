/*
 * pdot-asteroids: a small vector space-rocks game whose scores go on chain
 * through libpdot. The frame loop never waits on the network: chain work runs
 * in pdot_async (see chain.c) and the HUD shows how it is going.
 *
 *   asteroids [--rpc URL] [--bulletin URL|off] [--player SEED] [--operator SEED] [--collection N]
 *             [--fund AMOUNT] [--session FILE] [--metadata-hash on|off|SYM:DEC]
 *             [--relay URL --checkpoint FILE [--para-id N]] [--offline]
 *             [--screenshot FILE]
 *
 * Defaults suit devnet.sh: local Asset Hub and Bulletin dev nodes (the
 * leaderboard lives on Bulletin), //Alice as the player and //Bob as the
 * game's operator. Never pass a real key on the command line.
 *
 * Keys: arrows turn and thrust, space fires. At game over, type your initials
 * (or pick them with the arrows) and press enter to record the score; escape
 * quits, closing the session on chain first.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "chain.h"
#include "font.h"

#define W 960
#define H 720
#define MAX_ROCKS   64
#define MAX_SHOTS   8
#define ROCK_VERTS  11
#define PI_F        3.14159265f

/* //Alice and //Bob, the well-known dev keys (`subkey inspect //Alice`). */
static const char *DEV_PLAYER = "e5be9a5092b81bca64be81d212e7f2f9eba183bb7a90954f7b76361f6edb5c0a";
static const char *DEV_OPERATOR = "398f0c28f98885e046333d4a41c19cee4c37368a9832c6502f6cfd182e2aef89";

typedef struct { float x, y; } vec2;

typedef struct {
    vec2  p, v;
    float angle, spin;
    int   size;  /* 3 big, 2, 1 small; 0 free slot */
    float shape[ROCK_VERTS];
} rock;

typedef struct {
    vec2  p, v;
    float life;
} shot;

enum { S_TITLE, S_PLAY, S_OVER, S_QUIT };

typedef struct {
    int      state;
    vec2     p, v;
    float    angle;
    float    invulnerable;  /* seconds */
    float    fire_cooldown;
    int      lives, level;
    uint32_t score;
    int      submitted;     /* this game's score was sent */
    Uint32   quit_at;       /* when closing started */
    char     name[4];       /* initials for the score, kept between games */
    int      cursor;        /* the initial being edited, 0..2 */
    rock     rocks[MAX_ROCKS];
    shot     shots[MAX_SHOTS];
    vec2     stars[80];
} game;

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static float rock_radius(int size)
{
    return size == 3 ? 44.0f : size == 2 ? 24.0f : 12.0f;
}

static void wrap(vec2 *p)
{
    if (p->x < 0) p->x += W;
    if (p->x >= W) p->x -= W;
    if (p->y < 0) p->y += H;
    if (p->y >= H) p->y -= H;
}

static void spawn_rock(game *g, vec2 p, int size)
{
    int i, k;
    float speed = frand(30.0f, 70.0f) * (4 - size) * (1.0f + 0.1f * (float)g->level);
    float dir = frand(0, 2 * PI_F);

    for (i = 0; i < MAX_ROCKS && g->rocks[i].size; i++)
        ;
    if (i == MAX_ROCKS)
        return;
    g->rocks[i].p = p;
    g->rocks[i].v.x = cosf(dir) * speed;
    g->rocks[i].v.y = sinf(dir) * speed;
    g->rocks[i].angle = 0;
    g->rocks[i].spin = frand(-1.5f, 1.5f);
    g->rocks[i].size = size;
    for (k = 0; k < ROCK_VERTS; k++)
        g->rocks[i].shape[k] = frand(0.7f, 1.15f);
}

static void start_level(game *g)
{
    int i;
    for (i = 0; i < 3 + g->level; i++) {
        vec2 p;
        do {  /* not on top of the ship */
            p.x = frand(0, W);
            p.y = frand(0, H);
        } while (fabsf(p.x - g->p.x) < 150 && fabsf(p.y - g->p.y) < 150);
        spawn_rock(g, p, 3);
    }
}

static void reset_ship(game *g)
{
    g->p.x = W / 2.0f;
    g->p.y = H / 2.0f;
    g->v.x = g->v.y = 0;
    g->angle = -PI_F / 2;
    g->invulnerable = 2.5f;
}

static void new_game(game *g)
{
    memset(g->rocks, 0, sizeof g->rocks);
    memset(g->shots, 0, sizeof g->shots);
    g->lives = 3;
    g->level = 0;
    g->score = 0;
    g->submitted = 0;
    reset_ship(g);
    start_level(g);
    g->state = S_PLAY;
}

static int rocks_left(const game *g)
{
    int i, n = 0;
    for (i = 0; i < MAX_ROCKS; i++)
        n += g->rocks[i].size != 0;
    return n;
}

static void update(game *g, const Uint8 *keys, float dt, chain *ch)
{
    int i, j;

    if (keys[SDL_SCANCODE_LEFT])
        g->angle -= 4.0f * dt;
    if (keys[SDL_SCANCODE_RIGHT])
        g->angle += 4.0f * dt;
    if (keys[SDL_SCANCODE_UP]) {
        g->v.x += cosf(g->angle) * 300.0f * dt;
        g->v.y += sinf(g->angle) * 300.0f * dt;
    }
    g->v.x *= 1.0f - 0.4f * dt;
    g->v.y *= 1.0f - 0.4f * dt;
    g->p.x += g->v.x * dt;
    g->p.y += g->v.y * dt;
    wrap(&g->p);
    if (g->invulnerable > 0)
        g->invulnerable -= dt;

    g->fire_cooldown -= dt;
    if (keys[SDL_SCANCODE_SPACE] && g->fire_cooldown <= 0) {
        for (i = 0; i < MAX_SHOTS && g->shots[i].life > 0; i++)
            ;
        if (i < MAX_SHOTS) {
            g->shots[i].p.x = g->p.x + cosf(g->angle) * 14;
            g->shots[i].p.y = g->p.y + sinf(g->angle) * 14;
            g->shots[i].v.x = g->v.x + cosf(g->angle) * 520.0f;
            g->shots[i].v.y = g->v.y + sinf(g->angle) * 520.0f;
            g->shots[i].life = 1.0f;
            g->fire_cooldown = 0.18f;
        }
    }
    for (i = 0; i < MAX_SHOTS; i++) {
        shot *s = &g->shots[i];
        if (s->life <= 0)
            continue;
        s->life -= dt;
        s->p.x += s->v.x * dt;
        s->p.y += s->v.y * dt;
        wrap(&s->p);
    }

    for (i = 0; i < MAX_ROCKS; i++) {
        rock *r = &g->rocks[i];
        float rad;
        if (!r->size)
            continue;
        r->p.x += r->v.x * dt;
        r->p.y += r->v.y * dt;
        r->angle += r->spin * dt;
        wrap(&r->p);
        rad = rock_radius(r->size);
        for (j = 0; j < MAX_SHOTS; j++) {
            shot *s = &g->shots[j];
            float dx = s->p.x - r->p.x, dy = s->p.y - r->p.y;
            if (s->life > 0 && dx * dx + dy * dy < rad * rad) {
                int size = r->size;
                vec2 p = r->p;
                s->life = 0;
                r->size = 0;
                g->score += size == 3 ? 20 : size == 2 ? 50 : 100;
                if (size > 1) {
                    spawn_rock(g, p, size - 1);
                    spawn_rock(g, p, size - 1);
                }
                break;
            }
        }
        if (r->size && g->invulnerable <= 0) {
            float dx = g->p.x - r->p.x, dy = g->p.y - r->p.y;
            if (dx * dx + dy * dy < (rad + 10) * (rad + 10)) {
                if (--g->lives <= 0) {
                    g->state = S_OVER;
                    g->cursor = 0;
                    if (ch)
                        chain_quote(ch, g->score);
                    return;
                }
                reset_ship(g);
            }
        }
    }
    if (!rocks_left(g)) {
        g->level++;
        start_level(g);
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

static void draw_ship(SDL_Renderer *r, const game *g, int thrust)
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

static void draw_rock(SDL_Renderer *ren, const rock *r)
{
    SDL_FPoint v[ROCK_VERTS + 1];
    float rad = rock_radius(r->size);
    int k;

    for (k = 0; k <= ROCK_VERTS; k++) {
        float a = r->angle + 2 * PI_F * (float)k / ROCK_VERTS;
        float d = rad * r->shape[k % ROCK_VERTS];
        v[k].x = r->p.x + cosf(a) * d;
        v[k].y = r->p.y + sinf(a) * d;
    }
    SDL_RenderDrawLinesF(ren, v, ROCK_VERTS + 1);
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
        if (strcmp(e->account, v->player) == 0)
            SDL_SetRenderDrawColor(r, 255, 230, 120, 255);
        else
            SDL_SetRenderDrawColor(r, 120, 200, 255, 255);
        /* Every row has the same width (fixed fields), so they line up as columns. */
        font_draw(r, (W - font_width(1.3f, line)) / 2.0f, y, 1.3f, line);
    }
}

/* Game over: the initials to record with the score, arcade style. */
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
        int rank = v->board_state == 1 ? board_rank(&v->board, g->score) : 0;
        int record = !v->has_record || g->score > v->record;
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
    for (i = 0; i < 80; i++)
        SDL_RenderDrawPointF(r, g->stars[i].x, g->stars[i].y);

    SDL_SetRenderDrawColor(r, 220, 220, 230, 255);
    for (i = 0; i < MAX_ROCKS; i++)
        if (g->rocks[i].size)
            draw_rock(r, &g->rocks[i]);

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
        break;
    case S_PLAY:
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (i = 0; i < MAX_SHOTS; i++)
            if (g->shots[i].life > 0)
                SDL_RenderDrawPointF(r, g->shots[i].p.x, g->shots[i].p.y),
                SDL_RenderDrawPointF(r, g->shots[i].p.x + 1, g->shots[i].p.y);
        draw_ship(r, g, keys[SDL_SCANCODE_UP]);
        snprintf(line, sizeof line, "%06u", g->score);
        font_draw(r, 16, 14, 2.0f, line);
        for (i = 0; i < 3; i++)  /* right to left: lost lives go hollow from the left */
            draw_life(r, (float)(W - 24 - i * 26), 30, i < g->lives);
        break;
    case S_OVER:
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_draw_centered(r, W / 2.0f, 160, 4.0f, "GAME OVER");
        snprintf(line, sizeof line, "SCORE %u", g->score);
        font_draw_centered(r, W / 2.0f, 240, 2.5f, line);
        SDL_SetRenderDrawColor(r, 120, 200, 255, 255);
        if (!offline) {
            draw_record(r, v, 300);
            if (!g->submitted && v->state == CHAIN_READY)
                draw_initials(r, g, v);
        }
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_draw_centered(r, W / 2.0f, 590, 1.5f, "SPACE: PLAY AGAIN   ESC: QUIT");
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
            "usage: %s [--rpc URL] [--bulletin URL|off] [--player SEED] [--operator SEED] [--collection N]\n"
            "          [--fund AMOUNT] [--session FILE] [--metadata-hash on|off|SYM:DEC]\n"
            "          [--relay URL --checkpoint FILE [--para-id N]] [--offline]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv)
{
    chain_config cfg;
    const char *player = DEV_PLAYER, *op = DEV_OPERATOR;
    static game g;
    chain_view offline_view;
    chain *ch = NULL;
    const char *screenshot = NULL;  /* saves the title screen as BMP, then quits */
    int offline = 0, running = 1, i;
    SDL_Window *win;
    SDL_Renderer *ren;
    Uint64 last;
    char err[256];

    memset(&cfg, 0, sizeof cfg);
    cfg.rpc = "http://127.0.0.1:9944";
    cfg.bulletin_rpc = "http://127.0.0.1:9945";
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
        else if (strcmp(a, "--operator") == 0) op = val;
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
    if (parse_seed(player, cfg.player_seed) || parse_seed(op, cfg.operator_seed)) {
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
    reset_ship(&g);
    start_level(&g);  /* drifting rocks behind the title */

    last = SDL_GetPerformanceCounter();
    while (running) {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        const chain_view *v = ch ? chain_view_get(ch) : &offline_view;
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)SDL_GetPerformanceFrequency();
        SDL_Event e;

        last = now;
        if (dt > 0.05f)
            dt = 0.05f;
        if (ch)
            chain_poll(ch);  /* chain callbacks run here, on the game thread */

        while (SDL_PollEvent(&e)) {
            /* Closing the window is like escape: close the session first. */
            int quit = e.type == SDL_QUIT ||
                       (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE);
            if (e.type == SDL_KEYDOWN && e.key.repeat) {
                continue;
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
                } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && can_submit) {
                    g.submitted = 1;
                    chain_submit(ch, g.score, g.name);
                } else if (can_submit) {
                    edit_initials(&g, k);
                }
            }
        }

        if (g.state == S_PLAY) {
            update(&g, keys, dt, ch);
        } else {
            int k;  /* rocks keep drifting behind the menus */
            for (k = 0; k < MAX_ROCKS; k++) {
                rock *r = &g.rocks[k];
                if (!r->size)
                    continue;
                r->p.x += r->v.x * dt;
                r->p.y += r->v.y * dt;
                r->angle += r->spin * dt;
                wrap(&r->p);
            }
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
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
