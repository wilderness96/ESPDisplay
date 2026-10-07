/* Minimal LVGL config. Named lv_conf_c.h (not lv_conf.h) on purpose:
   LVGL bundles/example folders (e.g. LovyanGFX's examples) sometimes ship
   their own generic "lv_conf.h" which can shadow this one depending on
   include search order for library builds. A unique filename plus the
   explicit LV_CONF_PATH build flag in platformio.ini avoids that entirely.
   Trimmed to what this project needs — see LVGL's lv_conf_template.h in
   the library source for the full option list if you want to enable more
   widgets/features later. */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// LVGL gets its OWN dedicated pool (LV_MEM_CUSTOM 0), isolated from the system
// heap. This is the fix for the UI-freeze: LVGL and the Spotify TLS handshakes
// were both drawing from the same system heap (the previous LV_MEM_CUSTOM 1),
// so once the Spotify poll guard was relaxed enough to actually fetch data, the
// constant TLS churn fragmented that heap and occasionally starved an LVGL
// allocation — which trips LV_ASSERT_MALLOC (a bare while(1) since LV_USE_LOG=0)
// and freezes the whole UI thread. A private pool means TLS can fragment the
// system heap all it likes without ever touching LVGL's memory.
//
// This was impossible before: a 40K pool + the 15K frame buffer + extra .bss
// buffers overflowed dram0_0_seg. Once that .bss shrank, the pool fit. Sized
// generously over the measured peak (~23K, hit with the settings overlay open)
// so it never exhausts it.
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (34U * 1024U)   // ~11K over the ~23K peak; capped by .bss room (framebuffer + the two static poll-task stacks share dram0_0_seg)

#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_LABEL 1
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1

// Safety net: LVGL's default assert action (and LV_ASSERT_MALLOC's, since
// LV_USE_LOG is 0) is a bare `while(1)` — a permanent freeze needing a manual
// power cycle. On this PSRAM-less board, transient heap fragmentation from the
// repeated Spotify TLS handshakes can occasionally leave no block big enough
// for an LVGL allocation; that failed alloc must NOT wedge the device forever.
// Reboot instead: a few seconds of downtime on a clock, and it self-recovers.
// (The real reduction of these events is fewer/less-fragmenting TLS handshakes —
// see main.cpp — this just makes the failure mode survivable.)
#define LV_ASSERT_HANDLER_INCLUDE "esp_system.h"
#define LV_ASSERT_HANDLER   esp_restart();

// lv_label_set_text_fmt() uses LVGL's own minimal printf, not the system one.
// Without this, %f conversions (used throughout main.cpp for gauge/stat values)
// silently print a literal "f" instead of the number.
#define LV_SPRINTF_USE_FLOAT 1

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_28 1   // sunrise/sunset times on the clock screen
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// QR code widget (lv_qrcode_create/lv_qrcode_update) lives in extra/libs and
// needs both the lib and its Reed-Solomon backend enabled.
#define LV_USE_QRCODE 1

#endif
