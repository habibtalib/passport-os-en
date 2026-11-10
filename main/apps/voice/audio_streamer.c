// main/audio_streamer.c -- streaming audio pipeline implementation.
#include "audio_streamer.h"
#include "app_events.h"
#include "bsp_audio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "adpcm.h"
#include <stdlib.h>

static const char *TAG = "audio";

// 100ms @ 16kHz/16bit/mono = 3200B (must be consistent in three places: ble_audio.h AUDIO_FRAME_BYTES,
// and AUDIO_FRAME_BYTES in companion/relay.py)
#define CHUNK_BYTES  3200
// In-ring item = [uint32 session token][3200B PCM] (3204B, 4B aligned, 4x801).
// The token is incremented by start()/cancel() and written into the item header; ble_worker only processes
// items whose token == the current session; leftovers of an old session (unconsumed in ring / in flight / late blocks) are silently
// invalidated by the token -- after cancel/link loss no "drain" gate is needed, and start is never refused because of leftovers
// (task 08-27-ring-desync-diagnose A1 conclusion: the leftover gate is structurally fragile).
#define CHUNK_ITEM_BYTES  (4 + CHUNK_BYTES)
// BLE channel compression: IMA ADPCM 4:1 (3200B PCM -> 804B block, 8KB/s).
// Why not Opus: the real BLE bottleneck is the mbuf pool (4x256B) + "first fragment fails -> whole frame discarded",
// not air bandwidth; with the bottleneck fixed 8KB/s needs only about 0.7 packets per connection event, and Opus's extra compression
// has nowhere to go, yet it needs 38.5KB of static state + 16KB of stack, exactly the heap needed to fix the bottleneck (see task
// design.md section 3). ADPCM state is 4 bytes on the stack: no heap, no alloca, no new component.
// USB bandwidth is ample, no compression (lossy and unnecessary), distinguished by the s_compressed switch.
static volatile bool s_compressed = false;    // compression switch (set by mode.c per channel)
// Session token: start() increments it to open a new session, cancel() increments it to invalidate late blocks (see the audio_worker
// snapshot semantics). The worker writes the snapshot into every item header, and ble_worker only processes items matching the current value.
static volatile uint32_t s_session_token = 0;
static volatile uint32_t s_last_encoded_token = 0;  // session of the last ADPCM encode (token changed = first block of a new session)
static adpcm_state_t s_adpcm;                 // accessed only from ble_worker context (4 bytes)
// Ring buffer: NOSPLIT, two slots (200ms).
// WARNING: BYTEBUF must not be used: ESP-IDF v5.5's prvGetItemByteBuf returns only the segment up to the
// buffer tail when data wraps around the end, so xRingbufferReceiveUpTo returns a length far below the request (measured on real hardware:
// offset 16B each round -> two returns of 16+2544). The compression path needs "whole 3200B block" semantics, a short read would
// encode misaligned audio directly; NOSPLIT + xRingbufferReceive has the implementation guarantee whole item boundaries.
// Two slots give BLE jitter a 100ms buffer (the old implementation held only one block, a single notify stall dropped frames).
#define RING_SLOTS   2
// NOSPLIT items have an 8B header and are 4B aligned; xMaxItemSize = ALIGN(xSize/2)-8,
// to fit 3204 needs xSize/2 >= 3212 -> 6424, take 6432 to leave 8B of wraparound margin.
#define RING_BYTES   6432
// Semaphore wait limit (F1 event-driven replacement for polling): in-flight notify <=50ms. Timeout semantics: stop/cancel
// return directly on timeout (the beep may overlap the tail, non-critical path; cancel's leftovers are covered by token invalidation).
// The wait is not a single take: the semaphore only speeds up wake-up, and the wait_worker_exit loop rechecks the worker
// state as the final verdict (a stale give only causes one useless wake-up). Worst-case wait <=200ms, typically <30ms.
#ifndef WORKER_EXIT_TIMEOUT_MS
#define WORKER_EXIT_TIMEOUT_MS 200   // wait for the capture worker to leave its blocking read (formerly 30x5ms polling)
#endif

