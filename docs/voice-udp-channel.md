# AI Voice · Wi-Fi UDP channel design (2026-09-12)

> This document is a **contract that both the device side and the companion side must follow**. A change on one side must be synchronized to the other.

## 0. In one sentence

Replace the AI Voice uplink channel from BLE with Wi-Fi UDP: the device and the computer are on the same LAN, the companion broadcasts a beacon every second,
the device learns the computer's IP after hearing it, and from then on audio and events go over UDP unicast; **the BLE layer is removed entirely**.

---

## 1. Why switch (the current cost of BLE)

| Cost | Evidence |
| --- | --- |
| Entering voice kills Wi-Fi, so all other apps are disconnected | `app_voice.cc` `suspend_wifi_driver()` → `StopStation()` + `esp_wifi_deinit()` |
| The NimBLE controller stays resident at ~44.7KB, leaving a largest contiguous block of only ~7KB | Original comment in `ble_audio.c` |
| Growing the send stack from 4096 to 5120 (+1KB) makes `ble_hs_start` fail → **reboot loop** | Same as above |
| RF interference causes phantom key presses | `app_state.c` piled up 5 sets of mV gates for this |
| Code size | `ble_audio.c` is 747 lines, nearly half of the voice transport layer |

**Historical fact**: this site originally had a Wi-Fi channel, built on **WebSocket + mDNS** (`ws_client.c` / `mdns_resolver.c` /
`wifi_app.c`); it was removed wholesale in a refactor, and the git history was squashed (only 1 commit remains), so it cannot be recovered.
This time it is rebuilt with **UDP**, for these reasons:

- No HTTP Upgrade / mTLS handshake is needed, and the device-side code is about 1/3 the size of WS;
- UDP datagrams have natural boundaries: one datagram is one audio block, so **no fragment reassembly is needed**;
- An ADPCM block carries its own predictor/index header, so **losing a block costs only 100ms and does not lose sync**, which fits UDP's loss model naturally;
  with PCM, a missing byte would instead misalign everything after it;
- The endpoint already exists: the Home page avatar upload has long run over Wi-Fi (on the real device `http://192.168.5.4`), so the Wi-Fi stack is a verified path.

---

## 2. Roles and ports

| Side | Role | Behavior |
| --- | --- | --- |
| companion (computer) | Server | `bind 0.0.0.0:33333`; every **1000ms** broadcasts a beacon to `255.255.255.255:33333` |
| Device (ESP32-C3) | Client | `bind 0.0.0.0:33333` (receives the beacon); after learning the computer's IP, unicasts to `<pc_ip>:33333` |

Port constant: `VOICE_UDP_PORT = 33333` (same port in both directions).

- The device **does not broadcast**: it only needs to listen.
- The companion learns the device address from the source address of `recvfrom`, so the device does not need to announce anything.
- The companion always ignores `type==0x03` (beacon): that is the loopback of its own broadcast, not something sent by the device.

---

## 3. Packet format

```
Each UDP datagram = [1B type][payload]
```

| type | Direction | payload | Total length (including 1B type) |
| --- | --- | --- | --- |
| `0x01` AUDIO | device→computer | `[seq][0x80]` + 804B IMA ADPCM block | 807B |
| `0x02` EVENT | both ways | UTF-8 JSON line (**including the trailing `\n`**) | ≤ 513B |
| `0x03` BEACON | computer→device | UTF-8 JSON `{"v":1,"role":"pc","port":33333}` | ~40B |
| `0x04` PING | device→computer | empty | 1B |

`0x04 PING` is a **pure liveness** packet: while the voice app is running, the device sends one every **2000ms**.
It is consumed directly in the read thread by `udp_transport.py` (refreshing the device address and timestamp) and is **not dispatched to
the relay**, so the relay core does not know it exists. Without it, when the device is idle (not recording, no events) the companion
cannot tell "the device is still there" from "the device left the voice app".

### 3.1 Why AUDIO still keeps the 2B fragment header

In the BLE era the header was `[block seq][fragment seq|0x80 last fragment]`, used so that "a lost fragment corrupts only that block, and later blocks stay in sync".
Over UDP every block is always **a single fragment + last fragment**, so the header degenerates to `[seq][0x80]`, but **keeping it means the companion's
`reassemble_adpcm()` needs no changes at all**: it still terminates and decodes on "new seq + is_last".
`seq` does `++ mod 256` per block, and duplicate/late blocks are discarded by the companion's existing `last_seq` logic.

### 3.2 Why EVENT carries `\n`

Same as BLE: the companion's `reassemble_event()` splits lines on `\n`, with zero changes.

