/* The deterministic game: see sim.h for the rules that keep it so. */
#include "sim.h"

#include <float.h>
#include <string.h>

#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#error "sim.c needs single precision evaluated as single (FLT_EVAL_METHOD 0): use SSE, not x87"
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF  /* and -ffp-contract=off for GCC, which ignores this */
#endif

#define DT (1.0f / SIM_HZ)
#define TWO_PI (2.0f * SIM_PI)

/* ---- exact-by-construction helpers ------------------------------------- */

static uint32_t next(sim *s)
{
    uint32_t x = s->rng;  /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s->rng = x;
}

/* Uniform in [lo, hi): 24 random bits, which a float holds exactly. */
static float frand(sim *s, float lo, float hi)
{
    return lo + (hi - lo) * ((float)(next(s) >> 8) * (1.0f / 16777216.0f));
}

static float absf(float x)
{
    return x < 0 ? -x : x;
}

float sim_sin(float x)
{
    float k, x2;

    /* to [-pi, pi], then to [-pi/2, pi/2] */
    k = (float)(int)(x * (1.0f / TWO_PI) + (x >= 0 ? 0.5f : -0.5f));
    x = x - k * TWO_PI;
    if (x > SIM_PI / 2)
        x = SIM_PI - x;
    else if (x < -SIM_PI / 2)
        x = -SIM_PI - x;
    /* Taylor to x^9: under 4e-6 off at pi/2, plenty to draw and steer with */
    x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f *
           (1.0f - x2 / 72.0f))));
}

float sim_cos(float x)
{
    return sim_sin(x + SIM_PI / 2);
}

float sim_rock_radius(int size)
{
    return size == 3 ? 44.0f : size == 2 ? 24.0f : 12.0f;
}

static void wrap(sim_vec *p)
{
    if (p->x < 0) p->x += SIM_W;
    if (p->x >= SIM_W) p->x -= SIM_W;
    if (p->y < 0) p->y += SIM_H;
    if (p->y >= SIM_H) p->y -= SIM_H;
}

/* ---- the game ------------------------------------------------------------ */

static void spawn_rock(sim *s, sim_vec p, int size)
{
    float speed = frand(s, 30.0f, 70.0f) * (float)(4 - size) * (1.0f + 0.1f * (float)s->level);
    float dir = frand(s, 0, TWO_PI);
    sim_rock *r;
    int i, k;

    for (i = 0; i < SIM_ROCKS && s->rocks[i].size; i++)
        ;
    if (i == SIM_ROCKS)
        return;
    r = &s->rocks[i];
    r->p = p;
    r->v.x = sim_cos(dir) * speed;
    r->v.y = sim_sin(dir) * speed;
    r->angle = 0;
    r->spin = frand(s, -1.5f, 1.5f);
    r->size = size;
    for (k = 0; k < SIM_VERTS; k++)
        r->shape[k] = frand(s, 0.7f, 1.15f);
}

static void start_level(sim *s)
{
    int i;
    for (i = 0; i < 3 + s->level; i++) {
        sim_vec p;
        do {  /* not on top of the ship */
            p.x = frand(s, 0, SIM_W);
            p.y = frand(s, 0, SIM_H);
        } while (absf(p.x - s->p.x) < 150 && absf(p.y - s->p.y) < 150);
        spawn_rock(s, p, 3);
    }
}

static void reset_ship(sim *s)
{
    s->p.x = SIM_W / 2.0f;
    s->p.y = SIM_H / 2.0f;
    s->v.x = s->v.y = 0;
    s->angle = -SIM_PI / 2;
    s->invulnerable = 2.5f;
}

void sim_new(sim *s, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->rng = seed ? seed : 0x9e3779b9u;  /* xorshift never leaves 0 */
    s->lives = 3;
    reset_ship(s);
    start_level(s);
}