// Session-owned storage: inactive Voice must not take SRAM from Xiaozhi/Muse.
typedef struct {
    StaticRingbuffer_t ring;
    StaticTask_t audio_tcb, send_tcb;
    StackType_t *audio_stack;
    StackType_t *send_stack;
    uint8_t *ring_storage;
    uint8_t *chunk;
} stream_memory_t;
static stream_memory_t *s_memory;
// Keep the proven stack and buffer sizes, but do not require one ~18 KiB hole.
// Every allocation is session-owned and is released only after both tasks stop.
static void stream_memory_free(void) {
    if (!s_memory) return;
    free(s_memory->chunk);
    free(s_memory->ring_storage);
    free(s_memory->send_stack);
    free(s_memory->audio_stack);
    free(s_memory);
    s_memory = NULL;
}
static bool stream_memory_alloc(void) {
    s_memory = calloc(1, sizeof(*s_memory));
    if (!s_memory) return false;
    s_memory->audio_stack = malloc(3072);
    s_memory->send_stack = malloc(4096);
    s_memory->ring_storage = malloc(RING_BYTES);
    s_memory->chunk = malloc(CHUNK_ITEM_BYTES);
    if (!s_memory->audio_stack || !s_memory->send_stack ||
        !s_memory->ring_storage || !s_memory->chunk) {
        stream_memory_free();
        return false;
    }
    return true;
}
#define s_ring_struct (s_memory->ring)
#define s_ring_storage (s_memory->ring_storage)
#define s_chunk (s_memory->chunk)
#define s_audio_stack (s_memory->audio_stack)
#define s_ble_stack (s_memory->send_stack)
#define s_audio_tcb (s_memory->audio_tcb)
#define s_ble_tcb (s_memory->send_tcb)
static volatile bool s_quit, s_audio_done, s_send_done;
static RingbufHandle_t s_ring;
static SemaphoreHandle_t s_sem;          // binary: the wait signal shared by both workers when idle
// F1 event-driven waits (replacing polling, cancel/wrap-up no longer busy-wait):
// s_worker_exit_sem -- given when audio_worker leaves its blocking read (stop/cancel wait on it);
// s_ring_empty_sem  -- given when the ring becomes empty after ble_worker returns a block (cancel/start/drain wait on it).
// A binary semaphore remembers state: if give comes before take, take returns at once; multiple gives do not count, no side effect.
// The consumer of s_worker_exit_sem is wait_worker_exit: give only speeds up wake-up, whether it has exited
// is decided by the s_worker_busy state -- a stale give (an earlier exit not consumed) does not cause a misjudgment.
static SemaphoreHandle_t s_worker_exit_sem;
static SemaphoreHandle_t s_ring_empty_sem;
static volatile bool s_active = false;
static volatile bool s_worker_busy = false;  // whether audio_worker is still in its blocking read (for stop/cancel to wait on)
static volatile uint16_t s_peak = 0;
static bool s_drop_active = false;           // shared drop flag: capture/send sides share one edge
// The drop count is ++'d by the two workers (capture ring full / send failure) and taken by the app task -- on a single core ++ is not atomic,
// so it is protected by a critical section (microseconds, does not block real-time behavior).
static portMUX_TYPE s_drop_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_drop_count = 0;            // drop count for this session (cleared by start; taken and reported after voice.end)

// Rate-limit the send-failure log: under congestion (no subscriber / flow-control timeout) a Warning per frame would make the load worse.
// The first frame logs at once, then one aggregated line per second (frames = drops within that 1s window, cumulative stats go via s_drop_count).
static uint32_t s_fail_log_n = 0;
static TickType_t s_fail_log_at = 0;

static TaskHandle_t s_audio_task = NULL;
static TaskHandle_t s_ble_task = NULL;
// Static-task APIs use the session allocation above. Workers stay resident
// across PTT turns, then cooperatively stop before that allocation is freed.
// Set only when init fully succeeds: if any creation fails the public API idles (see the checks at each entry) and never accesses
// NULL ring/sem -- the main flow (mode switch etc.) does not crash because audio pipeline init failed, audio
// degrades gracefully (voice.start proceeds as usual, no data flows, review P2).
static volatile bool s_ready = false;

