// main/app_types.h -- plain C shared types: state machine / events / actions / UI snapshot.
// Includes no ESP-IDF headers, so it can be compiled and tested directly on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- Key actions (downlink injection, executed by the PC client) ----------------
typedef enum {
    APP_KEY_ENTER = 0,   // Enter in the input box (submit)
    APP_KEY_CLEAR,       // clear all text in the input box
} app_key_action_t;

// ---------------- State ----------------
// Note: named app_stage_t rather than app_state_t -- the latter is the full state struct in app_state.h.
typedef enum {
    APP_ST_HOME = 0,        // idle home page
    APP_ST_READY,           // workflow ready (PTT possible)
    APP_ST_LISTENING,       // recording (PTT held)
    APP_ST_TRANSCRIBING,    // legacy name: speech done, waiting for DOWN=Enter to send
    APP_ST_AGENT_RUNNING,   // Agent running
    APP_ST_APPROVAL,        // awaiting physical approval
    APP_ST_COUNT,
} app_stage_t;

// ---------------- Keys ----------------
// Physical key semantics (2026-08-29): UP (volume up) = record on press, send on release (hold to talk);
// DOWN (volume down) = click for Enter / long press 0.5s clears the input box; OK = click navigates / approves /
// long press 0.5s locks/unlocks the screen. Each key carries only "click + long press", no double click (see app_state.c).
typedef enum {
    APP_BTN_UP = 0,
    APP_BTN_DOWN,
    APP_BTN_OK,
} app_btn_t;

// ---------------- Tones ----------------
typedef enum {
    APP_TONE_START = 0,     // 440Hz/80ms  recording ready
    APP_TONE_SEND,          // 880Hz/60ms  voice sent
    APP_TONE_APPROVAL,      // 587+880/150ms approval alert
    APP_TONE_SUCCESS,       // 660->880/200ms done
    APP_TONE_REJECT,        // 220Hz/120ms rejected
    APP_TONE_ERROR,         // 180Hz/100ms offline / invalid action
    APP_TONE_COUNT,
} app_tone_t;

// ---------------- Events ----------------
typedef enum {
    APP_EV_KEY_PRESS = 0,
    APP_EV_KEY_RELEASE,
    APP_EV_KEY_CLICK,
    APP_EV_KEY_DOUBLE,
    APP_EV_KEY_LONG,        // instant the long press reaches the threshold (UP key 0.5s -> speech starts)
    APP_EV_KEY_LONG_UP,     // released after a long press timed out (long-press state no longer reports RELEASE; UP key -> speech ends)
    APP_EV_AGENT_STATUS,
    APP_EV_APPROVAL_REQUEST,
    APP_EV_TRANSCRIPT,
    APP_EV_AUDIO_DROP_START,
    APP_EV_AUDIO_DROP_END,
    // 2026-09-13 rename (D2): the channel moved from BLE to Wi-Fi UDP but the event names still said BLE,
    // so readers would think Bluetooth is still in use. Changed to LINK_* meaning "link", independent of the channel.
    APP_EV_LINK_UP,         // link up: peer is ready (sufficient condition for PTT)
    APP_EV_LINK_DOWN,       // link down (connection dropped or peer lost)
    APP_EV_LINK_DROP,       // an event line was dropped due to disconnect / not ready (UI toast feedback)
    APP_EV_AUDIO_ERROR,     // capture hardware error
    APP_EV_INIT_FAILED,     // Local transport/pipeline startup failed; re-enter to retry
    APP_EV_TONE_DONE,       // START tone finished playing (signal for async stream open, see design)
    APP_EV_TICK,            // 100ms heartbeat, drives timeout / screen-off
    // ---- USB wired channel (USB-Serial-JTAG, see design.md) ----
    APP_EV_USB_CONNECTED,   // USB session up (PC handshake ping received; the USB-side sufficient condition for link_up)
    APP_EV_USB_DISCONNECTED, // USB session down (cable unplugged: is_connected flipped)
    APP_EV_TIME_SET,         // time sync downlink (epoch seconds UTC; shared by both channels: CTRL time.set line + SYS time set)
    APP_EV_PC_STATUS,        // Hotkey forwarder capability heartbeat (not ASR status)
} app_event_type_t;

