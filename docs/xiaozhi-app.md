# Xiaozhi AI conversation app (passport-os/main/apps/xiaozhi): rework notes

This document records how two problems were located and fixed after the conversation capability of FoloToy `folo-ai-passport-xiaozhi` was merged into Passport OS: "the service doesn't work (no reply to the conversation)" and "instability and crashes after a conversation".

Hardware: ESP32-C3 (400KB SRAM / 8MB Flash); the available heap after the app starts is about **94KB**.
Desktop log baseline: `Initial Free Heap: 95 KB`; on entering the Xiaozhi app, `94 KB`.

---

## 1. Summary of conclusions

| # | Symptom | Root cause | Fix |
| --- | --- | --- | --- |
| 1 | Cannot connect to the Xiaozhi backend / no reply to the conversation | The official backend's **OTA config fetch** flow was skipped entirely; `wss://api.tenclass.net/xiaozhi/v1/` and the literal `test-token` were hard-coded, so the server-issued `websocket{url,token,version}` was never obtained | Added `xiaozhi_ota.cc`: follows the official flow with `POST /xiaozhi/ota/`, parses `websocket` / `activation`, and persists the result to NVS |
| 2 | No reply to the conversation | `listen`/`abort` messages **carry no `session_id`**, so the server cannot attach the turn to a session | Parse the server's `hello.session_id` and echo it in all outbound messages |
| 3 | No reply to the conversation (intermittent) | The handshake request lacks the `Protocol-Version` header; `Client-Id` used the MAC as a stand-in (the official requirement is a persisted UUIDv4) | Added `Protocol-Version`; generate a UUIDv4 on first run and write it to NVS |
| 4 | No reply to the conversation (intermittent) | Client-side **DTX silence suppression**: when silence is detected no packet is sent at all, and the server, receiving no audio, simply does not respond | `enable_dtx = false` (in the press-to-talk scenario DTX only does harm) |
| 5 | Reply not visible on screen | The reply text was bound to the `llm` message, but the official `llm` carries only `emotion`; the text is in **`tts/sentence_start`** | Rebound the text to `tts/sentence_start` (still compatible with `llm.text` from some servers) |
| 6 | **Crash (always reproducible)** | "Press OK to interrupt while the AI is speaking": the websocket task runs `StartPlayback()` and the input task runs `StopPlayback()`, operating on the same codec handle and buffers concurrently without a lock, causing use-after-free and a panic reboot | Audio lifecycle calls are allowed only from the control task; websocket callbacks only post events / write to the ring buffer |
| 7 | Crash (stack overflow) | Opus codec temporary buffers are allocated on the stack, **about 19KB for encoding and >6KB for decoding**; the original 24KB recording task stack was enough, but after splitting across several tasks every one fell short | Recording and playback share **one resident worker thread** (20KB stack); half-duplex naturally makes them mutually exclusive |
| 8 | Crash (stack overflow) | The OTA mbedTLS handshake plus NVS/SPI-Flash reads form a very deep call chain, and the 6KB control task stack overflowed | OTA moved to a temporary `xzg_ota` task (12KB stack) that is deleted after use |
| 9 | Hang | `StopRecording()` spins unbounded waiting for the task to exit; once the recording task is stuck in an I2S read with `portMAX_DELAY`, the `portMAX_DELAY` wait in `esp_websocket_client_stop()` never returns either | All task waits changed to **bounded timeouts**; on timeout, only log and continue the teardown |
| 10 | App stuck on "Thinking", OK does nothing | The state machine has no timeout: if the server sends no `tts`, it stays in Thinking forever | Added think/reply timeout fallbacks; on timeout, automatically abort back to Idle |

---

## 2. Key measured data (real-device COM3 serial log)

A few numbers worth keeping from the fixing process, so later code changes don't step on them again:

| Item | Measured value | Notes |
| --- | --- | --- |
| Opus **encode** stack requirement | ~19KB | `clt_mdct_forward_c` / `silk_NSQ_c`; an 8KB stack overflowed by 6040 bytes |
| Opus **decode** stack requirement | >6KB | `quant_all_bands`; a 4KB stack overflowed by 1320 bytes |
| Opus encoder **heap** usage | ~23KB | Opened/closed each turn, returning it to the heap |
| Server-issued sample rate | **24000 Hz** | The decoder and codec must be opened at 24kHz, otherwise pitch and speed are all wrong |
| Server-echoed `frame_duration` | Follows the client | Whatever the client reports, the server's `hello` echoes back |
| Available heap while speaking | ~14KB | Encoder and TLS both online; this is the tightest moment |
| Available heap while replying | ~20KB | Encoder released, decoder online |

**Frame length choice**: the client reports at **60ms** frames, with the encoder side fixed at **16kHz** mono.
20ms and SILK (VOIP) encoding were tried, and the server did not respond at all (reverted),
and frame length has almost no effect on the encoder stack requirement (the CELT temporary buffer size barely depends on frame length).

**Known benign logs**:

- `tts state=sentence_end (ignored)`: newer servers send this state, meaning only that one sentence has finished;
  it does not affect the audio stream or the state machine, and ignoring it is correct behavior.
- `Playback ring full (...), frame dropped`: the server sent a burst exceeding the buffer headroom.
  It should not appear on the normal path; if it appears frequently, increase `kPlayRingCap` (at the cost of an equal amount of heap).
- `WebSocket error, type=0/1` followed by an automatic `letting it auto-reconnect`: network jitter.
  Verified on a real device: disconnect, recording automatically wraps up, client automatically reconnects, handshake again, conversation continues,
  with no reboot and no user intervention throughout.


---

## 3. Architecture: a single-owner control task