// Audio frame send function (channel-agnostic, see audio_streamer.h). Registered at: main.c boot / mode switch.
// Default NULL: sends are always treated as failures (drop counting), the pipeline stays healthy (round 6/7 semantics preserved).
static audio_send_fn_t s_send_fn = NULL;

void audio_streamer_set_sender(audio_send_fn_t fn) { s_send_fn = fn; }

void audio_streamer_set_compressed(bool on) { s_compressed = on; }

// Number of blocks in the ring (pending + in-flight blocks taken by ble_worker but not yet returned). ++ after send, -- after return,
// zero <=> no data in the ring and none in flight -- exactly the "drain" semantics cancel/start/drain need.
// xRingbufferGetCurFreeSize is not used to test emptiness: under NOSPLIT it returns "the largest item that currently fits"
// (varies with the read/write pointer positions), and even an empty ring does not return to a fixed value.
static volatile uint32_t s_ring_items = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ring_items(void) {
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t n = s_ring_items;
    portEXIT_CRITICAL(&s_ring_mux);
    return n;
}

static void ring_items_inc(void) {
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_items++;
    portEXIT_CRITICAL(&s_ring_mux);
}

static void drop_inc(void) {
    portENTER_CRITICAL(&s_drop_mux);
    s_drop_count++;
    portEXIT_CRITICAL(&s_drop_mux);
}

// Called after ble_worker returns a block: when the ring is empty (no leftovers and in-flight ones returned) give the semaphore,
// waking drain's wait (voice.end frame order). Returning a block is the only operation that makes the ring idle, so this is the one correct detection point.
// Return a block: count --, and when it reaches zero give (wakes drain's wait).
static void ring_item_returned(void) {
    bool empty;
    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_items > 0) s_ring_items--;
    empty = (s_ring_items == 0);
    portEXIT_CRITICAL(&s_ring_mux);
    if (empty) xSemaphoreGive(s_ring_empty_sem);
}

// Wait for the ring to empty (at most ms milliseconds). The semaphore only speeds up wake-up, the ring state is the final verdict:
// (1) check the ring first, if empty -> done at once (otherwise we would wait out the timeout with no event to wait for);
// (2) if not empty -> wait for a return give (typically <=50ms). A stale give (a set semaphore left from an earlier empty ring)
// only causes one useless wake-up, and the loop rechecks the ring -- a stale event never causes a false "drained" verdict.
// Returns true <=> the ring was really empty at decision time. Wraparound safe: the deadline difference is cast to int32, negative means expired.
static bool wait_ring_empty(uint32_t ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    for (;;) {
        if (ring_items() == 0) return true;
        TickType_t remain = (TickType_t)(deadline - xTaskGetTickCount());
        if ((int32_t)remain <= 0) return false;
        xSemaphoreTake(s_ring_empty_sem, remain);
    }
}

// Wait for the capture worker to leave its blocking read (at most ms milliseconds). The semaphore only speeds up wake-up, the worker state is the
// final verdict (mirrors the wait_ring_empty pattern): loop rechecking s_worker_busy until it has really exited.
// A stale give (a set semaphore from the previous worker exit that nobody consumed) only causes one useless
// wake-up, and the loop rechecks the state -- a stale token never causes a false "exited" verdict (while the worker is still in its blocking read,
// an early return from stop would make the beep overlap the capture tail). Returns true <=> the worker had really exited at decision time.
static bool wait_worker_exit(uint32_t ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (s_worker_busy) {
        TickType_t remain = (TickType_t)(deadline - xTaskGetTickCount());
        if ((int32_t)remain <= 0) return false;
        xSemaphoreTake(s_worker_exit_sem, remain);
    }
    return true;
}

static void audio_worker(void *arg);
static void ble_worker(void *arg);

