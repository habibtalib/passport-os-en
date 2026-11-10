// main/app_sound.c -- tone implementation.
// Each tone is made of one or more square-wave segments (frequency x duration), joined seamlessly.
// Everything runs at 16kHz/16bit/mono -- the same format as the recording stream, so bsp_audio_set_format is called only once.
#include "app_sound.h"
#include "app_events.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app_sound";

#define SAMPLE_RATE 16000
#define CHUNK_SAMPLES 256   // ~16ms per chunk, keeps the temporary stack buffer small
#define AMPLITUDE 5000      // square-wave amplitude (+-), slightly softer than the demo's 6000

// ---- Tone table: each tone = several {frequency Hz, duration ms} segments ----
typedef struct { uint16_t hz; uint16_t ms; } tone_seg_t;

static const tone_seg_t TONE_TABLE[APP_TONE_COUNT][3] = {
    [APP_TONE_START]    = { { 440, 80 },  { 0, 0 }, { 0, 0 } },  // recording ready
    [APP_TONE_SEND]     = { { 880, 60 },  { 0, 0 }, { 0, 0 } },  // voice sent
    [APP_TONE_APPROVAL] = { { 587, 75 },  { 880, 75 }, { 0, 0 } }, // approval alert (two tones)
    [APP_TONE_SUCCESS]  = { { 660, 100 }, { 880, 100 }, { 0, 0 } }, // done (rising tone)
    [APP_TONE_REJECT]   = { { 220, 120 }, { 0, 0 }, { 0, 0 } },  // rejected
    [APP_TONE_ERROR]    = { { 180, 100 }, { 0, 0 }, { 0, 0 } },  // offline / invalid
};

// Play one segment: generate a whole square-wave chunk + blocking I2S write.
static void play_segment(uint16_t hz, uint16_t ms) {
    int16_t buf[CHUNK_SAMPLES];
    const int period = SAMPLE_RATE / hz;         // samples per period
    const int half = period / 2;
    int total = (int)SAMPLE_RATE * ms / 1000;
    int phase = 0;
    while (total > 0) {
        int n = total < CHUNK_SAMPLES ? total : CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            buf[i] = (phase < half) ? AMPLITUDE : -AMPLITUDE;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "bsp_audio_write failed");
            return;
        }
        total -= n;
    }
}

static void play_tone_impl(app_tone_t tone) {
    if (tone >= APP_TONE_COUNT) return;
    const tone_seg_t (*segs)[3] = &TONE_TABLE[tone];
    for (int i = 0; i < 3; i++) {
        if ((*segs)[i].hz == 0) break;
        play_segment((*segs)[i].hz, (*segs)[i].ms);
    }
}

// ---- Async playback: static queue + worker ----
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[APP_TONE_COUNT * sizeof(uint8_t)];
static QueueHandle_t s_queue;

static void sound_worker(void *arg) {
    (void)arg;
    uint8_t tone;
    for (;;) {
        if (xQueueReceive(s_queue, &tone, portMAX_DELAY) == pdTRUE) {
            play_tone_impl((app_tone_t)tone);
            // After the START tone finishes -> tell app_task to open the stream (time-multiplexed: beep before capture).
            // Other tones do not need it and produce no event.
            if (tone == APP_TONE_START) {
                app_event_t ev = { .type = APP_EV_TONE_DONE };
                app_event_post(&ev);
            }
        }
    }
}

esp_err_t app_sound_init(void) {
    esp_err_t e = bsp_audio_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(e));
        return e;
    }
    e = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_set_format failed: %s", esp_err_to_name(e));
        return e;
    }
    bsp_audio_set_volume(80);

    s_queue = xQueueCreateStatic(APP_TONE_COUNT, sizeof(uint8_t),
                                 s_queue_storage, &s_queue_struct);
    if (!s_queue) return ESP_FAIL;
    // Static stack (.bss): 1536B is enough for tones (a single 256-sample buf); at the heap limit every byte
    // decides whether the host task (5120B, created dynamically) can be created.
    // At 1536B the high-water mark left only 316B (measured 2026-08-28) -> 2048B.
    static StackType_t s_snd_stack[2048 / sizeof(StackType_t)];
    static StaticTask_t s_snd_tcb;
    // Priority 5 (raised from 3 on 2026-08-29, same level as ble_worker, still below audio_worker 6):
    // the recording beep must finish before the stream opens (TONE_DONE gate); at prio 3 it queued behind app_task(4) and
    // taskLVGL(4, 2-row buffer refreshing 240x320) -- measured on hardware, long-press detection to `capture start` took
    // 578ms while the beep itself is only 80ms. After raising the priority this drops back to ~80ms. A tone is just tens of ms of
    // blocking I2S writes (yielding the CPU while waiting for DMA), so it cannot starve same/lower-priority tasks; and it does not overlap capture.
    if (!xTaskCreateStatic(sound_worker, "sound_worker", 2048, NULL, 5,
                           s_snd_stack, &s_snd_tcb)) {
        ESP_LOGE(TAG, "sound worker creation failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "tones ready");
    return ESP_OK;
}

bool app_sound_play(app_tone_t tone) {
    if (xQueueSend(s_queue, &tone, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tone queue full, dropping %d", (int)tone);
        return false;
    }
    return true;
}

void app_sound_play_sync(app_tone_t tone) {
    play_tone_impl(tone);
}
