// Tetris (tc_core): pure logic layer, never touches LVGL.
// Memory: 200 bytes of static board + a constant shape table in flash; zero heap allocation during play.
// The C3 has no FPU; all integer arithmetic.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TC_COLS 10
#define TC_ROWS 20
#define TC_PIECE_KINDS 7

typedef enum {
    TC_READY = 0,
    TC_PLAYING,
    TC_DEAD,
} tc_phase_t;

// Reset to READY (clear board / score).
void tc_reset(void);

// READY -> PLAYING. seed drives the 7-bag shuffle (fed from esp_timer).
void tc_start(uint32_t seed);

// Periodic operations. After any change, redraw using tc_cur_cells/tc_board.
void tc_move(int dx);   /* -1 move left, +1 move right */
void tc_rotate(void);   /* rotate clockwise, with wall kick */
void tc_hard_drop(void);/* drop to the bottom and lock */
void tc_gravity(void);  /* one gravity tick: fall one row; on landing lock -> clear lines -> spawn next piece */

tc_phase_t tc_phase(void);
int tc_score(void);
int tc_lines(void);
int tc_level(void);

// Board snapshot: ROWS*COLS bytes, 0=empty, 1..7=piece color index. The falling piece is not included.
const uint8_t *tc_board(void);

// Absolute coordinates of the current piece's 4 cells (cx 0..9, cy may be -1..-2 meaning not yet on the board); returns false if no piece.
bool tc_cur_cells(int8_t out[4][2]);

// Color index of the current piece (1..7, same palette as tc_board values); returns 0 if no piece.
int tc_cur_id(void);
int tc_next_id(void);  // 1..7, same palette index as tc_cur_id

// Relative shape of the next piece's 4 cells (range 0..3), for the preview.
void tc_next_cells(int8_t out[4][2]);

#ifdef __cplusplus
}
#endif
