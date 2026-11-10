# App review: AI Voice and AI Device (2026-09-12)

Subjects of the review (Launcher name → code location):

| Section name | App ID | Code | Size |
| --- | --- | --- | --- |
| AI Voice | `APP_ID_VOICE` "Portable AI Voice" | `main/apps/voice/` | 28 files ≈ 4.4k lines |
| AI Device | `APP_ID_CODEX` "Codex Companion Sprite" | `main/apps/codex/` | 38 files ≈ 38.5k lines (of which `buddy_font_16.c` accounts for 31k lines and is reused by Home) |

---

## 1. AI Voice (apps/voice)

### 1.1 What it actually is today

**A pure "dumb terminal + external computer" scheme**; the device has no ASR at all.

```
Hold UP → play a 440Hz beep → capture only actually starts after the beep ends
   → bsp_audio_read takes 3200B every 100ms (16kHz/16bit/mono)
   → IMA ADPCM 4:1 compresses to 804B → BLE notify (2M PHY, DLE 251, split into 5 fragments)
   → the computer-side companion does ASR/LLM/injection
   → results come back down: transcript / agent.status / agent.approval_request
```

Key evidence:
- `audio_streamer.c:405` encodes ADPCM → `ble_audio_notify_audio`, 804B/100ms = 8KB/s
- `app_voice.cc:80-89` uplink `{"event":"voice.start","audio":"ima_adpcm"}` / `voice.end` / `status{drop}`
- `app_protocol.c:133-136` the downlink only returns text of ≤128B; **text injection has been retired to the PC** (`app_types.h:96`)

### 1.2 Three hard flaws you must know about

**1. 100% unusable offline, and the companion is not in the repo**
When not connected, `app_state.c:176` directly toasts `OFFLINE - PTT blocked`. The companion is
`companion/relay.py` + `companion/adpcm.py` (see the comments at `audio_streamer.c:16` and `adpcm.c:5`),
and **this repo does not have it**. Even the system clock needs the computer to send `time.set`.

**2. `APP_EV_TICK` has a handler but no producer → all timeouts are dead (functional bug)**
`app_state.c:725` is the only place it appears in the whole repo. Consequences:
- The 20s screen-off never fires
- **The 30s transcription timeout never fires**
- **The 60s PTT safety net never fires**: holding the key down records forever

This must be fixed first regardless of which option is chosen.

**3. Reinventing the wheel with two audio stacks alongside xiaozhi**
Both use `bsp_audio_read` at 16kHz/16bit mono, and both are key-triggered with no wake word; but voice goes through
ADPCM+BLE+computer-side ASR, while xiaozhi goes through Opus 60ms + WebSocket + cloud STT.
The codec lifecycle, ring, state machine and key semantics are **written twice, with no shared audio service layer**.

**4. To use BLE, entering voice stops Wi-Fi (directly harming other apps)**

Original text at `app_voice.cc:222`:
```c
// Voice transport is BLE/USB; release Wi-Fi runtime buffers while active.
suspend_wifi_driver();
```
It is the same pattern as codex (`app_codex.cc:135`). Consequences: enter voice → Wi-Fi stops → weather/time sync/
upload all cut off; exit → Wi-Fi reconnects (slow, power-hungry, may fail). **Both the weather and the clock on Home depend on Wi-Fi**,
so every time you go back to Home after using voice, the weather has to wait for the reconnect before it can update.

This is collateral damage from "choosing BLE", not a problem of voice alone.

**5. BLE puts the whole device on a memory cliff (the biggest threat to stable operation)**

Original comment at `ble_audio.c:186-189`:
> After `nimble_port_init(controller)` eats **~44.7KB** of heap, the remaining largest block is only **~7KB**,
> and dynamic stack allocation competes with the host task (4096B); measured, this makes host task creation fail → no advertising, cannot connect.
> **5120 leaves the heap 1KB smaller → `ble_hs_start` allocation fails in a reboot loop**