// ---------------- Link channels (dual-channel always-on architecture, 2026-08-28) ----------------
// The old exclusive mode (BLE=0/USB=2, NVS-persisted + reboot to switch) is fully retired: BLE advertising stays on,
// the USB data channel works on plug-in (usb_link on standby, auto-attaches when a host appears). Session routing follows
// link_channel: the most recently connected/used channel in the state machine, with audio and voice.* uplink following it.
typedef enum {
    APP_CHAN_BLE = 0,       // BLE (wireless): up once the EVENT characteristic is subscribed
    APP_CHAN_USB = 2,       // USB (wired): up once the PC ping handshake completes
    APP_CHAN_NONE = 0xFF,   // both channels down (link_up=false)
} app_chan_t;

// Agent state (conversion to/from protocol strings is in app_protocol.c)
typedef enum {
    APP_AGENT_READY = 0,
    APP_AGENT_THINKING,
    APP_AGENT_RUNNING,
    APP_AGENT_ERROR,
    APP_AGENT_DONE,
    APP_AGENT_COUNT,
} app_agent_state_t;

// Injection mode (kept for protocol compatibility: injected on the Mac side, the device no longer consumes it; UI shows the transcript)
typedef enum {
    APP_INJECT_TYPE = 0,    // old: HID per-character typing (retired, field kept only)
    APP_INJECT_PASTE,       // old: paste Cmd+V (retired, field kept only)
} app_inject_mode_t;

// Approval risk level
typedef enum {
    APP_RISK_LOW = 0,
    APP_RISK_MEDIUM,
    APP_RISK_HIGH,
    APP_RISK_COUNT,
} app_risk_t;

// Approval action
typedef enum {
    APP_ACTION_APPROVE = 0,
    APP_ACTION_REJECT,
    APP_ACTION_DETAILS,
} app_approval_decision_t;

// Text length limits (sent per item by the Mac side, the device only displays)
#define APP_TRANSCRIPT_MAX    128
#define APP_TASK_ID_MAX       32
#define APP_TITLE_MAX         64
#define APP_TARGET_MAX        64
#define APP_DIFF_MAX          64
// Display channel: must be >= APP_TRANSCRIPT_MAX, so transcript lines the relay split at 128B land on screen whole
#define APP_AGENT_MSG_MAX     APP_TRANSCRIPT_MAX
#define APP_TOAST_MAX         64

// Event (fixed-size struct, copied into the queue, union kept small)
typedef struct {
    app_event_type_t type;
    union {
        // mv = bsp_button_read_mv() at callback time (mV): a real press reads 3-5, a false press (RF
        // corruption) reads 2890 = the released level -- app_state uses it to tell real from false LONG (see READY state)
        struct { uint8_t btn; uint16_t mv; } key;       // KEY_*
        struct {
            uint8_t state;                              // app_agent_state_t
            char message[APP_AGENT_MSG_MAX];
        } agent_status;                                 // AGENT_STATUS
        struct {
            char task_id[APP_TASK_ID_MAX];
            char title[APP_TITLE_MAX];
            char target[APP_TARGET_MAX];
            char diff_summary[APP_DIFF_MAX];
            uint8_t risk;                               // app_risk_t
        } approval;                                     // APPROVAL_REQUEST
        struct {
            char text[APP_TRANSCRIPT_MAX];
            uint8_t inject_mode;                        // app_inject_mode_t (kept for protocol compatibility)
            bool final;                                 // false = preview (not finalized); true = finalized
        } transcript;                                   // TRANSCRIPT
        struct { int64_t epoch; } time_set;             // TIME_SET (UTC seconds, int64 aligned to 8, union still <=228B)
        struct { bool audio_ready; bool hotkey_ready; } pc_status;
    } u;
} app_event_t;

// ---------------- Actions ----------------
typedef enum {
    APP_ACT_NONE = 0,
    APP_ACT_UI_REFRESH,
    APP_ACT_UI_SCREEN_OFF,
    APP_ACT_UI_SCREEN_ON,
    APP_ACT_UI_PANEL_OFF,   // panel SLPIN power-down (60s idle level)
    APP_ACT_UI_PANEL_ON,    // panel SLPOUT power-up (wake)
    APP_ACT_SEND_VOICE_START,
    APP_ACT_SEND_VOICE_END,
    APP_ACT_SEND_KEY_ACTION,   // uplink key action (enter/clear, injected by the PC client)
    APP_ACT_SEND_AGENT_ACTION,
    APP_ACT_STREAM_START,
    APP_ACT_STREAM_STOP,
    APP_ACT_STREAM_CANCEL,   // cancel / link lost: stop capture + clear ring + drop in-flight frames (unlike STOP: does not drain the send)
    APP_ACT_PLAY_TONE,
    APP_ACT_TIME_SET,        // time_sync_set_epoch (time sync lands)
} app_action_type_t;

