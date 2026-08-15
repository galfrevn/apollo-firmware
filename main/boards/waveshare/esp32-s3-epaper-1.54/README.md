# Waveshare ESP32-S3-ePaper-1.54 (V2)

A pocket body for Apollo: 200x200 monochrome e-paper, no touch, a battery
already fitted, and three power rails that have to be switched on in software
before anything on the board answers.

[Product page](https://www.waveshare.com/esp32-s3-epaper-1.54.htm)

|  | |
| --- | --- |
| SoC | ESP32-S3-PICO-1-N8R8, 240MHz |
| Flash | **8MB** — half of what the 1.85C has |
| PSRAM | 8MB octal @ 80MHz |
| Display | 1.54" e-paper, 200x200, 1bpp |
| Audio | ES8311 + I2S, mic and speaker on board, PA gated by GPIO 46 |
| Sensors | SHTC3 temperature/humidity, PCF85063 RTC. **No IMU** |
| Buttons | BOOT (GPIO 0) and PWR (GPIO 18) |
| I2C | SDA 47, SCL 48 |

There are two hardware revisions. **This port targets V2 only** — V1 has 4MB
of flash and 2MB of quad PSRAM, and its partition layout does not match.
Confirm with `esptool flash_id` before flashing.

## The display is not driven yet

`GetDisplay()` is not overridden, so the board falls back to `NoDisplay`.
That is on purpose for the first pass: audio, the Apollo protocol, the power
rails and the battery gauge are all reachable without any e-paper driver, and
they are the parts worth answering first. A 200x200 1bpp panel that repaints
in a few hundred milliseconds is a different problem from an animated round
LCD, and none of the existing display classes (`lcd_display`,
`lvgl_display`, `emote_display`) transfer to it.

## Push to talk, and no wake word

The wake word is disabled in `config.json`. An always-on AFE is the largest
single current draw on this board, and it is not the interaction this body is
for: you hold BOOT to speak, which maps directly onto the protocol's
`hold_start` / `hold_end`. Triple-click BOOT to reach Wi-Fi config — a long
press cannot be used for it, because holding the button *is* the talk gesture.

A long press on **PWR** cuts the rails, audio first so the amplifier does not
pop on the way down.

## Three rails, and their polarity is not uniform

Nothing comes up until `BoardPowerBsp` runs, and the boot sequence then waits
for GPIO 18 to read high. The e-paper and audio rails are **active low**; the
battery rail is **active high**. `board_power_bsp.cc` is the only place that
should know that.

## Build

```bash
python3 scripts/build.py waveshare/esp32-s3-epaper-1.54
```

## Known gaps

- No e-paper driver, as above.
- The battery divider ratio is recovered from the xiaozhi board file's `* 2`,
  not from a schematic — the schematic for this board is not public. If one
  turns up and disagrees, it wins.
- Deep sleep is not wired. `SleepTimer` and `PowerSaveTimer` exist in
  `boards/common/` and are unused by every board in this repo; on a device
  that runs off a battery that is the difference between a product and a
  demo, and it is the next thing to do after the display.
