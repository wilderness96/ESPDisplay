#pragma once
#include <LovyanGFX.hpp>

/*
  Config for the Hosyond/Sunton "E32R40T" board — 4.0" 320x480 ST7796S,
  onboard ESP32-32E, resistive touch (XPT2046).
*/

class DisplayConfig : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796  _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  DisplayConfig() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = 1;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs   = 15;
      cfg.pin_rst  = -1;
      cfg.pin_busy = -1;
      cfg.panel_width  = 320;
      cfg.panel_height = 480;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 27;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    setPanel(&_panel_instance);

    // Touch (XPT2046) — shares SPI bus with LCD
    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 300;
      cfg.y_max = 3600;   // narrowed to shift Y down ~30px
      cfg.pin_int    = -1;      // -1 = polling mode (no IRQ wired)
      cfg.bus_shared = true;
      // Combined with the panel's rotation (setRotation(1), panel offset_rotation=0),
      // LovyanGFX computes an effective post-calibration transform from
      // (internal_rotation + this value). 1 here produced a full 180-degree flip on
      // both axes (taps registering diagonally opposite where they landed) — 3
      // cancels that out to an identity mapping. If it's still off, the raw ADC
      // axes themselves may be swapped/inverted for this touch chip's wiring;
      // watch the Serial monitor (lvglTouchReadCb logs each tap) and try the
      // other odd values (5, 7) next, since x_min/x_max/y_min/y_max already
      // assume a swapped landscape target box for any odd offset_rotation.
      cfg.offset_rotation = 3;

      // SPI (same bus as LCD)
      cfg.spi_host = VSPI_HOST;
      cfg.freq     = 2000000;   // 2MHz for touch
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_cs   = 33;

      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
  }
};