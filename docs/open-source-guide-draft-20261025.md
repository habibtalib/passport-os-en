# Passport OS Community Install Guide

This is the install guide for ordinary users. Using a prebuilt firmware does not require AI tools: download the firmware, flash it to matching hardware, then complete the first-time Wi-Fi setup. Go back to the [project home](../README.md) for the app list and the DIY development entry points.

## Supported devices

- FoloToy AI Passport
- ESP32-C3, 8 MB Flash
- A hardware revision that matches the `partitions.csv`, screen, audio codec and button circuit in the release package

Devices that look similar but differ in Flash size, screen, pins, audio chip or battery circuit cannot be flashed directly.

## Download the files

Download from the same GitHub Release:

- `FoloToy-AI-Passport-vX.Y.Z-full.bin`: first installation or a complete reset, written at address `0x0`.
- `FoloToy-AI-Passport-vX.Y.Z-app.bin`: a normal upgrade on a device with the same Passport OS partition table, written at address `0x10000`.
- `SHA256.txt`: verifies that the downloaded files are not corrupted or replaced.

Do not mix full images, application images and partition tables from different versions. The full image overwrites device configuration; to keep Wi-Fi, portrait, card or game records, use the application image and confirm the partition table is identical first.

## Windows installation

1. Install Python 3.11 or newer.
2. Open PowerShell and install esptool:

```powershell
py -m pip install esptool
```

3. Connect the device over USB and confirm the serial port in Device Manager, for example `COM3`.
4. Optional: verify the downloaded full image:

```powershell
Get-FileHash .\FoloToy-AI-Passport-vX.Y.Z-full.bin -Algorithm SHA256
```

5. For a first install or to clear data, run:

```powershell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 erase-flash
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  --flash-mode dio --flash-size 8MB --flash-freq 80m `
  0x0 .\FoloToy-AI-Passport-vX.Y.Z-full.bin
```

6. For a normal upgrade, write only the application area:

```powershell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  0x10000 .\FoloToy-AI-Passport-vX.Y.Z-app.bin
```

Do not unplug USB while flashing. After a successful write the device resets automatically; if the serial port shows no output for a while, replug USB and watch the device screen.

## First boot

On first boot the device shows the phone setup page and starts a hotspot with no password, for example `Passport-WiFi-D7A9`:

1. Connect your phone to this hotspot.
2. Open `http://192.168.4.1` in a browser.
3. Save your home Wi-Fi, city and profile.
4. Follow the page to upload a portrait or card; you can skip anything you do not need.
5. After saving, return to Home; the device connects to the network using its own Wi-Fi.

## Features and external services

- Weather and air quality on Home need a network and city coordinates.
- Radio depends on public online station addresses; stations can stop working because of region or service changes.
- Xiaozhi needs a network and the matching server-side configuration.
- AI Voice needs the Windows computer-side bridge program to be run separately.
- Muse needs phone pairing; when using a proxy, enter the proxy host's LAN IP and HTTP port, which depends on the proxy tool.
- Bark Battle, Tetris, Sound Jump, Stopwatch and Pomodoro work offline.

## Build from source

Source developers need ESP-IDF 5.5.3, Python 3, Git, and Linux, WSL or another environment that can run ESP-IDF:

```bash
git clone https://github.com/habibtalib/passport-os.git
cd passport-os
bash build.sh release
```

`bash build.sh release` loads the environment from `/opt/esp-idf` by default; if ESP-IDF is installed elsewhere, set it with `IDF_DIR=/actual/path/esp-idf bash build.sh release`.

This command writes the full image to `build-public/FoloToy-AI-Passport-full.bin` and the application image to `build-public/passport-os.bin`. Before building, ESP-IDF components are fetched according to `dependencies.lock`; do not commit the local `managed_components/` or the generated `sdkconfig` to the repository.

Run verification in an environment with ESP-IDF 5.5.3 activated:

```bash
bash tools/test-host.sh
./tools/validate.sh --static
./tools/validate.sh --firmware
```

## FAQ

**Cannot connect to the hotspot**: confirm the device has booted to the phone setup page; replug USB if needed. The hotspot is an open network and needs no password.

**Old data still appears after flashing**: only the application area was written, or the erase range was incomplete. To experience the full device flow, use the full image and run `erase-flash` first.

**Xiaozhi or Muse cannot connect**: first confirm home Wi-Fi works, then check the service account, the phone pairing state and the proxy host/port.

**Want to keep your own modified version**: change the corresponding app in the source, run the host tests and firmware validation, then build your own application image or full image.

## License and contributing

Passport OS itself is MIT. The Muse Noise protocol code and adaptation layer include Apache-2.0 upstream content; follow the `LICENSE`, `NOTICE.md` and third-party notes in the component directories. Before submitting code, do not include Wi-Fi passwords, API tokens, device logs, personal portraits or local configuration exports.

Issues, hardware compatibility reports and feature improvements are welcome; please state the firmware version, hardware revision, flashing method and whether device data was kept.
