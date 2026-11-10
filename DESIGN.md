---
name: Passport OS shared UI
description: Confirmed 2026-10-05 visual revision of the 240x320 LVGL whole-device UI
colors:
  travel-bg: "#181B1C"
  travel-surface: "#202526"
  travel-surface-alt: "#303738"
  travel-border: "#59605D"
  travel-text: "#EEE7D6"
  travel-text-dim: "#B3B2A5"
  travel-accent: "#DDB36C"
  travel-danger: "#F09B87"
  travel-on-accent: "#181B1C"
  travel-success: "#9DB89A"
  classic-bg: "#03120B"
  classic-surface: "#091E14"
  classic-surface-alt: "#122E20"
  classic-border: "#37694B"
  classic-text: "#A8EDBB"
  classic-text-dim: "#80AF90"
  classic-accent: "#8CF5A6"
  classic-danger: "#FF9585"
  classic-on-accent: "#03120B"
  classic-success: "#8CF5A6"
  classic-warning: "#F2CA6D"
  travel-warning: "#DDB36C"
  playful-bg: "#F5F7F6"
  playful-surface: "#E8EFEC"
  playful-surface-alt: "#D7E9E1"
  playful-border: "#7B8885"
  playful-text: "#252D30"
  playful-text-dim: "#56635F"
  playful-accent: "#A5315B"
  playful-danger: "#AE302E"
  playful-on-accent: "#FFFFFF"
  playful-success: "#286B59"
  playful-warning: "#805710"
  playful-secondary: "#28648B"
  playful-tertiary: "#A64B29"
  neon-bg: "#0A0B10"
  neon-surface: "#17151F"
  neon-surface-alt: "#252B32"
  neon-border: "#596373"
  neon-text: "#F4F4F6"
  neon-text-dim: "#A4AFC0"
  neon-accent: "#38E8E1"
  neon-danger: "#FF747D"
  neon-on-accent: "#10131A"
  neon-success: "#AAEE6D"
  neon-warning: "#FFD45A"
  neon-secondary: "#FF78CA"
  neon-tertiary: "#FFB75E"
typography:
  body:
    fontFamily: "Source Han Sans SC"
    fontSize: "16px"
    lineHeight: "20px"
    letterSpacing: "0px"
  title:
    fontFamily: "Source Han Sans SC"
    fontSize: "20px"
    lineHeight: "24px"
  numeric-label:
    fontFamily: "Montserrat"
    fontSize: "14px"
  numeric-display:
    fontFamily: "Montserrat"
    fontSize: "32px"
rounded:
  pixel: "0px"
spacing:
  panel-inset: "7px"
components:
  travel-top-bar:
    backgroundColor: "{colors.travel-surface}"
    rounded: "{rounded.pixel}"
    width: "240px"
    height: "26px"
  travel-panel:
    backgroundColor: "{colors.travel-surface}"
    rounded: "{rounded.pixel}"
    padding: "{spacing.panel-inset}"
---

# Design System: Passport OS shared UI

## Overview

This round of the visual revision is based on a hands-on review of the whole device and a 13-page interaction preview. The user explicitly asked to keep the original Home layout and drop the boxes; other pages are optimized page by page. The implementation stays within the bounds of the existing physical buttons, real state, audio and game logic.

The shared layout keeps the Travel Instrument and Classic Green themes and adds "Pixel White" (renamed from "Colorful Pixel" in rc.25): a neutral light canvas and pale gray-green surfaces, with raspberry, lake blue, orange and mint green as functional accents. Home uses a dark-scene variant of the same hues. rc.26 adds a high-saturation, multicolor dark theme, "Neon Black". All four themes share layout, fonts and images. At runtime the source of truth is the `UI_*` semantic roles, `ui_theme_color()` and `ui_theme_color_on_dark()`; the values above record the existing palettes and must not be used to bypass the theme API.

**Key Characteristics:**
- Fixed small screen; text, status and physical-button hints come first.
- Square geometry and flat color blocks; shared components do not rely on shadows for hierarchy.
- Apps may have local scene colors; these do not automatically become device-wide colors.

## Colors

### Primary

`travel-accent` / `classic-accent` map to `UI_ACCENT`, used to emphasize status, titles or actions; text on top of it uses `UI_ON_ACCENT`. `UI_SUCCESS` and `UI_DANGER` are existing status roles; in the Classic theme, success and accent share one color value.

### Neutral

Each of the four themes keeps roles for background, surface, selected surface, border, primary text and secondary text. The default theme is Classic Green. The waiting state uses `UI_WARNING` and always shows text as well. Values come from `ui_pixel.c::s_palettes`; roles come from `ui_palette.h`.

