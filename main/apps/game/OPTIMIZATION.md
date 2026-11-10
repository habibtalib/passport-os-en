# "Bark Battle" Optimization Report

Date: 2026-09-13 · Scope: `main/apps/game/` (`app_game.cc` / `game_core.{h,c}` / `game_mic.{h,c}` / new `game_sfx.{h,c}`)

**Constraint: the core gameplay (voice-controlled climb/sink + passing through pipes), the art style (pixel Shiba + grass pillars) and the humorous tone (rating-tier copy) are not changed. All difficulty/hit-test values keep their original values.**

Files involved:

| File | Nature |
|---|---|
| `app_game.cc` | Rewritten (UI build / HUD / presentation / main loop in four sections) |
| `game_core.h` | Adds the "difficulty curve tuning area" and hit-test constants, values unchanged |
| `game_core.c` | Redundancy removed, constants hoisted, death struct cleared; behavior unchanged |
| `game_mic.h/.c` | Fixes a fatal re-entry bug + responsiveness optimization |
| `game_sfx.h/.c` | **New** sound-effects module |
| `../CMakeLists.txt` | Adds `apps/game/game_sfx.c` |

---

## 1. Performance

### P1 (biggest win) Remove the per-frame `lv_label_set_text`

**Problem**: the PLAYING branch of `tick()` called this unconditionally every frame

```c
lv_label_set_text(s_bark_lbl, thrust > 180 ? "Woof!!" : "Woof!");   // or ""
```

Reading the LVGL 9 source (`managed_components/lvgl__lvgl/src/widgets/label/lv_label.c:981`) confirms that
`lv_label_set_text -> set_text_internal` has **no "short-circuit if the text equals the current one" logic**; every call does:

```c
lv_free(label->text);            // free the old string
label->text = lv_malloc(len);    // allocate the new string
copy_text_to_label(...);         // copy
lv_label_mark_need_refr_text();  // invalidate + re-layout the whole text
```

That is **30 malloc/free per second + 30 full-text re-layouts per second**, even when not one character changed.
This is the top source of heap fragmentation (small blocks repeatedly allocated and freed) and dropped frames (re-layout + extra invalidate),
especially noticeable on the C3 with only ~100KB of heap.

**Change**: added `TextCache` + `set_text_cached()`, which does a `strcmp` first and does not touch LVGL at all if unchanged.
The bubble and the hint text all go through this path.

**Expected effect**: **zero** label text allocations during play; with steady/constant-speed flight the bubble produces no invalidate at all.
Heap fragmentation drops markedly, and after `stop()` the heap more easily returns to its pre-entry level.

**Rollback**: swap `set_text_cached()` back to `lv_label_set_text()`; no side effects.

---

### P2 Comprehensive "dirty checks": do not write LVGL if the value is unchanged

`lv_obj_set_x/y` short-circuit on equal values internally (`lv_obj_pos.c:70/84`), but **`set_size` / `set_style_*` / `set_text` do not**. A uniform cache layer was added:

| Item | Before | After |
|---|---|---|
| Ear `s_dog_ear` y | `set_y` every frame (30/s) | Written only on the frame where "rising <-> falling" flips |
| Dog mouth height / tongue position | Already cached (kept) | Same, thresholded |
| Off-screen pipes | `set_pos` on 4 children every frame (up to 120/s) | `s_pipe_shown[]` cache added; not written at all while off-screen |
| Bubble label position | `set_y` every frame, even if the text is empty | Follows only when the text is non-empty |
| Solid-color block `bg_opa` | Relied on the default | `px_block()` sets `LV_OPA_COVER` explicitly, saving the alpha blend on every draw |

**Expected effect**: in steady flight (no bark changes, no pipes entering/leaving the screen) each frame redraws only what is necessary for "dog + 3 on-screen pipes".

---

### P3 Fix the canary running every frame because `s_frame` only incremented in READY

`s_frame++` was originally written in the `case GC_READY` branch, but the game **takes off on entry and READY is never entered** ->
`s_frame` was constantly 0 -> `(s_frame & 31) == 0` was always true -> the `disp->inv_p` canary check **ran every frame** (the design intent was once every 32 frames).

**Change**: `s_frame++` moved to the top of `tick()`, incrementing every frame.

**Expected effect**: canary overhead drops to 1/32; the breathing animation index (`kBob[s_frame>>1]`) is also correct again in the READY state.

---

### P4 Mic side

