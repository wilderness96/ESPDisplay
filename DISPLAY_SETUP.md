# ESP32 Display — Display Setup & Styling

## Hardware

**Board:** Hosyond/Sunton "E32R40T" — 4.0" 320×480 TFT, ST7796S driver, onboard ESP32-32E (ESP32-D0WD-V3, dual-core, 240MHz, 520KB SRAM, 4MB flash, no PSRAM), resistive touch (XPT2046), 4-line SPI.

All-in-one board — no external wiring required. Just USB-C for power and flashing.

**Confirmed pinout** (from manufacturer wiki, lcdwiki.com/4.0inch_ESP32-32E_Display):

| Function | Pin |
|---|---|
| LCD CS | IO15 |
| LCD DC | IO2 |
| SPI SCLK | IO14 (shared with touch) |
| SPI MOSI | IO13 (shared with touch) |
| SPI MISO | IO12 (shared with touch) |
| LCD RST | — (shares ESP32's own EN/reset line, no dedicated pin) |
| Backlight | IO27 |
| Touch CS | IO33 |
| Touch IRQ | IO36 |

Native resolution is 320×480 portrait; the display runs in 480×320 landscape via `display.setRotation(1)` in `main.cpp` (see the rotation gotcha below — don't also set `offset_rotation` for this).

### Rotation / color-invert gotcha

Two config knobs are easy to get tangled and both showed up as visible bugs on first flash (sideways image, inverted/negative colors):

- **`offset_rotation`** (in `_panel_instance.config()`) corrects for a panel's *physical* mounting angle relative to its ribbon cable — it should normally be `0` unless a board is known to be mounted oddly. It is **not** the way to pick portrait vs. landscape.
- **`display.setRotation(1)`** (called once in `setup()`) is the actual runtime API for landscape/portrait. It was already being called correctly, but `offset_rotation` was *also* set to `1` in the panel config — stacking two rotations, which rotated the image to the wrong orientation instead of the intended one. Fix: `offset_rotation = 0`, keep `setRotation(1)`.
- **`cfg.invert`** controls whether the panel expects inverted color data. It was guessed as `true` based on general ST7796S behavior; on this specific unit that produced a negative/inverted image, so it's `false`.

Both are in `DisplayConfig.h`. If the display is ever rotated wrong or colors look inverted again after hardware changes, check these two flags first before assuming it's an LVGL/font issue.

## Build toolchain

**PlatformIO**, Arduino framework, `board = esp32dev`. Key libraries: LVGL 8.3.x (UI/widgets), LovyanGFX (display driver), ArduinoJson (JSON parsing).

### The `lv_conf.h` gotcha

LVGL needs a project-specific `lv_conf.h` to enable non-default fonts, set memory limits, etc. Getting this file actually *seen* by the library build was the single trickiest part of this whole setup — it took several wrong turns to land on the real fix:

- **`LV_CONF_INCLUDE_SIMPLE`** (bare `#include "lv_conf.h"`) is the usual recommended approach, but it's ambiguous by search order — a bundled example `lv_conf.h` shipped inside another library's `examples/` folder (in this project's case, LovyanGFX's) can be found before your project's real one, depending on how PlatformIO orders `-I` search paths for that particular library's build. Renaming the project's config to something unique (`lv_conf_c.h`, see below) sidesteps this ambiguity entirely regardless of search order.
- **`LV_CONF_PATH` alone isn't enough either.** PlatformIO's per-library build environments (used to compile LVGL's own `.c` files, as opposed to your `src/` files) don't automatically inherit your project's `include/` directory on their `-I` search path — only your own `src/` files get that for free. So `LV_CONF_PATH=lv_conf_c.h` correctly named the file, but library sources still couldn't find it until `include/` was added to the *global* build flags with an explicit `-I`.
- **Windows absolute-path trap (dead end, don't do this):** an earlier attempt hardcoded `-D LV_CONF_PATH="C:/Users/.../include/lv_conf.h"` as an absolute, pre-quoted path. That's wrong on two counts — LVGL's `__LV_TO_STR` macro stringifies whatever you pass it, so pre-quoting produces a double-quoted (invalid) filename; and even after fixing the quoting, GCC's include-search still doesn't resolve a bare filename purely on drive-letter absolute paths the way you'd expect across `-I` boundaries.

Current working config:
```ini
build_flags =
    -I include
    -D LV_CONF_PATH=lv_conf_c.h
```

`include/lv_conf_c.h` is the actual config file (deliberately not named `lv_conf.h`, to dodge the shadowing issue above). The `-I include` flag is what makes it visible to every compiled file — project code and every library — regardless of which build environment SCons is using for that particular source file.

### DRAM budget

This board has no PSRAM, so LVGL's draw buffer and internal heap have to fit inside a fairly tight internal-SRAM budget alongside the WiFi stack's own static allocations. Current settings, tuned to fit:

- Draw buffer: `480 * 8` pixels (~7.5KB) — small buffer, this UI redraws slowly so more flush calls are fine
- `LV_MEM_SIZE`: 34KB — sized generously over the measured ~23K peak

If you add more widgets/screens later and hit DRAM overflow again, these two are the first things to shrink further, or move less-critical LVGL features off (e.g. disable animations, reduce enabled widget types in `lv_conf_c.h`).

### Fonts

Custom font sizes initially ran into the same config-not-included issue and produced silent link failures (`undefined reference to lv_font_montserrat_22`). That's resolved now that `lv_conf_c.h` reliably reaches every compiled file (see above) — the sizes the UI uses are set to `1` in `include/lv_conf_c.h`, and `main.cpp` references the font structs directly.

If you ever see that undefined-reference error again after adding a new font size or widget, it almost always means either `include/lv_conf_c.h` wasn't rebuilt into every object (do a full delete of `.pio/build/<env>`, not just "Clean"), or a new dependency shipped its own shadowing `lv_conf.h` example the same way LovyanGFX's did.

### Float formatting gotcha

`lv_label_set_text_fmt()` doesn't use the system `printf` — it goes through LVGL's own minimal internal formatter, which has float (`%f`) support **off by default**. Without `LV_SPRINTF_USE_FLOAT 1` in `lv_conf_c.h`, every `%.0f`/`%.1f` conversion (all the weather readouts in `main.cpp`) silently prints a literal `f` instead of the number. This is set now — if numbers ever go back to showing as `f`, check this define first.

## Layout (480×320 landscape)

```
┌─────────────────────────────────────────────┐
│                1:42:07 PM                   │  ← big clock, drawn on a
│             Friday, July 18                 │    canvas + zoom (48px is LVGL's
├─────────────────────────────────────────────┤    largest built-in font)
│  74°  Partly Cloudy  │ ☀ 6:12AM ☾ 8:47PM   │
│  H: 82 / L: 64       │ SUNRISE / SUNSET     │
│  ───────────────────────────────────────────│
│  71° FEELS  55% HUM  8 MPH WIND │  ·····   │  ← stat chips + outdoor-temp
│                                       ╱╲╱╲  │    graph (now + 3h)
└─────────────────────────────────────────────┘
```

Spotify now-playing shares the top row of the weather card (right of the divider).

## Styling choices

- **Palette:** dark near-black background (`#0a0f0d`), green clock digits (`#2fd67a`), per-metric accents — blue (`#38b0ff`) for temp/wind, orange (`#ff6b4d`) for feels-like, dawn gold / dusk orange for the sun tile
- **Typography:** Montserrat throughout; the clock is Montserrat 48 rendered onto a `lv_canvas` and scaled with `lv_img_set_zoom`, since plain labels can't exceed LVGL's largest built-in font
- **Big clock centering:** each character is drawn in its own fixed-width cell — treating the whole "HH:MM:SS" string as one block let a narrow digit like '1' shrink the block and drag the visual center left

## Networking

- Weather: plain HTTP to Open-Meteo every 10 min (public, keyless API — TLS would cost a ~40KB mbedTLS handshake this device can't meet)
- Spotify: HTTPS, persistent keep-alive connection, refresh-token flow; runs on a background FreeRTOS task so a wedged handshake never blocks the UI
- All network I/O is serialized behind one mutex (`NetworkLock`), with bounded DNS resolution — see the comments in `main.cpp` for the crash history that led there
