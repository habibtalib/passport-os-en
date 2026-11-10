<div align="center">

# Passport OS

**A small device worth keeping in your daily routine.**

Community firmware for the FoloToy AI Passport · ESP32-C3 · 8 MB Flash · 10 functions and apps

**English edition**

[See what it does](#ten-apps-in-your-pocket) · [Install the firmware](#get-started) · [Make your own version](#make-your-own-version) · [Related projects](#related-projects)

</div>

---

> **This is the English-only edition of Passport OS.** The device UI, web setup pages, logs, comments and documentation are all in English. The original Chinese project lives upstream at [manchunx7-bit/passport-os](https://github.com/manchunx7-bit/passport-os). This repo is [habibtalib/passport-os-en](https://github.com/habibtalib/passport-os-en).

## Why this project exists

First, thank you to everyone who has enjoyed and supported the project. The feedback turned something originally made for myself into a reason to keep tinkering.

The idea behind Passport OS is simple: turn this hardware into a small device I would use regularly. After several rounds of experiments and selection, I kept the 10 functions and apps that are useful day to day: chat for a moment, listen to the radio, help with typing, time something, or play a round.

People have suggested many personal improvements. Everyone uses the hardware differently, so rather than cramming every idea into one firmware, the project is open: add the features you like, remove the parts you do not need, and make a version that fits you.

> I hope it becomes both a tool you use every day and a toy worth tinkering with. Have fun!

## Ten apps in your pocket

| App | What it does | Requirements |
| --- | --- | --- |
| Home | Clock, profile, portrait and card, weather and air quality | Network for weather and air quality |
| Radio | Pick a city and station, listen to the radio, set a sleep timer | Wi-Fi and an available online station |
| AI Voice | Use the device as a voice-input tool for your computer, with hotkeys | Windows companion program |
| Xiaozhi AI | Spoken conversations through the microphone and speaker | Network and Xiaozhi service configuration |
| Muse | Send voice to Muse and read the reply on screen | Phone pairing, your own account and a working network |
| Bark Battle | Play a round of a small game with your voice | Works offline |
| Sound Jump | Long sound jumps far, short sound jumps near, stop the sound to jump | Works offline |
| Tetris | The familiar stack, clear lines, play again | Works offline |
| Stopwatch | Timing, splits, pause and resume | Works offline |
| Pomodoro | Focus, rest and a chime when time is up | Works offline |

Beyond the apps, it also includes whole-device features: themes, volume, brightness, auto screen-off, short-press wake, Wi-Fi setup, battery display and power management.

## Related projects

Muse and AI Voice were previously open-sourced on their own, each with its own source, tutorials and companion content:

| Project | Link | Contents |
| --- | --- | --- |
| Muse | [ai-passport-muse](https://github.com/manchunx7-bit/ai-passport-muse) | Muse app module source and usage guide |
| AI Voice | [folo-ai-passport-voice](https://github.com/manchunx7-bit/folo-ai-passport-voice) | Voice source, Windows desktop program and usage guide |

## Get started

**If you only want to use the existing features, you do not need AI tools or a build environment.** When a matching release package is available, download the firmware from [GitHub Releases](https://github.com/habibtalib/passport-os-en/releases). To change features, see the source build steps below.

Supported hardware: **a FoloToy AI Passport of the same hardware revision, ESP32-C3 with 8 MB Flash.** Similar-looking boards cannot be used directly; they may have incompatible displays, audio chips, buttons or partition layouts.

| Your situation | Image to choose | Flash address |
| --- | --- | --- |
| First installation, or to see the full Wi-Fi setup flow again | `*-full.bin` full image | `0x0` |
| Passport OS already installed and the partition layout is confirmed identical | `*-app.bin` application image | `0x10000` |

> **Know the difference between a full install and a normal upgrade.** The full image overwrites configuration in the range it writes; a complete reset also requires erasing flash. To keep Wi-Fi setup, profile and images, back up first, confirm the partition layout is identical, then use the application image; do not erase the whole flash first.

For file selection, Windows flash commands and common problems, see the [community install guide](docs/open-source-guide-draft-20261025.md).

<details>
<summary><strong>First boot: how do I connect my phone?</strong></summary>

1. Connect your phone to the `Passport-WiFi-xxxx` hotspot the device creates; no password is needed.
2. Open `http://192.168.4.1` in a browser.
3. Follow the page to pick Wi-Fi and set your city and profile; add a portrait and card if you like.
4. When setup is done, return to Home and start using it.

Xiaozhi and Muse need your own service configuration; when Muse uses a proxy, enter the proxy host's LAN IP and HTTP port. AI Voice also needs the Windows program from the related project above.

</details>

## Make your own version

**This repository contains the full firmware source.** Starting from the existing features, you can change the UI and interaction, add or remove apps, then build and flash to matching hardware.

**Get the source -> change features -> build the firmware -> flash the device -> try it on the real hardware.**

### Where to start

| What you want to change | Look here first |
| --- | --- |
| The UI or logic of an app | [`main/apps/`](main/apps/) |
| App entry, switching, settings and system interaction | [`main/launcher/`](main/launcher/) |
| Shared UI and font resources | [`main/ui/`](main/ui/) · [`main/fonts/`](main/fonts/) |
| Board drivers: buttons, display, audio, battery | [`components/bsp/`](components/bsp/) |
| Wi-Fi setup and connection management | [`components/esp-wifi-connect/`](components/esp-wifi-connect/) |
| Muse integration and protocol adaptation | [`components/passport_muse/`](components/passport_muse/) |
| Host tests and build tools | [`tests/`](tests/) · [`tools/`](tools/) |

### Build the first version

Prepare **Git and ESP-IDF 5.5.3** with its toolchain. On Windows you can build in WSL and flash from a Windows serial port.

```bash
git clone https://github.com/habibtalib/passport-os-en.git
cd passport-os
bash build.sh release
```

The build script loads the environment from `/opt/esp-idf` by default. If ESP-IDF is installed elsewhere, set the path with `IDF_DIR`, for example:

```bash
IDF_DIR="$HOME/esp/esp-idf" bash build.sh release
```

Third-party components are fetched by the ESP-IDF Component Manager from the dependency files. The build verifies distribution defaults, including public mode and empty proxy defaults. After the build you get:

| File | Purpose |
| --- | --- |
| `build-public/FoloToy-AI-Passport-full.bin` | Full image, written at `0x0` |
| `build-public/passport-os.bin` | Application image; for upgrades with an identical partition layout, written at `0x10000` |

### After changing, check first

In an environment with ESP-IDF 5.5.3 activated:

```bash
./tools/validate.sh --static
./tools/validate.sh --firmware
```

The second command rebuilds and verifies the full image, writing it to `build/FoloToy-AI-Passport-full.bin`. A passing build does not replace testing on the device: check the buttons, display, audio, Wi-Fi setup and whatever you changed.

<details>
<summary><strong>Ready to share your own firmware package?</strong></summary>

The local packaging tool can bundle a source snapshot, a manifest and a SHA256 checksum list:

```bash
python3 tools/package_firmware.py --date YYYY-MM-DD --verification /path/to/verification-public.json
```

Replace the date and verification file path with real values. The verification record must match the current firmware version and contain no private data. The packaging tool does not publish a GitHub Release automatically; after source changes, rebuild and verify again, and do not reuse validation results from an older version.

</details>

## Share code, keep your own keys

Release builds enable `PASSPORT_PUBLIC_RELEASE=ON` and clear the default proxy configuration. Each user enters their own Wi-Fi, account and service credentials on their own device.

When submitting code, issues or your own variant, do not attach passwords, API tokens, device configuration exports, private logs or personal images. When filing an issue, include the firmware version, hardware model, reproduction steps and whether old data was kept; this makes it much easier to track down.

## License and thanks

The project itself uses the [MIT license](LICENSE). Muse, the Noise protocol and other third-party content follow the `LICENSE`, `NOTICE` and asset notes in their own directories; the usage limits of the Jollybot artwork are in a [separate notice](main/apps/muse/JOLLYBOT-NOTICE.md).

Thanks to FoloToy for the hardware and upstream projects, and to everyone who shares ideas, reports issues and helps improve it.

**It started as a small device I wanted to use myself. Now, make it yours.**
