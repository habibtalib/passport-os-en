// Voice-controlled flyer (game_core): pure logic layer, never touches LVGL.
// The C3 has no FPU, so everything uses Q4 fixed point (1 unit = 1/16 pixel); multiply/divide only via shifts/small constants.
// gc_step() is called once per frame (33ms); the frame length is fixed and physics is tuned for 30fps.
//
// 2026-09-13 optimization: no values changed (gameplay balance kept 1:1), only
//   1. moved the feel/difficulty constants scattered in the .c file here, into one "tuning area", so one edit tries a new feel;
//   2. added magic numbers previously only in comments (e.g. hitbox inset) so the UI layer can use the same definition;
//   3. the death snapshot gets a single clear interface, so a restart never keeps the previous round's coordinates.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_BIRD_X 56      /* fixed x of the character (px) */
#define GC_BIRD_W 22
#define GC_BIRD_H 18
#define GC_TOP 26         /* bottom edge of the top bar: upper bound of the flight area */
#define GC_GROUND_Y 284   /* top edge of the grass: lower bound of the flight area */
#define GC_PIPE_W 34
#define GC_PIPE_LIP 10    /* height of the widened pipe lip */
#define GC_PIPES 3        /* pipe pool size: reused cyclically, zero allocation during play */

/* ---- Hitbox ---- */

/* How many px the collision box is inset from each side of the sprite. 3 = visual leniency (nose/ears sticking out 3px do not count).
      Lower it for hardcore (2 = slightly harder, 0 = pixel-exact); raise it for more leniency. **Changing this changes
      the difficulty**; to revert, set it back to 3. */
#define GC_HIT_INSET 3

/* How many px the pipe lip is wider than the pipe body on each side. The lip looks wider but collision uses only the body --
      intentional leniency, so scraping past the lip does not randomly kill the player. */
#define GC_PIPE_LIP_OVERHANG 4

/* ---- Difficulty curve tuning area ----
      The 6 numbers below are the whole source of difficulty; current values are identical to before the optimization.
      Tune difficulty only here; each item notes the effect of raising/lowering it, for easy rollback. */

/* Pipe horizontal speed (Q4/frame). 16 Q4 = 1px/frame = ~30px/s.
      Higher = shorter reaction time (harder); lower = more relaxed. Current 52 = ~98px/s. */
#define GC_BASE_SPEED 52

/* Speed increment per point and the cap score. Currently 2/8 -> +16 Q4 (~112px/s) at the cap.
      Higher = tenser late game; a lower cap score = reaches constant speed earlier. */
#define GC_SPEED_GAIN 2
#define GC_SPEED_BONUS_MAX 8

/* Horizontal pipe spacing (px). Currently 140 = one pipe every ~1.3-1.4s, leaving enough time to adjust.
      Lower = denser (harder), but below 120 it squeezes the reachability constraint. */
#define GC_SPACING 140

/* Gap height: starts at 120, narrows by 2 per point, minimum 96.
      Lower GAP_MIN = more hardcore; the current 96 is already fairly lenient for an 18px-tall dog. */
#define GC_GAP_START 120
#define GC_GAP_MIN 96
#define GC_GAP_SHRINK 2

/* Max vertical shift between adjacent gaps (px). This is the "reachability constraint": beyond what the dog can climb/sink
      is a guaranteed death, and players feel they "lost without touching anything" (added after two deaths in a row at 12 points).
      Higher = more thrilling but unreachable gaps appear; lower = gentler. */
#define GC_GAP_STEP_MAX 56

typedef enum {
    GC_READY = 0,  /* ready to jump: character hovers/breathes, make noise / press OK to start */
    GC_PLAYING,
    GC_DEAD,
} gc_phase_t;

// Death snapshot: reason 1=ground 2=hit above the gap 3=hit below the gap; coordinates in px.
typedef struct {
    int reason;
    int32_t bird_y;
    int32_t pipe_x;
    int32_t gap_top;
    int32_t gap_h;
} gc_death_info_t;

typedef struct {
    int32_t x;      /* Q4 */
    int32_t gap_y;  /* top of the gap, px (Q4) */
    int32_t gap_h;  /* gap height, px */
    bool scored;
} gc_pipe_t;

// Back to READY; clears pipes and score.
void gc_reset(void);

// READY -> PLAYING. seed drives the gap sequence (fed from esp_timer, different every round).
void gc_start(uint32_t seed);

// Advance one frame (only meaningful in PLAYING; READY/DEAD return immediately).
void gc_step(void);

// Sound level 0..255: 0 = constant slow sink, 255 = fastest constant climb, linear in between.
// Speed is clamped by the level endpoints, so no shooting skyward when talking / dropping instantly when quiet.
void gc_set_thrust(uint8_t thrust);

// Flap (OK key): sets this frame's velocity directly to the upward kick, taking effect before the sound thrust.
void gc_flap(void);

gc_phase_t gc_phase(void);
int32_t gc_bird_y_q4(void);   /* top edge of the character, Q4 */
int32_t gc_bird_vy_q4(void);  /* vertical velocity, Q4/frame */
const gc_pipe_t *gc_pipe(int index);
int gc_score(void);
gc_death_info_t gc_death_info(void);  /* valid in DEAD: cause + both sides' coordinates, for log tracing */

#ifdef __cplusplus
}
#endif
