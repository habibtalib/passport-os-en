# Home page (personal home) rework plan and handoff document

> Goal: add a "Home" app to Passport OS that shows the user's avatar, name, signature, Codex quota (credits), date and weather.
> Core constraint: the ESP32-C3 is single-core with little memory (usable heap about 200~300KB), so every design choice puts stability first.
> Handoff status: the 5 underlying modules plus 5.1~5.5 of Section 5 are all done (quota interface, app_home, Launcher registration,
> CMake, boot into Home).
> **Not yet build-verified**: this machine has no ESP-IDF (historical builds were done in WSL at `/opt/esp-idf`, and `wsl.exe`
> is blocked by security policy, so it cannot be run on its behalf). Please verify in WSL following Section 5.6.
> The two intentional deviations from this document are recorded in [Section 7](#7-deviations-from-this-document-trail).

---

## 1. Key hardware and framework facts (must-read for whoever takes over)

| Item | Value |
|---|---|
| Project directory | `F:/WORK/AI硬件/passport-os` (ESP-IDF ≥ 5.5.0, target=esp32c3) |
| Screen | ST7789P3 240×320 portrait, LVGL 9, RGB565 |
| Flash/partitions | 8MB: nvs 0x6000 / factory 4MB / cardid(nvs) / **storage(SPIFFS) 2MB @0x420000** (unused by the current firmware; the Home avatar will be stored here) |
| App framework | `passport_app_t` (four hooks: init/start/stop/on_key), registered in `s_apps[]` of `main/launcher/app_registry.cc` |
| App switching | `app_registry_switch_to()`: switch to a transition screen → stop the old app (free memory) → start the new app. Long-pressing OK returns to the Launcher globally |
| Launcher | Card carousel UI; on entering an app, `launcher_ui_hide()` deletes the whole UI directly to save memory. A new app must follow the "stop = delete the screen" pattern (see `launcher/settings.cc`) |
| Wi-Fi | `WifiManager::GetInstance()`: `IsConnected()/GetIpAddress()/GetRssi()/SetEventCallback()` (`components/esp-wifi-connect`) |
| Color byte order | LVGL internally uses little-endian RGB565, and `esp_lvgl_port` outputs to SPI with `swap_bytes=true`. **The web-side avatar encoding sends little-endian** (implemented accordingly) |
| Serial commands | u/d/o/l = up/down/OK/long-press OK; '1'-'5' = switch app directly (radio..settings). APP_ID_HOME=7 |
| Console | USB-Serial-JTAG (GPIO18/19); logs go here |

### Chinese fonts (important)
- `radio_font` (16px 4bpp): glyphs are subsetted to a fixed string, **enough only for the Launcher/Settings' own UI, and cannot display user-defined text**.
- `buddy_font_16` (`main/fonts/buddy_font_16.c`, 16px 1bpp): about 4360 common Chinese characters + full-width punctuation + the full ASCII set; **all dynamic text on Home uses it**.
- This font was regenerated this time: added two missing city-name characters (the "zhen" of Shenzhen and the "liu" of Liuyang), the full-width punctuation (! ? : ; — … double and single quotes, book-title marks, ~ · yen sign), and ASCII 0x20-0x7E. The file was replaced in place (1.3MB of source, negligible flash cost).
- **2026-09-12**: the file originally belonged to `main/apps/codex/`; when the AI Device module was removed it was moved to `main/fonts/` (**moved, not deleted**, because both Home and Radio depend on it). Declarations are unified in `main/fonts/app_fonts.h`.
- To add more characters, regenerate with this command (Node 24 is installed, npx is available):
  ```
  npx -y lv_font_conv@1.5.3 --no-compress --no-prefilter --no-kerning --bpp 1 --size 16 \
    --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf \
    --range 0x20-0x7e --symbols "<original symbol string + new characters>" --format lvgl \
    --lv-font-name buddy_font_16 -o main/fonts/buddy_font_16.c
  ```
  (The original symbol string is in the Opts comment at the top of the existing file; just copy it and append.)

### Time
- `main/apps/voice/time_sync.{c,h}`: the time zone is stored in NVS (tz_hour, default +8), read via `time_sync_tz_hour()`. The PC side can set the time over BLE/USB (codex/voice apps).
- **`time_sync_init()` is currently called only by the voice app** (app_voice.cc:211). The Home module just calls it once itself (an idempotent NVS read); this is written into the app_home spec.

### Codex quota data chain (current state, verified)
- The PC-side `codex-buddy/tools/windows_buddy_controller/background_bridge.py` heartbeat carries `tokens` / `tokens_today` fields (sent to the device over BLE).
- Device-side `buddy_protocol.c` parses → `buddy_state.c` reduces → the static `s_codex_state` in `app_codex.cc`.
- The `buddy_ui_snapshot_t` snapshot has `tokens` and `tokens_today` fields (buddy_types.h).
- **A public read function is missing** and must be added in app_codex (see Section 5.1).

---

## 2. Overall design (small-memory strategy)

### Data flow
```
Phone browser (on the same Wi-Fi as the device)
   │  opens http://<device IP> (while in upload mode)
   │  ├─ Image: browser-side crop to 64×64 + encode as little-endian RGB565 → POST /api/avatar (8192B raw byte stream)
   │  ├─ City: browser calls the open-meteo geocoding API to get latitude/longitude
   │  └─ Name/signature/city/lat-lon: POST /api/profile (JSON)
   ▼
Device (Home app)
   ├─ NVS "home_profile": name/signature/city/lat/lon + weather cache wtemp/wcode
   ├─ SPIFFS /storage/avatar.bin: 8192B raw frame (tmp streamed write + atomic rename)
   ├─ SNTP ntp.aliyun.com (started only while Home is active, released on stop)
   ├─ Weather: GET api.open-meteo.com/v1/forecast?current=temperature_2m,weather_code (once every 30 minutes)
   └─ Codex quota: reads the codex app's static state (apps run mutually exclusively, so no concurrency issue)
```

### Memory rules (colleagues must keep these)
1. **Zero image decoding on the device**: scaling/encoding all happens in the browser; the device only receives a fixed 8192 bytes.
2. **All network services on demand**: httpd exists only in upload mode (started by a key press, auto-closed after 10 minutes); SNTP runs only while Home is active; weather is a single GET, with TLS buffers 1024/512.
3. **Stop = clear everything**: delete all UI, join task exit, stop SNTP/upload service (copy the settings.cc + codex stop pattern).
4. **Version numbers instead of polling**: `home_profile_version()/home_weather_version()` refresh the corresponding UI row only when they change.
5. **Do IO outside critical sections**: home_profile's NVS/SPIFFS writes all happen outside the portMUX critical section (on the single-core C3 a critical section blocks scheduling).
6. Stack budget: Home task 4096; httpd 6144 (already in the code).

---

## 3. Completed files (code written, not built)

| File | Contents |
|---|---|
| `main/apps/app_base.h` | Enum gains `APP_ID_HOME` (=7, before APP_ID_COUNT) |
| `main/apps/codex/buddy_font_16.c` | Regenerated: + two city-name characters + full-width punctuation + ASCII (replaced in place) |
| `main/apps/home/home_profile.h/.c` | Profile NVS read/write (portMUX protection + version number); lazy SPIFFS mount (formats once, only if the mount fails); avatar tmp streamed write + size check + atomic rename replacement; interface: `init/get/version/set_text/has_avatar/read_avatar/write_avatar_chunk`. Capacity: name 32B, signature 96B, city 48B, lat/lon 16B (UTF-8 including the trailing 0; over-long input is rejected, not truncated) |
| `main/apps/home/home_clock.h/.c` | SNTP start/stop (idempotent); `home_clock_valid()` (epoch>2023 counts as valid); `format_time/date/weekday` (time zone via `time_sync_tz_hour()`) |
| `main/apps/home/home_weather.h/.c` | Open-Meteo fetch (esp_http_client open/read small-response mode, TLS with the cert bundle); cJSON parsing; NVS cache (shows the last weather when booting offline); WMO code → short English phrase mapping (all glyphs verified to be in buddy_font_16) |
| `main/apps/home/home_upload.h/.cc` | On-demand httpd (GET / page, GET/POST /api/profile, POST /api/avatar with 512B chunked streamed writes); embedded web page: browser canvas center-crops to 64×64 → little-endian RGB565 → fetch upload; city lookup calls open-meteo geocoding (over the phone's network); the URL comes from `WifiManager::GetIpAddress()` |

Notes (points that may need handling at build time):
- `REQUIRES` in `main/CMakeLists.txt` is **missing `esp_http_server`**; it must be added (used by home_upload).
- `esp_sntp.h`, included by `home_clock.c`, is provided by lwip and is usually visible transitively via esp_wifi; if it is not found, add `lwip` to REQUIRES.
- `esp_crt_bundle.h` in `home_weather.c` is provided by mbedtls (REQUIRES already has mbedtls).
- Before showing temperature with the Chinese degree character on the web page, confirm it is in the font (very likely; if not, change to "26C").

---

## 4. Upload mode interaction (design settled)

- Pressing **OK** on Home = turn on upload mode: the screen shows `http://<IP>`, and a phone on the same Wi-Fi opens it as the configuration page; the service auto-closes after 10 minutes of inactivity, and pressing OK again closes it manually.
- When the web page save/upload succeeds → the device-side version number increments → the Home UI **refreshes live** (no need to exit).

---

## 5. Remaining work (execute in order)

### 5.1 Codex quota read interface (app_codex.cc/.h)
```c
// Append to app_codex.h:
// Returns false if the Codex app has not run this session (Home shows "--")
bool codex_app_get_usage(uint64_t *tokens_today, uint64_t *tokens_total);

// Implementation in app_codex.cc:
bool codex_app_get_usage(uint64_t *today, uint64_t *total) {
    static bool s_ready = false;           // set to true in codex_app_start
    if (!s_ready) { if(today)*today=0; if(total)*total=0; return false; }
    buddy_ui_snapshot_t snap;
    buddy_state_snapshot(&s_codex_state, &snap);
    if (today) *today = snap.tokens_today;
    if (total) *total = snap.tokens;
    return true;
}
```
Concurrency safety note: apps run mutually exclusively via the registry (codex has necessarily stopped before switching to Home), so the snapshot read has no race.

### 5.2 app_home.cc (core UI; follow the settings.cc structure)
Create `main/apps/home/app_home.cc` + `app_home.h` (exporting `g_home_app`). Spec:

**Static resources**
- `static uint8_t s_avatar_buf[8192];` (statically allocated to avoid fragmentation)
- `static lv_img_dsc_t s_avatar_dsc = { .header = {.w=64,.h=64,.cf=LV_IMG_CF_TRUE_COLOR}, .data = s_avatar_buf, .data_size = 8192 };`
- `LV_FONT_DECLARE(buddy_font_16);`
- Colors follow the system: bg 0x0A0F14 / card 0x121B22 / border 0x22323D / text 0xF0F4F8 / dim 0x7E92A2 / accent 0xFFB74D.

**Layout (240×320)**
```
y8    Left: "Sep 12 Sat" (buddy16)     Right: battery percentage (montserrat_14, optional)
y40   Avatar 64×64 (24,40); to the right at x100: name y44 (buddy16), signature y72 single-line ellipsis (LV_LABEL_LONG_DOT)
y130  Large clock "12:34" centered (montserrat_20; "--:--" if time not set)
y168  Weather row centered (buddy16): "Shanghai Cloudy 26°"; with no city shows "Set a city to show weather"
y210  Codex card 208×34: left "Codex credits", right the value (≥10,000 shown abbreviated, e.g. 12.3K)
y252  Status row: in upload mode shows the URL (montserrat_14); otherwise shows the Wi-Fi/offline hint
y300  Bottom hint (buddy16): "Up: refresh  Down: details  OK: upload"
```
With no avatar: `memset(s_avatar_buf, dark fill)` + 2 lv_obj drawn on top as a person silhouette (round head + semicircle shoulders), adding no image assets.

**Background task** (xTaskCreate 4096 stack, priority 4, 1s loop)
- Every second: `home_clock_format_*` refreshes the clock/date (skip set if the string has not changed; LVGL diffs internally)
- When `home_profile_version()` changes: first, **without holding the LVGL lock**, `home_profile_read_avatar(s_avatar_buf)`, then take the lock and invalidate
- When `home_weather_version()` changes: refresh the weather row
- Weather refresh schedule: first at 15s after start, then once every 1800s; only initiated when `WifiManager::IsConnected()`; HTTP blocking inside this task is acceptable (2~8s)
- Wi-Fi ready → `home_clock_start()`; disconnected → `home_clock_stop()`
- Upload mode 10-minute timeout automatically calls `home_upload_stop()` and refreshes the status row

**Keys** (on_key, handles only BTN_EVT_CLICK; long-press OK is taken over globally by the registry)
- UP: reload avatar/profile immediately + trigger one weather refresh
- DOWN: the bottom detail row toggles between "Codex today/total" and "weather code + cache time"
- OK: toggle upload mode (on: `home_upload_start()` + show the URL; off: `home_upload_stop()`)

**Lifecycle**
- `init()`: `home_profile_init(); home_weather_init(); time_sync_init();` (idempotent)
- `start()`: build UI → start the task
- `stop()`: stop the task (atomic flag + wait for the handle to clear) → `home_upload_stop()` → `home_clock_stop()` → delete the screen
- `g_home_app`: `.id=APP_ID_HOME, .name="Home", .tag="Wi-Fi", .theme_color=0xFFB74D`

### 5.3 Launcher registration (launcher.cc)
- `extern const passport_app_t g_home_app;` + add to `s_apps[]` (**put it first**, so the carousel's first card is Home)
- Add `case APP_ID_HOME: return "Home";` to `app_chinese_name()` (the font coverage for this name was verified earlier)
- Add an APP_ID_HOME geometric icon (a small house) to `draw_app_icon()`: three decreasing rectangles as the roof (16,30,56,8)/(22,22,44,8)/(30,14,28,8) r2 + body (20,38,48,32) r4 + door (44,48,12,22) r2 + window (32,44,8,8) r4
- kMaxApps=8; with this there are 7 cards in total, so no change is needed

### 5.4 CMakeLists (main/CMakeLists.txt)
- Append to SRCS: `apps/home/home_profile.c`, `apps/home/home_clock.c`, `apps/home/home_weather.c`, `apps/home/home_upload.cc`, `apps/home/app_home.cc`
- Append `"apps/home"` to INCLUDE_DIRS
- Append `esp_http_server` to REQUIRES (and `lwip` if needed)

### 5.5 main.cc: boot into Home
- After `app_registry_init();` add `app_registry_switch_to(APP_ID_HOME);`
- Optional: map '6'/'7' to APP_ID_HOME in serial_cmd_task for easier debugging

### 5.6 Verification checklist
1. `idf.py -B build build` (first deal with the build issues in the Section 3 notes)
2. After flashing, check the serial output for: `home_profile`/`home_clock`/`home_upload` logs, and the registry's "Free heap after app stop" line
3. Function: placeholder display when unconfigured → upload avatar/profile from the phone → live refresh → profile persists after power cycle → date/weather appear over Wi-Fi → credits shown once Codex has connected → long-press OK returns to the Launcher, re-enter from the Home card (memory reclaimed normally, no sign of leaks after 10 repeated switches)
4. Stress: drop the network in upload mode / interrupt an upload midway (the old avatar must stay intact); SPIFFS auto-formats once on a first mount failure and is usable afterwards

---

## 8. Interaction model change: Home goes from "a card" to "the base" (2026-09-12)

In the original implementation Home was one of the 7 cards in the Launcher carousel. It has been changed so that **Home is the base, and the app board is the layer below it**:

```
        ┌── Home (APP_ID_HOME, base) ──┐
        │   OK short press ↓           │
        │   App board (Launcher, 6 cards) │
        └──────────────────────────────┘
   Long-press OK: from any screen (including the app board) → back to Home
```

Changes:

| File | Change |
|---|---|
| `app_registry.cc` | `s_apps[]` drops `g_home_app` (carousel 7→6 cards) |
| `app_registry.cc` | **`get_by_id()` gets an `APP_ID_HOME` special case**: Home is not in `s_apps[]`, and without this special case `switch_to(APP_ID_HOME)` cannot find the instance and the screen goes black (the pitfall most easily stepped on this time) |
| `app_registry.cc` | The long-press OK interception condition changes from `!= APP_ID_LAUNCHER` to `!= APP_ID_HOME`, so the app board is also intercepted |
| `app_home.cc` | OK short press → `app_registry_switch_to(APP_ID_LAUNCHER)`; **long-press Down** → toggle upload mode |
| `app_home.cc` / `launcher.cc` | The bottom hint becomes two lines, explaining the new key layout and how to go back |
| `main.cc` | Serial debug key `k` added = long-press Down (otherwise upload mode cannot be verified without the physical keys) |

Key convention: follows the project's established rule that "one key carries only click + long-press"; no double-click.
Home: Up = refresh, Down = detail toggle, long-press Down = upload mode, OK = app board.

## 7. Deviations from this document (trail)

1. **The Home task stack is 6144, not the 4096 in Section 2.** This task runs the HTTPS of `home_weather_refresh()`,
   and the mbedTLS handshake call chain risks overflowing a 4KB stack. Spending 2KB more buys no crash.
   If weather is later moved to its own task, it can drop back to 4096.
2. **The `buddy_font_16` declaration is wrapped in `extern "C"`.** Section 1 only wrote `LV_FONT_DECLARE(...)`,
   but `buddy_font_16.c` is a C file without `extern "C"` protection, and looking up the symbol from `app_home.cc` (C++)
   with C++ linkage is risky, so it is wrapped explicitly.
3. **REQUIRES additionally gets `spiffs` and `lwip`.** Section 3 only mentioned `esp_http_server`:
   `home_profile.c` uses `esp_spiffs.h`, and `home_clock.c` uses `esp_sntp.h` (provided by lwip).
4. **The weather row and status row are forced to a single line with ellipsis (`LV_LABEL_LONG_DOT`).** A city name is at most 47 bytes,
   and the default wrap mode would break the layout and push into the Codex card below.
5. **The weather text buffer is 96 bytes** (not 40): the worst case of 47 (city) + 1 + 15 (weather phrase) + 6 (temperature) would overflow.
6. `home_start()` finishes reading the profile and avatar **before** taking the LVGL lock, since the first SPIFFS mount can take tens of milliseconds.

## 6. Known risks
- Codex credits only have a value once the PC-side background bridge has connected over BLE; otherwise showing "--" is expected behavior.
- Weather goes over HTTPS (the full cert bundle is enabled); on the C3 the TLS peak takes about 40~60KB of heap, occurring only transiently inside the Home task; if it is still tight, buffer_size can be reduced to 512.
- open-meteo is generally reachable from within China; on failure the last cache stays displayed, with no retry storm (it naturally retries 30 minutes later).
- The 8192B static avatar buffer stays resident in .bss; if changed to heap allocation, be sure to free it on stop and null the pointer.

## 9. Home page visual redesign (2026-09-12)

The first version of Home worked, but its layout was a "stack of form fields": the y coordinates 8/40/130/168/210/252 were filled in casually,
with spacing 32→90→38→42 irregularly sparse and dense; only the Codex card had a background, everything else was bare gray text,
lacking a unified language. Goals of the redesign: establish a visual center + a unified card language + widen the weight hierarchy.

### Layout (240×320)

| Block | y range | Contents |
| --- | --- | --- |
| Status bar | 0–28 | Date, Wi-Fi status dot, mini battery (outline + inner bar scaling with SOC + percentage) |
| Identity card | 32–116 | Accent-color frame + 64×64 avatar (person silhouette when there is no avatar), name, signature |
| **Large clock** | 120–192 | 48px hour / colon / minute, three flex-centered segments, the colon breathes every second |
| Weather row | 192–216 | City (dim) + weather phrase (secondary) + temperature (accent color, 20px) |
| Codex dual cards | 226–282 | Two 104×56 stat cards of the same style: "Today" and "Total" |
| Bottom row | 290–312 | Three-state rotation (Down key): action hint / upload hint / IP + weather code |

### Key changes

1. **Enabled `CONFIG_LV_FONT_MONTSERRAT_48`** (changed in both `sdkconfig.defaults` and `sdkconfig`).
   The large clock is the pivot of the whole redesign: before, the largest size was montserrat_20, and time digits that small could not carry the layout.
   The glyph data, about 100KB, lives in flash `.rodata`, mapped directly via the MMU, so it **does not use SRAM** (measured
   free heap after the redesign is 132KB, only 3KB less than before). The C3 has no PSRAM; never read the glyph data into memory.
   `lv_font_montserrat_48.c` has `line_height = 52`, so the clock container is given a height of 72 and will not be clipped.
2. **Layout gridded**: all elements hang off the `kMargin` / `kContentW` constants, with no more scattered hard-coded y values.
3. **Text in four tiers** (kText / kSub / kMuted / kFaint), so every block has a clear primary and secondary;
   cards uniformly use a `kCard` background + 1px very dark `kLine` outline, defining edges by luminance difference rather than bright lines.
4. **The clock is split into three segments** so the colon can breathe every second. It uses `text_opa` instead of changing the text: the colon width stays the same,
   so no flex relayout jitter is triggered (this is the only animation on the whole screen).
5. **The clock and weather row are centered with flex containers**, without hand-computing the digit advance widths of the 48px font
   (advances differ by font size, and hand calculation is bound to misalign at some size).
6. **Bottom row three-state rotation**: the upload hint used to take the second row and is now folded into the rotation, and the saved space went to the stat cards.
   Upload mode has the highest priority and is recomputed every second, which also fixes the old problem of "showing an empty
   `http://` when Wi-Fi has just connected but has no IP yet".
7. **Temperature is shown as `24°` in montserrat_20**, instead of the Chinese bitmap degree character, since the digit glyphs are finer;
   `°` (0xB0) is within montserrat's Latin-1 coverage.

### Interaction (unchanged)

Up = refresh, Down = bottom row three-state rotation, long-press Down = upload mode, OK = app board.

### Measured

- Firmware 0x3010A0 ≈ 3.01MB, app partition 4MB, 25% remaining.
- Boot → Home 132KB; press OK → app board 100KB; long-press OK → back to Home 87KB (basically the same as the pre-redesign
  135→100→89KB, no new leak).

## 10. Whole-device pixel UI polish (2026-09-13, build passed)

Goal: fix the "good pixel-art foundation but the picture feels static/ordinary" problem. All changes are **composed from objects + low-frequency
lv_anim**, with no canvas and no image assets; when a screen is deleted, its animations are cleaned up by LVGL together with the objects
(`lv_obj_destructor` → `lv_anim_delete(obj,NULL)`, `lv_obj.c:614` checked).

> **Build result** (WSL Ubuntu-24.04 `/opt/esp-idf`; the default Ubuntu distribution's ext4.vhdx is corrupted
> and cannot start): firmware 0x2F0AA0 ≈ 3.08MB, app partition **26%** free, zero warnings. Log:
> `_diag/build_ui_polish2.log`. All remaining verification is on the real device (see 10.7).

### 10.1 Day/night theme (`ui_pixel.c/.h`)

- `ui_pixel_set_hour_theme(hour)`: hours 19~23 and 0~5 are night; `hour<0` (time not set) keeps
  day; returns true when the theme flips.
- `ui_pixel_theme_apply(scr)`: recolors the background of "the screen that most recently created `ui_pixel_background`"
  to the current theme (returns whether it took effect). Internally it uses a 46-entry **background-part registry**
  (`UI_BG_PARTS_MAX 64`) recording each color block's day/night colors and visibility.
- Night only changes "ambient colors": sky `0x0D2144`, grass `0x2A5220`, clouds tinted moonlight gray `0xA9BDD9`;
  paper cards/ink outlines stay as they are.
- New background elements: 5 stars (night only, twinkling in turn), 2 slowly drifting clouds (52s/68s per pass,
  bottom edge showing below the top bar), 2 small flowers at the two ends of the grass (day only, x2 and x229: the footer white text, centered,
  occupies x12..228, so the two ends are free), and 2×2 fireflies patrolling along the turf with fading in and out (night only, y313 below the text band).
- Wiring: Home `create_ui` sets the theme by the current hour, and the task's 1s tick checks for crossing the 19/6 o'clock boundaries;
  the Launcher checks at every card selection (`update_card_content`). The other apps (Settings/Xiaozhi AI/games)
  inherit the most recent theme and benefit automatically.

### 10.2 Weather pixel icons + spinner (`app_home.cc`)

- WMO code → block-composed drawing: clear (0) / partly cloudy (1,2) / overcast (3) / fog (45,48) / rain (51-67,80-82) /
  snow (71-77,85,86) / thunder (≥95), drawn in the 22×18 icon area on the left of the weather card (the `wicon_*` functions).
- While fetching, a four-dot spinner is shown, advanced one notch per second by the Home 1s tick; whether it succeeds or fails, a refresh happens once
  when the fetch ends (on success the icon changes, on failure the spinner is withdrawn).
- City name/weather phrase use fixed widths of 54/78px + DOTS truncation, and the row container has pad_left 28 to make room for the icon;
  the row height takes the content's net height of 18px to guarantee vertical centering.

### 10.3 Other Home changes

- The empty state without an avatar now uses the device-wide mascot (`ui_pixel_mascot_create`, 38×48 with blinking), replacing the gray silhouette.
- The weather row changes from a radius 9 rounded capsule back to a **square-cornered paper card + hard shadow** (`make_panel`), and the avatar ring
  goes radius 4→0: the whole device uses one "all square corners" pixel language.
- The bottom white text gets an ink-colored offset (+1,+1) hard shadow (`footer_set` updates both copies).

### 10.4 Launcher

- Card-selection animation `card_nudge(dir)`: the card and its hard shadow are nudged 8px in the switching direction and bounce back
  (70ms+110ms playback), and the icon hops in place (16→11→16). Pure lv_anim.
- The shadow block becomes the static handle `s_card_shadow` so it can be animated; `launcher_ui_hide` nulls it.
- The Launcher now includes `apps/home/home_clock.h` (reads the system time only, does not start SNTP).

### 10.5 `ui_pixel_hint_pill` behavior change

Hint text is now drawn twice (ink shadow + white text). launcher/settings both set the text once at creation, so no
change was needed; **if the hint text is ever changed dynamically, both labels must be changed together** (the function comment says so).

### 10.6 `buddy_font_16` upgraded to 2bpp

The 1bpp jagged edges were one of the main sources of the "ordinary feel". Regenerated with the same symbol set (13KB of symbols, extracted from the old file's header
Opts) using `--bpp 2`, replacing `main/fonts/buddy_font_16.c` in place
(source 31k → 47.6k lines, binary about +140KB flash, ample app partition headroom; the declaration and
`app_fonts.h` are unchanged). The old 1bpp backup: `/tmp/buddy_font_16_1bpp_backup.c`.
If the regeneration command in Section 1 of `home-page-plan.md` is used again, change `--bpp 1` to `--bpp 2`.

⚠️ **The header include of a font file generated by Windows npx is `lvgl/lvgl.h` (the else branch),
and this project does not define `LV_LVGL_H_INCLUDE_SIMPLE`, so it fails to compile** — after generating, change the
else branch to `#include "lvgl.h"` (generated by lv_font_conv on the WSL side this problem does not occur,
both branches are `lvgl.h`). Corrected accordingly.

### 10.7 Verification checklist (built; remaining items run on the real device)

1. ✅ `idf.py -B build build` passes with zero warnings (`_diag/build_ui_polish2.log`).
2. On boot, check Home: blue background by day / deep blue at night + twinkling stars + fireflies (requires SNTP; when time is not set it should be day).
3. Watch the spinner turn during a weather fetch, and the matching icon appear when done; change the city → different icons such as clear/rain.
4. With no avatar (clear NVS `home_profile`), check the mascot empty state; after uploading an avatar the photo should cover it normally.
5. Press Up/Down repeatedly in the Launcher: card nudge + icon hop, with no residual displacement on rapid presses (anim delete as a safety net).
6. Enter and leave Home/Launcher repeatedly ×10 and check that the heap is stable (new static objects about +2~4KB per screen).
7. Font: Chinese edges should be visibly smoother; if 2bpp rendering misbehaves (garbled screen/misalignment), roll back to
   `/tmp/buddy_font_16_1bpp_backup.c`; the other changes do not depend on 2bpp.

## 11. Green-phosphor lock-screen Home (2026-09-13 late night, built and flashed)

Following a retro-terminal-style reference image supplied by the user (a green-phosphor dot-matrix lock screen), the whole Home page was changed to **black background + monochrome
phosphor green**. Other apps are unaffected (the ui_pixel day/night pixel style remains).

### 11.1 Layout (240×320)

| Area | Position | Contents |
| --- | --- | --- |
| Status bar | y0..24 | Three Wi-Fi cells (left) + battery % (right), thin phosphor line frame |
| Left card | x6 y28 104×180 | Portrait area 96×120 (inset frame) + name + signature |
| Clock card | x114 y28 120×50 | Dot-matrix HH:MM (5×7 glyphs, each cell a 4×4 light dot, colon breathing) |
| Date card | x114 y82 120×44 | Date / location pin + city |
| Weather card | x114 y130 120×78 | Monochrome icon + temperature / phrase / divider / wind direction · wind force |
| Bottom art | y212..320 full width | 240×108 A8 artwork, with the hint black bar covering the lower edge |

### 11.2 Key implementation

- **Art = A8 alpha image + image_recolor (phosphor green)**. The software renderer only recognizes A8's
  "mask + solid color" fast path (`lv_draw_sw_img.c:233`); **I1 does not work**: the SW renderer treats I1
  as a luminance image (0→black, 1→white) and does not look up the palette (`lv_draw_sw_blend_to_rgb565.c:460`).
- The two built-in bottom images (city night view / mountain-lake moonlight) are procedurally drawn by `tools/gen_band_art.py` and
  generated with **Bayer ordered dithering** (`main/apps/home/band_art.c`, 25.9KB flash each, zero RAM).
  The style is a high-contrast poster look: silhouettes pure black, lights and moon pure bright, only the sky uses a regular dot pattern. The first version used
  Floyd-Steinberg, which turned the whole image into noise with no recognizable content, so it was dropped. To change scenes or add images, edit the script and rerun;
  do not hand-edit the C file. City scene elements: TV tower / river bridge / building windows lit / moon reflection; mountain-lake scene:
  big moon / three layers of ridges / pine forest / small boat with fishing lights.
- **Long-press Up = switch bottom image** (city → mountain lake). User-uploaded bottom images / portrait-format portraits (96×140 1bpp)
  are left for the next step: extend the home_upload web-side crop + dithering, with the pipeline copied from the 64×64 avatar.
- **Monochrome avatar**: the upload pipeline is unchanged (64×64 RGB565), and the device-side `convert_avatar_to_a8()`
  converts it to A8 by a luminance threshold (4KB .bss) → a phosphor dot-matrix photo.
- **Wind direction**: open-meteo adds `wind_direction_10m`, NVS key `wdir`,
  and `home_weather_wind_dir_text()` gives 8 compass directions in English (North wind / Southeast wind ..., glyphs verified to be in the font).
- The dot-matrix clock glyphs are unchanged (`kDigitGlyphs`); rendering changes from "merged into blocks" to "dot by dot".
- Color palette converged: kBg 0x03120B / kCardBg 0x051910 / kPhos 0x8CF5A6 / kPhosDim 0x3F8F5F /
  kPhosBorder 0x2E7C4E. Home no longer calls the ui_pixel day/night theme (the ui_pixel theme is left for other apps).
- Firmware 0x2FDA70 ≈ 3.13MB, app partition 25% free (+52KB = two A8 bottom images).

### 11.3 Portrait-format portrait (2026-09-13 late night, second round, built and flashed)

The portrait photo is upgraded from a 64×64 square image to a **96×120 portrait format** (exactly filling the Home portrait frame), and the format changes to
**1bpp packed** (browser-side crop + luminance threshold + packing, only 1,440B; the old 23KB RGB565 pipeline is dropped):

- Storage is renamed `portrait.bin` (the old `avatar.bin` is automatically invalidated by the size check 8192≠1440, so just upload
  once again; the old file stays in SPIFFS harmlessly).
- Web side: proportional "cover" scaling to 96×120, center crop → luma≥110 threshold → row-wise MSB packing.
- Device side: `expand_portrait_to_a8()` expands to A8 (11.5KB .bss) → recolor phosphor green.
  The total `.bss` is about the same as the old scheme (1.4K + 11.5K ≈ old 8K + 4K).

### 11.4 Next steps (not done)

1. Custom bottom image upload (240×108 1bpp dithered, the same pipeline in the browser; the device side needs ~26KB RAM
   to expand to A8, or render by expanding 1bpp row by row directly, which needs a measured heap-headroom check).
2. Large name font: render the name on the web side into a 1bpp image strip and upload it, bypassing the device font limitation.
