// main/boot_chime.c - retro boot sound: square-wave arpeggio E5-G5-B5-E6 (about 0.55s).
// Synthesis parameters are tuned by ear: each note decays linearly by 60% + 15ms silent gap, classic 8-bit boot feel.
// Playback runs in a dedicated short-lived task; on finish bsp_audio_deinit() returns all audio memory.
#include "boot_chime.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "boot_chime";
static atomic_bool s_busy;

#define CHIME_HZ     16000          // same format as the voice/game apps default
#define CHIME_AMP    7000           // ~21% full scale: clear but not harsh at 80% master volume
#define NOTE_BUF_N   1600           // 100ms @16kHz (does not fit on the task stack, heap-allocated temporarily)

// 2026-09-13: this used to be a permanent 3.2KB .bss. The boot chime plays only 0.55s yet permanently held internal DRAM -
// and this contiguous block in the C3 heap decides whether the Xiaozhi Opus encoder (~25KB contiguous block) can be opened at all.
// Changed to malloc inside the chime task and free after use.

// Square wave + linear decay envelope (decays to 40%): sounds more like an "electronic start" than a constant-amplitude square wave, which sounds like an alarm
static void synth_square(int16_t *note, int samples, int hz) {
    const float step = (float)hz / CHIME_HZ;
    float phase = 0.0f;
    for (int i = 0; i < samples; ++i) {
        phase += step;
        if (phase >= 1.0f) phase -= 1.0f;
        const float env = 1.0f - 0.6f * (float)i / samples;
        note[i] = (int16_t)((phase < 0.5f ? CHIME_AMP : -CHIME_AMP) * env);
    }
}

static void play_note(int16_t *note, int hz, int ms) {
    const int total = CHIME_HZ * ms / 1000;
    int done = 0;
    while (done < total) {
        const int n = (total - done > NOTE_BUF_N) ? NOTE_BUF_N : total - done;
        synth_square(note, n, hz);
        if (bsp_audio_write(note, n * sizeof(int16_t)) != ESP_OK) return;
        done += n;
    }
    // 15ms silent gap so each note is distinct
    memset(note, 0, CHIME_HZ * 15 / 1000 * sizeof(int16_t));
    bsp_audio_write(note, CHIME_HZ * 15 / 1000 * sizeof(int16_t));
}

static void chime_task(void *arg) {
    (void)arg;
    int16_t *note = malloc(NOTE_BUF_N * sizeof(int16_t));
    if (note == NULL) {
        ESP_LOGW(TAG, "note buf alloc failed, skip chime");
        atomic_store(&s_busy, false);
        vTaskDelete(NULL);
        return;
    }

    if (bsp_audio_init_playback() != ESP_OK ||
        bsp_audio_set_format(CHIME_HZ, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "audio unavailable, skip chime");
        bsp_audio_deinit();
        free(note);
        atomic_store(&s_busy, false);
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume(70);

    // Classic rising arpeggio, last note held longer to close
    static const struct { int hz, ms; } seq[] = {
        {659, 85}, {784, 85}, {988, 85}, {1319, 260},
    };
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i) {
        play_note(note, seq[i].hz, seq[i].ms);
    }

    // Tail ring-out: let DMA flush the last block before teardown, avoiding a click
    vTaskDelay(pdMS_TO_TICKS(80));
    bsp_audio_deinit();
    free(note);
    ESP_LOGI(TAG, "boot chime done");
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}

void boot_chime_start(void) {
    if (atomic_exchange(&s_busy, true)) return;
    if (xTaskCreate(chime_task, "boot_chime", 4096, NULL, 3, NULL) != pdPASS) {
        atomic_store(&s_busy, false);
        ESP_LOGW(TAG, "chime task create failed");
    }
}

bool boot_chime_busy(void) { return atomic_load(&s_busy); }
