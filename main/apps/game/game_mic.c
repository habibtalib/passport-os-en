#include "apps/game/game_mic.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "game_mic";

#define SAMPLES 256 /* 16ms @16kHz; small reads, low latency */

/* Raw-sample diagnostics (one line every ~2s, for calibrating the voice thresholds). Off normally: it scans
      256 extra samples and prints a UART log -- pure debug overhead. Set to 1 to re-enable. */
#define GAME_MIC_DEBUG_LOG 0

/* Number of blocks to drop for the codec power-on transient (10 x 16ms = 160ms).
      Full-scale garbage once shot the dog straight up; do not skip. */
#define MIC_WARMUP_BLOCKS 10

/* EMA coefficient: ema = ema*(4-1)/4 + mean/4, about a 48ms time constant. */
#define MIC_EMA_SHIFT 2

static atomic_uint s_level;
static atomic_bool s_ready;
static atomic_bool s_active;
static atomic_bool s_running;
static TaskHandle_t s_task;

/* Wait for the old task to exit: at most wait_ticks x 15ms. Returns whether it is gone. */
static bool wait_task_exit(int wait_ticks) {
    for (int i = 0; i < wait_ticks && s_task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return s_task == NULL;
}

static void mic_task(void *arg) {
    (void)arg;
    int16_t pcm[SAMPLES]; /* 512B on the task stack, not heap */

    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        atomic_store(&s_active, false);
        s_task = NULL;
        atomic_store(&s_running, false);
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_format(16000, 16, 1);

    uint32_t ema = 0;
    int warmup = MIC_WARMUP_BLOCKS;
#if GAME_MIC_DEBUG_LOG
    int dbg = 0;
#endif
    while (atomic_load(&s_active)) {
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* Mean rectification: cheaper than RMS (no multiply), good enough for the loudness mapping */
        int32_t sum = 0;
#if GAME_MIC_DEBUG_LOG
        int32_t mn = 32767, mx = -32768;
#endif
        for (int i = 0; i < SAMPLES; ++i) {
            int32_t v = pcm[i];
            sum += v < 0 ? -v : v;
#if GAME_MIC_DEBUG_LOG
            if (v < mn) mn = v;
            if (v > mx) mx = v;
#endif
        }
#if GAME_MIC_DEBUG_LOG
        if (++dbg >= 120) { /* one line every ~2s, for calibrating the voice thresholds */
            dbg = 0;
            int32_t mean_s = 0;
            for (int i = 0; i < SAMPLES; ++i) mean_s += pcm[i];
            ESP_LOGI(TAG, "raw: mean_abs=%ld mean_s=%ld min=%ld max=%ld",
                     (long)(sum / SAMPLES), (long)(mean_s / SAMPLES),
                     (long)mn, (long)mx);
        }
#endif
        const uint32_t mean = (uint32_t)(sum / SAMPLES);
        if (warmup) {
            if (--warmup == 0) atomic_store(&s_ready, true);
            continue;
        }
        /* EMA is all unsigned: ema*3/4 + mean/4. The old (mean-ema)/4 underflowed (unsigned) when mean<ema,
              instantly pinning the level at 4095 and sticking forever -- caught on real hardware.
              The first block seeds the EMA directly (instead of climbing from 0), saving ~64ms of convergence so sound responds faster. */
        ema = (ema == 0) ? mean : (ema * ((1u << MIC_EMA_SHIFT) - 1u) + mean) >> MIC_EMA_SHIFT;
        atomic_store(&s_level, (unsigned)(ema > 4095u ? 4095u : ema));
    }
    /* Game owner closes the codec only after both RX and SFX TX have exited. */
    ESP_LOGI(TAG, "mic task exit");
    s_task = NULL;
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

void game_mic_start(void) {
    /* Key fix: this used to be just `if (s_task) return;`. When re-entering the app right after exit, the old task
          was often still inside read (up to 16ms) and had not exited, so s_task was non-null -> immediate return, and
          the round silently had no microphone and was unplayable. Now: ask it to exit first, then wait for it. */
    if (s_task) {
        atomic_store(&s_active, false);
        if (!wait_task_exit(40)) { /* at most 600ms */
            ESP_LOGW(TAG, "old mic task did not exit, giving up this start");
            return;
        }
    }
    /* Clear the level/ready bits left over from the previous round, so the first frame does not take off with stale loudness. */
    atomic_store(&s_level, 0);
    atomic_store(&s_ready, false);
    atomic_store(&s_active, true);
    atomic_store(&s_running, true);
    if (xTaskCreate(mic_task, "game_mic", 3072, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "mic task create failed");
        atomic_store(&s_active, false);
        s_task = NULL;
        atomic_store(&s_running, false);
    }
}

void game_mic_stop(void) {
    if (!s_task) return;
    atomic_store(&s_active, false);
    if (!wait_task_exit(40)) { /* wait at most 600ms */
        ESP_LOGW(TAG, "mic task slow to exit, leaving it to self-delete");
    }
}

bool game_mic_prepare_stop(void) {
    atomic_store(&s_active, false);
    return !atomic_load(&s_running);
}

uint16_t game_mic_level(void) { return (uint16_t)atomic_load(&s_level); }

bool game_mic_ready(void) { return atomic_load(&s_ready); }
