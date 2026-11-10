# Passport OS Open-Source Preparation Audit

Audit date: 2026-10-25  
Audited: the current workspace and firmware 0.2.0-rc.55, just fully flashed to COM3  
Goal: decide what the open-source package should keep and exclude, and how community users download, flash and first configure it.

This report gives the preparation result first. It does not delete workspace content or change current build defaults. Historical candidates, diagnostic evidence and temporary build directories are kept until the maintainer confirms the list and cleanup is done.

## 1. Current firmware identity

| Item | Current value |
| --- | --- |
| Chip | ESP32-C3 |
| Flash | 8 MB |
| Firmware version | `0.2.0-rc.55` |
| Current application image | `passport-os-status-fixed.bin` |
| Application image SHA256 | `112eb7d1cce27e2c47d60b5913f6da954120de3f549e34e02034c74679016466` |
| Current full image | `FoloToy-AI-Passport-full.bin` |
| Full image SHA256 | `985ea2f3afa4cbead16933dad2624a5942c8b86c82719a1d2b789aed3231a0f9` |
| ESP-IDF | 5.5.3 |
| Partitions | factory `0x10000 / 0x400000`, NVS `0x9000 / 0x6000`, storage `0x420000 / 0x200000` |
| Current build switches | `PASSPORT_PUBLIC_RELEASE=ON`, `PASSPORT_RADIO_HLS_TEST=OFF`, `PASSPORT_MUSE_USE_JOLLYBOT=ON` |

The full erase, the 8 MB erase read-back, the full image write and its read-back verification all passed. The first-boot log confirms no old Wi-Fi was auto-connected, the open setup hotspot started normally, and the device stopped at the first-time setup page.

## 2. Features the current firmware actually contains

These features are compiled in `main/CMakeLists.txt` and are part of the current product scope; their source, assets and tests should be kept:

- Home: profile, portrait/card, weather, air quality, city catalog, clock, open phone Wi-Fi setup.
- Launcher and settings: four themes, brightness, auto screen-off, volume, key sounds, wireless network, network and time, timer recovery.
- Radio: city catalog, recommended stations, MP3 playback, volume, sleep timer and network state.
- AI Voice: computer-side voice input/hotkey bridge protocol, UDP audio, time sync and local settings.
- Xiaozhi: WebSocket voice conversation, Opus audio, OTA configuration, conversation state and UI.
- Muse: BLE pairing, Noise protocol, TLS/HTTP proxy routing, chat link and device state.
- Games: Bark Battle, Tetris, Sound Jump; the latter currently draws no predicted trajectory or landing guide line.
- Stopwatch and Pomodoro: timing, background recovery and alert sounds.
- Board support: ESP32-C3 pins, ST7789 display, LVGL, ES8311 audio, ADC three-key input, battery, power management and USB Serial/JTAG.

## 3. Recommended open-source package structure

### Must keep

```text
.
├── CMakeLists.txt / build.sh / dependencies.lock / partitions.csv
├── sdkconfig.defaults / LICENSE / README.md
├── .github/                         # CI, contributing, security and support templates
├── bootloader_components/bl_early/  # current bootloader-stage code
├── components/bsp/                  # current board drivers
├── components/esp-wifi-connect/     # Wi-Fi setup, DNS, station connection and saved Wi-Fi
├── components/noise_core/           # Muse Noise protocol, with Apache-2.0 notice
├── components/passport_muse/        # Muse adaptation layer, with upstream license and NOTICE
├── main/                            # all compiled sources, fonts and built-in assets of the current firmware
├── third_party/china-cities/        # MIT data and license needed to generate the city catalog
├── tests/                           # host tests, simulators and test stubs
├── tools/                           # build, verification, generation and packaging scripts
└── docs/                            # trimmed architecture, feature, release and contribution notes
```

`main/apps/home/cities.json.gz`, `main/apps/radio/radio_cities.inc`, `main/fonts/city_symbols.txt` and `main/ui_sfx_pcm.inc` are generated resources that take part in the build; the generation scripts and source data must also be kept.