esp_err_t audio_streamer_init(void) {
    // Idempotent: re-entry in the success state returns at once (review R1) -- rebuilding would overwrite the global handles, and old workers
    // (reading the global s_ring/s_sem pointers) would switch to the new resources -> two workers running concurrently / resource confusion.
    // Retry after failure: the all-or-nothing guarantee means no live worker after a failure and the global handles not overwritten (initially
    // NULL or a valid earlier leftover), so a retry rebuilds from a clean state.
    if (s_ready) return ESP_OK;
    if (s_memory) return ESP_ERR_INVALID_STATE;
    if (!stream_memory_alloc()) return ESP_ERR_NO_MEM;
    s_quit = s_audio_done = s_send_done = false;
    s_ready = false;   // reset first: the failure path stays false and the public API idles (review P2)
    // All or nothing: only publish the global handles once every resource is created -- the failure path never overwrites old handles
    // (on repeated init concurrent readers / leftover threads still see valid handles, no NULL exposed by a partial failure;
    // when the real firmware's first init fails the global handles stay at their initial NULL, protected by the s_ready idling).
    // Failure paths release semaphores, the ring and its session allocation.
    RingbufHandle_t ring = xRingbufferCreateStatic(RING_BYTES, RINGBUF_TYPE_NOSPLIT,
                                                   s_ring_storage, &s_ring_struct);
    if (!ring) { stream_memory_free(); return ESP_FAIL; }
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) { vRingbufferDelete(ring); stream_memory_free(); return ESP_FAIL; }
    SemaphoreHandle_t exit_sem = xSemaphoreCreateBinary();
    if (!exit_sem) {
        vSemaphoreDelete(sem);
        vRingbufferDelete(ring); stream_memory_free();
        return ESP_FAIL;
    }
    SemaphoreHandle_t empty_sem = xSemaphoreCreateBinary();
    if (!empty_sem) {
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vRingbufferDelete(ring); stream_memory_free();
        return ESP_FAIL;
    }
    s_ring = ring;
    s_ring_items = 0;
    s_sem = sem;
    s_worker_exit_sem = exit_sem;
    s_ring_empty_sem = empty_sem;

    // Audio first: capture (6) > send (5), so mic data never loses its capture rhythm because sending is slow.
    // Stacks: audio 3072 bytes (deep path bsp_audio_read->esp_codec_dev_read, driver ~1-1.5KB);
    // ble 4096 -- ADPCM encoding is only table lookups + shifts, the biggest local is the 804B output buffer (see ble_worker),
    // plus ample margin for the notify chain; the real-hardware HWM is printed in the stop log, tighten further based on it.
    // Creation failure check: a silent failure when memory is short would leave nobody consuming after start (semaphore given in vain, pipeline falsely alive).
    // All-or-nothing semantics: any failure rolls back tasks already built -- a lopsided state of "capture task running, send task missing"
    // (ring buffer piling up continuously, review P2-6) is never allowed. On failure the pointers stay NULL / deleted, the stop null
    // pointer guards already exist; the caller handles the explicit error by degrading gracefully.
    // A task creation failure likewise frees the semaphores already built (review R1): the workers were not built / were deleted, no task references them.
    TaskHandle_t audio_h = xTaskCreateStatic(audio_worker, "audio_worker", 3072, NULL, 6,
                                             s_audio_stack, &s_audio_tcb);
    if (!audio_h) {
        ESP_LOGE(TAG, "audio worker creation failed");
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vSemaphoreDelete(empty_sem);
        vRingbufferDelete(ring); stream_memory_free();
        s_ring = NULL; s_sem = s_worker_exit_sem = s_ring_empty_sem = NULL;
        return ESP_FAIL;
    }
    s_audio_task = audio_h;   // static task handle: for HWM logging and rollback deletion
    // The handle must be stored: uxTaskGetStackHighWaterMark(NULL) reports the caller's (app_task's)
    // margin, not ble_worker's -- the old code discarded the return value so the ble margin in the stop log was fake.
    TaskHandle_t ble_h = xTaskCreateStatic(ble_worker, "ble_worker", 4096, NULL, 5,
                                           s_ble_stack, &s_ble_tcb);
    if (!ble_h) {
        ESP_LOGE(TAG, "ble worker creation failed, rolling back audio worker");
        vTaskDelete(audio_h);   // the audio worker is blocked waiting, deletion is safe (holds no resources)
        s_audio_task = NULL;
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vSemaphoreDelete(empty_sem);
        vRingbufferDelete(ring); stream_memory_free();
        s_ring = NULL; s_sem = s_worker_exit_sem = s_ring_empty_sem = NULL;
        return ESP_FAIL;
    }
    s_ble_task = ble_h;
    s_ready = true;   // all resources ready: only from now on may the public API access ring/sem/worker
    ESP_LOGI(TAG, "streaming pipeline ready (static ring %d B / %d slots, block %d B)",
             RING_BYTES, RING_SLOTS, CHUNK_BYTES);
    return ESP_OK;
}