```text
Key task (3072 stack) ──post──┐
                              ├─► xzg_ctl (control task, 6KB stack)
websocket task (5KB stack) ──┐│     * the only caller of Protocol::Start/Stop/Trigger*
   └ callbacks do only two things:   * the only caller of AudioService Start*/Stop*
       · write events to queue ─────┘│     * the only caller of ui_*
       · push Opus into ring buffer ─┘     * the sender of recording frames (drain_audio_tx)
                                    │
xzg_ota (temporary, 12KB stack) ── OTA HTTPS handshake, deleted after use
xzg_audio (resident, 20KB stack) ── recording encode / playback decode (half-duplex, shared single stack)
```

Key points:

- **Never do blocking work in a websocket callback** (no decoding, no I2S writes, no lock waits).
  The old implementation put `PlayAudioFrame()` (decode + blocking I2S write) directly in the callback,
  which made the network task do the audio task's work.
- **`Protocol` is a function-local static object that is never destructed**, which eliminates "callback accesses a destroyed object" at the root.
- **`xzg_audio` is created at app startup**: at that point the heap is fresh and contiguous (largest block 50KB+), so a contiguous 20KB stack
  is guaranteed to be allocated; if requested only when the user presses to talk, the heap has already been fragmented by TLS/OTA (largest block only 7KB),
  and allocation would inevitably fail.
- **On switching away, `Deinit()` returns the resident thread and all buffers to the heap**: after leaving Xiaozhi,
  `free heap` returns to 80KB, so it does not encroach on other apps such as Radio / Codex.
- **Opus encoder kept resident (2026-09-14 memory fix)**: the encoder needs a ~25KB contiguous block (measured:
  open failed with ret:-7 at largest=22528/23552), while in the later part of a session largest is only 4~9KB;
  opening on use and closing after use inevitably fails on a fragmented heap. Now `Init()` calls `EnsureEncoder()` to open it and keep it resident,
  and `StopRecording()` no longer closes it; if the decoder cannot be opened during playback, the encoder is temporarily released
  (`enc_dropped_for_playback_`) and restored immediately after `StopPlayback()/AbortPlayback()` finishes playing.
  When decoder teardown is not confirmed idle, the handle is kept (the old code nulled it = a permanent ~13KB leak).
- **Accompanying global memory shuffling** (otherwise the resident encoder starves the TLS handshake:
  `Dynamic Impl: alloc(3778) failed`): the Home page `s_portrait_a8` (14.4KB A8 portrait buffer)
  and the boot chime `s_note` (3.2KB) were changed from .bss to allocate-on-use and return-after-use; free heap
  during a Xiaozhi session rose from ~30KB to ~48KB. Verified on a real device: 3 conversation rounds of recording/playback all work, and after exit
  free=99.5KB/largest=51.2KB with no leak (`_diag/xz_memfix4.log`).

---

## 4. File responsibilities

| File | Responsibility |
| --- | --- |
| `app_xiaozhi.cc` | Single-owner control task, state machine, timeout fallbacks, key events, recording frame sending |
| `xiaozhi_protocol.{h,cc}` | WebSocket v3: request headers, `hello` handshake, `session_id`, fragment reassembly, message parsing |
| `xiaozhi_audio.{h,cc}` | ES8311 half-duplex audio: resident worker thread, ring buffer, bounded waits, memory gate |
| `xiaozhi_ota.{h,cc}` | Official backend OTA/activation: fetch `websocket{url,token,version}`, show the activation code |
| `xiaozhi_store.{h,cc}` | NVS config read/write (namespace `xiaozhi`, compatible with the official `websocket`) |
| `xiaozhi_ui.{h,cc}` | LVGL UI; all entry points re-validate the widget pointers under the lock |

---

## 5. Configuration

NVS namespace `xiaozhi` (can be written directly with the `nvs_flash` tool or with serial commands added later):

| key | Default | Description |
| --- | --- | --- |
| `ws_url` | empty | For a self-hosted server, set to `ws://192.168.x.x:8000/xiaozhi/v1/` |
| `token` | empty | Usually left empty for a self-hosted server; for the official backend it is written automatically by OTA |
| `version` | 3 | Protocol version |
| `ota_url` | `https://api.tenclass.net/xiaozhi/ota/` | Change this for a self-hosted server |
| `client_id` | auto-generated on first run | Persisted UUIDv4, **do not edit by hand** (changing it means changing the device identity) |

Priority: **OTA-issued > `xiaozhi` namespace > official same-name `websocket` namespace > built-in defaults**.
Cached for 10 minutes; after 3 consecutive connection failures it is fetched again.

### Official backend (xiaozhi.me)

On first connection the device POSTs to the OTA endpoint:

- If `activation{code,message}` is returned, the screen shows "Pending activation, binding code XXXXXX";
  add this device's MAC in the xiaozhi.me console. After binding it retries automatically every 30 seconds, **no reboot needed**.
- If `websocket{url,token,version}` is returned, the device is already bound and connects automatically to start conversing.

The device MAC is visible directly on the Xiaozhi app screen (the first line below the top bar, "Device MAC: xx:xx:...").

### Self-hosted server (xiaozhi-esp32-server)

Point `ota_url` in NVS at the self-hosted server's OTA address, or write `ws_url` + `version` directly.

---

## 6. Keys

| Key | State | Action |
| --- | --- | --- |
| OK single press | Disconnected | Retry the connection immediately |
| OK single press | Idle | Start speaking |
| OK single press | Listening | End speaking, upload and wait for the reply |
| OK single press | Replying | Interrupt (cut the sound first, then notify the server) |
| OK single press | Thinking | Ignored (a 25-second timeout is the fallback) |
| Volume+ / Volume- | Any | Volume ±10% |
| OK long press | Any | Return to the launcher (handled by the global handler) |
