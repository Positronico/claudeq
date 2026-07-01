# Claudeq — hardware alternatives (quick-swap reference)

Claudeq's reference board is the **Waveshare ESP32-S3-Touch-LCD-3.49** (172×640 portrait, AXS15231B).
This lists boards you could swap to and roughly what each port costs. Nothing about Claudeq's *design*
is board-locked — the whole **bridge / protocol / hooks / launcher and most of the LVGL UI logic are
hardware-agnostic**. Difficulty is driven almost entirely by **one thing: the display + touch
controller** (and audio, only if you use voice).

## What makes a board portable (must-haves)
- **ESP32-S3 (Xtensa)** — keeps the native `esp_wifi` stack, the ESP-IDF build target, and
  microlink/Tailscale working. (A different chip — ESP32-P4/RISC-V, RP2040, STM32 — changes everything.)
- **≥ 8 MB PSRAM** — microlink (Tailscale) needs it, as do the LVGL framebuffers.
- **Native 2.4 GHz WiFi** — the S3 has it onboard (no WiFi co-processor).
- **A touch LCD.** If it's the **AXS15231B** (QSPI panel + I²C touch, same as the deck), the display
  driver in `main.cpp` works as-is → only resolution + layout change.
- *Optional:* **ES8311 + ES7210** codec (speaker + mic) — what powers voice dictation + alert sounds.
  Boards without a mic codec still work for everything visual/touch/network; voice input just won't.

## Tiers by port effort

| Tier | Board | Chip / WiFi | Display | What the port takes |
|---|---|---|---|---|
| **0 — drop-in** | **Waveshare ESP32-S3-Touch-LCD-3.49** (any variant: Case A/B, battery/EN) | S3 / native | 172×640 **AXS15231B** | none — flash the current image |
| **1 — dims + pinout** | **Waveshare ESP32-S3-Touch-LCD-3.5B** | S3 / native | 320×480 **AXS15231B** | LCD dims + LVGL relayout + pin map |
| **1 — dims + pinout** | **LilyGO T-Display-S3-Long** | S3R8 / native | 180×640 **AXS15231B** | closest analog (172→180); pin map + minor layout. Usually **no mic codec** |
| **1 — dims + pinout** | **Guition JC3248W535 / JC3248W535C** (also sold as DIYmalls / Surenoo) | S3-WROOM-1-N16R8 / native | 320×480 **AXS15231B** | cheap twin of the 3.5B; pin map + relayout. Usually **no mic codec** |
| **2 — new display driver** | Waveshare 4.3 / **4.3C** / 7 | S3 / native | 800×480 / 1024×600 **RGB** | swap `esp_lcd` panel init to RGB + relayout. **4.3C has ES8311+ES7210** (good for voice) |
| **2 — new display driver** | **M5Stack CoreS3** | S3 / native | 320×240 **SPI (ILI9342)** | new panel/touch init; **has mic + speaker**, but small screen |
| **2 — new display driver** | **Sunton ESP32-8048S043** & kin | S3 / native | 4.3″ 800×480 **RGB** | very cheap/common; RGB panel driver |
| **2 — new display driver** | Makerfabs MaTouch / Elecrow CrowPanel (S3) | S3 / native | various SPI/RGB | per-board panel driver |
| **3 — avoid (hard)** | **ESP32-P4** boards (e.g. …-Touch-LCD-4B, 720×720) | **P4 / via ESP32-C6** | MIPI-DSI | different arch **and** WiFi-over-hosted **and** display — full re-bring-up |
| **3 — avoid (hard)** | RP2040 / RP2350, STM32 | non-Espressif | — | no native `esp_wifi`; different SDK entirely |

## The recurring per-board cost (even for Tier 1)
1. **Pinout remap** — every board wires QSPI / I²C-touch / backlight / (audio) to different GPIOs;
   update the pin map (`app_config.h` + the panel/touch init in `main.cpp`).
2. **LCD dimensions** — `EXAMPLE_LCD_H_RES` / `V_RES` (+ `LCD_NOROT_*`) and the landscape-reader buffer.
3. **LVGL relayout** — the UI is tuned for the narrow 172-wide strip; a 320×480 / 180×640 panel is
   roomier and needs the chip strip, feed, Settings, and reader re-proportioned.

## Notes
- The **AXS15231B** is now the common controller for these "long/wide" ESP32-S3 touch strips, so there's
  a small ecosystem of Tier-0/1-compatible boards across Waveshare, LilyGO, and Guition.
- **Voice** is the main feature that depends on board-specific audio (ES8311 speaker + ES7210 mic). On
  boards without a mic codec, drop voice; everything else (tap-to-answer, macros, feed, Settings,
  Tailscale, reader) is unaffected.

## Links
- Waveshare ESP32-S3-Touch-LCD-3.49 — https://www.waveshare.com/esp32-s3-touch-lcd-3.49.htm
- Waveshare ESP32-S3-Touch-LCD-3.5B — https://www.waveshare.com/esp32-s3-touch-lcd-3.5b.htm
- Waveshare ESP32-S3-Touch-LCD-4.3C (RGB + ES8311/ES7210) — https://www.waveshare.com/esp32-s3-touch-lcd-4.3c.htm
- LilyGO T-Display-S3-Long — https://lilygo.cc/products/t-display-s3-long (driver: https://github.com/Xinyuan-LilyGO/T-Display-S3-Long)
- Guition JC3248W535C — https://grokipedia.com/page/JC3248W535C
