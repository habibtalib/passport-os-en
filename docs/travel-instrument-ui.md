# Passport: travel instrument UI

Base: main `d19caf6439bfa64441d458927a83fccc67526634`. The previous functional fixes remain in place. The device now keeps both refined visual directions as selectable themes.

## Dual themes

- **Classic green phosphor**: restores the previous version's near-black green background, phosphor-green text and more pronounced terminal-style borders.
- **Retro travel instrument**: charcoal, warm white and amber, with more open title and content layout.
- Both themes share all pages, fonts, images and display buffers. Only two ten-color palettes and one current-theme value are stored. "Appearance theme" in Settings switches themes instantly; the choice is written to NVS and persists after reboot; restoring default settings returns to the travel instrument theme.
- The user has verified the travel instrument version on real hardware and reported no problems with operation or display. The classic green phosphor theme and the new switching flow still need to be retested on hardware with the final firmware.

## Visual rules

- Charcoal `#181B1C` is the background, warm white `#EEE7D6` is body text, and amber `#DDB36C` is used for focus, gauges and key information. Gray-beige is used for secondary text; soft green is used only for success states.
- Titles use an open layout with a single underline; description text, subtitles and dialog body text do not use closed frames. Input fields, risk warnings and the actual game field keep the borders they need.
- Chinese body text mainly uses the existing 16px font, and page titles keep the existing 20px subset; English and digits use the existing Montserrat 14/20px. No new font files are introduced; the dot-matrix clock is the only large-size gauge exception. The existing Chinese subsets are not forcibly merged for now, to avoid missing glyphs and a new full font.
- Body letter spacing is 0 and title letter spacing is 1px. Long Xiaozhi AI replies use 4px line spacing and 12px paragraph spacing, so the pixel text is not crowded. Fixed-width names and app subtitles are shown with ellipsis.

## Page sync

| Scope | Implementation |
| --- | --- |
| Boot | PASSPORT OS / TRAVEL INSTRUMENT, keeping the bounded title reveal |
| Home | Passport identity label, photo archive, dot-matrix clock; the profile area now uses a bottom divider, the temperature font size is reduced, the travel base map uses amber; the duplicate text shadow is removed |
| Drawer | PASSPORT / TOOLS directory, open icons and titles, frameless English subtitles, thin page tick marks; the eight corner-bracket objects are removed |
| Settings | 16px list body text, left-side selection marker, unified title and action hints |
| AI Voice, all states | Open title, frameless content panel, description text unified at 16px; ready/sending descriptions lose their frames, while emphasis is kept for risk warnings and primary actions |
| Xiaozhi AI | Frameless status line, divider above the body text, widened reading area, increased line and paragraph spacing |
| Radio and Settings/provisioning subpages | Unified colors, keeping the tuning scale and amber pointer; generic panel frames removed, the clock returns to the secondary font size, body text reuses the Chinese 16px font |
| Two games | Synced system colors and hints, thin lines above and below the result page, functional colors of game objects kept; volume track and fill remain distinguishable |
| Phone profile/provisioning pages and success page | Same color scheme, open titles and cards, input fields and buttons keep their interaction borders; uses device-local fonts, no web fonts |

## Resource constraints and verification

No new font bitmaps, image assets, full-screen offscreen buffers or resident background animations were added. The Home page loses one duplicate label and gains one identity label; the Drawer drops eight decorative objects and gains one directory label. Other pages reuse existing widgets, with changes mainly to styles and layout. The number of resident page objects did not grow because of decoration; strings and style attributes may still change memory usage slightly, so the object count cannot be converted directly into measured heap savings.

The dual-theme commit `b5dd137fba4754c16c9d9a63dd1a8ab29243c323` passed a full ESP-IDF 5.5.3 build, image merge and partition protection check. The app is 3,038,992 bytes, 1,008 bytes larger than the single travel-instrument version; about 28% of the 4 MiB app partition remains free. The two palettes themselves take 80 bytes of read-only data, plus one active-theme state value and one NVS key. The screen buffer, hardware pins and partition table are unchanged.

For the build and firmware artifacts, see [GitHub Actions 34987914612](https://github.com/manchunx7-bit/passport-os/actions/runs/34987914612).

Host regression tests pass. Hardware acceptance still requires: page-by-page checks at 240×320 for long names/SSIDs and mixed Chinese/English text, all AI Voice states and long replies, and readability at minimum brightness; cycling through all apps while recording the minimum free heap and maximum contiguous block. Until measured on hardware, peak RAM, power consumption and the rendering of all pages are not claimed as verified.
