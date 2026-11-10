#include "apps/xiaozhi/app_xiaozhi.h"
#include "apps/xiaozhi/xiaozhi_protocol.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#include "apps/xiaozhi/xiaozhi_ui.h"
#include "apps/xiaozhi/xiaozhi_ota.h"
#include "apps/xiaozhi/xiaozhi_store.h"
#include "wifi_manager.h"
#include "bsp_audio.h"
#include "launcher/settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include <freertos/semphr.h>
#include <cstring>
#include <string>

static const char *TAG = "app_xiaozhi";

// ---------------------------------------------------------------------------
// Architecture: single-owner control task (xzg_ctl)
//
// The real failure before the fix: multiple tasks operated the same audio state at once, with no locking at all --
//   * websocket task: receives cloud audio -> StartPlayback()/PlayAudioFrame()
//   * input task: key press -> StopPlayback()/StartRecording()
// "Pressing OK while the AI is speaking to interrupt" made the two run concurrently: the input task frees playback_buf_ and
// closes opus_dec_handle_ while the websocket task is still decoding with them -> use-after-free
// -> panic -> reboot. Reproduced on device (log stops right after "Opus decoder closed" followed by
// "Starting audio playback", then the device reboots).
//
// Division of responsibilities after the fix:
//   * Only the control task may call Protocol::Start/Stop/Trigger*, the Start*/Stop* of AudioService,
//     and all ui_*.
//   * websocket callbacks only write the event queue + put Opus into the ring buffer, then return immediately.
//   * Key callbacks only post commands, then return immediately.
//   * The Protocol instance is a "never destructed" function-static object, eliminating callbacks touching a destroyed object at the root.
// ---------------------------------------------------------------------------