bool audio_streamer_deinit(void) {
    if (!s_memory) return true;
    audio_streamer_cancel();
    s_ready = false;
    s_active = false;
    s_quit = true;
    xSemaphoreGive(s_sem);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (!s_audio_done || !s_send_done) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            ESP_LOGE(TAG, "Voice pipeline stop timed out; retaining live buffers");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Workers suspend only after leaving codec/network IO and returning items.
    vTaskDelete(s_audio_task); s_audio_task = NULL;
    vTaskDelete(s_ble_task); s_ble_task = NULL;
    vRingbufferDelete(s_ring); s_ring = NULL;
    vSemaphoreDelete(s_sem); s_sem = NULL;
    vSemaphoreDelete(s_worker_exit_sem); s_worker_exit_sem = NULL;
    vSemaphoreDelete(s_ring_empty_sem); s_ring_empty_sem = NULL;
    stream_memory_free();
    s_ring_items = 0; s_peak = 0;
    ESP_LOGI(TAG, "Voice pipeline released: %u bytes", (unsigned)(sizeof(stream_memory_t) + 3072 + 4096 + RING_BYTES + CHUNK_ITEM_BYTES));
    return true;
}

esp_err_t audio_streamer_start(void) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;   // init failed: start refused (idle, no access to NULL ring/sem)
    if (s_active) return ESP_OK;                  // idempotent: already in a session
    // Session token: every new session takes a new value, and in-ring / in-flight items (old token) are silently dropped
    // in ble_worker -- leftovers never flow into the new session, and start is never refused because of leftovers (no drain gate,
    // see the CHUNK_ITEM_BYTES comment; after cancel it can reopen at once without waiting for the ring to empty).
    s_session_token++;
    s_active = true;
    s_peak = 0;
    s_drop_active = false;
    s_drop_count = 0;
    // The ADPCM adaptive reset is consumed by ble_worker (token change detection, this task does not touch the encoder
    // state): reset before the first block of a new session, not carrying over the previous session's silence/noise adaptation.
    xSemaphoreGive(s_sem);
    ESP_LOGI(TAG, "capture started (%s, session %u)", s_compressed ? "ADPCM 4:1" : "PCM",
             (unsigned)s_session_token);
    return ESP_OK;
}

void audio_streamer_stop(void) {
    if (!s_active) return;
    s_active = false;             // audio worker exits at the next loop
    // Wait for the worker to leave its blocking read (semaphore wake-up + final state verdict, see wait_worker_exit),
    // to avoid the SEND beep overlapping the capture tail writing to the codec.
    wait_worker_exit(WORKER_EXIT_TIMEOUT_MS);
    if (s_audio_task) {   // the deep path is reached after the first session; shrink the stack later based on HWM on real hardware (see the init comment)
        ESP_LOGI(TAG, "stack margin: audio %u/3072, ble %u/4096",
                 (unsigned)uxTaskGetStackHighWaterMark(s_audio_task),
                 (unsigned)uxTaskGetStackHighWaterMark(s_ble_task));
    }
    ESP_LOGI(TAG, "capture stopped");
}

void audio_streamer_cancel(void) {
    if (!s_ready) return;   // init failed: idempotent idle (no access to NULL ring, review P2)
    // Idempotent: s_active is not checked -- capture may already be stopped (link lost after STOP), and leftovers in the ring must still be invalidated.
    // Semantics: (1) invalidate the session token: the worker snapshotted the old token during the blocking read, its late block
    // (sent after read returns) carries the old value and is silently dropped by ble_worker as invalid -- leftover and in-flight
    // frames never flow into the next session, and no drain wait is needed (cancel returns fast).
    // (2) stop capture (the ring gets no new writes) (3) wait for audio_worker to leave its blocking read (after that the ring is stable).
    // The ring reset API is not used (ESP-IDF has no xRingbufferReset; and any "clear" implementation would
    // conflict with blocks ble_worker has taken but not yet returned) -- token invalidation is equivalent to clearing, at zero cost.
    s_session_token++;
    s_active = false;
    wait_worker_exit(WORKER_EXIT_TIMEOUT_MS);
    ESP_LOGI(TAG, "capture cancelled (leftovers dropped by token invalidation)");
}

