# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Home Assistant display device using a **Waveshare ESP32-C6 1.47" development board**:
- **MCU:** ESP32-C6, 80MHz (clocked down to save power), 4MB Flash
- **Display:** 172×320 LCD (ST7789 driver), 262K colour, portrait orientation
- **Connectivity:** Wi-Fi 6, BLE5
- **Target:** Home Assistant at `http://192.168.1.29:8123/`

Displays three emonPi sensor values: solar generation, grid import/export, outside temperature.

## Build & Flash (PlatformIO)

```bash
pio run                        # build
pio run --target upload        # build and flash
pio device monitor             # serial monitor at 115200
pio run --target upload && pio device monitor   # flash then monitor
```

In VS Code with the PlatformIO extension: use the bottom toolbar icons or `Ctrl+Alt+B` / `Ctrl+Alt+U`.

## Project Structure

```
platformio.ini        # board: esp32-c6-devkitc-1, libs: LovyanGFX, ArduinoJson
src/
  main.cpp            # all application logic (display, WiFi, HA polling)
  secrets.h           # WiFi credentials + HA token — never commit this file
```

`secrets.h` must be created/populated before building (see template in the file).

## Architecture

Single-file Arduino sketch (`src/main.cpp`) with three layers:

1. **LGFX class** — LovyanGFX display driver config for the ST7789 panel. Pin assignments (SCLK=7, MOSI=6, DC=15, CS=14, RST=21, BL=22) match the Waveshare schematic; adjust here if display is blank or garbled.

2. **HA REST polling** (`haGet` / `pollHA`) — HTTP GET to `/api/states/<entity_id>` every 5 seconds with a Bearer token header. Parses the `state` field from the JSON response.

3. **Display rendering** (`drawSection` / `updateDisplay`) — Full screen redraw on each poll. Three equal sections (96px each): Solar (orange), Grid import/export (salmon/green, sign-aware), Outside temp (lavender). Each section has a 24px coloured left accent strip with an icon, and a large Font7 number (scaled 1.1×W × 1.35×H) in the remaining space.

## Home Assistant Entities

| Entity | Meaning |
|--------|---------|
| `sensor.power_emonpi_power1` | Grid power — positive = importing, negative = exporting |
| `sensor.power_emonpi_power2` | Solar generation |
| `sensor.temperature_emonpi_t2` | Outside temperature |

## Power / Heat

Running at ~80-90mA (5V). Main consumers are WiFi and backlight. Tuning knobs:
- `lcd.setBrightness(60)` in `setup()` — currently 60/255; lower saves mA, higher improves readability
- `setCpuFrequencyMhz(80)` in `setup()` — can go to 40MHz if polling latency allows
- `POLL_MS` — currently 5000ms; faster = more WiFi activity = more heat

## HA Long-Lived Access Token

Profile (bottom-left in HA) → scroll to **Long-Lived Access Tokens** → **Create Token**. Paste into `src/secrets.h`. Token is only shown once.