// Maximum number of actions one event can produce. emit() silently drops when full, so this value must be >= the longest
// reduction path, otherwise the tail actions are lost -- 4 -> 6 (2026-08-29): pressing
// volume up to record while the screen is off / panel powered down is the longest path; after the wake triple (UI_PANEL_ON + UI_SCREEN_ON + UI_REFRESH)
// come PLAY_TONE + SEND_VOICE_START = 5 entries, and the 5th was exactly the one dropped -- the result was
// the beep played and capture started, yet the Mac side never received voice.start (the whole utterance never reached ASR).
// Equally long are "screen off + volume-down long press to clear" (wake triple + SEND_KEY_ACTION + PLAY_TONE)
// and offline PTT (wake triple + PLAY_TONE(ERROR) + UI_REFRESH). 6 is chosen to leave one entry of margin.
#define APP_ACT_MAX 6

typedef struct {
    app_action_type_t type;
    union {
        uint8_t tone;                                   // PLAY_TONE
        struct { uint8_t action; } key_action;          // SEND_KEY_ACTION(app_key_action_t)
        struct {
            char task_id[APP_TASK_ID_MAX];
            uint8_t decision;                           // app_approval_decision_t
        } agent_action;                                 // SEND_AGENT_ACTION
        struct { int64_t epoch; } time_set;             // TIME_SET
    } u;
} app_action_t;

// ---------------- UI snapshot ----------------
typedef struct {
    app_stage_t    state;
    bool           link_up;       // false -> OFFLINE (link_name DISCONNECTED) banner + PTT disabled; true = channel up
    bool           pc_ready;      // Fresh PC audio + hotkey readiness, separate from UDP discovery
    bool           init_failed;
    bool           screen_on;
    bool           panel_on;      // panel power (false after SLPIN power-down)
    bool           net_busy;      // audio frames being dropped -> BUSY banner
    bool           ble_connected; // whether the BLE connection is established (icon gray when not subscribed)
    char           link_name[8];  // current link channel name: "BLE"/"USB" (the disconnect banner renders by this)
    bool           battery_available;
    uint8_t        battery_soc;   // 0..100
    bool           wifi_connected;
    int8_t         wifi_rssi;     // Actual station signal, independent of PC discovery.
    char           agent_message[APP_AGENT_MSG_MAX];
    bool           transcript_final; // false -> the current agent_message is a transcript preview (not finalized, UI adds a cursor feel)
    char           agent_state_name[16];
    char           task_id[APP_TASK_ID_MAX];
    char           approval_title[APP_TITLE_MAX];
    char           approval_target[APP_TARGET_MAX];
    char           approval_diff[APP_DIFF_MAX];
    uint8_t        approval_risk;   // app_risk_t
    uint32_t       elapsed_ms;      // how long the current state has lasted (filled in by the main loop at snapshot time)
    char           toast[APP_TOAST_MAX];
} app_ui_snapshot_t;

// ---------------- Timeout constants ----------------
#define APP_IDLE_BACKLIGHT_OFF_MS   20000u   // no key -> backlight off (screen off, rendering skipped)
#define APP_IDLE_PANEL_OFF_MS       60000u   // no key -> panel SLPIN power-down (uA level)
#define APP_TRANSCRIBE_TIMEOUT  30000u   // transcription wait timeout
#define APP_AGENT_RUN_TIMEOUT   90000u   // Agent run timeout
#define APP_TICK_MS             100u     // app task heartbeat
// S3: maximum wait for TONE_DONE after the START tone (fallback to open the stream). 500 -> 200 (2026-08-29):
// the beep itself is only 80ms; the 500ms margin was for "the sound task being squeezed", but it is also the worst-case
// PTT latency -- on real hardware we once caught 622ms from press to recording start (= this value + APP_TICK_MS granularity), the user was already
// speaking while the device had not started recording. Dropping to 200 still leaves 2.5x margin; the cost is that when the beep is really delayed it overlaps
// capture (the first few frames record the 440Hz), which only happens on the degraded path and is acceptable. A fallback trigger leaves a trace on the serial port
// (STREAM_START is identifiable in the main.c heartbeat), so next time it is slow we can read the log instead of guessing.
#define APP_TONE_PENDING_TIMEOUT_MS 200u
// Upper limit protecting a single PTT utterance: relaxed to 10 minutes (600 seconds), supporting long continuous natural speech input,
// while preventing a physically stuck key from draining the battery.
#define APP_PTT_MAX_TALK_MS     600000u

#ifdef __cplusplus
}
#endif
