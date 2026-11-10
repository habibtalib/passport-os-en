#include "apps/game/game_core.h"

#include <stddef.h>

// Physics constants (Q4/frame, 30fps). Conversion: 16 Q4 = 1px/frame = 30px/s.
// Vertical motion uses a "target velocity" model: the sound level 0..255 maps linearly to slow sink <-> climb; vy eases
// toward the target and is clamped by it -- the first version used gravity/thrust acceleration, so talking shot the dog
// skyward and silence dropped it instantly, too hard to pass the first level (user-tested).
//
// 2026-09-13 optimization: constants moved to the "difficulty curve tuning area" in game_core.h; this file keeps only
// the difficulty-independent pure physics (motion feel + ceiling handling). Values are identical to before.
#define SINK4 37       /* target velocity when silent: constant slow sink ~69px/s, half a screen in ~2s, and guaranteed to
                                                    reach the next gap 56px lower at the fastest pipe speed */
#define CLIMB4 (-58)   /* target velocity at full volume: constant climb ~109px/s, no faster however loud */
#define FLAP4 (-50)    /* OK flap: one light tap ~ -94px/s */

/* Divisor for vy converging to the target velocity: 4 = converges 1/4 per frame, ~130ms time constant, responsive but no jumps.
      Lower = more responsive but more "twitchy"; higher = mushier. */
#define VY_EASE_DIV 4

/* Soft ceiling: within CEIL_HARD of the top force the head down; CEIL_HARD..CEIL_SOFT linearly ease off the throttle.
      The hover band stays below the shallowest gap (gap_y>=50), otherwise the character rubs the ceiling and
      clips the edge of a shallow-gap pipe -- "looks untouched yet dead". */
#define CEIL_HARD 25
#define CEIL_SOFT 50

/* Top/bottom margin of the random gap band (px): keeps gaps from spawning against the top bar or the grass. */
#define BAND_MARGIN 24

static struct {
    gc_phase_t phase;
    int32_t y;       /* top edge of the character, Q4 */
    int32_t vy;      /* Q4/frame */
    int32_t thrust;  /* 0..255 sound level */
    bool flap_pending;
    int32_t last_gap_y; /* top of the previous gap (px): baseline for the reachability constraint */
    gc_death_info_t death; /* death-cause snapshot, for upper-layer logs */
    gc_pipe_t pipes[GC_PIPES];
    int score;
    uint32_t rng;
} s;

/* Current gap height: narrows with score, floored at GC_GAP_MIN. */
static int32_t gap_h_now(void) {
    int32_t gap = GC_GAP_START - s.score * GC_GAP_SHRINK;
    return gap < GC_GAP_MIN ? GC_GAP_MIN : gap;
}

/* Current pipe speed: speeds up slightly with score, capped at GC_SPEED_BONUS_MAX points. */
static int32_t speed_now(void) {
    const int32_t bonus =
        s.score > GC_SPEED_BONUS_MAX ? GC_SPEED_BONUS_MAX : s.score;
    return GC_BASE_SPEED + bonus * GC_SPEED_GAIN;
}

static uint32_t next_rand(void) {
    /* LCG: deterministic, no libc dependency */
    s.rng = s.rng * 1664525u + 1013904223u;
    return s.rng >> 8;
}

static void recycle_pipe(gc_pipe_t *p, int32_t x_q4) {
    const int32_t gap = gap_h_now();
    const int32_t band_top = GC_TOP + BAND_MARGIN;
    const int32_t span = (GC_GROUND_Y - GC_TOP) - gap - (BAND_MARGIN * 2);
    int32_t cy = band_top + (int32_t)(next_rand() % (uint32_t)span);
    /* Reachability constraint: the gap top moves at most GC_GAP_STEP_MAX from the previous one, clamped back into the legal range.
          Otherwise a random jump beyond the dog's climb/sink ability means a certain crash, "lost without touching anything". */
    int32_t lo = s.last_gap_y - GC_GAP_STEP_MAX;
    int32_t hi = s.last_gap_y + GC_GAP_STEP_MAX;
    if (lo < band_top) lo = band_top;
    if (hi > band_top + span) hi = band_top + span;
    if (cy < lo) cy = lo;
    if (cy > hi) cy = hi;
    s.last_gap_y = cy;
    p->x = x_q4;
    p->gap_h = gap;
    p->gap_y = cy << 4;
    p->scored = false;
}

void gc_reset(void) {
    s.phase = GC_READY;
    s.y = ((GC_TOP + GC_GROUND_Y) / 2 - GC_BIRD_H / 2) << 4;
    s.vy = 0;
    s.thrust = 0;
    s.flap_pending = false;
    s.last_gap_y = GC_TOP + 60; /* gap-chain baseline: middle of the legal band, so the first pipe starts nearby */
    /* Clear the whole death snapshot -- it used to clear only reason, so after a restart bird_y/pipe_x still held
          the previous round's coordinates, and dying on the first frame of a new round would log misleading data. */
    s.death.reason = 0;
    s.death.bird_y = 0;
    s.death.pipe_x = 0;
    s.death.gap_top = 0;
    s.death.gap_h = 0;
    s.score = 0;
    for (int i = 0; i < GC_PIPES; ++i) {
        gc_pipe_t *p = &s.pipes[i];
        p->x = (240 + 60 + i * GC_SPACING) << 4;
        p->gap_h = GC_GAP_START;
        p->gap_y = (GC_TOP + 40) << 4;
        p->scored = true; /* READY: neither drawn nor judged */
    }
}