### Pixel White

This is an additional theme and does not replace the two existing ones. rc.24 changed the large bright-yellow generic surface to pale gray-green, made auxiliary tracks light mint, and changed the background to a neutral light tone, removing the earlier blue-violet cast. Raspberry marks the selected action and primary accent, lake blue is used for the Stopwatch and voice-type icons, orange for Radio and its icon, and green for success and the matching app icons. Primary and secondary text use neutral dark gray; the battery frame and network signal on the app selection page also use the body text color. Color lands mainly on content and status, while dividers and the top bar are desaturated; no decorative objects, fonts or textures are added.

The A8 portrait and night-scene assets on Home record highlights as opaque pixels, so they need a dark base to keep their original light and shade. The colorful Home uses a `#181B1C` dark base, `#F1EEE6` portrait, `#8DDFC5` base art, `#86C9FF` clock, `#FFAD78` temperature and `#F58FAA` weather phrase. From rc.24 the top bar matches the dark page base, body text is `#E9EDEB`, secondary text `#AFBDB7` and dividers `#53605D`; the signal and battery frame use a light color, and the battery fill changes with the success, warning and danger colors of the dark scene. The dark roles are maintained in `ui_pixel.c::s_playful_dark` and read via `ui_theme_color_on_dark()`; the original themes still return their original palette through that API. The original portrait and night-scene art, layout and uploaded data are unchanged.

`UI_SECONDARY` / `UI_TERTIARY` are two supplementary accent colors; Classic Green and Travel both map them back to their original accent. `UI_INK` in the light theme is a dark ink color, kept separate from the white `UI_ON_ACCENT` text on selected items. Status colors keep their success, warning and danger meanings. Dark game scenes keep using their own light captions and flagpole, and must not directly inherit the dark body text of the light theme.

The Settings page cycles Classic Green -> Travel Instrument -> Pixel White -> Neon Black. Pixel White is still `UI_THEME_PLAYFUL` internally, the NVS values 0, 1, 2 keep their original meaning, Neon Black is 3, and restoring defaults still uses Classic Green.

### Neon Black

rc.26 adds a standalone dark theme. A near-black canvas runs through Home and the top bar, light portraits keep their original highlight mask; the clock is electric cyan, the night scene fluorescent pink, the temperature amber and the weather phrase lime. App icons use functional colors with two-tone details, and the Stopwatch main digits use fluorescent pink. Body text across the device uses neutral white and cool gray.

From rc.27 the shared top bar and bottom divider are a uniform 1px continuous cool-gray line (`UI_BORDER`), dropping the short cyan and fluorescent pink segments that made the line look broken. The top divider spans the full screen width, and the bottom divider keeps the 12px content inset on each side. App icons keep offset short strokes on the outside, and the Settings selection bar has 6px stepped corners cut at the top-left and bottom-right. Icons and the selection bar are drawn in the draw callbacks of existing objects, with no permanent animation, framebuffer, image or font added. The three existing themes keep their original colors, layout and rendering. `ui_theme_is_multicolor()` is used only for the auxiliary colors and app icons of the two colorful themes; the dark-scene and dark-ink mappings specific to Pixel White stay independent.

## Typography

Body text uses `buddy_font_16` and shared titles use `ui_font_20`; both are LVGL font subsets of Source Han Sans SC, and the line heights above come from the actual font structure. They are not webfonts a browser can load directly. Body text uses the default letter spacing `UI_LS_BODY`; the shared title plate uses `UI_LS_TITLE` (1px) on its own, and the Sound Jump overlay panel does not call the title plate, so it does not inherit that spacing.

Numeric labels use Montserrat 14px. The large Stopwatch digits use 32px, dropping to Montserrat 20px when the hour length grows. Do not derive a full device-wide type scale from these app usages. When UI text changes, check the glyph coverage of the corresponding font subset.

## Layout

Shared components are positioned for a 240x320 screen with a 26px top bar. The battery percentage occupies x=160-201 and the icon x=206-232; the remaining top bar content ends at x=155, leaving at least a 4px gap. The time on the app selection page uses a centered slot at x=92, 56px wide, centered on screen x=120. Top bar text is centered by `ui_pixel_status_text_align()` within the 25px area above the divider according to the font line height, and the battery text is positioned on the vertical center of the battery icon. The shared title plate and footer content are 216px wide with 12px on each side; Stopwatch body text uses 16px on each side. These are existing coexisting usages; no unified spacing grid or responsive breakpoints have been abstracted.

