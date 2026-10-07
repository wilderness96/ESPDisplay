# ESP32 4" Display — Clock, Weather & Spotify Dashboard

Firmware for the **4.0" ESP32-32E all-in-one display module**
([Hosyond/Sunton "E32R40T"](https://www.lcdwiki.com/4.0inch_ESP32-32E_Display))
— a wall clock with Open-Meteo weather, sunrise/sunset, an outdoor-temperature
graph, and Spotify now-playing. All configuration happens **on the device**
after flashing; nothing needs to be baked into the build.

## Target hardware

| | |
|---|---|
| MCU | ESP32-32E (ESP32-D0WD-V3), dual-core Xtensa LX6 @ 240 MHz, 4 MB QSPI flash, 520 KB SRAM, **no PSRAM** |
| Display | 3.95–4.0" TN TFT, 320×480, ST7796S driver, 4-line SPI, 262K colors |
| Touch | Resistive, XPT2046 over SPI (shares the LCD's SPI bus) |
| Power | USB-C (one-click download circuit — no BOOT button needed), 3.7 V LiPo charging circuit (TP4054) on the 2 P battery pad |
| Other | RGB status LEDs (IO22/16/17), speaker pads (FM8002E), microSD slot, I²C/SPI/UART expansion headers, serial on IO1/IO3 (CH340C USB-serial) |

The module is a complete dev board: flashing is just USB-C + PlatformIO.
If you're on the E32R40T (touch) variant, pin assignments are already
confirmed in [`src/DisplayConfig.h`](src/DisplayConfig.h); the touchless
E32N40T is the same LCD on the same pins.

The firmware is deliberately sized for this board's tight memory (no PSRAM —
see [Memory & performance notes](#memory--performance-notes)).

## What it shows

```
┌─────────────────────────────────────────────┐
│                1:42:07 PM                   │  NTP-synced clock (12h)
│             Friday, July 18                 │
├─────────────────────────────────────────────┤
│  74°  Partly Cloudy  │ ☀ 6:12AM ☾ 8:47PM   │  Open-Meteo: temp, condition,
│  H: 82 / L: 64       │ SUNRISE / SUNSET     │  hi/lo, sunrise/sunset
│  ───────────────────────────────────────────│
│  71° FEELS  55% HUM  8 MPH WIND │  ·····   │  stat chips + outdoor-temp
│                     now-playing  ╱╲╱╲       │  graph (now + next 3 hours)
└─────────────────────────────────────────────┘
```

- **Clock** — big green digits (canvas + zoom, since 48 px is LVGL's largest
  built-in font), day + date, NTP with periodic resync.
- **Weather** — Open-Meteo every 10 min, retrying every 30 s on failure;
  feels-like, humidity, wind, and a mini line graph of the current temp plus
  the next three hours.
- **Spotify** — now-playing (title marquee, artist, paused dimming), polled
  every 5 s on a background task with a persistent TLS connection.
- **Settings menu** (tap the clock or the gear icon) — device IP + settings
  PIN, touch recalibration, sleep, close.
- **Sleep mode** — backlight + panel off, all polling paused; touch to wake.

## Quick start

1. **Install PlatformIO** — easiest via the
   [PlatformIO IDE extension](https://platformio.org/install/ide) for VS Code.
2. **Clone and open** this repo in VS Code. The first build downloads the
   toolchain and libraries (LVGL 8.3, LovyanGFX, ArduinoJson) automatically.
3. **Flash** — plug the board in over USB-C and hit *Upload*
   (or `pio run -t upload`). The USB-C port has a one-click download circuit,
   so no BOOT-button dance. Open the serial monitor (`pio device monitor`,
   115200 baud) to watch setup.
4. **First boot — touch calibration**: the display walks you through an
   interactive crosshair calibration. Tap each crosshair. It's saved to NVS
   and never asked again (redoable from the settings menu).
5. **First boot — WiFi**: with no stored credentials the device drops into a
   captive-portal setup page. Join the **"ClockSetup"** AP (QR code is on
   screen) or go to http://192.168.4.1, pick your network, enter the
   password, save. The device reboots onto your network.
6. **Configure location + Spotify** (see below): tap the clock to open
   Settings, note the **PIN** and IP shown there, open
   `http://<device-ip>/` in a browser, enter the PIN, fill in the form, save.
   Takes effect immediately, no reboot.

### Spotify setup

The device runs the **Authorization Code + refresh token** flow — you do the
browser consent once, it does the rest:

1. Create an app at https://developer.spotify.com/dashboard (Client ID +
   Secret).
2. Add a redirect URI (e.g. `http://localhost:8888/callback`).
3. Run the authorization code flow **once** in a browser with scope
   `user-read-currently-playing` and keep the resulting **refresh token**.
4. Enter Client ID, Client Secret, and Refresh Token on the device's settings
   page.

> **Note:** Spotify refresh tokens expire after roughly 6 months. When the
> serial log starts printing `Spotify token refresh failed: 400`, redo step 3
> and enter the new token on the settings page.

### Timezone

The clock uses NTP + a POSIX TZ string. Set `TZ_STRING` in
[`src/main.cpp`](src/main.cpp) to your zone before flashing (default is
`UTC`), e.g. `"PST8PDT,M3.2.0,M11.1.0"` for US Pacific. Weather is requested
with `timezone=auto` from the location you configure, so it's always local
regardless of this.

## Compile-time defaults

Everything below is blank on purpose — the device collects real values on
first use. Set them in `src/main.cpp` only if you want built-in fallback
defaults (used on the very first boot, before anything is stored in NVS):

| Macro | Meaning |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | First-boot WiFi fallback |
| `LAT` / `LON` | First-boot weather location fallback |
| `TZ_STRING` | Timezone for the NTP clock |
| `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET` / `SPOTIFY_REFRESH_TOKEN` | First-boot Spotify fallback |

## Project layout

```
platformio.ini          build config (esp32dev, huge_app partition, crash backtrace decoder)
src/main.cpp            the whole app: LVGL UI, WiFi setup portal, settings
                        page, weather, Spotify, touch calibration, sleep
src/DisplayConfig.h     panel/touch pin + rotation + calibration config (confirmed for E32R40T)
include/lv_conf_c.h     LVGL config (renamed + -I include so it can't be shadowed — see DISPLAY_SETUP.md)
DISPLAY_SETUP.md        display bring-up notes: rotation/color-invert gotcha,
                        lv_conf.h shadowing, DRAM budget, float formatting
```

## Build & flash

```bash
pio run -t upload     # build + flash
pio device monitor    # serial console at 115200
```

`monitor_filters = esp32_exception_decoder` in `platformio.ini` decodes any
crash backtrace into function names + line numbers while the monitor is open.

### Notes

- **Driver:** the USB-serial chip is a CH340C — Windows needs the CH340
  driver (bundled in the [lcdwiki data package](https://www.lcdwiki.com/4.0inch_ESP32-32E_Display))
  if the port doesn't show up.
- **Partition table:** `huge_app.csv` — no OTA partition, so the app gets
  ~3 MB instead of ~1.25 MB.
- **Power for battery use:** the 2 P pad takes a 3.7 V polymer cell; the
  on-board TP4054 circuit charges it from USB (4.2–6.5 V input).

## Memory & performance notes

This board has no PSRAM, and the firmware is shaped around that:

- **One network connection at a time.** All network I/O (Spotify task +
  weather poll on the main loop) is serialized behind a single mutex so the
  small socket table and heap are never contended.
- **Bounded DNS.** `getaddrinfo()` in lwIP can wedge forever when the
  interface flaps mid-query; hosts are resolved in a disposable worker task
  with a hard deadline, then connected by IP.
- **Background Spotify task.** The TLS handshake runs off the UI thread on
  core 0 with backoff on failure, so a wedged endpoint can't freeze the
  display.
- **Task watchdog.** Any subscribed task that stops looping for 90 s panics
  with a decoded backtrace instead of silently freezing.
- **Weather over plain HTTP** (Open-Meteo is keyless); Spotify over HTTPS
  with certificate validation disabled to save heap (`setInsecure`).
- **LVGL on a fixed pool** (34 KB) with a small 8-row draw buffer — sized to
  fit the internal SRAM; see [`DISPLAY_SETUP.md`](DISPLAY_SETUP.md) for the
  DRAM budget and the `lv_conf.h` shadowing gotcha that cost the most time.

## Troubleshooting

| Symptom | First things to check |
|---|---|
| Sideways or mirrored display | `setRotation()` vs `offset_rotation` in `DisplayConfig.h` (see DISPLAY_SETUP.md) |
| Inverted/negative colors | `cfg.invert` in `DisplayConfig.h` |
| Weather frozen at "--" | Location not set on the settings page (LAT/LON empty), or no internet |
| Spotify silent | Token expired (log: `refresh failed: 400`) — re-mint the refresh token |
| Garbage numbers with a literal "f" | `LV_SPRINTF_USE_FLOAT` missing from `lv_conf_c.h` |
| Fonts link errors (`undefined reference to lv_font_montserrat_*`) | `lv_conf_c.h` shadowing — full wipe of `.pio/build/<env>`, see DISPLAY_SETUP.md |
| Stuck touch / drifting calibration | "Recalibrate Touch" in the settings menu |
| Crash on boot or under load | `pio device monitor` — the backtrace decoder names the function |

## Hardware references

Full pin map, schematics, ST7796S/XPT2046 datasheets, and example code:
[lcdwiki.com/4.0inch_ESP32-32E_Display](https://www.lcdwiki.com/4.0inch_ESP32-32E_Display).