static int rocks_left(const sim *s)
{
    int i, n = 0;
    for (i = 0; i < SIM_ROCKS; i++)
        n += s->rocks[i].size != 0;
    return n;
}

static void move_rock(sim_rock *r)
{
    r->p.x += r->v.x * DT;
    r->p.y += r->v.y * DT;
    r->angle += r->spin * DT;
    if (r->angle > SIM_PI)
        r->angle -= TWO_PI;
    else if (r->angle < -SIM_PI)
        r->angle += TWO_PI;
    wrap(&r->p);
}

void sim_drift(sim *s)
{
    int i;
    for (i = 0; i < SIM_ROCKS; i++)
        if (s->rocks[i].size)
            move_rock(&s->rocks[i]);
}

void sim_step(sim *s, uint8_t input)
{
    float c, sn;
    int i, j;

    if (s->over)
        return;
    s->ticks++;
    if (input & SIM_LEFT)
        s->angle -= 4.0f * DT;
    if (input & SIM_RIGHT)
        s->angle += 4.0f * DT;
    if (s->angle > SIM_PI)
        s->angle -= TWO_PI;
    else if (s->angle < -SIM_PI)
        s->angle += TWO_PI;
    c = sim_cos(s->angle);
    sn = sim_sin(s->angle);
    if (input & SIM_THRUST) {
        s->v.x += c * 300.0f * DT;
        s->v.y += sn * 300.0f * DT;
    }
    s->v.x *= 1.0f - 0.4f * DT;
    s->v.y *= 1.0f - 0.4f * DT;
    s->p.x += s->v.x * DT;
    s->p.y += s->v.y * DT;
    wrap(&s->p);
    if (s->invulnerable > 0)
        s->invulnerable -= DT;

    s->fire_cooldown -= DT;
    if ((input & SIM_FIRE) && s->fire_cooldown <= 0) {
        for (i = 0; i < SIM_SHOTS && s->shots[i].life > 0; i++)
            ;
        if (i < SIM_SHOTS) {
            s->shots[i].p.x = s->p.x + c * 14;
            s->shots[i].p.y = s->p.y + sn * 14;
            s->shots[i].v.x = s->v.x + c * 520.0f;
            s->shots[i].v.y = s->v.y + sn * 520.0f;
            s->shots[i].life = 1.0f;
            s->fire_cooldown = 0.18f;
        }
    }
    for (i = 0; i < SIM_SHOTS; i++) {
        sim_shot *sh = &s->shots[i];
        if (sh->life <= 0)
            continue;
        sh->life -= DT;
        sh->p.x += sh->v.x * DT;
        sh->p.y += sh->v.y * DT;
        wrap(&sh->p);
    }

    for (i = 0; i < SIM_ROCKS; i++) {
        sim_rock *r = &s->rocks[i];
        float rad;
        if (!r->size)
            continue;
        move_rock(r);
        rad = sim_rock_radius(r->size);
        for (j = 0; j < SIM_SHOTS; j++) {
            sim_shot *sh = &s->shots[j];
            float dx = sh->p.x - r->p.x, dy = sh->p.y - r->p.y;
            if (sh->life > 0 && dx * dx + dy * dy < rad * rad) {
                int size = r->size;
                sim_vec p = r->p;
                sh->life = 0;
                r->size = 0;
                s->score += size == 3 ? 20 : size == 2 ? 50 : 100;
                if (size > 1) {
                    spawn_rock(s, p, size - 1);
                    spawn_rock(s, p, size - 1);
                }
                break;
            }
        }
        if (r->size && s->invulnerable <= 0) {
            float dx = s->p.x - r->p.x, dy = s->p.y - r->p.y;
            if (dx * dx + dy * dy < (rad + 10) * (rad + 10)) {
                if (--s->lives <= 0) {
                    s->over = 1;
                    return;
                }
                reset_ship(s);
            }
        }
    }
    if (!rocks_left(s)) {
        s->level++;
        start_level(s);
    }
}
