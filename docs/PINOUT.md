# ESP32-S3-Touch-LCD-3.49 — Hardware bible (V2)

Source of truth: Waveshare official ESP-IDF examples in `vendor/ws-v2/ESP-IDF`
(driver `espressif/esp_lcd_axs15231b` v1.0.1). My physical board is assumed **V2**
(auto-detected at runtime, see below).

## Chip
ESP32-S3R8 (rev v0.2), 16 MB flash (W25Q128), 8 MB PSRAM. USB-Serial/JTAG native.
Serial port on this Mac: `/dev/cu.usbmodem8341101`. MAC `a4:cb:8f:da:ce:70`.

## Display — AXS15231B, QSPI
- Host `SPI3_HOST`, quad mode. 40 MHz, spi_mode=3, cmd_bits=32, param_bits=8.
- CS=**GPIO9**, CLK=**GPIO10**, D0=**11**, D1=**12**, D2=**13**, D3=**14**, DC=-1.
- RESET: **not a GPIO** → TCA9554 **bit5** (pulse high→low 250ms→high 30ms).
- TE: GPIO21 wired but UNUSED (sync via SPI on_color_trans_done callback + semaphore).
- Native panel **172(H) × 640(V)**, RGB565, 16bpp. MADCTL=0x00, COLMOD=0x55.
- No HW mirror/invert/swap; rotation to 640×172 done in software flush.
- Init actually sent: SLPOUT(0x11)+100ms, MADCTL=0x00, COLMOD=0x55, then 0x11+100ms, DISPON(0x29)+100ms. (Rich gamma table in driver is bypassed.)
- QSPI framing: cmd word `(0x02<<24)|(cmd<<8)`, color `(0x32<<24)|(cmd<<8)`. draw_bitmap sends CASET(0x2A) only, RAMWR(0x2C) when y_start==0 else RAMWRC(0x3C).
- Flush: `esp_lcd_panel_draw_bitmap`. Double full-screen PSRAM buffers (172*640*2=220KB each) + 22KB DMA bounce. LVGL full_refresh=1.

## Backlight
- Brightness PWM **GPIO42**, LEDC low-speed, TIMER_3, CHANNEL_1, 8-bit, 50 kHz. **Active-low** (duty 0 = brightest).
- ON/OFF enable: TCA9554 **bit1** (BL_EN). Set 0 then 1 to enable.

## Touch — AXS15231B integrated, I2C (polled)
- Bus **I2C_NUM_1**: SDA=**GPIO17**, SCL=**GPIO18**, 300 kHz. Addr **0x3B** (7-bit).
- INT/RST = -1 (polled). (TCA9554 bit0 = touch INT exists but unused.)
- Read: write 11-byte cmd `{0xB5,0xAB,0xA5,0x5A, 0x00,0x00,0x00,0x0E, 0x00,0x00,0x00}`, read 32 bytes.
  - `buff[1]` = #points (valid 1..4).
  - X = `((buff[2]&0x0F)<<8)|buff[3]` (12-bit). Y = `((buff[4]&0x0F)<<8)|buff[5]`.

## Battery / system voltage
- **BAT_ADC = GPIO4 = ADC1 channel 3**, behind an on-board **3:1 divider** → `V_sys = adc_mV * 3`. Read with
  `adc_oneshot` @ 12-bit / `ADC_ATTEN_DB_12`, curve-fitting calibration (raw 3.3V/4096 fallback if cali fails).
  Uses **ADC1** so it coexists with WiFi (ADC2 would not). See `main/battery.cpp`.
- The reading is effectively **VBAT** (the 1S Li-ion), not the input rail: it sits ~4.14V on battery and only
  ~4.16–4.18V when charging/near-full, with **no usable jump when USB-C is plugged in** (hardware-confirmed). So
  **charging is NOT detectable** by voltage; GPIO16 (SYS_OUT) is also constant. A charging indicator would need
  the charger's STAT pin wired to a spare GPIO (hardware mod). Percentage is an approximate resting-voltage curve
  saturating to 100% at ~4.15V (no BQ27220 fuel gauge is populated).
- Status-bar gauge is **drawn with LVGL rects** (body + nub + fill), NOT a font glyph — the merged deck font
  lacks the FontAwesome battery icons (`LV_SYMBOL_BATTERY_*` would tofu). Green >35% / amber ≤35% / red ≤15%.
  Polled every 10s by the power task.

## Buttons (three side buttons, "KEY&POWER" block on the schematic)
| Button | Wiring | Firmware use |
|---|---|---|
| **RESET** | ESP32-S3 EN/reset line | Hardware chip reset — reboots the deck; NOT reprogrammable. |
| **PWR** (middle) | Power-latch circuit (SYS_EN = TCA9554 bit6 latch; SYS_OUT = GPIO16 sense) | Long-press power-off is the latch hardware. Short-press sense on GPIO16 is possible but unused. |
| **BOOT** | **GPIO0** (active-low, external pull-up; strapping pin, safe to read at runtime) | Repurposed: short = cycle backlight brightness, long (≥1.2s) = toggle **lock** (pocket mode). |

