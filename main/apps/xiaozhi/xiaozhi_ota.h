#pragma once

// Xiaozhi backend OTA / device activation client.
//
// Why this layer is needed: the official backend (api.tenclass.net) does not hand out a usable
// WebSocket credential for free. The device must first call the OTA endpoint with Device-Id/Client-Id, and the server then returns
//
//   * websocket{url, token, version}  -- the real conversation address and token
//   * activation{code, message}       -- the device is not bound yet; asks the user to bind this MAC in the backend
//
// The official firmware (xiaozhi-esp32 ota.cc / application.cc) does exactly this. The earlier implementation hardcoded
// wss://api.tenclass.net/xiaozhi/v1/ + "test-token", skipping the whole activation flow;
// on the official backend that gave "connects but the conversation returns nothing", and this was the root cause.
//
// Self-hosted servers (xiaozhi-esp32-server) usually provide the same OTA path, so this flow
// also works with a self-hosted backend.

#include <string>

namespace xiaozhi {

enum class OtaResult {
    Ok,               // Got the websocket config
    NeedsActivation,  // Server requires binding the device first (with an activation code)
    NoConfig,         // Response OK but no websocket section (continue with local NVS / defaults)
    NetworkError,     // Request failed (no network, TLS, timeout)
    ParseError,       // Response could not be parsed
};

struct OtaOutcome {
    OtaResult result{OtaResult::NetworkError};
    std::string url;                  // websocket url
    std::string token;                // websocket token
    int version{0};                   // Protocol version (0 = not specified by server)
    std::string activation_code;      // Activation code (when NeedsActivation)
    std::string activation_message;   // Hint text from the server
    std::string detail;               // Diagnostic info / HTTP status code
};

// Exchange for config via /xiaozhi/ota/. On success also writes url/token/version into the NVS namespace
// "websocket" (same layout as the official firmware), and exposes them through the "xiaozhi" namespace of xiaozhi_store.
//
// Must be called from the control task (does one HTTPS request with TLS internally, taking hundreds of ms).
OtaOutcome ota_fetch_config();

// HTTP status code returned by the server (for diagnostics; 0 means no response)
int ota_last_http_status();

} // namespace xiaozhi
