#pragma once

// Xiaozhi audio service: ES8311 half-duplex recording / playback.
//
// Threading model (the core of the crash fix):
//   * The websocket task only calls PushIncomingAudio(): it writes the ring buffer and never decodes or touches the
//     codec; when the queue is full it waits at most 300ms, letting TCP backpressure absorb the burst.
//   * One resident worker thread xzg_audio handles both "record encode" and "playback decode" modes. The two modes
//     are naturally mutually exclusive (half-duplex), so one shared large stack is enough -- the Opus codecs use stack
//     temporary buffers, measured ~19KB for encoding and >6KB for decoding; a separate large stack for each simply does not fit in a ~90KB heap.
//   * The Start*/Stop* lifecycle interfaces must be called from the same task (the control task); a recursive lock inside
//     protects them, and every wait has a clear upper bound, so the caller is never hung indefinitely.
//
// Key differences from the old implementation:
//   1. The old StopRecording() spun unboundedly with `while (handle) vTaskDelay(10)`: once the recording task
//      got stuck in an I2S read with portMAX_DELAY, the caller hung forever, and in turn the portMAX_DELAY wait of
//      esp_websocket_client_stop() never returned -> watchdog reset.
//      Now every wait has a limit; on timeout it only logs and continues wrapping up.
//   2. The old implementation called StartPlayback()/StopRecording() directly from the websocket task, operating the same codec handle
//      concurrently with the recording task and the key task (esp_codec_dev_open/close is not thread-safe)
//      -> random crashes (reproduced on device: "press OK to interrupt while the AI is speaking" always rebooted). Now the codec lifecycle
//      happens only in the control task.
//   3. The old implementation decoded + wrote I2S synchronously in the network task for every frame, directly blocking network I/O.
//   4. The recording thread no longer sends network packets itself: mbedTLS ssl_write is stack-hungry, so sending is done by the control task,
//      saving stack and also letting the protocol object be accessed by one task only.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <atomic>
#include "esp_err.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace xiaozhi {

// Encoding: 60ms @ 16kHz mono 16bit (1920 bytes PCM -> ~240 bytes Opus)
constexpr size_t kEncPcmFrameBytes = 1920;
constexpr int kEncFrameDurationMs = 60;

constexpr size_t kOpusFrameMaxBytes = 512;
// Decoding: server-sent 24kHz/60ms mono PCM is 2880 bytes.
// Aligned with the official folo-ai-passport-xiaozhi: hardware-native 24kHz output, no upsampling needed.
constexpr size_t kMaxPcmFrameBytes = 2880;

class AudioService {
public:
    static AudioService &GetInstance();
    // Non-blocking flag only; safe from input/launcher without constructing the
    // singleton or touching codec/worker handles.
    static void RequestStop();

    esp_err_t Init();
    // Called on app exit: stop everything, destroy the resident worker thread, and return buffers to the heap.
    // So memory is not held by Xiaozhi after switching to another app (a prerequisite for multi-app coexistence).
    bool Deinit();

    // ---- Called from the websocket task: bounded backpressure, drop if persistently full ----
    void PushIncomingAudio(const uint8_t *opus_data, size_t len);

    // ---- Called from the control task: recording lifecycle ----
    //
    // The encoder is "opened early and kept resident for reuse" (fixed 2026-09-13):
    // On device, the moment when a large contiguous block is easiest to get is AudioService::Init() (largest is
    // about 30KB then); by the time talk is pressed the heap has been fragmented by websocket/TLS and the UI,
    // largest is only 15-22KB, while esp_opus_enc_open needs a contiguous block of about 25KB,
    // and so it always fails (ESP_OPUS_ENC: Opus encoder init failed. ret:-7).
    // Therefore the encoder is opened and kept resident in Init(), and StopRecording() no longer closes it.
    // The playback decoder is reserved synchronously in Init and reused for normal turns and reconnects.
    bool EnsureEncoder();

    // Server hello confirms sample rate, then allocates the small session rings.
    // Decoder memory is reserved before TLS and retained across reconnects.
    bool PrepareSessionResources();
    // Release the rings after disconnection; Deinit releases codec states too.
    bool ReleaseSessionResources();

    bool StartRecording();
    void StopRecording();
    bool IsRecording() const { return is_recording_; }