namespace {

using xiaozhi::EvType;
using xiaozhi::Event;
using xiaozhi::OtaResult;
using xiaozhi::State;

constexpr int kProtoQueueDepth = 8;  // text only; critical transitions use mailbox
constexpr int kCmdQueueDepth = 8;
// The control task only does state machine / UI / small-message sending (ssl_write uses about 2-3KB of stack).
// The two truly stack-hungry things have been split off: the OTA mbedTLS handshake -> xzg_ota temporary task;
// Opus encode/decode -> xzg_audio resident shared task (24KB, see the note in xiaozhi_audio.cc).
constexpr size_t kControlTaskStack = 6144;
constexpr uint32_t kLoopTickMs = 20;

// Timers (milliseconds)
constexpr int64_t kConnectTimeoutMs = 12000;    // upper limit for waiting for hello after starting a connection
constexpr int64_t kThinkingTimeoutMs = 50000;   // leaves response time on weak networks, still prevents a permanent hang
constexpr int64_t kListeningTimeoutMs = 30000;
constexpr int64_t kSpeakingStallTimeoutMs = 30000; // no audio/PCM progress, not reply duration
constexpr int64_t kReconnectDelayMs = 6000;     // reconnect backoff after disconnect
constexpr int64_t kActivationRepollMs = 30000;  // re-query interval while waiting for the user to bind
constexpr int64_t kWifiRetryMs = 1000;
constexpr int64_t kOtaRefreshMs = 10 * 60 * 1000;  // connection parameter cache lifetime (10 minutes)
constexpr int kOtaRetryAfterFailures = 3;          // refetch config after this many consecutive failures

enum class Cmd : uint8_t {
    Connect = 0,
    StartListen,
    StopListen,
    Abort,
    VolUp,
    VolDown,
    ToggleListen,
    Stop,
};

QueueHandle_t s_proto_q = nullptr;
QueueHandle_t s_cmd_q = nullptr;
StaticQueue_t s_proto_queue_storage{}, s_cmd_queue_storage{};
uint8_t s_proto_items[kProtoQueueDepth * sizeof(Event)];
uint8_t s_cmd_items[kCmdQueueDepth * sizeof(Cmd)];
StaticSemaphore_t s_stopped_storage{};
TaskHandle_t s_ctl_task = nullptr;
std::atomic<bool> s_control_running{false};
bool s_start_failed = false;
SemaphoreHandle_t s_stopped_sem = nullptr;
std::atomic<bool> s_app_active{false};
// Independent fallback flag for the Stop command: see the post_cmd() comment (Stop must still work when the queue is full).
std::atomic<bool> s_stop_requested{false};
// Whether the protocol singleton has been fully constructed. Constructing it writes NVS (including SPI-Flash erase/write, a very deep call chain),
// so it may only happen in the control task with a large enough stack; the key task (3072-byte stack) must never trigger it.
std::atomic<bool> s_protocol_ready{false};

// Control-task private state
State s_cur_state = State::Disconnected;
bool s_connect_pending = false;
int64_t s_connect_started_ms = 0;
int64_t s_thinking_since_ms = 0;
int64_t s_speaking_since_ms = 0;
uint32_t s_playback_progress = 0;
int64_t s_reconnect_at_ms = 0;
int64_t s_reconnect_delay_ms = kReconnectDelayMs;
int64_t s_listening_since_ms = 0;
int64_t s_next_connect_attempt_ms = 0;
bool s_activation_pending = false;
std::string s_activation_code;
bool s_audio_ready = false;
bool s_session_ready = false;
bool s_audio_recovery_required = false;
std::string s_retry_reason;
// From the user pressing OK to start until the user presses OK again / an abnormal abort. Only while this flag is true does TTS
// automatically return to the next round of listening after playback; this avoids the mic being re-armed by a state event after the user deliberately interrupts.
bool s_conversation_active = false;
// A user's OK while disconnected resumes listening only after the new hello
// and audio reservation succeed. Background reconnects never open the mic.
bool s_listen_after_connect = false;

// Connection parameter cache: the OTA request runs one HTTPS exchange (several hundred ms + about 30KB heap),
// and it must not be repeated on every reconnect. Refetch only on first use / expiry / consecutive failures.
bool s_cfg_valid = false;
std::string s_cfg_url;
std::string s_cfg_token;
int s_cfg_version = 3;
int64_t s_cfg_fetched_ms = 0;
int s_connect_failures = 0;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

void post_cmd(Cmd c) {
    if (s_cmd_q == nullptr) return;
    // Fixed 2026-09-13 (A4): Stop must never be dropped. It used to be posted non-blocking in all cases. When the control task
    // is stuck in connect_xiaozhi() (OTA/websocket, up to 20s) and the queue is full, Stop was dropped,
    // stop() waited until timeout but the task was still alive; and start() only checked s_app_active and created another
    // control task -> two tasks operating the audio at once = the UAF recorded in the file header recurs.
    // Now: Stop uses a 500ms blocking post + an independent s_stop_requested flag as double insurance,
    // so even if the post fails, the control task sees the flag on its next loop iteration and exits.
    if (c == Cmd::Stop) {
        s_stop_requested = true;
        xiaozhi::AudioService::RequestStop();
    }
    const TickType_t wait = (c == Cmd::Stop) ? pdMS_TO_TICKS(500) : 0;
    if (xQueueSend(s_cmd_q, &c, wait) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropped cmd=%d", (int)c);
    }
}

// Never-destructed protocol instance: websocket callbacks/recording task may both reference it, so static lifetime is safest.
xiaozhi::Protocol &proto() {
    static xiaozhi::Protocol inst;
    return inst;
}

void schedule_reconnect(int64_t delay_ms) {
    if (s_reconnect_at_ms != 0) return; // error/disconnect can describe the same failure
    if (delay_ms == kReconnectDelayMs) {
        delay_ms = s_reconnect_delay_ms;
        s_reconnect_delay_ms = delay_ms >= 30000 ? 60000 : delay_ms * 2;
    }
    s_reconnect_at_ms = now_ms() + delay_ms;
    s_connect_pending = false;
}

void clear_timers() {
    s_listening_since_ms = 0;
    s_connect_pending = false;
    s_thinking_since_ms = 0;
    s_speaking_since_ms = 0;
}

void on_state_changed(State st) {
    State old = s_cur_state;
    s_cur_state = st;
    if (st == State::Listening && old != st) s_listening_since_ms = now_ms();
    if (st != State::Listening) s_listening_since_ms = 0;
    xiaozhi::ui_set_state(st);
    if (!s_retry_reason.empty() &&
        (st == State::Disconnected || st == State::Error)) {
        xiaozhi::ui_set_status_text(s_retry_reason);
    }

    if (st == State::Thinking) {
        s_thinking_since_ms = now_ms();
        s_speaking_since_ms = 0;
        // The server VAD in auto mode has finished sentence segmentation. First wait for the recording worker to fully exit, ensuring
        // later TTS does not run in parallel with the mic, and the C3 need not bear the memory cost of AEC/full duplex.
        xiaozhi::AudioService::GetInstance().StopRecording();
    } else if (st == State::Speaking) {
        s_thinking_since_ms = 0;
        s_speaking_since_ms = now_ms();
        s_playback_progress = xiaozhi::AudioService::GetInstance().PlaybackProgress();
        // Start playback as soon as we enter "Speaking". Without this step, audio the cloud has already pushed would only pile up in the
        // ring buffer until the tick fallback notices -- showing up as dropped frames at the start + a reply delay of about 1 second.
        if (!xiaozhi::AudioService::GetInstance().IsPlaying()) {
            if (!s_session_ready || !xiaozhi::AudioService::GetInstance().StartPlayback()) {
                s_session_ready = false;
                s_audio_recovery_required = true;
                s_conversation_active = false;
                proto().Stop();
                s_retry_reason = "Audio start failed, auto retry";
                schedule_reconnect(kReconnectDelayMs);
            }
        }
    } else {
        s_thinking_since_ms = 0;
        s_speaking_since_ms = 0;
    }

    // Leaving "Speaking": play out the remaining audio in the ring buffer before wrapping up (the tail is not cut off)
    if (old == State::Speaking && st != State::Speaking) {
        xiaozhi::AudioService::GetInstance().StopPlayback();
        // Conversation mode: start the next round of recording only after the previous reply has fully played and the speaker is muted.
        // Executed through the command queue, avoiding recursive audio lifecycle switches on the protocol event handling stack.
        if (st == State::Idle && s_conversation_active && s_session_ready &&
            proto().HandshakeDone() && proto().IsConnected()) {
            post_cmd(Cmd::StartListen);
        }
    }
}

// The OTA HTTPS request runs a full mbedTLS handshake -- the most stack-hungry call chain in this firmware
// (measured on device: a 6144-byte stack is not enough and overflows and reboots). But it only happens when "fetching config", and the stack should
// be returned as soon as it finishes. So it runs in a delete-after-use temporary task, keeping the control task itself on a small stack.
struct OtaJob {
    xiaozhi::OtaOutcome out;
    std::atomic<unsigned> refs{2};  // controller + worker
    std::atomic<bool> done{false};
};

OtaJob *s_ota_job = nullptr;  // only controller owns this pointer
std::atomic<bool> s_ota_worker_running{false};

void release_ota_job(OtaJob *job) {
    if (job->refs.fetch_sub(1) == 1) delete job;
}

void ota_worker(void *arg) {
    auto *job = static_cast<OtaJob *>(arg);
    job->out = xiaozhi::ota_fetch_config();
    job->done.store(true);  // publishes out before controller copies it
    release_ota_job(job);
    s_ota_worker_running.store(false);
    vTaskDelete(nullptr);
}

// Returns false if no result was obtained this time (including out of memory/timeout).
bool fetch_ota_config(xiaozhi::OtaOutcome *out) {
    // Keep a timed-out worker's owner until completion. No forced task deletion,
    // leaked semaphore, concurrent use of OTA's static response, or permanent
    // failure latch. A late result is consumed on the next retry/app opening.
    const bool fresh = s_ota_job == nullptr;
    if (fresh) {
        if (s_ota_worker_running.load()) {
            out->detail = "previous OTA request is finishing";
            return false;
        }
        s_ota_job = new (std::nothrow) OtaJob{};
        if (s_ota_job == nullptr) {
            out->detail = "no memory for OTA job";
            return false;
        }
        s_ota_worker_running.store(true);
        if (xTaskCreate(ota_worker, "xzg_ota", 12288, s_ota_job, 4, nullptr) != pdPASS) {
            s_ota_worker_running.store(false);
            delete s_ota_job;
            s_ota_job = nullptr;
            out->detail = "task create failed";
            return false;
        }
    }
    const int64_t started = now_ms();
    while (!s_ota_job->done.load()) {
        if (!fresh || s_stop_requested || now_ms() - started >= 20000) {
            out->detail = "OTA request still running; will retry after completion";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    *out = s_ota_job->out;
    release_ota_job(s_ota_job);
    s_ota_job = nullptr;
    // Force a yield to the FreeRTOS Idle task: the 12KB stack and TCB of ota_worker are reclaimed by the Idle task after it self-deletes.
    // control_task has priority 5, so it must delay briefly to let Idle run, reclaim and merge heap blocks,
    // avoiding a not-enough-memory failure when later audio init requests a 22KB stack because the old stack has not been freed yet.
    vTaskDelay(pdMS_TO_TICKS(60));
    return true;
}

void do_connect() {
    if (proto().NeedsRestart() && proto().HasClient()) {
        s_conversation_active = false;
        s_session_ready = false;
        clear_timers();
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        // Only the controller may join/destroy the websocket worker.
        proto().Stop();
    }
    if (proto().IsConnected()) return;

    // If the client object is still there -> esp_websocket_client is reconnecting on its own reconnect_timeout_ms;
    // do not tear it down and rebuild it (the two sides would interrupt each other and recovery would be slower). Rebuild only after several consecutive failures.
    if (proto().HasClient() && s_connect_failures < kOtaRetryAfterFailures) {
        ESP_LOGI(TAG, "WebSocket client still present, letting it auto-reconnect");
        s_next_connect_attempt_ms = now_ms() + 2000;
        return;
    }

    if (!WifiManager::GetInstance().IsConnected()) {
        xiaozhi::ui_set_state(State::Disconnected);
        xiaozhi::ui_set_status_text("Network lost");
        s_next_connect_attempt_ms = now_ms() + kWifiRetryMs;
        return;
    }

    // Rebuilding after repeated failures must stop the old auto-reconnecting
    // TLS client before running another HTTPS handshake in this small heap.
    if (proto().HasClient()) proto().Stop();
    if (s_audio_recovery_required && s_audio_ready) {
        if (!xiaozhi::AudioService::GetInstance().Deinit()) {
            schedule_reconnect(kReconnectDelayMs);
            return;
        }
        s_audio_ready = false;
        s_audio_recovery_required = false;
    }
    s_retry_reason.clear();

    const int64_t t = now_ms();
    const bool cfg_stale = !s_cfg_valid || (t - s_cfg_fetched_ms > kOtaRefreshMs) ||
                           (s_connect_failures >= kOtaRetryAfterFailures);

    // 1. If needed, first exchange for the websocket config with the backend. The official backend requires this step:
    //    it then returns websocket{url,token,version}, or, if the device is not bound,
    //    activation{code,message} asking the user to bind first.
    if (cfg_stale) {
        xiaozhi::ui_set_status_text("Preparing connection");
        xiaozhi::OtaOutcome ota;
        if (!fetch_ota_config(&ota)) {
            s_retry_reason = "Cannot reach server";
            xiaozhi::ui_set_status_text(s_retry_reason);
            schedule_reconnect(kReconnectDelayMs);
            return;
        }

        if (ota.result == OtaResult::NeedsActivation) {
            s_activation_pending = true;
            s_activation_code = ota.activation_code;
            if (!s_activation_code.empty()) {
                ESP_LOGW(TAG, "Activation required, code=%s", s_activation_code.c_str());
                // The verification code must be fully visible in the message area (the status capsule is too narrow and would truncate it),
                // and give the real binding path for ordinary users -- direct MAC binding is a developer back door that users do not have.
                xiaozhi::ui_set_status_text("Activate " + s_activation_code);
                xiaozhi::ui_set_ai_text(
                    "Device not bound yet. On your phone open xiaozhi.me, log in, tap Add Device, "
                    "and enter code " + s_activation_code +
                    ". It connects automatically within about 30 s after binding.");
                xiaozhi::ui_set_hint("Auto-connects once bound");
            } else {
                ESP_LOGW(TAG, "Activation required (no code returned)");
                xiaozhi::ui_set_status_text("Waiting for binding");
                xiaozhi::ui_set_ai_text(ota.activation_message.empty()
                                            ? "Device not activated yet. Open xiaozhi.me to add this device."
                                            : ota.activation_message);
            }
            s_next_connect_attempt_ms = now_ms() + kActivationRepollMs;
            return;
        }
        s_activation_pending = false;

        if (ota.result == OtaResult::Ok) {
            s_cfg_url = ota.url;
            s_cfg_token = ota.token;
            s_cfg_version = (ota.version > 0) ? ota.version : 3;
            s_cfg_valid = true;
            s_cfg_fetched_ms = t;
            s_connect_failures = 0;
            ESP_LOGI(TAG, "OTA config accepted: url=%s version=%d token=%s",
                     s_cfg_url.c_str(), s_cfg_version, s_cfg_token.empty() ? "(none)" : "(set)");
        } else {
            ESP_LOGW(TAG, "OTA gave no websocket config (%s); falling back to NVS/defaults",
                     ota.detail.c_str());
        }
    }

    // 2. Decide the final connection parameters: OTA/cache > this app's NVS > official same-named NVS > built-in defaults
    std::string url = s_cfg_url;
    std::string token = s_cfg_token;
    int version = s_cfg_version;

    if (url.empty()) url = store_get_string(kNsXiaozhi, "ws_url", "");
    if (url.empty()) url = store_get_string(kNsWebsocket, "url", "");
    if (url.empty()) url = "wss://api.tenclass.net/xiaozhi/v1/";

    if (token.empty()) token = store_get_string(kNsXiaozhi, "token", "");
    if (token.empty()) token = store_get_string(kNsWebsocket, "token", "");
    // Note: no longer fall back to the literal "test-token". With no token, simply send no Authorization header,
    // letting the server treat it as anonymous/not activated and telling the user clearly on screen, instead of failing silently with a fake token.

    if (version <= 0) version = store_get_int(kNsXiaozhi, "version", 0);
    if (version <= 0) version = store_get_int(kNsWebsocket, "version", 0);
    if (version <= 0) version = 3;

    ESP_LOGI(TAG, "Connecting: url=%s version=%d token=%s (free=%u largest=%u)",
             url.c_str(), version, token.empty() ? "(none)" : "(set)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // The audio subsystem is initialized only after "the 12KB stack of the OTA temporary task has been returned":
    // the resident recording task needs a 16KB contiguous stack, and the earlier it is requested while the heap is more intact, the more reliable it is.
    if (!s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Init() != ESP_OK) {
            ESP_LOGE(TAG, "Audio init failed; Xiaozhi stays unusable");
            s_retry_reason = "Audio start failed, auto retry";
            xiaozhi::ui_set_status_text(s_retry_reason);
            schedule_reconnect(kReconnectDelayMs);
            return;
        }
        s_audio_ready = true;
    }

    xiaozhi::ui_set_status_text("Connecting");
    if (!proto().Start(url, token, version)) {
        s_retry_reason = "Cannot reach server";
        xiaozhi::ui_set_status_text(s_retry_reason);
        s_connect_failures++;
        schedule_reconnect(kReconnectDelayMs);
        return;
    }
    s_connect_pending = true;
    s_connect_started_ms = now_ms();
}

void do_start_listen() {
    if (proto().NeedsRestart() || !proto().IsConnected()) {
        post_cmd(Cmd::Connect);
        return;
    }
    if (!s_session_ready || !proto().HandshakeDone()) {
        xiaozhi::ui_set_status_text("Getting ready, please wait");
        return;
    }
    // Judge by the protocol layer's live state, not this task's cached s_cur_state (which may lag one loop)
    const State st = proto().GetState();
    if (st == State::Listening || st == State::Thinking) return;

    xiaozhi::ui_clear_text();

    if (!proto().TriggerStartListen()) {
        xiaozhi::ui_set_status_text("Send failed, retry");
        return;
    }
    s_conversation_active = true;
    s_listening_since_ms = now_ms();

    // The recording task only captures + encodes; frames are sent by this task (drain_audio_tx)
    if (!xiaozhi::AudioService::GetInstance().StartRecording()) {
        // The server has already received listen/start, so an abort must be sent to close it out, otherwise the session dangles
        ESP_LOGE(TAG, "StartRecording failed; aborting session");
        s_conversation_active = false;
        s_session_ready = false;
        s_audio_recovery_required = true;
        proto().TriggerAbort();
        proto().Stop();
        s_retry_reason = "Mic start failed, auto retry";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
    }
}

// Send out the Opus frames the recording task has encoded; returns the number of frames sent this time.
// Called only from the control task -- so the mbedTLS send stack usage lands on the 10KB control task,
// rather than pushing the recording task to ~14KB (on device 10KB overflows the stack inside ssl_write and reboots).
unsigned s_tx_frames = 0;

int drain_audio_tx() {
    static uint8_t frame[xiaozhi::kOpusFrameMaxBytes];
    size_t len = 0;
    int sent = 0;
    while (xiaozhi::AudioService::GetInstance().PopEncodedFrame(frame, sizeof(frame), &len)) {
        if (!proto().SendAudio(frame, len)) {
            ESP_LOGW(TAG, "SendAudio failed, dropping frame (%u bytes)", (unsigned)len);
            break;
        }
        s_tx_frames++;
        if (s_tx_frames % 50 == 0) {
            ESP_LOGI(TAG, "Sent %u audio frames so far (%u bytes last)", (unsigned)s_tx_frames,
                     (unsigned)len);
        }
        if (++sent >= 8) break;   // send at most 8 frames per pass, to avoid holding the control loop for long
    }
    return sent;
}

void do_stop_listen() {
    if (s_cur_state != State::Listening) return;
    // In auto conversation mode, OK means ending the whole conversation, not submitting the current sentence (sentence segmentation is done by the server
    // VAD). First fully stop recording, then abort, to avoid still uploading sound after exit.
    s_conversation_active = false;
    xiaozhi::AudioService::GetInstance().StopRecording();
    proto().TriggerAbort();
}

void do_abort() {
    // Cut the sound first, then notify the server: it feels like "instantly quiet"
    s_conversation_active = false;
    xiaozhi::AudioService::GetInstance().AbortPlayback();
    if (proto().IsConnected()) {
        proto().TriggerAbort();
    } else {
        on_state_changed(State::Idle);
    }
}

void do_stop_app() {
    ESP_LOGI(TAG, "Stopping Xiaozhi app: free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_conversation_active = false;
    s_listen_after_connect = false;
    s_session_ready = false;
    // The worker owns a second reference and completes without this task. Keep
    // a global running flag so the next app cannot reuse OTA's response buffer.
    if (s_ota_job != nullptr) {
        release_ota_job(s_ota_job);
        s_ota_job = nullptr;
    }
    xiaozhi::AudioService::GetInstance().StopRecording();
    xiaozhi::AudioService::GetInstance().AbortPlayback();
    if (s_protocol_ready) {
        proto().Stop();
    }
    s_protocol_ready = false;
    // Reconnects within this app retain the cache; leaving returns all strings
    // allocated during OTA/TLS so they cannot split the next app's codec heap.
    s_cfg_valid = false;
    std::string{}.swap(s_cfg_url);
    std::string{}.swap(s_cfg_token);
    std::string{}.swap(s_activation_code);
    std::string{}.swap(s_retry_reason);
    // Return the resident recording task and PCM buffer to the heap together: after switching to another app Xiaozhi should not hold memory
    if (s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Deinit()) {
            s_audio_ready = false;
        } else {
            ESP_LOGE(TAG, "Audio teardown incomplete; retained resources for safety");
        }
    }
    // The registry keeps the last UI alive until its safe screen handoff.
    clear_timers();
    s_activation_pending = false;

    ESP_LOGI(TAG, "Xiaozhi app stopped cleanly. free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void set_volume(int delta) {
    int vol = (int)bsp_audio_get_master_volume() + delta;
    if (vol > 100) vol = 100;
    if (vol < 0) vol = 0;
    settings_set_volume((uint8_t)vol);
    xiaozhi::ui_set_volume((unsigned)vol);
    ESP_LOGI(TAG, "Volume -> %d%%", vol);
}

void handle_proto_event(const Event &ev) {
    switch (ev.type) {
    case EvType::Connected:
        if (proto().HandshakeDone()) break;  // stale connected before queued hello
        s_connect_pending = true;
        s_connect_started_ms = now_ms();
        ESP_LOGI(TAG, "Socket connected, waiting for server hello...");
        xiaozhi::ui_set_status_text("Establishing connection");
        break;

    case EvType::HelloOk:
        if (!proto().HandshakeDone() || !proto().IsConnected()) break;
        s_connect_pending = false;
        s_reconnect_at_ms = 0;
        s_connect_failures = 0;
        // Playback sample rate follows the server (official measured 24000 sent down; this device defaults to 16000)
        if (proto().ServerSampleRate() > 0) {
            xiaozhi::AudioService::GetInstance().SetPlaybackSampleRate(proto().ServerSampleRate());
        }
        if (!xiaozhi::AudioService::GetInstance().PrepareSessionResources()) {
            ESP_LOGE(TAG, "Server hello succeeded but post-hello audio reservation failed");
            s_session_ready = false;
            s_audio_recovery_required = true;
            s_retry_reason = "Audio out of memory, auto retry";
            s_conversation_active = false;
            proto().Stop();
            schedule_reconnect(kReconnectDelayMs);
            xiaozhi::ui_set_status_text(s_retry_reason);
            break;
        }
        s_session_ready = true;
        s_reconnect_delay_ms = kReconnectDelayMs;
        s_retry_reason.clear();
        xiaozhi::ui_set_status_text("Ready");
        if (s_listen_after_connect) {
            s_listen_after_connect = false;
            do_start_listen();
        }
        break;

    case EvType::StateChanged:
        on_state_changed(ev.state);
        break;

    case EvType::Disconnected:
        s_retry_reason = "Network lost";
        xiaozhi::ui_set_status_text(s_retry_reason);
        if (s_connect_pending) s_connect_failures++;
        s_connect_pending = false;
        clear_timers();
        s_conversation_active = false;
        s_session_ready = false;
        // On disconnect, clean up the audio fully, so no leftover task holds the codec
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        // Playback resources stay resident within one websocket session, but the TLS handshake on reconnect
        // needs this short-term contiguous memory. Release only at the disconnect boundary; multi-turn conversation is unaffected.
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        schedule_reconnect(kReconnectDelayMs);
        break;

    case EvType::SocketError:
        s_connect_failures++;
        s_connect_pending = false;
        clear_timers();
        s_conversation_active = false;
        s_session_ready = false;
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        s_retry_reason = "Cannot reach server";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
        break;

    case EvType::UserText:
        xiaozhi::ui_set_user_text(ev.text);
        break;

    case EvType::AiSentence:
        xiaozhi::ui_set_ai_text(ev.text);
        break;

    case EvType::LlmEmotion:
        break;

    case EvType::Alert:
        ESP_LOGW(TAG, "Server alert: %s", ev.text);
        xiaozhi::ui_set_status_text(ev.text);
        break;

    case EvType::SessionInfo:
    case EvType::HandshakeTimeout:
        break;
    }
}

void handle_cmd(Cmd c) {
    switch (c) {
    case Cmd::Connect:
        s_reconnect_at_ms = 0;
        do_connect();
        break;
    case Cmd::StartListen: do_start_listen(); break;
    case Cmd::StopListen:  do_stop_listen();  break;
    case Cmd::Abort:       do_abort();        break;
    case Cmd::VolUp:       set_volume(+10);   break;
    case Cmd::VolDown:     set_volume(-10);   break;
    case Cmd::ToggleListen:
        // The input task never dereferences a client that the controller may
        // concurrently stop/destroy. Resolve the button against live state here.
        if (proto().NeedsRestart() || !proto().IsConnected() ||
            !proto().HandshakeDone() || !s_session_ready) {
            s_listen_after_connect = true;
            do_connect();
        }
        else if (proto().GetState() == State::Speaking) do_abort();
        else if (proto().GetState() == State::Listening) do_stop_listen();
        else if (proto().GetState() != State::Thinking) do_start_listen();
        break;
    case Cmd::Stop:        do_stop_app();     break;
    }
}

void tick() {
    const int64_t t = now_ms();

    // 1. Wi-Fi connected / reconnect time reached -> start connecting
    if (!proto().IsConnected() && s_reconnect_at_ms != 0 && t >= s_reconnect_at_ms) {
        s_reconnect_at_ms = 0;
        post_cmd(Cmd::Connect);
    }
    if (s_next_connect_attempt_ms != 0 && t >= s_next_connect_attempt_ms) {
        s_next_connect_attempt_ms = 0;
        post_cmd(Cmd::Connect);
    }

    // 2. Handshake timeout: connected but the server never replies with hello
    if (s_connect_pending && t - s_connect_started_ms > kConnectTimeoutMs) {
        ESP_LOGW(TAG, "Server hello timeout, retrying");
        s_connect_pending = false;
        s_connect_failures++;
        proto().Stop();
        s_retry_reason = "Server not responding";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
    }

    if (proto().GetState() == State::Listening && s_listening_since_ms != 0 &&
        t - s_listening_since_ms >= kListeningTimeoutMs) {
        s_listening_since_ms = 0;
        s_conversation_active = false;
        xiaozhi::AudioService::GetInstance().StopRecording();
        proto().TriggerAbort();
        xiaozhi::ui_set_ai_text("Recording stopped. Press OK to talk again.");
    }

    // 3. No response in Thinking -> actively abort, so the UI does not hang forever on "Thinking" with OK doing nothing
    if (proto().GetState() == State::Thinking && s_thinking_since_ms != 0 &&
        t - s_thinking_since_ms > kThinkingTimeoutMs) {
        ESP_LOGW(TAG, "Thinking timeout, aborting this turn");
        s_thinking_since_ms = 0;
        s_conversation_active = false;
        proto().TriggerAbort();
        xiaozhi::ui_set_ai_text("No reply received in time. Please talk again.");
    }

    // Continuous stories/music may last arbitrarily long. Only lack of both
    // accepted network audio and successful PCM writes counts as a stall.
    if (proto().GetState() == State::Speaking) {
        const uint32_t progress = xiaozhi::AudioService::GetInstance().PlaybackProgress();
        if (progress != s_playback_progress) {
            s_playback_progress = progress;
            s_speaking_since_ms = t;
        }
        if (s_speaking_since_ms != 0 && t - s_speaking_since_ms > kSpeakingStallTimeoutMs) {
            ESP_LOGW(TAG, "Playback stalled without audio/PCM progress; aborting");
            do_abort();
        }
    }

    // 5. Fallback: cloud audio is already in the ring buffer but the playback task has not started (e.g. the server pushed audio
    //    directly without sending tts/start). Normally tts start arrives first; this is only insurance.
    if (s_session_ready && proto().GetState() == State::Speaking &&
        !xiaozhi::AudioService::GetInstance().IsPlaying() &&
        xiaozhi::AudioService::GetInstance().HasPendingPlayback()) {
        ESP_LOGI(TAG, "Audio queued without playback task; starting playback");
        xiaozhi::AudioService::GetInstance().StartPlayback();
    }
}

void control_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Control task started (stack=%u, free=%u largest=%u)",
             (unsigned)kControlTaskStack, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // The audio subsystem is not initialized here: wait until the OTA temporary task has returned its 12KB stack and the heap is most intact,
    // then create the resident recording task (see do_connect).
    proto().SetEventQueue(s_proto_q);
    s_protocol_ready = true;   // only after this may the key task access the protocol singleton
    xiaozhi::ui_init();
    xiaozhi::ui_show();

    post_cmd(Cmd::Connect);

    Event pev;
    Cmd cmd;

    while (true) {
        bool did_work = false;

        while (proto().PopCriticalEvent(&pev)) {
            handle_proto_event(pev);
            did_work = true;
        }
        if (proto().GetState() != s_cur_state) {
            on_state_changed(proto().GetState());
            did_work = true;
        }

        while (xQueueReceive(s_proto_q, &pev, 0) == pdTRUE) {
            handle_proto_event(pev);
            did_work = true;
        }

        bool stop_requested = false;
        // A4 fallback: if the Stop command was dropped because the queue was full, the flag is still set, and the exit is made up here.
        if (s_stop_requested) stop_requested = true;
        while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            if (cmd == Cmd::Stop) {
                stop_requested = true;
                break;
            }
            handle_cmd(cmd);
            did_work = true;
        }

        if (stop_requested) {
            do_stop_app();
            break;
        }

        // Sending of recorded frames is done here (not in the recording task), see the note on drain_audio_tx
        if (drain_audio_tx() > 0) did_work = true;

        tick();

        // If there is work, loop again immediately; when idle, tick every 20ms (imperceptible for key response)
        if (!did_work) vTaskDelay(pdMS_TO_TICKS(kLoopTickMs));
    }

    s_app_active = false;
    s_ctl_task = nullptr;
    if (s_stopped_sem) xSemaphoreGive(s_stopped_sem);
    ESP_LOGI(TAG, "Control task exiting");
    s_control_running.store(false);
    vTaskDelete(NULL);
}

} // namespace

// ---------------------------------------------------------------------------
// passport_app_t interface
// ---------------------------------------------------------------------------

void xiaozhi_app_init(void) {
    // Heavy initialization (audio buffers/codec) is done inside the control task, see control_task().
    ESP_LOGI(TAG, "Xiaozhi app init (deferred to control task)");
}

void xiaozhi_app_start(void) {
    s_start_failed = false;
    // Fixed 2026-09-13 (A4): the old logic only checked s_app_active. If the previous stop() failed to collect the
    // control task (Stop dropped / stuck in connecting), a second xzg_ctl would be created here -- two tasks
    // operating proto() and the audio service at once, which would certainly hit the use-after-free recorded in the file header.
    // Now: as long as the old task handle exists, refuse to start outright.
    if (s_ctl_task != nullptr) {
        ESP_LOGE(TAG, "Previous control task still alive; refusing to start a second one");
        return;
    }
    if (s_app_active) {
        ESP_LOGW(TAG, "Xiaozhi app already active");
        return;
    }

    // A previous bounded stop may have retained a slow worker to avoid UAF.
    // Give it one more cleanup opportunity before creating a new owner.
    if (s_audio_ready) {
        if (!xiaozhi::AudioService::GetInstance().Deinit()) {
            ESP_LOGE(TAG, "Previous audio worker is still alive; refusing a second owner");
            return;
        }
        s_audio_ready = false;
    }

    if (s_proto_q == nullptr) s_proto_q = xQueueCreateStatic(kProtoQueueDepth, sizeof(Event), s_proto_items, &s_proto_queue_storage);
    if (s_cmd_q == nullptr) s_cmd_q = xQueueCreateStatic(kCmdQueueDepth, sizeof(Cmd), s_cmd_items, &s_cmd_queue_storage);
    if (s_stopped_sem == nullptr) s_stopped_sem = xSemaphoreCreateBinaryStatic(&s_stopped_storage);

    if (s_proto_q == nullptr || s_cmd_q == nullptr || s_stopped_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create queues/semaphore (free=%u)",
                 (unsigned)esp_get_free_heap_size());
        s_start_failed = true;
        xiaozhi::ui_init();
        xiaozhi::ui_show();
        xiaozhi::ui_set_status_text("Start failed, retry");
        xiaozhi::ui_set_ai_text("Out of memory for now. OK to retry, or hold OK to go back.");
        return;
    }