---

## 4. Discovery and link determination

```
companion ──beacon(1s)──► device
                             │ records src_ip + timestamp
                             ▼
                     first time / IP changed / >3s since last → uplink device.hello
                             │
device ──AUDIO/EVENT unicast──► companion (source address is the device address)
```

| State | Determination |
| --- | --- |
| `link_up` | Peer IP learned **and** < `PEER_TIMEOUT_MS` (3000) since the last packet received from the peer |
| `link_up == false` | UI shows an OFFLINE banner, **PTT disabled** (same semantics as BLE "EVENT not subscribed") |
| `connected` | Same as `link_up` (in the BLE era connected and subscribed were two states; there is no such distinction over UDP) |

The device-side external interface maps one-to-one onto the original `ble_audio_*`, so `mode.c` / `app_voice.cc` only need the function names swapped.

### 4.1 How to reconnect (fully automatic on the device / manual restart on the computer)

**Device side: fully automatic, with no key or menu involved.**

- While the voice app runs, the socket stays `bind 33333` listening for beacons (`rx_task` in `udp_audio.c`).
- On receiving a beacon → learn the computer IP → immediately reply with a `device.hello`. Log:
  `companion beacon from 192.168.x.x → link up`.
- If the computer stays silent longer than `VOICE_UDP_PEER_TIMEOUT_MS` (3000 ms, = 3 beacon periods) → `link_up=false`
  → the UI pops up a "Disconnected from computer, reconnecting" banner and **PTT is disabled**.
- As soon as the beacon resumes it continues automatically: `peer_accept_beacon()` notices that "the peer disappeared and came back"
  (interval > `PEER_STALE_US`, effectively 3 s) and **sends hello again**, so the companion learns the device address anew.
- Leaving the voice app clears the peer memory (`udp_audio_deinit()` clears `s_peer_known`), and discovery starts over the next time it is entered.

**Computer side: v1 does not auto-reconnect; after a disconnect the relay must be restarted.**

- The original class comment in `udp_transport.py` reads *"Wi-Fi UDP transport layer (single device; v1 does not auto-reconnect, aligned with BLE semantics)"*.
- If the device is silent for `LINK_TIMEOUT` (6.0 s, = 3 PING periods) → `on_disconnect` is triggered
  → `_handle_disconnect()` in `relay.py` sets `_stop` and pushes a stop into both queues
  → `_drain()` exits → the `finally` in `run()` wraps up the session and calls `t.disconnect()`
  → **`run()` returns and the process ends**.
- So the "reconnect" action = **run `python companion/relay.py` again on the computer**.

The computer-side channel is determined by `companion/config.local.json`:

```json
{ "channel": "wifi", "udp_port": "" }
```

(Leaving `udp_port` empty uses 33333. `_build_transport()` in `relay.py` picks `UdpTransport` accordingly.)

**Three prerequisites for connecting** (if any is not met, it stays OFFLINE forever):

1. The device and the computer are **on the same subnet** (connected to the same router); the router must not have **AP isolation** enabled.
2. The companion is running on the computer, with `channel` set to `wifi`.
3. The Windows **firewall allows inbound UDP 33333** (a prompt appears on first run; click Allow).

**Log reference**:

| Observation | Meaning |
| --- | --- |
| `UDP channel ready (port 33333), waiting for companion beacon` | The device is listening but has not yet heard the computer |
| `companion beacon from 192.168.x.x → link up` | Reconnected successfully |
| Only the first line, never progressing | The computer is not broadcasting / different subnet / blocked by firewall / AP isolation |

> A pitfall hit in testing: the device connected to `ZTE-2302-5G` and got `192.168.5.4`, while the only routable address of that computer at the time was
> `192.168.1.3` (Ethernet, with all Wi-Fi adapters disconnected) → different subnets, so the beacon could never reach the device.
> **The computer must be on the same Wi-Fi/router as the device**; merely "having internet access" is not enough.

---

## 5. Session sequence

```
Device                                        Computer companion
 │  hold UP → local beep → start capture
 │ ── EVENT voice.start(audio=ima_adpcm) ──►   start ASR stream
 │ ── AUDIO ×N (one block every 100ms) ────►   decode block by block → feed Volcano ASR
 │                                             └─ interim results → floating window / downlink transcript
 │  release UP → drain ring → voice.end
 │ ── EVENT voice.end ────────────────────►   send ASR finalization
 │ ── EVENT status(drop=n) ───────────────►   reconcile with the locally received frame count
 │ ◄─ EVENT agent.status(done) ────────────   device replies DONE
```