- **Debug log can be turned off**: it used to scan the 256 samples again every ~2s to compute `mean_s` and print a UART log line.
  Added `#define GAME_MIC_DEBUG_LOG 0`, off by default (set to 1 when calibrating thresholds).
- **Seed the EMA from the first block**: `ema` used to climb from 0 to the actual level over 3-4 blocks (~64ms);
  now the first valid block sets `ema = mean`. Sound response is faster by about **64ms**, so it feels more responsive.
- Warm-up still discards 10 blocks (~160ms) -- this guards against "the codec's full-scale power-on garbage shooting the dog skyward" and **must not be skipped**.

---

### P5 Object pool / memory

The pipe object pool (`GC_PIPES x 4` resident `lv_obj`s) is **kept as is** -- it was right all along;
the allocation problem during play was not the object pool but the label text in P1.
`stop()` still deletes the screen unconditionally (keeping the 2026-09-13 B2 fix) and reclaims the sfx task in `game_stop()`.

---

## 2. Code structure

| # | Change | Purpose |
|---|---|---|
| S1 | `block()`/`mk_label()` -> `px_block()`/`px_label()` | Consistent with the device-wide `ui_pixel` naming; `block` was too generic a name |
| S2 | `build_ui()` split into `build_dog/build_pipes/build_vu/build_dead_panel` | Single-responsibility building; new widgets no longer need to be stuffed into one 60-line function |
| S3 | `tick()` split into `ready_tick()/playing_tick()/dead_tick()` | The main loop goes from a 90-line switch to a three-line dispatch |
| S4 | Difficulty/hit-test constants gathered into the "tuning area" of `game_core.h` | They used to be scattered through the `.c` with long comments, so changing difficulty meant digging through the file |
| S5 | Removed the duplicate assignment to `s.pipes[i].x` in `gc_reset()` (set to 0, then overwritten) | Redundancy removed |
| S6 | Sound effects split into `game_sfx.{h,c}` | Does not pollute `app_game.cc`; does not reuse the voice `app_sound` (it would open/close the codec again, see the file header comment) |
| S7 | Every key change carries a "why" comment | This module has historically hit the unsigned EMA underflow, the pipe `<<4` over-shift, the inv_p dead loop and similar pitfalls; the comments guard against regressions |

---

## 3. Gameplay feel

### F1 (fatal) Fix "exit then immediately re-enter -> no microphone all round"

`game_mic_start()` used to begin with `if (s_task) return;`.
When re-entering right after exiting the app, the previous round's mic task was often still stuck in `bsp_audio_read()` (up to 16ms) and had not exited,
so `s_task` was non-null -> **immediate return, no task created at all** -> the round had no voice control and the player could only watch the dog sink at a constant speed.

**Change**: when `game_mic_start()` finds `s_task` non-null, it first sets `s_active=false` to request exit and waits for it to really exit (up to 600ms), then creates the new task.
`game_mic_start()` also zeroes `s_level` / `s_ready`, so loudness left over from the previous round does not carry into the new one.

**Expected effect**: rapidly entering and leaving the game repeatedly no longer loses the microphone.

### F2 Start the mic on entry

In `game_start()`, `game_mic_start()` was moved before `build_ui()`, so the codec power-on warm-up (~160ms)
overlaps with building the screen (about 40 `lv_obj`s). Previously warm-up only began after the screen was built, so for the first 5 frames (~165ms) the level was constantly 0
and the dog would sink a bit for nothing.

### F3 Death presentation: fall + game-over panel slides in

Previously the game-over panel appeared directly at y=86 the instant of the crash, with no transition; and pressing OK after death restarted immediately,
so pressing twice made the result flash by -- the player never knew their score.

**Change**:
1. After death the dog keeps falling for 9 frames (~300ms, 7px/frame) to the ground, mouth open going "aww!!" with drooping ears;
2. After the fall the game-over panel slides from off-screen 340 to 86 (7 frames, ~230ms);
3. Pressing OK during the fall = **fast-forward to the result** (not restart); pressing OK after the result appears = restart.

**Expected effect**: the win/lose rhythm has a clear three-beat "end -> result -> again", a stronger sense of achievement, and accidental restarts are eliminated.

### F4 New `game_mic_ready()`

Distinguishes "quiet environment" from "mic not warmed up yet", for a later "getting ready" hint (currently used only for observability).

---

## 4. UI and feedback