bool audio_streamer_active(void) { return s_active; }

void audio_streamer_release_audio(void) {
    // Only the voice owner calls this after stop/cancel. Never delete a codec
    // that the worker is still reading when its bounded stop wait timed out.
    if (!s_active && !s_worker_busy) bsp_audio_deinit();
    else ESP_LOGW(TAG, "Audio still busy; defer codec release");
}

uint16_t audio_streamer_peak(void) { return s_peak; }

uint32_t audio_streamer_take_drops(void) {
    portENTER_CRITICAL(&s_drop_mux);
    uint32_t d = s_drop_count;
    s_drop_count = 0;
    portEXIT_CRITICAL(&s_drop_mux);
    return d;
}

// Wait for the ring to empty (at most ms milliseconds), used to guarantee frame order before voice.end.
// Polls only, does not consume: a full ring means data remains (including in-flight blocks ble_worker took but has not returned); taking without sending = dropping the tail.
void audio_streamer_drain(uint32_t ms) {
    if (!s_ready) return;   // init failed: idle (no access to NULL ring)
    // Wait for the ring to empty (semaphore event-driven + ring state verdict, see wait_ring_empty). On timeout only log
    // -- voice.end frame order is guaranteed by the caller (end is sent after drain), and with stray frames in flight end is still sent,
    // backed up by the status reconciliation frame.
    if (!wait_ring_empty(ms)) {
        ESP_LOGW(TAG, "drain timed out, stray frames may still be in flight");
    }
}

static void audio_worker(void *arg) {
    (void)arg;
    while (!s_quit) {
        xSemaphoreTake(s_sem, portMAX_DELAY);    // wait for the start signal
        if (s_quit) break;
        s_worker_busy = true;
        while (s_active) {
            // Session token this block belongs to (snapshotted on entering the loop): if a cancel happens during the blocking read,
            // the late block (this one) still carries the old token and is silently dropped by ble_worker as invalid.
            uint32_t my_token = s_session_token;
            // +4: the item header is reserved for the token; the whole is 4B aligned, the DMA target (+4) is still on a 4B boundary
            esp_err_t e = bsp_audio_read(s_chunk + 4, CHUNK_BYTES);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "bsp_audio_read failed (%s), stopping stream",
                         esp_err_to_name(e));
                app_event_t ev = { .type = APP_EV_AUDIO_ERROR };  // hardware error event with independent semantics
                app_event_post(&ev);
                s_active = false;
                break;
            }
            // Peak sample (UI volume bar). Widen to int32 before negating:
            // negating int16 -32768 directly is signed overflow (UB).
            const int16_t *p = (const int16_t *)(s_chunk + 4);
            uint16_t peak = 0;
            for (size_t i = 0; i < CHUNK_BYTES / 2; i++) {
                int32_t v = p[i];
                uint16_t a = (uint16_t)(v < 0 ? -v : v);
                if (a > peak) peak = a;
            }
            s_peak = peak;

            // Stamp the session token (item header) and enqueue it into the ring together with the block
            *(uint32_t *)s_chunk = my_token;
            if (xRingbufferSend(s_ring, s_chunk, CHUNK_ITEM_BYTES, 0) == pdTRUE) {
                ring_items_inc();
            } else {
                // Ring full: the source drops at once (non-blocking -- blocking for space would delay the next block's capture and
                // break the 100ms capture rhythm; the mic keeps running and the I2S driver drops samples automatically), report BLE BUSY
                drop_inc();
                if (!s_drop_active) {
                    s_drop_active = true;
                    app_event_t ev = { .type = APP_EV_AUDIO_DROP_START };
                    app_event_post(&ev);
                }
            }
        }
        s_worker_busy = false;
        xSemaphoreGive(s_worker_exit_sem);   // F1: notify the waiters in stop/cancel
    }
    s_audio_done = true;
    vTaskSuspend(NULL);
}