### Should not go into the open-source source package

- `build/`, `build-*`, `managed_components/`, `.cache/`, `sdkconfig`, `.elf`, `.map`, `.bin`, `.log`: machine-generated or local configuration. Components are fetched again from `dependencies.lock` and the ESP-IDF Component Manager.
- The historical rc.12-rc.55 archives, old candidate directories and old verification files under `release/`: they are not current source and make it easy for users to flash the wrong version.
- `_diag/`, `output/`, temporary previews, screenshots, private serial logs and device verification directories: keep them in the maintainer's archive, not in the public source package.
- `all_chars.txt`, `ui_chars.txt`, `.impeccable/`, design-process drafts and experimental assets not referenced by the build or tests.
- Old HLS-specific candidates, old release notes, historical visual audits and duplicated feature descriptions: merge into current documents or move to a history archive.
- `web/phone-location.html` is currently embedded neither in the firmware CMake nor in the phone setup page; exclude it unless it is published as a standalone web page.

### Must be handled separately

1. **Muse avatar assets**: the firmware just flashed used the Jollybot frames in `main/apps/muse/muse_avatar_frames.c`; the repository also has `muse_avatar_public_frames.c`, documented as a public geometric avatar under MIT. Jollybot is neither an Apache-2.0 nor an MIT asset and is not relicensed just because the software source uses MIT. Recommendation: switch the public source default to `PASSPORT_MUSE_USE_JOLLYBOT=OFF`, publish the MIT geometric avatar, and keep Jollybot only as a maintainer build option once separate permission is obtained.
2. **Muse upstream code**: `components/passport_muse/LICENSE`, `NOTICE.md` and `components/noise_core/NOTICE.md` must ship with the source; Apache-2.0 code, the Meta upstream origin, local adaptation changes and personal configuration defaults must be described separately.
3. **Fonts and images**: fonts are derived resources compiled into the firmware, so before open-sourcing keep their origin, generation parameters and license notes. Recommendation: add a unified `docs/THIRD-PARTY-NOTICES.md`.
4. **Network services**: weather, IP city lookup, public radio, the Xiaozhi service and the Muse proxy are not hosted by this project; the docs should state network availability, third-party terms, and the account/proxy configuration users take responsibility for.

## 4. Issues to fix before release

- The default of `PASSPORT_MUSE_USE_JOLLYBOT` and the public build path are both currently `ON`, which conflicts with the public avatar notes.
- `dependencies.lock` is ignored by `.gitignore` but is an important input for reproducible builds; the official repository should stop ignoring it and commit it.
- The current workspace contains many uncommitted changes and untracked files, so the current Git HEAD cannot be used directly as the release source.
- The version is still `0.2.0-rc.55`. A formal community open-source release should bump the version separately, keeping source, firmware, release notes and the SHA256 list consistent.
- `tools/package_firmware.py` already has source snapshot and per-file hash capability, but packaging must happen after cleanup; do not reuse historical release ZIPs.

## 5. Recommended community install path

Ordinary users need neither AI nor ESP-IDF: download the `*-full.bin` matching the hardware, verify the SHA256, write it at `0x0`, connect to the open `Passport-WiFi-xxxx` hotspot on first boot, and finish Wi-Fi setup at `http://192.168.4.1`. A normal upgrade writes the `*-app.bin` confirmed against the same partition table at `0x10000`.

Source developers then install ESP-IDF 5.5.3 and run `bash build.sh release` or `./tools/validate.sh --firmware`. The build script regenerates `managed_components` and verifies partitions, app size, SRAM and public default configuration.

## 6. Audit conclusion

The project has the source, build scripts, tests and license basis needed for open sourcing; the problems are too much historical content and the Muse avatar release boundary. Recommendation: first produce a clean public branch, use the MIT geometric avatar by default, keep the complete feature source and necessary tests, then decide whether to publish the Jollybot assets.

This report did not delete anything, change defaults or republish.