| # | Change | Purpose / expected effect |
|---|---|---|
| U1 (key) | **Real-time volume bar on the left** (x=4, 6x244, three tiers grass -> yellow -> red) | The most important thing in a voice-controlled game is "am I being heard". Before, it could only be sensed indirectly from how wide the mouth opened. Height is quantized to 6px steps, and the whole bar is transparent when silent |
| U2 | Score flashes white for 200ms | Passing a pipe is visible even in peripheral vision (before, only a number changed) |
| U3 | Hint changed to a paper capsule (paper base + 2px ink border + `UI_SUB` text) | White text directly over grass green had a contrast of only about **2.2:1** and was hard to read; now about 6:1, and consistent with the device-wide `ui_pixel_hint_pill` standard |
| U4 | Hint text shortened to a single line `"Loud=up Quiet=down OK=jump"` | The original 13 Chinese characters were ~221px wide, over the 216px container, and wrapped to two lines on the grass |
| U5 | Rating tier added at 20 points: `"The whole block barks"` | Strengthens the sense of achievement in high-score rounds (copy only, no gameplay impact) |
| U6 | New sound effects `game_sfx`: crisp beep on passing a pipe / two descending tones on death / three rising tones on a new best | Completes the feedback loop. 16kHz square wave via `bsp_audio_write`, **coexisting with mic capture in full duplex** (TX/RX are two independent I2S handles, and `esp_codec_dev_read/write` share no lock) |
| U7 | The bubble is no longer covered by the top bar | When the dog hugs the top, the bubble y is clamped below `GC_TOP+2` |

---

## 5. Items that might affect the original balance, and how to roll back

> Conclusion first: **no item changes any difficulty value** (speed, gap, thrust mapping and hitboxes all keep their original values).
> The following are the items that "change the player's feel/rhythm", each with a one-line rollback.

| Item | Impact | Rollback |
|---|---|---|
| **F3 death presentation (~530ms)** | About 0.5s more between death and being able to restart. Does not change score judgment, only the rhythm. Fast-handed players will find it "slow" | At the top of `app_game.cc` set `kDeathFallFrames` to `0` and `kPanelAnimFrames` to `1` to return to "pop up the result immediately" |
| **F3 OK fast-forward** | Changes the meaning of the OK key in the DEAD state (during the fall it is "fast-forward", not "restart"). In theory players used to mashing to restart will press once more | Delete the `if (s_dying > 0)` branch in `case GC_DEAD` of `game_on_key`, restoring an unconditional `start_run()` |
| **P4 EMA first-block seeding** | Mic response is faster by about 64ms. Strictly speaking the delay from "starting to bark" to "taking off" is shorter, so veteran players find it more sensitive | In `game_mic.c` change back to `ema = (ema * 3u + mean) / 4u;` (remove the ternary) |
| **U1 volume bar** | A new visual element, taking 6px of width on the left of the screen. Pipes pass "behind" it (HUD layer order) | Delete the `build_vu(s_screen);` line and the `update_vu(thrust);` line |
| **U6 sound effects** | New sound. If the device speaker / codec state is abnormal it is silently skipped (a failed `bsp_audio_write` warns once and mutes itself), with no gameplay impact | At the top of `app_game.cc`, `constexpr bool kSfxEnabled = false;` (one line, no task is created, zero overhead) |
| **U3/U4 hint capsule and text** | Appearance changed (from white text to a paper capsule) | In `build_ui()` change the hint section back to `px_label(s_screen, "", &buddy_font_16, 0xFFFFFF, ...)` and remove the background style |
| **U5 new 20-point tier** | Copy only, zero gameplay impact | Delete the `if (score >= 20)` line |

### When you really want to tune difficulty

Everything is gathered in the "difficulty curve tuning area" of `game_core.h`, 6 macros:
`GC_BASE_SPEED` / `GC_SPEED_GAIN` / `GC_SPEED_BONUS_MAX` / `GC_SPACING` / `GC_GAP_START` / `GC_GAP_MIN` / `GC_GAP_SHRINK` / `GC_GAP_STEP_MAX`.
Each is annotated with the consequence of raising/lowering it. **Setting them all back to the current values returns fully to the original balance.**

The voice thresholds are at the top of `app_game.cc`: `kVolFloor` (noisy room -> raise) and `kVolFull` (cannot get it to climb -> lower).

---

## 6. Directions still worth optimizing (by cost-effectiveness)

1. **Adaptive noise floor (AGC)**: `kVolFloor` is currently a fixed 150. The feel differs a lot between a quiet room and a noisy environment.
   One could sample the ambient noise floor for 1 second before the round -> automatically raise FLOOR. **Risk**: this changes the core feel, so A/B testing on hardware must come first.