The shared footer has two lines: the current action and secondary navigation. Sound Jump inherits that information order but positions itself according to the scene height; its 189px game area, progress and charge bar belong to the app layout, described in `docs/sound-jump.md`.

## Elevation & Depth

The shared backgrounds, top bar, panels and Stopwatch components that were checked separate layers through surface color, thin borders and whitespace, with no explicit shadow styles. Sound Jump uses geometric layering inside the scene to show the thickness of the floating island, and does not register it as a general elevation token.

## Shapes

Shared `block()`, backgrounds and panels have zero corner radius. Ordinary panels in the Classic theme have a 1px border by default, Travel has none by default; callers can override this explicitly. The title plate has a full border in Classic and only a bottom line in Travel. Borderless must not be written as a global rule.

## Components

- **Top bar:** `ui_pixel_top_bar()` uses the surface color and a 1px bottom line, and holds the app name and status.
- **Panel:** `ui_pixel_panel_create()` uses the semantic surface color passed by the caller and the shared inner padding; when selected and enabled, `ui_pixel_set_selected()` switches to a brighter surface and the accent border.
- **Home:** portrait, name and signature on the left; clock, date and weather on the right; the original pixel-art area stays at the bottom. The outer frame and portrait inner frame are removed; long names and signatures are truncated to a single line and the weather description is widened. The portrait and profile are not moved to a new full-width layout.
- **App selection:** no large cards or icon frames; the current icon and name are in the center, adjacent app names show above and below, and only one position number is kept, with no small description text.
- **Key hints:** tool pages use a 272px divider at the bottom, the main action at 276px and the back hint at 298px. The voice page states explicitly: hold Up to speak, Down for Enter. Hints point to the physical buttons; they are not touch buttons.
- **Voice and Xiaozhi:** the voice body is fixed to "AI Voice input" and updates with the real connection state, recording and duration; when the audio channel is not ready, it gives the CABLE Output / CABLE Input selection path. The Wi-Fi signal comes from the actual station connection and RSSI, and is shown separately from the PC relay connection state. Xiaozhi uses the shared top bar and two-line key hints; the idle hint is centered, and during a conversation the body is left-aligned and scrolls to the latest reply; the status line and body have no outer frame, and Up/Down only adjust volume.
- **Radio and Stopwatch:** Radio keeps the scale and the real level bars, with volume as brief feedback; Stopwatch highlights the main time, centiseconds drop one size, and no placeholder row is shown when no split is recorded.
- **Games:** the Sound Jump ready page enlarges the Claude character without changing the game collision size. Bark Battle keeps auto-start on entering the page, and failure highlights the score and play again; Tetris keeps the full 20-row board and cuts tutorial text and secondary stats.
- **Battery and signal:** the shared implementation uses geometric blocks. The battery percentage is fixed to a single right-aligned line with enough room for `100%`; levels 0-15 use the danger color, 16-35 the warning color and 36-100 the success color, and unknown values show `--`. Zero and unknown values show no fill; do not decorate apps with fake measured values.
- **Settings focus:** the current row on the Settings page uses an accent background with `UI_ON_ACCENT` text, and the row pending confirmation for restore defaults uses the danger color. Before confirming it says "Press again to confirm", and Up/Down cancels.
- **Interaction motion:** app selection uses a 140ms, 6px vertical icon feedback while the text stays still; Settings focus uses a 140ms background fade-in. Continuous input cancels the old animation on the same object, with no back-and-forth or looping animations stacked.
- **Transitions:** keep the previous screen and show the target page once ready; do not add "returning" or "opening" messages, blank transition screens or full-screen fades.
- **Static pages:** battery, signal and connection-dot colors update only when values change; the Radio volume bar keeps a 20Hz update opportunity, and the clock and sleep timer text update at 1Hz.
- **Local game scenes:** Sound Jump reuses the background, top bar, panel and fonts; the character, platforms, notes and trajectory are drawn with LVGL geometry. Fixed colors in the scene do not change with the system theme; this is an app boundary, not a new rule of the shared theme.

## Do's and Don'ts

### Do:
- **Do** read theme colors through the existing UI semantic roles, and keep the differences between the themes.
- **Do** check text coverage, truncation and key hints in actual 240x320 rendering.
- **Do** keep game scene composition, color and animation limited to the app documents.

### Don't:
- **Don't** promote local Stopwatch or game dimensions to a device-wide layout standard.
- **Don't** treat a font subset as a complete font that covers arbitrary text.
- **Don't** describe synthetic fixture atlases as on-device interaction, microphone experience or display verification.