    // ---- Called from the control task: fetch encoded audio frames and send ----
    bool PopEncodedFrame(uint8_t *out, size_t out_cap, size_t *out_len);
    bool HasPendingEncoded() const;
    void ResetEncodedQueue();

    // ---- Called from the control task: playback lifecycle ----
    void SetPlaybackSampleRate(int hz);
    int GetPlaybackSampleRate() const { return playback_rate_; }

    bool StartPlayback();
    void StopPlayback();    // play out the remaining buffered audio, then stop (normal tts stop)
    void AbortPlayback();   // discard the buffer and stop immediately (interrupt / app exit)
    bool IsPlaying() const { return is_playing_; }
    bool HasPendingPlayback() const;
    uint32_t PlaybackProgress() const { return playback_progress_.load(); }

    void Reset();

private:
    enum class Work : uint8_t { Idle, Record, Play };

    AudioService();
    ~AudioService();
    AudioService(const AudioService &) = delete;
    AudioService &operator=(const AudioService &) = delete;

    static void WorkerEntry(void *arg);
    void WorkerLoop();
    void RecordSection();
    void PlaySection();

    bool WaitWorkIdle(uint32_t timeout_ms, const char *name);
    bool OpenDecoder();
    std::atomic<uint32_t> playback_progress_{0};

    // Generic byte ring buffer (stores [u16 len][payload] records)
    struct Ring {
        uint8_t *buf;
        size_t cap;
        size_t head;
        size_t tail;
        size_t records;
        SemaphoreHandle_t lock;
        const char *name;
    };
    static void RingInit(Ring &r, uint8_t *buf, size_t cap, SemaphoreHandle_t lock,
                         const char *name);
    static size_t RingUsed(const Ring &r);
    static size_t RingFree(const Ring &r);
    static size_t RingCount(const Ring &r);
    static bool RingPush(Ring &r, const uint8_t *data, size_t len);
    static size_t RingPop(Ring &r, uint8_t *out, size_t out_cap);
    static void RingReset(Ring &r);
    static void RingRelease(Ring &r, uint8_t *&owned_buffer);

    void *opus_enc_handle_{nullptr};
    void *opus_dec_handle_{nullptr};
    int opus_dec_rate_{0};                  // sample rate used by the current decoder

    std::atomic<bool> is_recording_{false};
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> play_stop_{false};    // hard stop: exit immediately
    std::atomic<bool> play_drain_{false};   // soft stop: play out the buffer, then exit

    // Reserve the current service default before TLS; reconcile with hello
    // before accepting any audio if a server negotiates a different rate.
    int playback_rate_{24000};

    // One worker performs encode/decode exclusively. Both modes share the
    // 512-byte Opus scratch; WebSocket writes only to the separate ring.
    uint8_t *opus_scratch_{nullptr};
    // Shared 2880 B PCM holds either 1920 B recording or 2880 B playback.
    uint8_t *pcm_scratch_{nullptr};

    TaskHandle_t worker_task_{nullptr};  // resident: one task shared by recording/playback
    StackType_t *worker_stack_{nullptr}; // borrows the fixed work area that is mutually exclusive with the radio
    StaticTask_t worker_tcb_{};
    std::atomic<Work> work_{Work::Idle};
    std::atomic<bool> worker_quit_{false};
    SemaphoreHandle_t work_signal_{nullptr};
    SemaphoreHandle_t work_idle_{nullptr};
    SemaphoreHandle_t worker_stopped_{nullptr};

    SemaphoreHandle_t lifecycle_lock_{nullptr};
    // Lifetime-long synchronization must not pin holes inside session heap.
    StaticSemaphore_t sync_storage_[6]{};
    bool bsp_owned_{false};

    // Playback ring buffer 3072 ~= 12-15 Opus frames ~= 0.8 s of margin. Together with short TCP backpressure
    // (the 300ms bounded wait in PushIncomingAudio) it absorbs common bursts; frames are still dropped if persistently full.
    // Shrinking the capacity from 8KB to 3KB returns 5120 bytes of internal SRAM directly to the system heap, ensuring
    // the recording stack and Opus encoder can get contiguous memory.
    static constexpr size_t kPlayRingCap = 3072;
    uint8_t *play_ring_buf_{nullptr};
    Ring play_ring_{};

    static constexpr size_t kTxRingCap = 512;
    uint8_t *tx_ring_buf_{nullptr};
    Ring tx_ring_{};
};

} // namespace xiaozhi