## Standby / screen power (companion firmware)
- "Standby" = **screen off only** — WiFi + the WebSocket stay associated so the deck stays pingable and wakes on
  events. Blanked with the **backlight only** (BL_EN bit1 low + PWM duty 255); `flush_cb` skips the SPI redraw
  while off. We do NOT send the panel DISPOFF — on this board it woke to a black screen (see gotcha below), and
  the backlight is the dominant draw anyway.
- Owned entirely by the power task (`example_backlight_loop_task` in `main.cpp`): it polls BOOT, runs the idle
  auto-off timer (`STANDBY_IDLE_MS`, Settings "Auto sleep" toggle), and services wake/lock request flags. Every
  panel transition runs under the LVGL lock, so it can't race `flush_cb`. Other threads only post flags
  (`app_note_user_activity` from touch, `app_wake_for_event` from ask/alert/reply) — never touch the panel.
- Wake events = a question (`ask`), an `alert`, or a `reply`/`done`/`notify` feed line. Routine tool-use activity
  does NOT wake the screen. `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` is set for standing modem power-save.
- **Lock (pocket mode)** = a BOOT **long-press** sets `g_locked` and blanks the screen (same backlight-off path as
  standby). While locked the deck is deliberately deaf: `example_lvgl_touch_cb` drops every touch, and both wake
  hooks early-return, so neither a pocket touch nor an incoming event wakes it. The ONLY exit is another
  long-press (`s_lock_toggle_req` → toggle), which wakes the screen and restores `s_cur_duty` (prior brightness)
  and re-enables touch. Auto-standby is suppressed while locked. Sounds are unaffected (audio ≠ display), so
  alerts still chirp. Lock is transient — a reboot clears `g_locked`.

## System I2C (I2C_NUM_0): SDA=GPIO47, SCL=GPIO48, 300 kHz
| Device | 7-bit addr |
|---|---|
| TCA9554 I/O expander | **0x20** |
| PCF85063 RTC | 0x51 |
| QMI8658 IMU | 0x6B |
| ES8311 DAC (speaker) | 0x18 |
| ES7210 ADC (mics) | 0x41 |

## TCA9554 expander @0x20 bit map
| bit | function | dir |
|---|---|---|
| 0 | touch INT | in (unused) |
| 1 | **BL_EN** backlight enable | out |
| 2 | IMU INT1 | in |
| 3 | IMU INT2 | in |
| 4 | RTC INT | in |
| 5 | **LCD_RST** | out |
| 6 | **SYS_EN** power latch — **set HIGH to stay powered** (LOW = power off) | out |
| 7 | **NS_MODE** speaker amp enable — set HIGH to enable audio out | out |

## Audio — I2S0 (TDM), shares system I2C for codec control
- MCLK=**GPIO7**, BCLK=**GPIO15**, WS/LRCK=**GPIO46**, DOUT=**GPIO45** (→ES8311 spkr), DIN=**GPIO6** (←ES7210 mics).
- ES8311 @0x18, ES7210 @0x41. use_mclk=1, pa_gain=6dB. Speaker amp via TCA9554 bit7.
- Rates used: play 16k/16-bit/2ch or 24k; record via ES7210 (MIC1|MIC3).
- codec_board type string: `S3_LCD_3_49`.

## Managed components (idf >=4.1, builds on 5.4.1)
- `espressif/esp_lcd_axs15231b: "^1.0.1"`
- `espressif/esp_io_expander_tca9554: "^2.0.1"`
- `lvgl/lvgl: "^8.4.0"`
- `espressif/esp_codec_dev: "~1.3.4"` (audio)

## V1 vs V2 detection
Probe TCA9554 at I2C **0x20** on bus0 (47/48). ACK → **V2**. No ACK → **V1**:
- V1: LCD_RST = GPIO21 direct; backlight = GPIO8 direct; no expander.
- V2: as above (RST/BL via expander; GPIO8 = EXIO_INT input; GPIO21 = TE).
QSPI/touch/audio pins identical across V1/V2.

## Gotchas
- Drive **SYS_EN (bit6) HIGH early** or the board can power off on battery.
- Backlight is **active-low**; enable bit1 AND set a non-max duty.
- Panel is physically tall (172w×640h); we use it rotated to 640×172 landscape via software.
- **ES8311 is a MONO codec.** Configure I2S + esp_codec_dev as mono (`channel=1`, `I2S_SLOT_MODE_MONO`). A stereo config double-speeds playback (pitched-up, harsh "bop"). Keep the speaker amp (NS_MODE bit7) ON during playback so tones aren't chopped.
- Alert sounds are **streamed PCM clips** from the bridge (binary WS frames), not synth — see `bridge/sounds/` + `audio_play_pcm`.
- ES8311 (speaker out) + ES7210 (mic in) share **one I2S** (BCLK15/WS46/MCLK7; out=DOUT45, in=DIN6) → mic capture needs full-duplex or play/record switching.
- **Avoid `esp_lcd_panel_disp_on_off()` for standby on this board.** The AXS15231B driver's `panel_axs15231b_disp_off` sends DISPOFF/DISPON, but in practice the panel woke to a **black screen** (backlight on, no image) after a DISPOFF/DISPON cycle regardless of the bool passed — a plain DISPON didn't restore it. Blank with the **backlight** (BL_EN + PWM) instead; a panel that never gets DISPOFF always has a valid image the instant the backlight returns.