The downlink (computer→device) has only `type=0x02` EVENT: `transcript` / `agent.status` /
`agent.approval_request` / `time.set`, all posted as events via `app_protocol_parse()`, exactly the same as BLE.

---

## 6. Packet loss and reconciliation

- Each audio datagram = one 100ms block; a block carries its own predictor/index header → **losing a block costs only that 100ms, and later blocks stay in sync**.
- The companion's `_finalize_adpcm_block()` already implements miss counting and "zero-fill to keep the beat"; this change **does not touch it**.
- The device-side `audio_streamer_take_drops()` continues to report with the `status` frame after `voice.end`, so the two-sided reconciliation convention is unchanged.
- New compared with BLE: UDP has no retransmission, so **the loss rate is genuinely reflected in the transcription** (BLE masked it with link-layer retransmission). On a LAN, measured loss should be close to 0.

---

## 7. Removal list / untouched list

**Remove**

- `main/apps/voice/ble_audio.c` / `ble_audio.h` (747 lines) + their entries in `main/CMakeLists.txt` and the `bt` dependency
- `suspend_wifi_driver()` / `resume_wifi_driver()` in `app_voice.cc` (entering voice no longer stops Wi-Fi)
- The half-finished USB branch in `mode.c` (`mode_channel_up(USB)` is always false and was never usable)

**Untouched**

- `app_protocol.c` / `audio_streamer.c` / `adpcm.c` / `app_state.c` / `app_ui.c` / `app_events.c`
  — these are all channel-agnostic, with not a single line changed.

---

## 8. Open items / risks (to be aware of)

1. **LAN prerequisite**: the device and the computer must be on the same subnet. If the router has **AP isolation (client isolation)** enabled,
   the broadcast cannot reach the device → OFFLINE forever. Home routers disable this feature by default.
2. **No encryption, no authentication** (⚠️ the only **security regression** in this change): in the BLE era, pairing encryption prevented eavesdropping and injection;
   UDP is plaintext with no identity check, so any host on the same subnet can forge a beacon to make the device connect to it, or forge downlink JSON.
   This is currently accepted (personal device + home LAN). To tighten it: add a shared secret to the beacon and an HMAC to the UDP payload.
3. **Firewall must be allowed on first run**: Windows blocks inbound UDP 33333; allow it in the prompt (or add a rule manually).
4. **Single-device assumption**: port 33333 is fixed, so two sets of devices behind the same gateway would conflict. This version is designed for a single device.
5. **To be confirmed on real hardware**: the heap headroom of the ESP32-C3 with Wi-Fi resident + LVGL + audio capture coexisting
   (the original BLE approach freed memory via `esp_wifi_deinit()`; now that Wi-Fi stays resident, it must be measured).

---

## 9. Measured results (2026-09-12)

| Item | Before | After |
| --- | --- | --- |
| Firmware size | 3,108,096 B | **2,870,416 B (−232 KB)** |
| App partition free | 26% | **32%** |
| Transport layer code | `ble_audio.c` 747 lines + `mode.c` 55 lines | `udp_audio.c` 348 lines + `mode.c` 56 lines |
| NimBLE symbols in the ELF | present | **zero** (`nm` finds no `ble_audio*` / `nimble_port*` / `ble_gap_*` / `ble_gatts*`) |
| Wi-Fi when entering voice | `StopStation()` + `esp_wifi_deinit()` | **left completely alone** |

Build: `BUILD_EXIT=0` (WSL + ESP-IDF 5.5.3).

> The size reduction mainly comes from the `bt` component no longer being linked: deleting source alone would not drop 232KB; it is the result of the whole NimBLE stack
> dropping out of the link. This also shows that as long as nobody references it, `CONFIG_BT_ENABLED=y` does not bring code into the firmware.
>
> A correction to an earlier optimistic estimate: it was once written that "voice 4.4k lines → ~1.5k lines". Measured, the voice directory (including headers,
> UI and state machine) is still 4,865 lines; what was actually cut is about 400 lines of transport layer and the entire NimBLE dependency;
> the UI / state machine / protocol layer should have stayed all along and should not have been counted as "removable".
>
> Follow-up cleanup (rechecked 2026-09-12): the `ble_audio.c` (747) / `ble_audio.h` (128) planned for deletion in §7
> were still in the workspace at the time (not in CMakeLists, included by nobody); deletion has been confirmed. The voice directory went
> from 4,865 → **3,991 lines** (`_diag/elf_verify2.log`), and neither the firmware size nor the symbol table changed
> (`_diag/build_udp2.log`: still `0x2bcc90` / 32% free).
