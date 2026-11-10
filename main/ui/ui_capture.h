#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * On-device UI screenshot (for debugging).
 *
 * The C3 internal heap cannot hold a full 240x320 RGB565 frame (153600 bytes), so this **renders in horizontal bands**:
 * it manipulates the LVGL layer directly (same mechanism as lv_snapshot), restricting the layer buf_area /
 * _clip_area to a 240 x N band so the software renderer draws only that band, then immediately dumps the
 * pixels as hex text from the console, then renders the next band.
 * Peak memory is only 240 * N * 2 bytes (15KB at N=32), not 150KB.
 *
 * Hex (not raw binary) is used because during a screenshot other tasks (Wi-Fi / battery etc.) still log,
 * and a raw binary stream would be interrupted and unrecoverable; each line carries an `H:<row>:` prefix, the host only accepts this prefix,
 * so interleaved log lines are naturally dropped and missing rows can be detected.
 *
 * The caller must **already hold** bsp_lvgl_lock (lv_obj_redraw needs to run in the LVGL context).
 */
void ui_capture_dump_hex(void);

#ifdef __cplusplus
}
#endif