    // Clear any leftover events/commands
    xQueueReset(s_proto_q);
    xQueueReset(s_cmd_q);
    while (xSemaphoreTake(s_stopped_sem, 0) == pdTRUE) { }

    s_app_active = true;
    s_stop_requested = false;
    s_cur_state = State::Disconnected;
    s_listen_after_connect = false;
    clear_timers();
    s_reconnect_at_ms = 0;
    s_next_connect_attempt_ms = 0;
    s_activation_pending = false;
    s_conversation_active = false;
    s_session_ready = false;
    s_audio_recovery_required = false;
    s_retry_reason.clear();
    s_connect_failures = 0;

    ESP_LOGI(TAG, "Starting Xiaozhi app...");
    s_control_running.store(true);
    BaseType_t ok = xTaskCreate(control_task, "xzg_ctl", kControlTaskStack, nullptr, 5, &s_ctl_task);
    if (ok != pdPASS) {
        s_control_running.store(false);
        ESP_LOGE(TAG, "Failed to create control task (free=%u largest=%u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_app_active = false;
        s_start_failed = true;
        xiaozhi::ui_init();
        xiaozhi::ui_show();
        xiaozhi::ui_set_status_text("Start failed, retry");
        xiaozhi::ui_set_ai_text("Out of memory for now. OK to retry, or hold OK to go back.");
        return;
    }
}

bool xiaozhi_app_prepare_stop(void) {
    if (s_app_active.exchange(false)) post_cmd(Cmd::Stop);
    if (!s_control_running.load() && s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Deinit()) s_audio_ready = false;
    }
    // OTA owns its own stack and HTTPS client. It must also finish before a
    // different app allocates its audio/TLS working set.
    return !s_control_running.load() && !s_ota_worker_running.load() && !s_audio_ready;
}

