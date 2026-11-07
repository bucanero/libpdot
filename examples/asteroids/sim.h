/*
 * The game itself, without SDL: a deterministic simulation stepped at a fixed
 * 60 ticks a second from one input byte per tick. Given the same seed and the
 * same inputs, every build produces the same game, bit for bit, so the
 * operator can replay a game to check its score (replay.h).
 *
 * What keeps it deterministic:
 *   - a fixed time step, never the frame time;
 *   - its own integer RNG (xorshift32), seeded per game;
 *   - only IEEE-exact float operations (+ - * /, comparisons, conversions):
 *     sim_sin/sim_cos are polynomials instead of libm's sinf/cosf, which
 *     differ between C libraries;
 *   - no fused multiply-add: sim.c is built with -ffp-contract=off, and
 *     single precision evaluated as single (FLT_EVAL_METHOD 0, so no x87).
 * Rendering-only randomness (stars, flicker) stays outside.
 */
#ifndef ASTEROIDS_SIM_H
#define ASTEROIDS_SIM_H

#include <stdint.h>

#define SIM_W       960
#define SIM_H       720
#define SIM_HZ      60
#define SIM_ROCKS   64
#define SIM_SHOTS   8
#define SIM_VERTS   11
#define SIM_PI      3.14159265f

/* One tick's input. */
#define SIM_LEFT    1
#define SIM_RIGHT   2
#define SIM_THRUST  4
#define SIM_FIRE    8
#define SIM_INPUTS  15  /* every valid bit */

typedef struct { float x, y; } sim_vec;

typedef struct {
    sim_vec p, v;
    float   angle, spin;
    int     size;  /* 3 big, 2, 1 small; 0 free slot */
    float   shape[SIM_VERTS];
} sim_rock;

typedef struct {
    sim_vec p, v;
    float   life;
} sim_shot;

typedef struct {
    uint32_t rng;
    sim_vec  p, v;
    float    angle;
    float    invulnerable;  /* seconds */
    float    fire_cooldown;
    int      lives, level;
    int      over;          /* no lives left: the game has ended */
    uint32_t score;
    uint32_t ticks;         /* steps taken */
    sim_rock rocks[SIM_ROCKS];
    sim_shot shots[SIM_SHOTS];
} sim;

/* A new game from seed: 3 lives, level 0, the first rocks. */
void sim_new(sim *s, uint32_t seed);
/* One tick with these inputs (SIM_* bits). Does nothing once over. */
void sim_step(sim *s, uint8_t input);
/* One tick of rocks drifting, for the menus: no ship, no score. */
void sim_drift(sim *s);

float sim_sin(float x);
float sim_cos(float x);
float sim_rock_radius(int size);

#endif