`audio_streamer.c:83-84` also records that after the encoder + NimBLE controller, **the heap is only ~3KB**,
so the worker stack must be made static in `.bss` (dynamic allocation would fail). A historical conclusion is also left at the same place:
> "In the Opus era it was raised all the way to 16384 and still crashed; the root cause is **heap exhaustion**, not the stack"

The system is sensitive to memory changes on the order of 1KB, which makes this the number-one risk source for "stable operation".

Also: `audio_streamer_peak()` computes the peak but nobody calls it → **the recording screen has no waveform**;
the USB channel is half-finished (`mode.c:13-19` `mode_channel_up(USB)` is always false, and the whole repo has no `usb_link`).
There is also "RF corrosion causing phantom key presses", for which `app_state.c:10-72` piled up 5 sets of mV gates;
the root cause is again the BLE radio, and removing BLE makes this pitfall disappear too.

### 1.3 Positioning correction: it is a "wireless microphone", not a voice assistant

> 2026-09-12 user clarification: the requirement is **device as microphone → computer-side ASR → text paste**.
> Pursue **extreme lightness**, but require **every app to run normally and stably**.

This positioning overturns the early-draft conclusion below about "switching to Xiaozhi MCP". When the positioning changes, the answer changes.

### 1.4 First rule out two paths that "look more elegant"

**1. The device is recognized directly by the computer as a USB microphone (zero companion) → the C3 hardware does not support it**

Espressif's official FAQ and the esp-iot-solution peripheral support matrix:

- The ESP32-C3 only has the **USB-Serial-JTAG** peripheral: "**custom descriptors are not possible**",
  and "secondary development to modify USB functions or descriptors is not supported"
- USB-OTG (which can do USB Audio / HID / MSC) exists only on the S2 / S3 / P4; the C3 column is X

→ **The C3 cannot be a UAC microphone, so the companion must exist. This is fixed by the hardware.**
(If the project moves to the ESP32-S3 in the long run, this path would let the companion disappear entirely.)

**2. On-device local ASR → impossible**

On a C3 without PSRAM, ESP-SR only supports the WakeNet9s wake word and **does not support MultiNet command words**,
let alone continuous recognition of free speech.

**Conclusion**: "device captures audio → sends to the computer → computer ASR + paste" **is itself the optimal solution on this hardware**.
The problem is not "what to do" but "**which channel to transmit over**".

### 1.5 The real optimization: switch the channel from BLE to Wi-Fi

| Dimension | BLE (current) | **Wi-Fi UDP (recommended)** |
| --- | --- | --- |
| Protocol stack memory | NimBLE controller **~44.7KB**, remaining largest contiguous block only ~7KB | **0**: lwIP is already running (weather/time/upload all use it) |
| Effect on other apps | **Must `suspend_wifi_driver()`** (`app_voice.cc:222`); weather/time/upload all stop | No need to stop; apps coexist |
| Stability | Stack 4096→5120 (1KB more) makes `ble_hs_start` fail in a **reboot loop** | No such risk |
| Bandwidth | 8KB/s (2M PHY + DLE 251; mbuf pool backpressure is the real bottleneck) | LAN Mbps level, very ample |
| End-to-end latency | 100ms/packet cadence | Lower, but **the bottleneck is ASR, not transport**, so the perceived improvement is limited |
| RF interference | Yes (phantom key presses; `app_state.c` piled up 5 sets of mV gates for it) | None |
| Code size | `ble_audio.c` 747 lines + a whole BLE wrapper | One UDP socket, a few dozen lines |
| Prerequisite | Pairing is enough, same network not required | **The device and the computer must be on the same LAN** |

**Key insight: this channel was actually built long ago.** The Home avatar upload already runs over Wi-Fi
(httpd, verified on a real device at `http://192.168.5.4`). The device ↔ computer IP path already exists,
and voice only needs one more UDP port, with almost zero new infrastructure.

**Costs and responses**:
- Needs the same LAN: your scenario (a person speaking in front of the computer) satisfies this naturally
- Needs to know the peer IP: use UDP broadcast + companion reply for automatic discovery; or reuse the existing upload-page pattern of
  "the device opens a service, the computer connects"
