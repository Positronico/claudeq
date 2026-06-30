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