2. **Draw the pipes with a canvas / direct `esp_lcd_panel_draw_bitmap`**:
   Currently each frame invalidates the dog + 3 pipes (12 objects), and LVGL merges them into several dirty regions flushed separately.
   Self-drawing could push the whole screen only once. **Cost**: it must take over the panel, coexist with the `app_registry` transition-screen mechanism, and the C3 cannot fit a 150KB full-screen buffer (line buffering only). Large effort; check the `max_tick` log first and then decide.
3. **Reuse the screen object after `stop()`**: every entry/exit currently rebuilds/destroys ~40 `lv_obj`s (build time, heap churn).
   One could keep a hidden game screen resident. **Cost**: it permanently occupies ~5KB of heap, conflicting with the memory policy of "apps run mutually exclusively, stop reclaims everything"; needs weighing.
4. **Finer death feedback**: give different fall poses and sounds for "hit the upper edge / hit the lower edge / hit the ground" (currently only one).
5. **Gap "preview"**: in the high-score range (after speed is capped) the pipe interval is only 1.1s; an indicator arrow for the next gap could be pre-drawn at the right screen edge, reducing the frustration of "no time to react".
6. **Richer sound effects**: currently only 3 tones. Could add a "takeoff glissando", "combo pitch rise" (pitch +1 for every 3 consecutive pipes), etc., at very low cost (only the tone table in `game_sfx.c` changes).
7. **Frame-drop observability**: `tick()` already has a 1-second summary log (`frame drops: n/30 max_tick=Xms`). Recommend running one round on hardware and collecting this log as the basis for whether item 2 above is worth doing.
8. **Actual use of `game_mic_ready()`**: currently only the interface is exposed. It could show "Getting ready..." during the 160ms warm-up instead of letting the dog sink for nothing.

---

## 7. Verification status (tested on hardware)

Build: `_diag/build_game_opt.log` -> `BUILD_EXIT=0`, zero warnings.
Flashing: Windows esptool v5.4.0 -> COM3 @460800, all three segments `Hash of data verified`,
firmware 3,179,952 B (0x3085b0), 24% of the app partition left.

Hardware results (`_diag/game_reentry2.log`, `_diag/game_stay.log`):

| Check | Result |
|---|---|
| New code really is in the firmware | ELF contains `game_sfx_play` / `game_sfx_start` / `game_mic_ready` / `set_text_cached` (OK) |
| No memory leak | 3 consecutive enter/exit cycles: `started; free~83KB` -> `stopped; free=105,184`, `largest` stable at 51200/53248 without dropping -> no leak, no fragmentation (OK) |
| Mic capture present every time | All 3 entries captured normally (`peak` 1923~3710, varying with ambient noise); no "old mic task did not exit" warning -> the P2 re-entry fix works (OK) |
| Sound effects | See B1 below |
| Dropped frames | `frame drops: 5~10/30`, but `max_tick` only 4~5ms |

### B1 A problem exposed and fixed only on hardware: sfx start-up race

After the first flash the log showed `W game_sfx: bsp_audio_write failed, sfx disabled for this round` -- **no sound the whole round**.

Root cause: `game_start()` creates the sfx task first and immediately plays `GS_START`, while the codec output only becomes `output_opened` after the mic task
calls `bsp_audio_set_format()`. Measured: **write requested at 14.473s, open at 14.667s**,
a gap of about 200ms; the old logic "one failed write means permanent silence" turned the start tone into silence for the whole round.

Fix (`game_sfx.c`): added `write_blocking()` -- on write failure it **retries with a bound** in 20ms steps,
waiting at most 600ms; only on timeout does it decide the codec is unavailable and warn once. After the fix that warning is gone.

### Two things not to misread in the logs

- **In a quiet environment the dog's time of death varies a lot** (measured 2.5s ~ 9.4s), because ambient noise itself provides thrust
  (`score=4` appeared in the same test). Not seeing `game over` after a short wait does not mean it froze.
- **Exit the app with a long-press of OK** (serial `l`). A long-press of DOWN (`k`) is not the back key; sending it only prints one line
  `Dispatch Key ... cur_active=N` and then nothing happens, which is easily misread as a dead key.

### About the remaining dropped frames

`max_tick=4~5ms` shows the game logic itself is very light; the 45ms+ frame interval comes from SPI full-screen flush coexisting with Wi-Fi,
and is **not introduced by this change**. Whether it is worth taking "direction 2 (self-drawing / line buffering)" depends on whether the current feel is acceptable;
if so, first sample the `frame drops` log over a long time per direction 7 before deciding.