void gc_start(uint32_t seed) {
    gc_reset();
    s.rng = seed ? seed : 0x1234u;
    for (int i = 0; i < GC_PIPES; ++i) {
        recycle_pipe(&s.pipes[i], (240 + 60 + i * GC_SPACING) << 4);
    }
    s.phase = GC_PLAYING;
}

void gc_set_thrust(uint8_t thrust) { s.thrust = thrust; }

void gc_flap(void) {
    if (s.phase == GC_PLAYING) s.flap_pending = true;
}

static bool hit(int32_t ax, int32_t ay, int32_t aw, int32_t ah,
                int32_t bx, int32_t by, int32_t bw, int32_t bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

void gc_step(void) {
    if (s.phase != GC_PLAYING) return;

    /* Vertical motion: the sound level maps linearly to a slow sink <-> climb target velocity, and vy converges to it by
          1/4 each frame (~130ms time constant, responsive but no jumps). The speed limit is the target endpoints:
          however loud the speech, it is only a constant climb; when quiet, only a constant slow sink. */
    int32_t target;
    if (s.flap_pending) {
        s.flap_pending = false;
        target = FLAP4; /* flap: a one-off light tap, then it still converges to the sound target */
    } else {
        target = SINK4 + (int32_t)s.thrust * (CLIMB4 - SINK4) / 255;
        /* Ceiling handling: within CEIL_HARD of the top force the head down; CEIL_HARD..CEIL_SOFT the climb eases off
              linearly the closer to the top line. */
        int32_t headroom = (s.y >> 4) - GC_TOP;
        if (headroom < 0) headroom = 0;
        if (headroom < CEIL_HARD) {
            target = SINK4 + 15;
        } else if (headroom < CEIL_SOFT && target < SINK4) {
            target = SINK4 + (target - SINK4) * (headroom - CEIL_HARD) /
                                 (CEIL_SOFT - CEIL_HARD);
        }
    }
    s.vy += (target - s.vy) / VY_EASE_DIV;
    s.y += s.vy;

    /* Pipe advance + reuse. speed_now() is already Q4/frame, so do not <<4 again --
          the extra 4 bits once made pipes flash by at 1860px/s, a certain death within the first second. */
    const int32_t speed = speed_now();
    int32_t rightmost = 0;
    for (int i = 0; i < GC_PIPES; ++i) {
        s.pipes[i].x -= speed;
        if (s.pipes[i].x > rightmost) rightmost = s.pipes[i].x;
    }
    for (int i = 0; i < GC_PIPES; ++i) {
        gc_pipe_t *p = &s.pipes[i];
        if (p->x < -(GC_PIPE_W << 4)) {
            recycle_pipe(p, rightmost + (GC_SPACING << 4));
        } else if (!p->scored && (p->x >> 4) + GC_PIPE_W < GC_BIRD_X) {
            p->scored = true;
            ++s.score;
        }
    }

    /* Collision: the character box is inset by GC_HIT_INSET px (visual leniency). Touching the ground kills; hitting the ceiling only slides along it
          without killing -- otherwise "the louder you shout the faster you die", contrary to the intuition that louder = climb harder. */
    const int32_t bx = GC_BIRD_X + GC_HIT_INSET;
    const int32_t by = (s.y >> 4) + GC_HIT_INSET;
    const int32_t bw = GC_BIRD_W - (GC_HIT_INSET * 2);
    const int32_t bh = GC_BIRD_H - (GC_HIT_INSET * 2);
    if (by < GC_TOP) {
        s.y = (int32_t)(GC_TOP - 2) << 4;
        if (s.vy < 0) s.vy = 0;
    }
    if (by + bh > GC_GROUND_Y) {
        s.death.reason = 1; /* ground */
        s.death.bird_y = by;
        s.phase = GC_DEAD;
        return;
    }
    for (int i = 0; i < GC_PIPES; ++i) {
        const gc_pipe_t *p = &s.pipes[i];
        const int32_t px = p->x >> 4;
        const int32_t gap_top = p->gap_y >> 4;
        /* The upper pipe height must use gap_top-GC_TOP: using gap_y directly as the height makes the collision rect
              GC_TOP=26px longer than the visible pipe (an invisible zone), so the character is judged dead without touching a pillar (measured) */
        const bool hit_top =
            hit(bx, by, bw, bh, px, GC_TOP, GC_PIPE_W, gap_top - GC_TOP);
        const bool hit_bot =
            hit(bx, by, bw, bh, px, gap_top + p->gap_h, GC_PIPE_W,
                GC_GROUND_Y - gap_top - p->gap_h);
        if (hit_top || hit_bot) {
            s.death.reason = hit_top ? 2 : 3;
            s.death.bird_y = by;
            s.death.pipe_x = px;
            s.death.gap_top = gap_top;
            s.death.gap_h = p->gap_h;
            s.phase = GC_DEAD;
            return;
        }
    }
}

gc_phase_t gc_phase(void) { return s.phase; }
int32_t gc_bird_y_q4(void) { return s.y; }
int32_t gc_bird_vy_q4(void) { return s.vy; }
const gc_pipe_t *gc_pipe(int index) {
    return (index >= 0 && index < GC_PIPES) ? &s.pipes[index] : NULL;
}
int gc_score(void) { return s.score; }
gc_death_info_t gc_death_info(void) { return s.death; }
