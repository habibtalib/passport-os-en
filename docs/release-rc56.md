# Passport OS 0.2.0-rc.56

Local private candidate; not published, not flashed.

## Muse network configuration

- Removed the Muse "use phone hotspot when out" setting, its NVS routing rules and the automatic switching logic.
- Muse still supports a direct connection, and a manually entered computer LAN proxy IP and port.
- The network diagnostics page shows only direct or computer proxy. The device hotspot used for first-time Wi-Fi setup is unchanged.

## Verification scope

- ESP-IDF 5.5.3 / ESP32-C3 public release build passes.
- Firmware layout, app partition headroom, SRAM budget and public defaults checks pass.
- Host regression for the Muse flow on the phone setup page passes.
- This round was not flashed to hardware and had no on-device network acceptance.