static void ble_worker(void *arg) {
    (void)arg;
    while (!s_quit) {
        size_t len = 0;
        // NOSPLIT: whole-item semantics, len always equals the written length (no short reads as with BYTEBUF)
        void *item = xRingbufferReceive(s_ring, &len, pdMS_TO_TICKS(50));
        if (!item) {
            continue;   // ring empty: the natural state
        }
        int rc = 0;
        if (len != CHUNK_ITEM_BYTES) {
            // Should not happen (NOSPLIT guarantees whole items); if it does, drop this block and leave evidence,
            // never interpret the buffer as fixed length (the old BYTEBUF path read out of bounds exactly like that).
            ESP_LOGW(TAG, "abnormal ring block length %u (expected %d), dropped",
                     (unsigned)len, CHUNK_ITEM_BYTES);
            rc = -1;
        } else if (*(const uint32_t *)item != s_session_token) {
            // Session gate: leftovers / late blocks of an old session (normal length but token mismatch) are silently dropped
            // -- after cancel/link loss the leftovers in the ring are invalidated by this, and start can reopen at once without draining.
            // Silent: a normal part of the lifecycle (occurs on every cancel), not counted, no alarm, no event triggered.
            vRingbufferReturnItem(s_ring, item);
            ring_item_returned();
            continue;
        } else if (s_compressed) {
            // BLE compression path: a whole 3200B PCM block -> one 804B ADPCM block sent in one go.
            // Each block carries its own predictor/index header and decodes alone -- losing a block loses only that 100ms.
            if (s_last_encoded_token != s_session_token) {
                // First block of a new session (token change detection): reset the adaptation, not carrying over the previous session's
                // silence/noise adaptation
                adpcm_state_reset(&s_adpcm);
                s_last_encoded_token = s_session_token;
            }
            uint8_t block[ADPCM_BLOCK_BYTES];
            size_t n = adpcm_encode_block(&s_adpcm, (const int16_t *)(item + 4),
                                          CHUNK_BYTES / 2, block, sizeof(block));
            if (n == 0) {   // parameters/capacity mismatch (compile-time constants, theoretically unreachable): drop the block and leave evidence
                ESP_LOGW(TAG, "ADPCM encode failed (block %u B), dropped", (unsigned)len);
                rc = -1;
            } else {
                // An unregistered channel counts as a send failure (consistent with the old semantics)
                rc = s_send_fn ? s_send_fn(block, n) : -1;
            }
        } else {
            // Uncompressed path (USB): the PCM block is sent as is (the 4B token is in-ring metadata,
            // not put on the link, the 3200B payload matches the old protocol)
            rc = s_send_fn ? s_send_fn((const uint8_t *)item + 4, CHUNK_BYTES) : -1;
        }
        vRingbufferReturnItem(s_ring, item);
        ring_item_returned();
        if (rc == 0) {
            // Only clear BLE BUSY once sending recovers and the ring has headroom (no backlog) --
            // relying on send success alone is not enough: if sending is slow but always succeeds, drops caused by a full ring would never be cleared
            if (s_drop_active && ring_items() < RING_SLOTS) {
                s_drop_active = false;
                app_event_t ev = { .type = APP_EV_AUDIO_DROP_END };
                app_event_post(&ev);
            }
        } else {
            // No subscriber / disconnected / encode failure / abnormal block length: drop the whole block and count it (reported in the status frame after voice.end)
            drop_inc();
            if (!s_drop_active) {
                s_drop_active = true;
                app_event_t ev = { .type = APP_EV_AUDIO_DROP_START };
                app_event_post(&ev);
            }
            s_fail_log_n++;
            if (s_fail_log_at == 0 ||   // first failure logs at once (afterwards aggregated in 1s windows)
                xTaskGetTickCount() - s_fail_log_at >= pdMS_TO_TICKS(1000)) {
                ESP_LOGW(TAG, "audio frame send failed, dropped (%u frames/window)", s_fail_log_n);
                s_fail_log_n = 0;
                s_fail_log_at = xTaskGetTickCount();
            }
        }
    }
    s_send_done = true;
    vTaskSuspend(NULL);
}