- The companion changes its receiving end: BLE notify → UDP socket; the workload is small

### 1.6 Recommendation: what to keep, what to change

**Keep (these designs are right, don't touch them)**

- **ADPCM**: `adpcm.h:25-28` block header 4B + 800B, **each block independently decodable** → packet loss affects only one block,
  naturally resistant to UDP loss. It fits "extreme lightness" better than Opus; history has confirmed this too:
  `audio_streamer.c:90` keeps the original words "In the Opus era it was raised all the way to 16384 and still crashed; the root cause is heap exhaustion, not the stack"
- The press-and-speak key interaction
- The companion handles ASR + paste (the hardware dictates this)

**Change (ordered by cost-effectiveness)**

1. **Channel BLE → Wi-Fi UDP**: one move removes the 44.7KB resident cost + the forced Wi-Fi stop +
   the reboot-loop risk + phantom key presses, **hitting both "lightweight" and "stable operation" goals at once**
2. Also delete the `suspend_wifi_driver()` call
3. Delete `ble_audio.c` (747) + the half-finished USB `mode.c` + assorted dead code
   → estimated to drop 4.4k lines to ~1.5k lines
4. **Add the missing `APP_EV_TICK` producer** (P0, independent of the channel change, must be fixed either way)
5. Hook up `audio_streamer_peak()` so the recording screen shows a waveform

**About the "physical approval handbrake"**: it is a separate matter from the "wireless microphone"
(`app_state.c:585-612`, OK = approve / UP = reject / no timeout / always on, never sleeps the screen).
If you don't use it, cut it together for further weight reduction; if you do, keep it: it is a pure state machine and uses no extra memory.

> Note: if `suspend_wifi_driver()` / `resume_wifi_driver()` are not paired
> (an early return on an error path), Wi-Fi stays down permanently. After switching to the Wi-Fi channel this hazard disappears automatically.

---

## 2. AI Device (apps/codex)

### 2.1 Decisive finding: the data chain is broken

This is not "weak functionality"; **the received data never enters the state machine at all**.

```
buddy_ble.c:369   emits BUDDY_BLE_EVENT_RX_LINE
        ↓
app_codex.cc:112  on_codex_ble_event() only handles CONNECTED / DISCONNECTED
        ↓  ✗ no else branch handles RX_LINE
buddy_orchestrator_process_rx()   ← zero callers under all of main/
                                    (only the definition at buddy_orchestrator.c:42
                                      and the declaration at .h:29; confirmed by Grep)
        ↓  ✗
buddy_protocol_parse() never executes → s_codex_state is never written after init
```

**Verified** (`app_codex.cc:112-119`, original text):
```c
static void on_codex_ble_event(const buddy_ble_event_t *event, void *context) {
    (void)context;
    if (event->type == BUDDY_BLE_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Codex BLE connected");
    } else if (event->type == BUDDY_BLE_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Codex BLE disconnected");
    }
}
```

**Knock-on consequences**:
- `tokens` / `tokens_today` are **always 0**, regardless of whether BLE is connected
- Task listening / approval popups / celebration animations **never trigger**
- `buddy_app_logic.c` (544 lines) is pure dead code; `buddy_screenshot.c` is not in CMakeLists
- `app_codex.cc:190-210`: **all three keys OK / UP / DOWN only play sound effects**,
  and the "Up switches page / Down rejects / OK allows" written in the UI (`buddy_ui.c:379`) is not implemented
- The Home hint always shows "Please connect Codex Buddy on your computer" (`buddy_ui.c:322-323`)

**Direct effect on Home (worse than expected)**:
`s_usage_ready` is set after `codex_app_start()` (`app_codex.cc:164`) and is **never reset afterwards**.
So as soon as the user has entered the codex app once, the two stat cards on Home change from `--` to **permanently showing `0`**:
it looks like "there is data but it is 0", which is more misleading than `--`.

### 2.2 Risks of embedding it in Home

| Risk | Description | Level |
| --- | --- | --- |
| **Wi-Fi vs BLE architecture conflict** | `app_codex.cc:135` `suspend_wifi_driver()`: codex stops Wi-Fi at startup to make room for the 38KB canvas. Embedding into Home = **either Wi-Fi is cut permanently** (weather/time/upload all useless), **or 38KB + Wi-Fi buffers coexisting very likely runs out of memory** | 🔴 Architecture-level deadlock |
| **42KB+ resident** | 38KB I4 canvas (`buddy_ui.c:18,505`) + 6KB task stack + 4KB NimBLE host stack. Goes from "on-demand use" to "permanent use" | 🔴 |
| **Resident CPU** | 5fps full-screen 38KB memset + per-pixel redraw (`buddy_ui.c:485,558`), competing with Home refresh for the lock and the single core | 🟠 |
| **Zero benefit** | What moves over is just "credits that are always 0 + a BSOD sprite", with no new capability gained | 🔴 |

**Conclusion: do not embed.** Home already gets, through `codex_app_get_usage()`, the only genuinely valuable slot of codex
(the two stat cards); moving more over would just move "emptiness" up, and at the price of Wi-Fi being permanently down.

### 2.3 Recommendation (two steps)

**Step 1 (immediately, 1~2 hours): fix the data chain so the two cards on Home mean something**

Add an `RX_LINE` branch to `on_codex_ble_event()` that forwards to `buddy_orchestrator_process_rx()`.
Then **fix `s_usage_ready` while at it**: it is currently never reset once set, which makes Home show a misleading `0`.
The suggestion is to make `codex_app_get_usage()` return false while BLE is not connected, so Home naturally falls back to `--`.

This is the step with **the smallest change and the biggest gain**.

**Step 2 (product decision, pick one)**

- **A. Keep the section**: if you want to keep that pixel sprite as a desk-setup toy, keep it, but do not embed it
- **B. Delete the section**: keep `codex_app_get_usage()` as a data source, and delete
  `buddy_ble` / `state` / `ui` / `orchestrator` / `app_logic` / `screenshot`.
  The Launcher goes from 6 cards to 5

> ⚠️ **Confirm before deleting**: `buddy_font_16.c` (31,044 lines) is **Home's Chinese font**,
> and all Chinese on Home (name/signature/weather/hints) depends on it. When deleting the codex directory, it **must never be deleted along with it**.

**Fix along the way**: `codex_app_stop()` does not release the `buddy_sound` task (4096B stack, no deinit,
resident permanently after stop) + the NVS handle at `buddy_settings.c:318` is never `nvs_close`d
→ **entering and leaving codex repeatedly leaks**.

---

## 3. Action priorities

| Priority | Action | Estimate |
| --- | --- | --- |
| P0 | Add `APP_EV_TICK` (all voice timeouts are dead; holding the key records forever) | Small |
| P0 | Add the `RX_LINE` dispatch in `on_codex_ble_event` + fix the `s_usage_ready` semantics | 1~2h |
| **P1** | **voice channel BLE → Wi-Fi UDP** (saves 44.7KB, no longer stops Wi-Fi, removes the reboot loop and phantom key presses) | Medium |
| P1 | Fix the codex sound task / NVS handle leaks | Small |
| P2 | Hook `audio_streamer_peak()` into the recording screen for a waveform | Small |
| P2 | Delete the BLE stack / half-finished USB / dead code (voice 4.4k → ~1.5k lines) | Medium |
| P2 | Decide whether to keep or drop the codex section (delete or keep) | ✅ Decided: **remove**, see Section 5 |
| — | Whether to keep the approval handbrake (unrelated to the "wireless microphone", can be cut) | Needs a decision |

---

## 4. Appendix: method and evidence sources of this review

- voice: read all 28 files of `main/apps/voice/` in full, focusing on `app_state.c` /
  `audio_streamer.c` / `ble_audio.c` / `app_protocol.c` / `app_voice.cc`
- codex: read `main/apps/codex/` in full; all key conclusions were rechecked with Grep
  (`buddy_orchestrator_process_rx` having zero callers repo-wide was confirmed twice)
- External options (verified online on 2026-09-12):
  - **The ESP32-C3 only has USB-Serial-JTAG, cannot customize descriptors, and cannot be a USB UAC microphone**
    (Espressif official FAQ + esp-iot-solution peripheral matrix; USB-OTG exists only on S2/S3/P4)
    → the path "the device is recognized directly by the computer as a microphone, zero companion" is hardware-blocked on the C3
  - **On the C3 (no PSRAM), ESP-SR only supports the WakeNet9s wake word, not MultiNet command words**
    → the local ASR route does not hold

---

## 5. AI Device black screen diagnosis and removal (executed 2026-09-12)

The user reported "after opening AI Device it is a black page". After diagnosis it was confirmed that this module **has never run successfully on a real device** in this project,
and its value needs a PC-side Codex bridge, which is not needed now → **removed as a whole** with the user's authorization.

### 5.1 Investigation process and evidence

| Step | Method | Result |
| --- | --- | --- |
| 1 | Grep all historical logs for codex traces | **Zero hits**: this module has never been run on a real device since the repo was created |
| 2 | Added diagnostic logs (canvas allocation / heap / LVGL lock) → built and flashed → entered the app with serial `'4'` | `canvas 38468 bytes OK`, `ui_init done; screen loaded`, **no panic, no reboot** |
| 3 | Added a canvas pixel histogram (every 2s counting pixels of the 16 palette indices) | `nonzero=4595` (ink=608 / dim=1583 / line=428 / orange=111), `active=1` |
| 4 | Cross-checked against the LVGL 9 source | **Root cause**: see below |

Conclusion: **the canvas content is correct, and the codex screen really is the active screen, but the physical screen shows only the base color `0x080A0C` (near black)**,
so what the user sees is a black page.

### 5.2 Root cause

`main/apps/codex/buddy_ui.c` uses an `lv_canvas` + `LV_COLOR_FORMAT_I4` (4bpp indexed color) as the whole-screen
frame buffer (to save RAM: I4 takes only 38KB while RGB565 takes 150KB, and only ~124KB is free when entering codex).

But **the LVGL 9 software renderer does not support blending an I4 source image onto an RGB565 target**:

- In the image blend branch at `src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c:444-451`, among the indexed formats
  **only `case LV_COLOR_FORMAT_I1` exists** (controlled by `LV_DRAW_SW_SUPPORT_I1`);
  I2/I4/I8 all fall to `default: LV_LOG_WARN("Not supported source color format")` → not a single pixel is drawn.
- The other target formats (`lv_draw_sw_blend_to_rgb888/argb8888/l8/a8/al88`) likewise only have the I1 branch.
- In this project's `sdkconfig`, `CONFIG_LV_DRAW_SW_SUPPORT_I1=y`, and LVGL's Kconfig
  **has no switch for I2/I4/I8 at all**: so this is not a missed config option but the capability boundary of this LVGL version.

This also explains why there was not a single warning in the logs: `LV_LOG_LEVEL` was not turned up to WARN.

### 5.3 Why remove instead of "fix"

There is no cheap way to fix I4 rendering:

- Switching to a full-screen RGB565 canvas → needs 150KB, **not enough memory** (only ~124KB left when entering codex).
- Bypassing LVGL and pushing line buffers directly with `esp_lcd_panel_draw_bitmap()` → amounts to turning codex into a special-case app
  outside the LVGL framework, taking over the panel and the transition-screen mechanism, which is a refactor, not a fix.
- Even if the display were fixed, the app **would still be unusable**: its data/command chain is only half connected
  (`buddy_orchestrator_process_rx` still has zero callers, the three keys only play sounds),
  and the real functionality depends on a PC-side Codex bridge, which the user explicitly does not need at present.

Cost comparison: it takes a 38KB canvas + ~45KB NimBLE controller, and **calls `suspend_wifi_driver()` at startup**,
shutting Wi-Fi off (weather/time/upload all cut).

### 5.4 What was actually done

- **Lifesaving**: `buddy_font_16.c` (the 4360-character Chinese font, shared by Home + Radio) was **moved** to
  `main/fonts/buddy_font_16.c`, with the declaration unified into `main/fonts/app_fonts.h`.
- **Backup**: the whole codex directory (38 files) was backed up to
  `F:\WORK\AI硬件\_diag\removed-codex-backup-2026-09-12\codex\` (it is also in git history).
- **Build**: `main/CMakeLists.txt` drops 17 codex source files, the `apps/codex` INCLUDE,
  and the codex-specific `-Wl,--wrap=ble_att_svr_register` link option (the wrapper is defined in `buddy_ble.c`).
- **Registration**: `app_registry.cc` drops `g_codex_app` and its extern; **an unknown-app-id guard was added**
  (when the target app does not exist, the switch is refused outright; otherwise it would hang on the transition screen, which is the black screen).
- **Enum**: `app_base.h` deletes `APP_ID_CODEX` and renumbers (WIFI=4 / SETTINGS=5 / HOME=6);
  `main.cc` serial keys `'1'-'7'` → `'1'-'6'`.
- **Launcher**: removed the two cases for the "AI Device" card name and icon.
- **Home**: the `codex_app_get_usage()` call was taken off, and the two stat cards (today/total) always show `--`
  (visually identical to before the removal, since it was also `--` when the bridge had never connected); `update_codex_labels` renamed to `update_stat_labels`.
- **Deletion**: the entire `main/apps/codex/` directory.

### 5.5 Real-device verification (`_diag/after_remove.log`)

| Item | Before removal | After removal |
| --- | --- | --- |
| Launcher cards | 6 | **5** |
| Boot landing on Home | `Switching app: 0 -> 7` | `0 -> 6` (enum renumbering) |
| Free heap after screen-on | 132 KB | **141 KB** (+.bss reclaimed ≈ +9KB) |
| Free heap after leaving Home | 146 KB | **155 KB** |
| Serial `'4'` | → Codex (black screen) | → Wi-Fi (correct) |
| codex/BLE logs | a screenful of BLE_INIT/NimBLE | **zero hits** |
| panic / reboot | none | none |

Firmware size 3,161,360 → **3,108,096 bytes (-52KB)**, app partition 26% free.

### 5.6 Found and fixed along the way: leaving voice always crashes (pre-existing bug, unrelated to codex)

While running the all-apps regression after removing codex, it was found that **entering "AI Voice" and then leaving it panics and reboots 100% of the time**
(`Load access fault`). Neither `Switching app: 2 -> 3` nor `Stopping app: Portable AI Voice` ever appeared
in the historical logs: this path had never been tested before.

The crash address was reverse-looked-up with `riscv32-esp-elf-addr2line`, and the call chain is:

```
app_registry_switch_to (app_registry.cc:141)
 → voice_app_stop (app_voice.cc:292)
  → app_ui_deinit (app_ui.c:412) → app_ui_hide (app_ui.c:407)
   → lv_screen_load → lv_obj_add_flag → lv_obj_invalidate
    → lv_obj_tree_walk → lv_obj_get_display → lv_obj_get_parent   ← crashes here
```

**Root cause**: `app_ui_show()` in `app_ui.c` caches `lv_screen_active()` (at that moment app_registry's
**transition screen**) into `s_previous_screen`, and `release_transition_screen()` deletes it once the switch completes
→ when leaving voice, `app_ui_hide()` calls `lv_screen_load()` with a dangling pointer → dereferencing freed memory.
And "restore the previous screen" was redundant to begin with: `switch_to()` has already loaded the transition screen before `stop()`, and afterwards will start the next
app / return to the Launcher, so screen ownership is managed uniformly by the registry.

**Fix**: `app_ui.c` deletes `s_previous_screen` and all its restore logic, and `app_ui_hide()` only hides its own
chrome. In the whole repo only voice has this pattern.

**Verification**: `_diag/voice_exit_test.log` (before the fix) → always panics; `_diag/voice_fix_verify.log` (after the fix)
→ entering and leaving voice twice in a row both show `Voice App cleanly stopped. Free heap: 105 KB`, zero panic, zero reboot,
stable heap; `_diag/full_sweep.log` → all five apps enter and leave in sequence, and long-press OK returns to Home throughout.

### 5.7 If it is ever restored

1. Retrieve the source from `_diag/removed-codex-backup-2026-09-12/codex/` or git history (note that
   `buddy_font_16.c` must stay in `main/fonts/`; do not put it back in the codex directory).
2. **First solve I4 rendering**: change the canvas of `buddy_ui.c` to a format LVGL can blend, or switch to
   pushing line buffers directly to the panel. `buddy_i4.c` / `buddy_sprite.c` are the I4 indexed drawing layer, which can be kept and reused, changing only the final output step.
3. Then complete the wiring of `buddy_orchestrator_process_rx()`, otherwise it is still just a moving black-screen pet.

  - Xiaozhi AI v2 capability matrix (ESP-SR wake word / Opus / streaming ASR+LLM+TTS / voiceprint /
    on-device MCP / cloud MCP including PC desktop operation / supports ESP32-C3)
    — Note: this applies to the "voice assistant" positioning; after the user clarified the positioning as "wireless microphone",
    the Xiaozhi scheme is no longer an alternative direction for voice (already rewritten into 1.3~1.6)

---

## 6. AI Voice channel rework: BLE → Wi-Fi UDP (executed 2026-09-12)

The direction was decided by the user: **rewrite the channel as Wi-Fi UDP**, and **replace BLE completely (no fallback kept)**.
The full protocol contract is in [`voice-udp-channel.md`](./voice-udp-channel.md); here only "what was done + measurements + boundaries" are recorded.

### 6.1 In one sentence

Entering voice no longer kills Wi-Fi; the uplink audio/events change from NimBLE notify to UDP unicast; the whole NimBLE layer drops out of the link.

### 6.2 Protocol (both device side and companion must follow; a change on one side must be synchronized to the other)

| type | Direction | payload | Total length (including 1B type) |
| --- | --- | --- | --- |
| `0x01` AUDIO | device→computer | `[seq][0x80]` + 804B IMA ADPCM block | 807B |
| `0x02` EVENT | both ways | UTF-8 JSON line (**including the trailing `\n`**) | ≤ 513B |
| `0x03` BEACON | computer→device | `{"v":1,"role":"pc","port":33333}`, broadcast every 1s | ~40B |
| `0x04` PING | device→computer | empty, one every 2s, **liveness only** (not dispatched to the relay) | 1B |

- Port **33333**; companion = server (bind + broadcast beacon), device = client (bind + learn the computer IP from the beacon).
- `link_up` = peer IP learned **and** < 3000ms since the last packet received from the peer.
- **AUDIO keeps the BLE-era 2B fragment header** (it degenerates to `[seq][0x80]` over UDP), so the companion's
  `reassemble_adpcm()` needs no changes at all; EVENT likewise carries `\n`, so `reassemble_event()` also needs zero changes.

### 6.3 What changed

**Device side (`passport-os`)**

| File | Change |
| --- | --- |
| `main/apps/voice/udp_audio.c` / `.h` | **New** (348 lines): socket + RX task + beacon learning + PING liveness |
| `main/apps/voice/mode.c` | Rewritten as single-channel forwarding (original BLE/USB dual channel → only UDP remains; the half-finished USB branch deleted) |
| `main/apps/voice/app_voice.cc` | Deleted `suspend_wifi_driver()` / `resume_wifi_driver()`; `ble_audio_*` → `udp_audio_*` |
| `main/apps/voice/app_state.c` / `app_ui.c` | Display name "BLE" → "WiFi" |
| `main/CMakeLists.txt` | `ble_audio.c` → `udp_audio.c`; `bt` removed from REQUIRES |
| `main/apps/voice/ble_audio.c` / `.h` | **Deleted** (747 lines; backup see §6.7) |

**companion (`folo-ai-passport-voice/companion/`)**

| File | Change |
| --- | --- |
| `udp_transport.py` | **New**: `UdpTransport`, implementing the existing 5-method contract (scan/connect/write/notify/disconnect) + beacon loop + read thread |
| `relay.py` | `_build_transport()` gains a `channel == "wifi"` branch |
| `fre_state.py` | `VALID_CHANNELS` adds `"wifi"`; the old "WiFi channel has been removed" error text deleted |
| `tests/test_udp_transport.py` | **New**: 13 unit tests (including a cross-module contract with `reassemble_adpcm` and a real-socket loopback) |

The `relay.py` core and `adpcm.py` / `asr_client.py` / `inject.py` have **not a single line changed**: the 5-method contract absorbed every difference.

### 6.4 Measured results

| Item | Before | After |
| --- | --- | --- |
| Firmware size | 3,108,096 B | **2,870,416 B (−232 KB)** |
| App partition free | 26% | **32%** |
| NimBLE symbols in the ELF | present | **zero** (`nm` finds no `ble_audio*` / `nimble_port*` / `ble_gap_*` / `ble_gatts*`) |
| Wi-Fi when entering voice | `StopStation()` + `esp_wifi_deinit()` | **left completely alone** |
| Transport layer code | `ble_audio.c` 747 lines | `udp_audio.c` 348 lines |

Build `BUILD_EXIT=0`; real-device `_diag/udp_voice.log`: entering/leaving voice is clean: `UDP RX task started (port 33333)`
→ `UDP channel ready` → `UDP channel closed`, Wi-Fi stays up throughout (no `esp_wifi_deinit`), no NimBLE logs,
and after exit `Free heap: 131 KB`, no panic. Companion unit tests `13 passed` (`_diag/pytest_udp.log`).

### 6.5 Verification boundary (the part that was not run through; to be aware of)

**Real-time device→computer audio e2e was not run through on this machine**, because of the network topology rather than the code: the device is connected to `ZTE-2302-5G`
and got `sta ip: 192.168.5.4`, while this machine's only routable address is `192.168.1.3` (Ethernet), and the Wi-Fi adapters are all
"disconnected"; `Test-Connection 192.168.5.4` returns False.

→ To reproduce e2e, just: on a computer **on the same subnet as the device**, set the companion's `channel` to `wifi`
(leave `udp_port` empty for the default 33333) and run `relay.py`; on first run, allow inbound UDP 33333 in the firewall prompt.

### 6.6 Remaining risks (carried over from `voice-udp-channel.md` §8)

1. Needs the same subnet; if the router has **AP isolation (client isolation)** enabled, the beacon cannot reach the device → OFFLINE forever.
2. **No encryption, no authentication** (the only security regression this time): any host on the same subnet can forge a beacon / downlink JSON.
3. Single-device assumption (fixed port 33333; two sets of devices behind the same gateway would conflict).
4. The heap headroom with Wi-Fi resident + LVGL + capture coexisting has been measured (131 KB after exit), but **continuous long-duration recording** has not been stress-tested.

### 6.7 Supplementary cleanup (found during this recheck)

The design document §7 planned to remove `ble_audio.c/.h`, but on recheck these two files were found **still in the workspace** (git shows `M`,
i.e. modified but not deleted). It was confirmed that they are **not in SRCS of `main/CMakeLists.txt` and nothing has `#include "ble_audio.h"`**
(only comments mention them in the whole repo) → orphaned dead code, now deleted.

- Backup: `_diag/removed-ble-backup-2026-09-12/` (`ble_audio.c` 747 lines + `ble_audio.h` 128 lines, complete).
- Also recoverable from git history (`git show HEAD:main/apps/voice/ble_audio.c`).
- Rebuilt after deletion to verify (`_diag/build_udp2.log`) → **the build is unaffected** (these two files never took part in compilation).