void xiaozhi_app_stop(void) {
    ESP_LOGI(TAG, "Requesting Xiaozhi app stop");
    s_app_active = false;

    if (s_ctl_task == nullptr) {
        // The control task has already exited: still clean up the protocol stack and audio here, to guarantee idempotence
        do_stop_app();
        xiaozhi::ui_hide();
        return;
    }

    // Ask the control task to wrap up, and wait until it really ends. Never vTaskDelete a running task
    // (the old implementation would delete the wifi task that might be stuck in connect_xiaozhi(), a typical case of
    //  "deleting a task while it holds the heap lock / is in an intermediate state" -> heap corruption -> random reboots).
    post_cmd(Cmd::Stop);
    if (xSemaphoreTake(s_stopped_sem, pdMS_TO_TICKS(6000)) != pdTRUE) {
        ESP_LOGE(TAG, "Control task did not stop in time; leaving it to self-exit");
    } else {
        xiaozhi::ui_hide();
    }
}

void xiaozhi_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (s_start_failed && btn == BSP_BTN_OK && event == BTN_EVT_CLICK) {
        xiaozhi_app_start();
        return;
    }
    if (!s_app_active) return;
    // Do not touch the protocol singleton before it is fully constructed: the key task has only a 3072-byte stack, while constructing Protocol
    // goes through the NVS write (SPI-Flash erase/write) call chain, which is very deep.
    if (!s_protocol_ready) return;
    if (event != BTN_EVT_CLICK) return;

    if (btn == BSP_BTN_OK) {
        post_cmd(Cmd::ToggleListen);
    } else if (btn == BSP_BTN_UP) {
        post_cmd(Cmd::VolUp);
    } else if (btn == BSP_BTN_DOWN) {
        post_cmd(Cmd::VolDown);
    }
}

const passport_app_t g_xiaozhi_app = {
    .id = APP_ID_XIAOZHI,
    .name = "Xiaozhi AI",
    .en_name = "XIAOZHI",
    .desc = "Continuous voice chat with Xiaozhi\nNeeds internet, auto sentence break",
    .tag = "Direct",
    .theme_color = 0x38BDF8,
    .init = xiaozhi_app_init,
    .start = xiaozhi_app_start,
    .stop = xiaozhi_app_stop,
    .on_key = xiaozhi_app_on_key,
    .prepare_stop = xiaozhi_app_prepare_stop,
};
