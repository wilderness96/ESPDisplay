// ---------- Clock + Weather + Spotify ----------
// Shows big clock (NTP-synced) + Open-Meteo weather + Spotify now-playing.
// WiFi, location, and Spotify are all configured on-device rather than by
// editing this file. WiFi setup runs automatically (no menu button) whenever
// the device isn't connected — see openWifiSetup(). Location and Spotify are
// set from the always-on settings page, reached via the Settings menu (tap
// the clock or the gear icon) — see startSettingsServer() below.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>   // Spotify is HTTPS-only
#include <ArduinoJson.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <lvgl.h>
#include "DisplayConfig.h"
#include <esp_bt.h>             // esp_bt_controller_mem_release / ESP_BT_MODE_BTDM
#include <WebServer.h>          // on-device WiFi setup portal + settings page (core-bundled, no extra lib_deps)
#include <DNSServer.h>          // captive-portal DNS redirect for the setup portal
#include <Preferences.h>        // NVS storage for WiFi credentials + Spotify/location settings
#include <esp_random.h>         // esp_random() — hardware RNG for the settings-page PIN
#include <freertos/FreeRTOS.h>  // background task for Spotify networking — see spotifyPollTask()
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>       // hardware task watchdog — see TASK_WDT_TIMEOUT_S

// Forward decls — referenced by LVGL event callbacks defined ahead of their bodies.
void openSettingsMenu();
void closeSettingsMenu();
void sleepScreen();
void wakeScreen();
void updateClock();
void openWifiSetup();
void connectWifi();
void loadRuntimeConfig();
void startSettingsServer();
void stopSettingsServer();
bool loadTouchCalibration();
void runTouchCalibration();
void recalibrateTouchAndRedraw();
void spotifyApplyIfDirty();

// ---------- Config ----------
// WIFI_SSID/PASSWORD, LAT/LON, and the SPOTIFY_* values below are only the
// *compile-time fallback defaults* — used the very first boot, before anything
// has been configured on-device. In normal use, WiFi is set via the QR/portal
// (see openWifiSetup()) and Spotify + LAT/LON are set via the always-on
// settings page (see startSettingsServer()); both write to NVS via
// Preferences and take effect without a reflash. loadRuntimeConfig() is what
// actually loads the values the rest of the app uses.
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

#define LAT ""
#define LON ""
#define TZ_STRING "UTC"  // POSIX TZ string for the NTP clock — set this to your zone, e.g. "PST8PDT,M3.2.0,M11.1.0" (US Pacific) or "EST5EDT,M3.2.0,M11.1.0" (US Eastern)

#define WEATHER_POLL_MS 10UL * 60UL * 1000UL  // refresh every 10 min
#define NTP_RESYNC_MS   6UL * 60UL * 60UL * 1000UL // resync every 6h

// Tripwire for the silent display freeze: any subscribed task (loopTask + the
// Spotify poll task) that stops looping for this long gets a panic with a decoded
// backtrace (esp32_exception_decoder) instead of silence. 90s is well above the
// legitimate worst-case pass of any task (connectWifi's 20s wait + a fully
// timed-out Spotify token+now-playing+art cycle ~40s).
#define TASK_WDT_TIMEOUT_S 90

// ---------- Spotify (now playing) ----------
// One-time setup for the values below (see the guide notes):
//   1. Create an app at https://developer.spotify.com/dashboard -> Client ID + Secret.
//   2. Add a Redirect URI (e.g. http://localhost:8888/callback).
//   3. Run the Authorization Code flow ONCE in a browser with scope
//      "user-read-currently-playing" to get a refresh token.
// The device then swaps that refresh token for short-lived access tokens on
// its own. HTTPS is required (no HTTP fallback), but with BLE removed there's
// plenty of heap for the handshake.
// NOTE (2026): Spotify refresh tokens now expire after ~6 months. When the
// device starts logging "Spotify token refresh failed: 400", re-run the
// one-time auth to mint a new refresh token and enter it on the settings page.
// See SPOTIFY_SETUP.md for the full walkthrough.

#define SPOTIFY_CLIENT_ID     ""
#define SPOTIFY_CLIENT_SECRET ""
#define SPOTIFY_REFRESH_TOKEN ""
#define SPOTIFY_POLL_MS (5UL * 1000UL)   // check the current track every 5s
// A mbedTLS handshake doesn't need one 40KB block — it needs a ~16KB receive
// buffer, a ~16KB send buffer, and smaller temporaries, which can land in
// *separate* free blocks. So a poll is worth attempting when BOTH hold:
//  - the largest block can fit the single biggest allocation (~17KB + margin), and
//  - total free covers the whole ~48KB footprint (spread across blocks) with room.
// The first TLS handshake after boot fragments the largest block down to ~39KB
// and it never recovers; the old single "largest >= 40KB" guard therefore
// skipped every poll forever. With album art gone there's ~113KB free total, so
// checking total free (not just the largest block) is what lets the handshake
// through — and setHandshakeTimeout(5) means a poll that slips through but still
// can't allocate fails in <=5s and retries, instead of the old 2-minute hang.
#define TLS_MIN_CONTIG_BLOCK 20000   // largest block must hold mbedTLS's biggest single alloc
#define TLS_MIN_TOTAL_FREE   55000   // total free must cover the full handshake footprint

// Runtime config — loaded from NVS by loadRuntimeConfig(), editable from the
// settings page (see startSettingsServer() below) without a reflash.
String cfgSpotifyClientId, cfgSpotifyClientSecret, cfgSpotifyRefreshToken;
String cfgLat, cfgLon;

// "Sleep" (settings menu): backlight off + panel deep-sleep + all outbound
// polling paused, for nights/weekends. Set/cleared on loopTask only; read by
// the poll tasks to park themselves. The panel is asleep but the ESP32, WiFi
// and the XPT2046 touch chip are all alive — loop() polls touch directly and
// wakes on press. Touches that arrive during the dark are DRAINED (held until
// release) so the wake gesture never also clicks whatever was underneath.
volatile bool deviceSleeping = false;   // read from spotifyPollTask() (core 0) too

// Random 4-digit PIN, regenerated every boot, gating the settings page.
// Shown on-device in the settings menu (see openSettingsMenu()) so only
// someone standing at the clock can read it.
char settingsPin[5] = "0000";

void generateSettingsPin() {
  snprintf(settingsPin, sizeof(settingsPin), "%04u", (unsigned)(esp_random() % 10000));
  Serial.printf("Settings page PIN: %s\n", settingsPin);
}

// ---------- Display / LVGL ----------
DisplayConfig display;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[480 * 8];  // single buffer — this UI redraws slowly, so a small buffer is fine; sized down (480*32 -> *16 -> *8) to free .bss for LVGL's dedicated memory pool (LV_MEM_SIZE in lv_conf_c.h). Only full-screen redraws (screen switches) are marginally slower; the big heap block is unaffected.

void lvglFlushCb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  display.startWrite();
  display.setAddrWindow(area->x1, area->y1, w, h);
  display.pushPixels((uint16_t*)&color_p->full, w * h, true);
  display.endWrite();
  lv_disp_flush_ready(disp);
}

// Last time the screen was touched anywhere, and whether it's happened at
// all yet — drives the settings gear icon's show-on-touch/auto-hide timeout
// (see SETTINGS_GEAR_TIMEOUT_MS in loop()).
volatile uint32_t lastTouchMs = 0;
volatile bool screenEverTouched = false;

void lvglTouchReadCb(lv_indev_drv_t* indev_drv, lv_indev_data_t* data) {
  uint16_t x, y;
  static bool wasPressed = false;   // edge-detect so we log once per tap, not every ~30ms poll
  if (display.getTouch(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
    if (!wasPressed) Serial.printf("Touch down at (%d, %d)\n", x, y);
    wasPressed = true;
    lastTouchMs = millis();
    screenEverTouched = true;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    wasPressed = false;
  }
}

// ---------- Touch calibration ----------
// LovyanGFX's built-in interactive calibration — draws crosshairs directly on
// the panel (bypassing LVGL entirely) and has the user tap each one, then
// hands back an 8-value calibration matrix that display.getTouch() applies to
// every raw touch reading from then on. This replaces hand-tuning the touch
// x_min/x_max/y_min/y_max in DisplayConfig.h by trial and error. The panel's
// wiring-related touch settings (SPI pins, offset_rotation) stay in
// DisplayConfig.h as-is — this only calibrates the min/max/scale on top.
uint16_t touchCalData[8];

// Loads a previously-saved calibration from NVS and applies it. Returns false
// if none is stored yet (first boot, or after a NVS wipe) — DisplayConfig.h's
// compiled-in x_min/x_max/y_min/y_max keep acting as the fallback until then.
bool loadTouchCalibration() {
  Preferences prefs;
  prefs.begin("cfg", true);   // read-only
  bool ok = prefs.getBytesLength("touch_cal") == sizeof(touchCalData);
  if (ok) prefs.getBytes("touch_cal", touchCalData, sizeof(touchCalData));
  prefs.end();
  if (ok) display.setTouchCalibrate(touchCalData);
  return ok;
}

// Runs the interactive calibration. Blocks until the user has tapped through
// it (no timeout — same as LovyanGFX's own examples). Draws straight to the
// panel, so the caller is responsible for repainting the LVGL UI afterward
// (e.g. lv_obj_invalidate(lv_scr_act()) + lv_timer_handler()).
void runTouchCalibration() {
  Serial.println("Touch calibration starting — tap each crosshair as it appears.");
  uint8_t size = (uint8_t)((display.width() > display.height() ? display.width() : display.height()) >> 3);
  // calibrateTouch() is void in this LovyanGFX version — it blocks until the
  // user has tapped through all the points (no success/failure to check;
  // there's also no cancel), and fills touchCalData in place. Waits on a human
  // with no way to feed the watchdog from inside LovyanGFX — unsubscribe for
  // its duration, re-arm after (harmless error codes during first-boot use).
  esp_task_wdt_delete(NULL);
  display.calibrateTouch(touchCalData, 0xFFFFFFU, 0x000000U, size);
  esp_task_wdt_add(NULL);

  Preferences prefs;
  prefs.begin("cfg", false);
  prefs.putBytes("touch_cal", touchCalData, sizeof(touchCalData));
  prefs.end();
  display.setTouchCalibrate(touchCalData);
  Serial.println("Touch calibration saved.");

  display.fillScreen(0x000000U);   // clear the calibration graphics before LVGL redraws over it
}

// "Recalibrate Touch" button in the settings menu (see buildUi()).
void recalibrateTouchAndRedraw() {
  closeSettingsMenu();
  lv_timer_handler();   // flush the menu closing before we take over the raw display
  runTouchCalibration();
  lv_obj_invalidate(lv_scr_act());   // the calibration UI drew over everything LVGL had on screen
  lv_timer_handler();
}

// ---------- Widgets ----------
// The clock is drawn onto a canvas and scaled up via lv_img_set_zoom, since
// that's the size (48px) exceeds LVGL's largest built-in font.
#define CLOCK_CANVAS_W 280
#define CLOCK_CANVAS_H 70
#define CLOCK_CANVAS_ZOOM 360   // 360/256 = 1.40625x -> ~67px equivalent

lv_obj_t* timeCanvas;
static lv_color_t* timeCanvasBuf = nullptr;
static lv_coord_t clockTextX = 0;   // left edge of the first character cell
static lv_coord_t clockCellW = 0;   // fixed per-character slot width — see buildUi()

lv_obj_t* clockDateLabel;   // day + date, shown under the clock
lv_obj_t* tempLabel;
lv_obj_t* conditionLabel;
lv_obj_t* hiLoLabel;
lv_obj_t* feelsLikeLabel;
lv_obj_t* humidityLabel;
lv_obj_t* windLabel;
lv_obj_t* songLabel;        // Spotify: track title
lv_obj_t* artistLabel;      // Spotify: artist
static lv_style_t styleCard;

// Settings menu — opened by tapping the clock.
lv_obj_t* settingsOverlay = nullptr;   // full-screen dim backdrop; tapping it closes the menu
lv_obj_t* settingsCloseBtn = nullptr;
lv_obj_t* wifiStatusLabel = nullptr;   // shown in place of the buttons while the setup portal is open
lv_obj_t* wifiQr = nullptr;            // WIFI: QR code so a phone can join "ClockSetup" with one scan
lv_obj_t* deviceInfoLabel = nullptr;   // "IP: ... / PIN: ...", for reaching the Spotify/location settings page
lv_obj_t* settingsGearBtn = nullptr;   // hidden by default — see lastTouchMs / SETTINGS_GEAR_TIMEOUT_MS in loop()

// Full-screen container for the clock/weather UI; the settings overlay and
// gear icon hang off the screen root and layer over it.
lv_obj_t* clockScreen = nullptr;

// Sunrise/sunset tile on the clock screen. Costs zero extra network:
// sunrise/sunset arrive in the same Open-Meteo response the weather panel
// already fetches every 10 minutes.
lv_obj_t* sunriseLabel = nullptr;
lv_obj_t* sunsetLabel  = nullptr;
lv_obj_t* sunriseAmPmLabel = nullptr;   // "AM"/"PM" rendered small beside the
lv_obj_t* sunsetAmPmLabel  = nullptr;   // 22px time — an LVGL label carries one
                                        // font, so the suffix needs its own

// ── Outdoor-temp mini graph (permanent, bottom-right of the card): the live
//    current temp plus the next 3 hourly forecasts, each printed under its
//    dot. ──
lv_obj_t* chartView = nullptr;
lv_obj_t* tempChart = nullptr;
lv_chart_series_t* tempSeries = nullptr;
float hourlyTempF[4] = {NAN, NAN, NAN, NAN};   // dot 0 = now, dots 1-3 = next 3 hours

// DRAW_PART_END: print each point's temp just under its dot.
static void chartPointLabelEvent(lv_event_t* e) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
  if (!lv_obj_draw_part_check_type(dsc, &lv_chart_class, LV_CHART_DRAW_PART_LINE_AND_POINT)) return;
  int idx = (int)dsc->id;
  if (idx < 0 || idx >= 4 || isnan(hourlyTempF[idx])) return;

  // Dot position. For dots 0-2, LVGL hands it to us in p1. For the final dot
  // on the right, LVGL draws it in a separate pass with p1/p2 == NULL, so we
  // compute its pixel position from the id (relative to the chart, hence the
  // +coords to make it absolute).
  lv_coord_t cx, cy;
  if (dsc->p1) {
    cx = dsc->p1->x;
    cy = dsc->p1->y;
  } else {
    lv_point_t pp;
    lv_chart_get_point_pos_by_id(tempChart, tempSeries, idx, &pp);
    lv_area_t co;
    lv_obj_get_coords(tempChart, &co);
    cx = co.x1 + pp.x;
    cy = co.y1 + pp.y;
  }

  char buf[8];
  lv_snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lroundf(hourlyTempF[idx]));

  lv_point_t sz;
  lv_txt_get_size(&sz, buf, &lv_font_montserrat_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

  lv_area_t a;
  a.x1 = cx - sz.x / 2;
  a.x2 = a.x1 + sz.x;
  a.y1 = cy + 5;   // just below the dot
  a.y2 = a.y1 + sz.y;

  lv_draw_label_dsc_t ldsc;
  lv_draw_label_dsc_init(&ldsc);
  ldsc.color = lv_color_hex(0xe8f2ec);
  ldsc.font = &lv_font_montserrat_12;
  lv_draw_label(dsc->draw_ctx, &ldsc, &a, buf, NULL);
}

void updateTempChart() {
  if (!tempChart || !tempSeries) return;
  float mn = 1e9f, mx = -1e9f;
  bool any = false;
  for (int i = 0; i < 4; i++) {
    float t = hourlyTempF[i];
    if (isnan(t)) continue;
    any = true;
    if (t < mn) mn = t;
    if (t > mx) mx = t;
    lv_chart_set_value_by_id(tempChart, tempSeries, i, (lv_coord_t)lroundf(t));
  }
  if (!any) return;
  // Asymmetric padding: extra room *below* the lowest dot so its under-dot
  // temp label has somewhere to go without colliding with the hour axis.
  int lo = (int)floorf(mn) - 5;
  int hi = (int)ceilf(mx) + 2;
  if (hi - lo < 7) hi = lo + 7;   // keep a sane spread when temps are near-flat
  lv_chart_set_range(tempChart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
  lv_chart_refresh(tempChart);
}

const char* weatherCodeToText(int code) {
  // Open-Meteo WMO weather codes, condensed
  if (code == 0) return "Clear";
  if (code == 1 || code == 2) return "Partly Cloudy";
  if (code == 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 85 && code <= 86) return "Snow Showers";
  if (code >= 95 && code <= 99) return "Thunderstorm";
  return "Unknown";
}

// Small "value over caption" chip used in the weather card's bottom row.
lv_obj_t* createStatChip(lv_obj_t* parent, lv_color_t valueColor, const char* caption, lv_obj_t** valueOut,
                          const lv_font_t* valueFont = &lv_font_montserrat_22,
                          const lv_font_t* capFont = &lv_font_montserrat_12) {
  lv_obj_t* col = lv_obj_create(parent);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_style_pad_row(col, 2, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* val = lv_label_create(col);
  lv_obj_set_style_text_font(val, valueFont, 0);
  lv_obj_set_style_text_color(val, valueColor, 0);
  lv_label_set_text(val, "--");

  lv_obj_t* cap = lv_label_create(col);
  lv_obj_set_style_text_font(cap, capFont, 0);
  lv_obj_set_style_text_color(cap, lv_color_hex(0x7a8a85), 0);
  lv_label_set_text(cap, caption);

  *valueOut = val;
  return col;
}

void drawClockText(const char* txt) {
  if (!timeCanvasBuf) return;
  lv_canvas_fill_bg(timeCanvas, lv_color_hex(0x0a0f0d), LV_OPA_COVER);  // erase previous digits

  // Each character is drawn independently, centered inside its own
  // fixed-width slot (clockTextX + i*clockCellW). Treating the whole
  // "HH:MM:SS" string as one block — even left-anchored — let a narrow
  // digit like '1' shrink the block's total width, dragging the visual
  // center left (worst case: "11:11:11"). Per-character cells remove that
  // coupling entirely: no digit's width can ever affect any other
  // character's position.
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.color = lv_color_hex(0x2fd67a);
  dsc.font = &lv_font_montserrat_48;
  dsc.align = LV_TEXT_ALIGN_CENTER;

  char cell[2] = {0, 0};
  for (int i = 0; i < 8 && txt[i]; i++) {
    cell[0] = txt[i];
    lv_canvas_draw_text(timeCanvas, clockTextX + i * clockCellW, 8, clockCellW, &dsc, cell);
  }
}

// ---------- Shared network plumbing ----------
// The network calls below can block for seconds if the peer is unreachable
// (a TCP connect to a dead host has to time out). That's long enough to make
// touch input — which is only serviced when lv_timer_handler() runs on the
// main loop — feel completely locked up (e.g. tapping the gear icon to reach
// Settings just does nothing for a few seconds). Spotify fetching is done
// from a small FreeRTOS task instead, so a dead endpoint only stalls that
// task, never the UI. Only plain data crosses between tasks, guarded by
// per-feature mutexes; every LVGL call stays on the main loop, since LVGL
// itself isn't thread-safe.

// Serializes ALL network I/O across spotifyPollTask() and pollWeather()
// (called from the main loop) so at most one TLS/HTTP connection is ever
// open at a time, device-wide.
//
// Added after real heap instrumentation (timestamped free-heap +
// largest-contiguous-block logging around every network call) showed
// exactly what was driving a recurring stack-overflow crash: whenever two
// polls happened to have connections open at the same moment,
// largest_block would crash from a healthy ~36KB down to
// 1-9KB — and once that got bad enough, it stopped recovering at all.
// Serializing connections is a
// direct fix for the mechanism the data showed, rather than another guess at
// a stack size.
//
// NetworkLock is a tiny RAII guard so every function that touches the
// network can just declare one locally and not worry about releasing it on
// early-return paths (there are several) — the destructor does it
// automatically when the function returns, however it returns.
static SemaphoreHandle_t networkMutex = nullptr;

class NetworkLock {
public:
  bool ok;
  NetworkLock() {
    // Bounded acquisition. portMAX_DELAY here used to be the propagation
    // channel for every "display frozen, console silent" event on this
    // device: one task wedges inside a socket call, everything else queues
    // behind the held mutex forever — loopTask's weather poll among them,
    // and the clock freezes with it. With a hard ceiling the absolute worst
    // case is a skipped poll; callers check ok and bail.
    ok = xSemaphoreTake(networkMutex, pdMS_TO_TICKS(20000)) == pdTRUE;
  }
  ~NetworkLock() { if (ok) xSemaphoreGive(networkMutex); }
};

// ---- Bounded DNS ----
// The task watchdog caught spotifyPoll parked inside http.GET() for 90+s.
// Reading this core's sources: every other step of that call chain is
// timer-bounded (connect = select() with timeout, TLS handshake timeout,
// SO_RCVTIMEO/SO_SNDTIMEO on the socket, HTTPClient's own 5s timers) — but
// WiFiClientSecure::connect(host,...) starts with WiFi.hostByName() →
// getaddrinfo(), and the esp-idf 4.4 lwIP port waits on a semaphore with NO
// timeout. When the interface flaps mid-query the DNS callback never fires
// and the caller parks forever (holding networkMutex → the UI thread's next
// weather poll queues behind it → the exact "display frozen, console silent"
// symptom that started this whole hunt).
//
// So: resolve in a disposable worker task with a hard deadline, then connect
// by IP — HTTPClient::connect() reuses an already-connected client without
// re-resolving. A wedged resolver gets abandoned (leaks ~6KB: the task stack
// + ctx) while the caller returns a clean failure into the normal backoff;
// the task watchdog catches anything pathological even if it recurs.
// The timeout must exceed a *healthy* slow DNS (lwIP normally answers or
// gives up well inside ~10s) minus the WDT margin — 20s, with TASK_WDT_TIMEOUT_S=90 still leaving
// room for a full token+poll+art chain behind it.
static bool resolveWithTimeout(const char* host, IPAddress& out, uint32_t timeoutMs) {
  struct ResolveCtx {
    char host[96];
    volatile bool done;
    IPAddress ip;
  };
  ResolveCtx* ctx = new ResolveCtx;
  if (!ctx) return false;
  strncpy(ctx->host, host, sizeof(ctx->host) - 1);
  ctx->host[sizeof(ctx->host) - 1] = '\0';
  ctx->done = false;

  if (xTaskCreate([](void* p) {
        ResolveCtx* c = (ResolveCtx*)p;
        IPAddress a;
        if (WiFi.hostByName(c->host, a)) {
          c->ip = a;
          c->done = true;   // last touch of ctx before exiting; caller frees on done
        }
        vTaskDelete(NULL);
      }, "dnsOnce", 6144, ctx, 1, NULL) != pdPASS) {
    delete ctx;
    return false;
  }

  uint32_t start = millis();
  while (!ctx->done && millis() - start < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (ctx->done) {
    out = ctx->ip;
    delete ctx;      // resolver finished; safe (done is its last write)
    return true;
  }
  // Wedged (or just slow): abandon it. ctx + task leak — see note above.
  Serial.printf("DNS lookup stalled for %s — abandoned after %ums\n", host, (unsigned)timeoutMs);
  return false;
}

// Forces an immediate weather poll on the next loop() pass — set by
// wakeScreen() after the display wakes from Sleep, instead of leaving weather
// sitting on whatever it was up to WEATHER_POLL_MS ago.
static volatile bool weatherForceRefresh = false;

void buildUi() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0f0d), 0);

  // Full-screen container for the clock/weather UI (built below).
  clockScreen = lv_obj_create(scr);
  lv_obj_remove_style_all(clockScreen);
  lv_obj_set_size(clockScreen, 480, 320);
  lv_obj_set_pos(clockScreen, 0, 0);
  lv_obj_clear_flag(clockScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_style_init(&styleCard);
  lv_style_set_bg_color(&styleCard, lv_color_hex(0x11201a));
  lv_style_set_bg_opa(&styleCard, LV_OPA_COVER);
  lv_style_set_border_color(&styleCard, lv_color_hex(0x1c2622));
  lv_style_set_border_width(&styleCard, 1);
  lv_style_set_radius(&styleCard, 6);
  lv_style_set_pad_all(&styleCard, 8);

  // ── Clock (top, large) ───────────────────────────────────────────
  // Drawn onto a canvas and scaled with lv_img_set_zoom — 48px is the
  // largest built-in LVGL font, and canvas/img zoom (unlike zooming a
  // plain label) is reliably supported since it computes its own extended
  // draw size for the transform.
  timeCanvasBuf = (lv_color_t*)malloc(CLOCK_CANVAS_W * CLOCK_CANVAS_H * sizeof(lv_color_t));
  timeCanvas = lv_canvas_create(clockScreen);
  if (timeCanvasBuf) {
    lv_canvas_set_buffer(timeCanvas, timeCanvasBuf, CLOCK_CANVAS_W, CLOCK_CANVAS_H, LV_IMG_CF_TRUE_COLOR);
    // Fixed per-character cell width, sized to the widest digit ("0" and
    // "8" are the widest in Montserrat; "1" is narrower, which is exactly
    // what caused the drift this is fixing). Computed once, reused for
    // every draw — see drawClockText().
    clockCellW = lv_txt_get_width("0", 1, &lv_font_montserrat_48, 0, LV_TEXT_FLAG_NONE);
    clockTextX = (CLOCK_CANVAS_W - 8 * clockCellW) / 2;
    drawClockText("--:--:--");
  } else {
    Serial.println("Clock canvas buffer allocation failed!");
  }
  // Zoom scales around the canvas's own center (img/canvas default pivot).
  // Full-size clock, kept at its original position.
  lv_obj_align(timeCanvas, LV_ALIGN_TOP_MID, 0, (114 - CLOCK_CANVAS_H) / 2);
  lv_img_set_zoom(timeCanvas, CLOCK_CANVAS_ZOOM);

  // Day + date, on one line under the clock (moved here from the card's
  // top-right, which Spotify now occupies). The clock's digits end well above
  // the band's bottom, so this tucks in below them; the card was nudged down a
  // touch to make room. Tweak y if it crowds.
  clockDateLabel = lv_label_create(clockScreen);
  lv_obj_set_style_text_font(clockDateLabel, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(clockDateLabel, lv_color_hex(0x9fb3ab), 0);
  lv_label_set_text(clockDateLabel, "---");
  lv_obj_align(clockDateLabel, LV_ALIGN_TOP_MID, 0, 92);

  // ── Weather card — pushed down to clear the bigger clock, and now
  //    stretches to the bottom of the screen with a proper internal layout.
  lv_obj_t* card = lv_obj_create(clockScreen);
  lv_obj_add_style(card, &styleCard, 0);
  lv_obj_set_pos(card, 14, 126);   // nudged down from 114 to clear the day/date under the clock
  lv_obj_set_size(card, 452, 320 - 126 - 10);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_style_pad_row(card, 6, 0);   // tightened to give the rows more usable height
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  // Top row: [temp + condition/hi-lo] [sunrise/sunset tile] | divider | [Spotify now-playing]
  lv_obj_t* topRow = lv_obj_create(card);
  lv_obj_set_style_bg_opa(topRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(topRow, 0, 0);
  lv_obj_set_style_pad_all(topRow, 0, 0);
  lv_obj_set_style_pad_column(topRow, 14, 0);
  lv_obj_clear_flag(topRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(topRow, LV_PCT(100));
  lv_obj_set_height(topRow, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(topRow, LV_FLEX_FLOW_ROW);
  // START (not SPACE_BETWEEN) packs the temp group + sun tile + divider
  // together on the left; spotifyRow grows to fill the rest, so the divider
  // hugs the weather side instead of floating mid-row.
  lv_obj_set_flex_align(topRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Left group: the temperature, left of the divider — a big number with its
  // labels stacked underneath. The wrapper stays even though it holds one child;
  // the divider and the Spotify block hang off this row's start.
  lv_obj_t* leftGroup = lv_obj_create(topRow);
  lv_obj_set_style_bg_opa(leftGroup, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(leftGroup, 0, 0);
  lv_obj_set_style_pad_all(leftGroup, 0, 0);
  lv_obj_set_style_pad_column(leftGroup, 16, 0);   // no-op with one child; harmless if another lands here
  lv_obj_clear_flag(leftGroup, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(leftGroup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(leftGroup, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(leftGroup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  // Temperature readout: big number with condition + hi/lo stacked underneath.
  lv_obj_t* tempCol = lv_obj_create(leftGroup);
  lv_obj_set_style_bg_opa(tempCol, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tempCol, 0, 0);
  lv_obj_set_style_pad_all(tempCol, 0, 0);
  lv_obj_set_style_pad_row(tempCol, 2, 0);
  lv_obj_clear_flag(tempCol, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(tempCol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tempCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tempCol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  tempLabel = lv_label_create(tempCol);
  lv_obj_set_style_text_font(tempLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(tempLabel, lv_color_hex(0x38b0ff), 0);
  lv_label_set_text(tempLabel, "--\xC2\xB0");

  lv_obj_t* condCol = lv_obj_create(tempCol);
  lv_obj_set_style_bg_opa(condCol, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(condCol, 0, 0);
  lv_obj_set_style_pad_all(condCol, 0, 0);
  lv_obj_set_style_pad_row(condCol, 4, 0);
  lv_obj_clear_flag(condCol, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(condCol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(condCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(condCol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  conditionLabel = lv_label_create(condCol);
  lv_obj_set_style_text_font(conditionLabel, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(conditionLabel, lv_color_hex(0xe8f2ec), 0);
  lv_label_set_text(conditionLabel, "Loading...");

  hiLoLabel = lv_label_create(condCol);
  lv_obj_set_style_text_font(hiLoLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hiLoLabel, lv_color_hex(0x7a8a85), 0);
  lv_label_set_text(hiLoLabel, "H: -- / L: --");

  // Sunrise/sunset tile. Sits right of the weather and LEFT of the divider —
  // it reads as part of the weather block, which it is (same API response).
  lv_obj_t* sunTile = lv_obj_create(topRow);
  // Styled like the other transparent wrappers on this row (tempCol,
  // spotifyRow) — LVGL's default lv_obj carries a background + padding, so
  // every wrapper has to opt out or it reads as a card inside the card.
  lv_obj_set_style_bg_opa(sunTile, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sunTile, 0, 0);
  lv_obj_set_style_pad_all(sunTile, 0, 0);
  lv_obj_set_style_pad_row(sunTile, 1, 0);
  lv_obj_clear_flag(sunTile, LV_OBJ_FLAG_SCROLLABLE);
  // Worst case width is "12:59"+suffix — the 12h clock format caps the hour
  // part at two digits regardless of where the device is actually pointed.
  // (The 12h conversion in applyWeather() stays generic; this is sizing only.)
  lv_obj_set_width(sunTile, 78);
  lv_obj_set_height(sunTile, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(sunTile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sunTile, 0, 0);

  // Caption/value rhythm: tiny gray label, big
  // colored value. AM/PM renders as a small gray suffix on its own baseline —
  // an LVGL label has one font, so the split is per-label, wrapped in a
  // transparent flex row that bottom-aligns the pair.
  // (riseRow/setRow: transparent wrappers, ROW flow, cross-axis END so the
  // 12px suffix sits on the 22px digits' baseline instead of floating.)
  auto sunRow = [&](lv_obj_t*& valOut, lv_obj_t*& apOut, uint32_t color) {
    lv_obj_t* row = lv_obj_create(sunTile);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    valOut = lv_label_create(row);
    lv_obj_set_style_text_font(valOut, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(valOut, lv_color_hex(color), 0);
    lv_label_set_text(valOut, "--");

    apOut = lv_label_create(row);
    lv_obj_set_style_text_font(apOut, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(apOut, lv_color_hex(0x7a8a85), 0);
    lv_label_set_text(apOut, "");
  };

  lv_obj_t* riseCap = lv_label_create(sunTile);
  lv_obj_set_style_text_font(riseCap, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(riseCap, lv_color_hex(0x7a8a85), 0);
  lv_label_set_text(riseCap, "SUNRISE");
  sunRow(sunriseLabel, sunriseAmPmLabel, 0xf0c860);              // dawn gold

  lv_obj_t* setCap = lv_label_create(sunTile);
  lv_obj_set_style_pad_top(setCap, 3, 0);
  lv_obj_set_style_text_font(setCap, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(setCap, lv_color_hex(0x7a8a85), 0);
  lv_label_set_text(setCap, "SUNSET");
  sunRow(sunsetLabel, sunsetAmPmLabel, 0xe8944a);                // dusk orange

  // Divider between the weather/sunrise block and the now-playing text.
  lv_obj_t* divider = lv_obj_create(topRow);
  lv_obj_set_style_bg_color(divider, lv_color_hex(0x22322c), 0);
  lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(divider, 0, 0);
  lv_obj_set_style_radius(divider, 0, 0);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(divider, 1);
  lv_obj_set_height(divider, 96);   // brackets the sun tile beside it

  // Now-playing text column — the only thing right of the divider.
  lv_obj_t* spotifyRow = lv_obj_create(topRow);
  lv_obj_set_style_bg_opa(spotifyRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spotifyRow, 0, 0);
  lv_obj_set_style_pad_all(spotifyRow, 0, 0);
  lv_obj_clear_flag(spotifyRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_height(spotifyRow, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(spotifyRow, 1);   // absorb remaining row width
  lv_obj_set_flex_flow(spotifyRow, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(spotifyRow, 2, 0);
  lv_obj_set_flex_align(spotifyRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  lv_obj_t* npCap = lv_label_create(spotifyRow);
  lv_obj_set_style_text_font(npCap, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(npCap, lv_color_hex(0x1db954), 0);   // Spotify green
  lv_label_set_text(npCap, "NOW PLAYING");

  songLabel = lv_label_create(spotifyRow);
  lv_obj_set_style_text_font(songLabel, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(songLabel, lv_color_hex(0xe8f2ec), 0);
  lv_obj_set_width(songLabel, LV_PCT(100));
  lv_label_set_long_mode(songLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);   // marquee long titles
  lv_label_set_text(songLabel, "--");

  artistLabel = lv_label_create(spotifyRow);
  lv_obj_set_style_text_font(artistLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(artistLabel, lv_color_hex(0x7a8a85), 0);
  lv_obj_set_width(artistLabel, LV_PCT(100));
  lv_label_set_long_mode(artistLabel, LV_LABEL_LONG_DOT);   // ellipsize long artists
  lv_label_set_text(artistLabel, "");

  // Horizontal divider above the stat chips.
  lv_obj_t* hDivider = lv_obj_create(card);
  lv_obj_set_style_bg_color(hDivider, lv_color_hex(0x22322c), 0);
  lv_obj_set_style_bg_opa(hDivider, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hDivider, 0, 0);
  lv_obj_set_style_radius(hDivider, 0, 0);
  lv_obj_clear_flag(hDivider, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(hDivider, LV_PCT(100));
  lv_obj_set_height(hDivider, 1);

  // Bottom row: [outdoor stat chips, tightly grouped] | divider | [outdoor-temp graph, right side]
  lv_obj_t* bottomRow = lv_obj_create(card);
  lv_obj_set_style_bg_opa(bottomRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bottomRow, 0, 0);
  lv_obj_set_style_pad_all(bottomRow, 0, 0);
  lv_obj_set_style_pad_column(bottomRow, 12, 0);
  lv_obj_clear_flag(bottomRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(bottomRow, LV_PCT(100));
  lv_obj_set_flex_grow(bottomRow, 1);
  lv_obj_set_flex_flow(bottomRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bottomRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* outdoorGroup = lv_obj_create(bottomRow);
  lv_obj_set_style_bg_opa(outdoorGroup, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(outdoorGroup, 0, 0);
  lv_obj_set_style_pad_all(outdoorGroup, 0, 0);
  lv_obj_set_style_pad_column(outdoorGroup, 14, 0);   // tightened from the old space-evenly spread
  lv_obj_clear_flag(outdoorGroup, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(outdoorGroup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(outdoorGroup, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(outdoorGroup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  createStatChip(outdoorGroup, lv_color_hex(0xff6b4d), "FEELS LIKE", &feelsLikeLabel);
  createStatChip(outdoorGroup, lv_color_hex(0x38b0ff), "HUMIDITY", &humidityLabel);
  createStatChip(outdoorGroup, lv_color_hex(0x2fd67a), "WIND", &windLabel);

  // Vertical divider between the outdoor stats and the temp graph.
  lv_obj_t* bottomDivider = lv_obj_create(bottomRow);
  lv_obj_set_style_bg_color(bottomDivider, lv_color_hex(0x22322c), 0);
  lv_obj_set_style_bg_opa(bottomDivider, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bottomDivider, 0, 0);
  lv_obj_set_style_radius(bottomDivider, 0, 0);
  lv_obj_clear_flag(bottomDivider, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(bottomDivider, 1);
  lv_obj_set_height(bottomDivider, 42);

  // Outdoor-temp graph fills the rest of the row — the spot the Govee box used
  // to hold. Permanent now (no tap toggle), since the indoor sensor is gone.
  chartView = lv_obj_create(bottomRow);
  lv_obj_set_style_bg_opa(chartView, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chartView, 0, 0);
  lv_obj_set_style_pad_all(chartView, 0, 0);
  lv_obj_set_style_pad_row(chartView, 2, 0);
  lv_obj_clear_flag(chartView, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_height(chartView, LV_PCT(100));
  lv_obj_set_flex_grow(chartView, 1);
  lv_obj_set_flex_flow(chartView, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(chartView, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Header dropped for vertical room — the full-height chart takes its place.
  // No Y-axis number labels: each dot's temp is printed under it
  // (chartPointLabelEvent). Nudge set_size / pad values if anything clips.
  tempChart = lv_chart_create(chartView);
  lv_obj_set_size(tempChart, 168, 56);
  lv_obj_clear_flag(tempChart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(tempChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tempChart, 0, 0);
  // Inset the plot so the first/last dots sit off the edges — gives their
  // under-dot temp labels room to center without clipping.
  lv_obj_set_style_pad_left(tempChart, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_right(tempChart, 14, LV_PART_MAIN);
  lv_obj_set_style_line_color(tempChart, lv_color_hex(0x22322c), LV_PART_MAIN);   // division lines
  lv_obj_set_style_text_color(tempChart, lv_color_hex(0x7a8a85), LV_PART_TICKS);
  lv_obj_set_style_text_font(tempChart, &lv_font_montserrat_12, LV_PART_TICKS);
  lv_obj_set_style_size(tempChart, 4, LV_PART_INDICATOR);   // data-point marker size
  lv_chart_set_type(tempChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(tempChart, 4);
  lv_chart_set_div_line_count(tempChart, 2, 0);
  lv_obj_add_event_cb(tempChart, chartPointLabelEvent, LV_EVENT_DRAW_PART_END, NULL);
  tempSeries = lv_chart_add_series(tempChart, lv_color_hex(0x38b0ff), LV_CHART_AXIS_PRIMARY_Y);

  // ── Settings menu — tap the clock (or the gear icon) to open it. ──
  lv_obj_add_flag(timeCanvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(timeCanvas, [](lv_event_t* e) { openSettingsMenu(); }, LV_EVENT_CLICKED, NULL);

  // Settings entry point (bottom-center of the screen) — the clock itself is
  // also tappable, but the gear is the discoverable path. Hidden by default;
  // loop() reveals it on any touch and hides it again after
  // SETTINGS_GEAR_TIMEOUT_MS of no further touches.
  // On the clock screen it lands over the (non-clickable) temp chart, so
  // it's a minor cosmetic overlap, not a false-tap risk.
  settingsGearBtn = lv_btn_create(scr);
  lv_obj_set_size(settingsGearBtn, 36, 36);
  lv_obj_align(settingsGearBtn, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_style_bg_color(settingsGearBtn, lv_color_hex(0x11201a), 0);
  lv_obj_set_style_bg_opa(settingsGearBtn, LV_OPA_50, 0);
  lv_obj_set_style_border_width(settingsGearBtn, 0, 0);
  lv_obj_set_style_radius(settingsGearBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(settingsGearBtn, 0, 0);
  lv_obj_set_ext_click_area(settingsGearBtn, 12);   // it's small — pad the hit area generously
  lv_obj_add_flag(settingsGearBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(settingsGearBtn, [](lv_event_t* e) { openSettingsMenu(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* gearLabel = lv_label_create(settingsGearBtn);
  lv_obj_set_style_text_color(gearLabel, lv_color_hex(0x9fb3ab), 0);
  lv_label_set_text(gearLabel, LV_SYMBOL_SETTINGS);
  lv_obj_center(gearLabel);

  // Full-screen dim backdrop. Tapping the backdrop itself (not the card) closes the menu.
  settingsOverlay = lv_obj_create(scr);
  lv_obj_remove_style_all(settingsOverlay);
  lv_obj_set_size(settingsOverlay, 480, 320);
  lv_obj_set_pos(settingsOverlay, 0, 0);
  lv_obj_set_style_bg_color(settingsOverlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsOverlay, LV_OPA_70, 0);
  lv_obj_clear_flag(settingsOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(settingsOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(settingsOverlay, [](lv_event_t* e) {
    if (lv_event_get_target(e) == settingsOverlay) closeSettingsMenu();
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(settingsOverlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* settingsCard = lv_obj_create(settingsOverlay);
  lv_obj_add_style(settingsCard, &styleCard, 0);
  lv_obj_set_size(settingsCard, 260, LV_SIZE_CONTENT);
  lv_obj_center(settingsCard);
  lv_obj_clear_flag(settingsCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(settingsCard, 16, 0);
  lv_obj_set_style_pad_row(settingsCard, 10, 0);
  lv_obj_set_flex_flow(settingsCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(settingsCard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* settingsTitle = lv_label_create(settingsCard);
  lv_obj_set_style_text_font(settingsTitle, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(settingsTitle, lv_color_hex(0xe8f2ec), 0);
  lv_label_set_text(settingsTitle, "Settings");

  // For the Spotify/location settings page — refreshed each time the menu
  // opens (openSettingsMenu()) since the IP isn't known yet at build time.
  deviceInfoLabel = lv_label_create(settingsCard);
  lv_obj_set_style_text_font(deviceInfoLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(deviceInfoLabel, lv_color_hex(0x7a8a85), 0);
  lv_obj_set_style_text_align(deviceInfoLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(deviceInfoLabel, "");

  wifiQr = lv_qrcode_create(settingsCard, 120, lv_color_hex(0x0a0f0d), lv_color_hex(0xe8f2ec));
  lv_obj_add_flag(wifiQr, LV_OBJ_FLAG_HIDDEN);   // only shown while the setup portal is open

  wifiStatusLabel = lv_label_create(settingsCard);
  lv_obj_set_style_text_font(wifiStatusLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(wifiStatusLabel, lv_color_hex(0x9fb3ab), 0);
  lv_obj_set_style_text_align(wifiStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(wifiStatusLabel, 228);
  lv_label_set_long_mode(wifiStatusLabel, LV_LABEL_LONG_WRAP);
  lv_label_set_text(wifiStatusLabel, "");
  lv_obj_add_flag(wifiStatusLabel, LV_OBJ_FLAG_HIDDEN);   // only shown while the setup portal is open

  lv_obj_t* recalTouchBtn = lv_btn_create(settingsCard);
  lv_obj_set_width(recalTouchBtn, LV_PCT(100));
  lv_obj_set_style_bg_color(recalTouchBtn, lv_color_hex(0x22322c), 0);
  lv_obj_add_event_cb(recalTouchBtn, [](lv_event_t* e) { recalibrateTouchAndRedraw(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* recalTouchLabel = lv_label_create(recalTouchBtn);
  lv_label_set_text(recalTouchLabel, "Recalibrate Touch");
  lv_obj_center(recalTouchLabel);

  lv_obj_t* sleepBtn = lv_btn_create(settingsCard);
  lv_obj_set_width(sleepBtn, LV_PCT(100));
  lv_obj_set_style_bg_color(sleepBtn, lv_color_hex(0x1a1a2e), 0);   // night blue — reads as "lights out"
  lv_obj_add_event_cb(sleepBtn, [](lv_event_t* e) { sleepScreen(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* sleepLabel = lv_label_create(sleepBtn);
  lv_label_set_text(sleepLabel, "Sleep");
  lv_obj_center(sleepLabel);

  settingsCloseBtn = lv_btn_create(settingsCard);
  lv_obj_set_width(settingsCloseBtn, LV_PCT(100));
  lv_obj_set_style_bg_color(settingsCloseBtn, lv_color_hex(0x22322c), 0);
  lv_obj_add_event_cb(settingsCloseBtn, [](lv_event_t* e) { closeSettingsMenu(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* closeLabel = lv_label_create(settingsCloseBtn);
  lv_label_set_text(closeLabel, "Close");
  lv_obj_center(closeLabel);

}

void openSettingsMenu() {
  if (!settingsOverlay) return;
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("not connected");
  lv_label_set_text_fmt(deviceInfoLabel, "Settings page:\nhttp://%s   PIN: %s", ip.c_str(), settingsPin);
  lv_obj_clear_flag(settingsOverlay, LV_OBJ_FLAG_HIDDEN);
}

void closeSettingsMenu() {
  if (!settingsOverlay) return;
  lv_obj_add_flag(settingsOverlay, LV_OBJ_FLAG_HIDDEN);
}

// Puts the display to bed: BL off, ST7796 into sleep mode (SLPIN — panel
// stops scanning, wakes need a re-init, which LovyanGFX::wake() handles),
// and raises the flag that parks the network poll tasks. Nothing else stops:
// the clock keeps ticking, WiFi stays up, the web server keeps answering,
// and loop() polls the touch chip for the wake gesture.
void sleepScreen() {
  closeSettingsMenu();
  deviceSleeping = true;
  Serial.println("Sleep: backlight off, panel sleeping, polls paused. Touch to wake.");
  display.setBrightness(0);
  display.sleep();
}

// Wake half of the sleep feature. Called from loop()'s sleep branch when the
// touch chip reports a press; drains the press first so waking never doubles
// as a tap on the UI underneath (LVGL never sees this press — lv_timer_handler
// is skipped the whole time we're asleep).
void wakeScreen() {
  uint32_t holdStart = millis();
  uint16_t tx, ty;
  while (display.getTouch(&tx, &ty) && millis() - holdStart < 3000)
    vTaskDelay(pdMS_TO_TICKS(20));   // let go of it (3s cap so a stuck press can't wedge the loop)

  display.wakeup();                  // LovyanGFX: wake + re-init + restore brightness
  display.setBrightness(200);        // operating level — same value setup() uses
  deviceSleeping = false;
  lastTouchMs = millis();
  screenEverTouched = true;

  // The panel lost its GRAM in sleep and LVGL skipped rendering the whole
  // time — repaint everything, and force fresh data rather than showing a
  // picture of last night for another poll interval.
  lv_obj_invalidate(lv_scr_act());
  updateClock();
  weatherForceRefresh = true;
  Serial.println("Screen woken by touch");
}

// ---------- Networking ----------
uint32_t lastGoodWeatherMs = 0;
#define WEATHER_STALE_AFTER_MS (WEATHER_POLL_MS * 2)

// ---------- Spotify (now playing) ----------
// Auth is the Authorization Code *refresh-token* flow. You do the browser
// consent once to get a long-lived refresh token (see setup notes); the device
// swaps it for short-lived access tokens on its own. HTTPS is mandatory here
// (no HTTP fallback like Open-Meteo), which is why the BLE removal matters —
// it frees the heap the mbedTLS handshake needs.
//
// Both token refresh and the now-playing poll run on a background task
// (spotifyPollTask, core 0), not the UI thread. Reason: this device does a
// fresh TLS handshake every poll, and under heap fragmentation those handshakes
// can outright fail (mbedTLS "X509/BIGNUM allocation failed") — and a stalled
// connect() could block for a very long time before giving up (which is why the
// handshake is now hard-bounded by setHandshakeTimeout(5); see spotifyFetchAccessToken).
// On the UI thread a stall would freeze the whole display, clock included; off
// it, it just delays the next now-playing update. Only a small snapshot struct
// crosses between tasks, guarded by spotifyDataMutex; every LVGL call stays on
// the main loop — see spotifyApplyIfDirty(), called from loop().
//
// If Spotify's handshakes still fail too often on this PSRAM-less board, the
// durable fix is moving the Spotify calls off the ESP32 entirely (a small LAN
// relay doing the HTTPS/OAuth work, with the device just polling plain HTTP).
// See [[board-heap-constraints]].
struct SpotifyNowPlaying {
  String songName;
  String artistName;
  bool playing = false;
  bool nothingPlaying = false;   // true on HTTP 204 — no active device
};

static SemaphoreHandle_t spotifyDataMutex = nullptr;   // also guards cfgSpotifyClientId/Secret/RefreshToken
static SpotifyNowPlaying spotifyPending;
static volatile bool spotifyNowPlayingDirty = false;



String spotifyAccessToken = "";
uint32_t spotifyTokenExpiresAt = 0;   // millis() deadline for the current access token — bg-task-only, no mutex needed

bool spotifyFetchAccessToken() {
  String clientId, clientSecret, refreshToken;
  xSemaphoreTake(spotifyDataMutex, portMAX_DELAY);
  clientId = cfgSpotifyClientId;
  clientSecret = cfgSpotifyClientSecret;
  refreshToken = cfgSpotifyRefreshToken;
  xSemaphoreGive(spotifyDataMutex);

  if (WiFi.status() != WL_CONNECTED) return false;
  if (refreshToken.length() == 0) return false;   // not configured yet

  NetworkLock lock;   // see the comment above networkMutex's declaration
  if (!lock.ok) { Serial.println("net busy, request skipped"); return false; }
  WiFiClientSecure client;
  client.setInsecure();   // skip cert validation to save heap (same tradeoff as weather used)
  client.setHandshakeTimeout(5);   // SECONDS — hard-bound the TLS handshake. Default is 120s, so a flaky handshake would otherwise block this task ~2min holding its ~50KB mbedTLS context, pinning the largest heap block and starving every later poll. This is the actual "no Spotify data" hang.
  client.setTimeout(5);   // SECONDS (WiFiClientSecure::setTimeout takes seconds, not ms — the old setTimeout(5000) meant 5000s). Socket read timeout for the post-handshake response.

  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) return false;
  // setReuse(false) forces "Connection: close": a fresh client is created for
  // each request and never reused, so a kept-alive socket would linger rather
  // than close and slowly exhaust this device's small socket table.
  http.setReuse(false);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setAuthorization(clientId.c_str(), clientSecret.c_str());   // HTTPClient builds the Basic header

  // Pre-resolve + pre-connect by IP so no getaddrinfo sits on this path —
  // HTTPClient reuses the connected client (see resolveWithTimeout).
  {
    IPAddress tokIp;
    if (!resolveWithTimeout("accounts.spotify.com", tokIp, 15000)) return false;
    // The (ip, port, host, …) overload — NOT plain (ip, port): that path calls
    // mbedtls_ssl_set_hostname() with the IP string as SNI, and Spotify's edge
    // answers a mis-SNI'd session with HTTP 421. Bounded DNS, correct SNI.
    if (!client.connect(tokIp, 443, "accounts.spotify.com", NULL, NULL, NULL)) {
      Serial.println("Spotify token connect failed");
      return false;
    }
  }
  int code = http.POST("grant_type=refresh_token&refresh_token=" + refreshToken);
  bool ok = false;
  if (code == 200) {
    JsonDocument filter;                 // only pull the two fields we use
    filter["access_token"] = true;
    filter["expires_in"] = true;
    JsonDocument doc;
    // getString() (not getStream()) so chunked responses are de-chunked first.
    if (!deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter))) {
      spotifyAccessToken = String((const char*)(doc["access_token"] | ""));
      uint32_t expiresIn = doc["expires_in"] | 3600;
      spotifyTokenExpiresAt = millis() + (expiresIn - 60) * 1000UL;   // refresh a minute early
      ok = spotifyAccessToken.length() > 0;
    }
  } else {
    Serial.printf("Spotify token refresh failed: %d\n", code);
  }
  http.end();
  return ok;
}


// Fetches now-playing info.
// Only ever called from spotifyPollTask() (background task) — stashes
// results for the UI thread instead of touching LVGL directly (see
// spotifyApplyIfDirty(), called from loop()).
// Returns true on success (or "nothing to do" — WiFi down / heap too low to
// safely attempt), false on an actual failed request, so the caller
// (spotifyPollTask()) can back off when polls start failing instead of
// retrying at full speed. Added after a crash trail showed repeated "SSL -
// Memory allocation failed" starting within a few seconds of boot and
// recurring on essentially every 5s poll with nothing slowing it down —
// each failed TLS attempt still costs heap/socket resources on the way to
// failing, and hammering that every 5s forever is what was driving this
// device into a stack-overflow crash — the backoff below means a wedged
// endpoint gets progressively less traffic instead of a fresh attempt
// every 5s forever.
bool spotifyPollOnce() {
  if (WiFi.status() != WL_CONNECTED) return true;

  // A fragmented heap makes a TLS handshake likely to fail anyway (mbedTLS
  // needs a decent contiguous chunk for X509/BIGNUM buffers) and, worse, a
  // failing handshake can take a long time to give up. Skip the attempt
  // entirely rather than repeatedly fighting for heap that isn't there.
  uint32_t contigHeap = ESP.getMaxAllocHeap();   // largest single block
  uint32_t freeHeap   = ESP.getFreeHeap();       // total across all blocks — mbedTLS spreads its buffers across them
  if (contigHeap < TLS_MIN_CONTIG_BLOCK || freeHeap < TLS_MIN_TOTAL_FREE) {
    Serial.printf("Spotify poll skipped, heap tight: largest=%u free=%u\n", (unsigned)contigHeap, (unsigned)freeHeap);
    return false;   // still counts toward backoff — a low-heap skip means something's already under pressure
  }

  // Persistent now-playing TLS connection (used by the block below; declared
  // up here because the token handshake needs it closed first — this heap
  // cannot host two ~50KB mbedTLS contexts at once, and unlike a plain
  // socket, an idle keep-alive client holds its context around.)
  static WiFiClientSecure npClient;

  // Refresh the access token if we don't have one or it's about to expire.
  if (spotifyAccessToken.length() == 0 || (int32_t)(millis() - spotifyTokenExpiresAt) >= 0) {
    npClient.stop();   // free its mbedTLS context so the token handshake fits
    if (!spotifyFetchAccessToken()) return false;
  }

  bool nowPlayingOk = true;

  {
    // Client/HTTP kept in a tight scope so their destructors — which is what
    // actually frees WiFiClientSecure's ~50KB mbedTLS context, not http.end() —
    // run promptly instead of lingering to function return.
    // Phase markers: the task watchdog proved spotifyPoll parks in one of
    // these steps for 90+s despite the socket timeouts (a blocked task prints
    // nothing, so the last marker before a TWDT panic names the step —
    // "taking netlock" = waiting on networkMutex, "GET" = DNS/TCP/TLS/recv).
    NetworkLock lock;   // see the comment above networkMutex's declaration — released when this block ends
    if (!lock.ok) { Serial.println("net busy, request skipped"); return false; }
    // Persistent TLS connection. Until now every 5s poll ran a full
    // TCP+TLS handshake — ~17k/day against a link demonstrably bad at
    // exactly that — and both watchdog catches were wedges inside GET() on a
    // *fresh* handshake. Spotify's API is built for keep-alive; holding the
    // connection makes connect churn, per-poll mbedTLS context alloc/free,
    // and stale-edge-connection resets mostly disappear. static here is
    // single-owner: only ever touched from spotifyPollTask.
    WiFiClientSecure& client = npClient;
    if (!client.connected()) {
      client.stop();                 // drop any half-dead PCB before re-arming
      client.setInsecure();
      client.setHandshakeTimeout(5); // SECONDS — hard-bounds the handshake
      client.setTimeout(5);          // SECONDS — socket read timeout
      IPAddress npIp;
      if (!resolveWithTimeout("api.spotify.com", npIp, 15000)) return false;
      // (ip, port, host, …) overload so the TLS SNI stays "api.spotify.com"
      // — see the 421 note in spotifyFetchAccessToken.
      if (!client.connect(npIp, 443, "api.spotify.com", NULL, NULL, NULL)) {
        Serial.println("Spotify connect failed");
        return false;
      }
    }

    // Per-poll HTTPClient wrapper over the persistent client. With
    // setReuse(true) this core's HTTPClient::disconnect() leaves the
    // connection OPEN when the wrapper is destroyed after the body was read
    // — verified in the 2.0.17 sources (it only calls _client->stop() when
    // !_reuse).
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing")) return false;
    http.setReuse(true);
    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
    int code = http.GET();
    if (code < 0) client.stop();   // transport-level failure: dead or wedged — force a clean connection next poll
    // 401 (expired token) is a real response, not a broken connection —
    // doesn't count against the backoff below.
    bool ok = (code == 200 || code == 204 || code == 401);

    if (code == 200) {
      JsonDocument filter;                 // title + first artist + cover URLs/sizes
      filter["is_playing"] = true;
      filter["item"]["name"] = true;
      filter["item"]["artists"][0]["name"] = true;
      filter["item"]["album"]["images"][0]["url"] = true;
      filter["item"]["album"]["images"][0]["width"] = true;
      JsonDocument doc;
      // getString() (not getStream()) so chunked responses are de-chunked first.
      if (!deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter))) {
        bool playing = doc["is_playing"] | false;
        const char* name = doc["item"]["name"] | "";
        const char* artist = doc["item"]["artists"][0]["name"] | "";
        if (strlen(name) > 0) {
          SpotifyNowPlaying np;
          np.songName = name;
          np.artistName = artist;
          np.playing = playing;
          np.nothingPlaying = false;
          xSemaphoreTake(spotifyDataMutex, portMAX_DELAY);
          spotifyPending = np;
          spotifyNowPlayingDirty = true;
          xSemaphoreGive(spotifyDataMutex);

        }
      }
    } else if (code == 204) {          // 204 = nothing on the active device
      SpotifyNowPlaying np;
      np.nothingPlaying = true;
      xSemaphoreTake(spotifyDataMutex, portMAX_DELAY);
      spotifyPending = np;
      spotifyNowPlayingDirty = true;
      xSemaphoreGive(spotifyDataMutex);
    } else if (code == 401) {          // token rejected — force a refresh next round
      spotifyAccessToken = "";
    } else {
      Serial.printf("Spotify now-playing failed: %d\n", code);
    }
    http.end();
    nowPlayingOk = ok;
  }
  return nowPlayingOk;
}

// Applies whatever spotifyPollTask() has fetched — LVGL calls, so this only
// ever runs on the UI thread (called from loop()). Cheap: no network here.
void spotifyApplyIfDirty() {
  if (!spotifyNowPlayingDirty) return;

  SpotifyNowPlaying np;
  xSemaphoreTake(spotifyDataMutex, portMAX_DELAY);
  np = spotifyPending;
  spotifyNowPlayingDirty = false;
  xSemaphoreGive(spotifyDataMutex);

  // Only touch LVGL when the *displayed* content actually changes. The poll task
  // re-sets the dirty flag every ~5s even when the same track is still playing;
  // deduping here avoids needlessly rewriting/redrawing the labels every poll.
  // (The UI-freeze fix is songLabel using LV_LABEL_LONG_DOT instead of a
  // permanently-animating scroll — see buildUi(); this dedup is just efficiency.)
  static String lastSong = "\x01";   // impossible sentinel so the first apply always runs
  static String lastArtist = "\x01";
  static int lastState = -1;         // -1 unset, 0 paused, 1 playing, 2 nothing-playing

  if (np.nothingPlaying) {
    if (lastState != 2) {
      lv_label_set_text(songLabel, "Nothing playing");
      lv_label_set_text(artistLabel, "");
      lastState = 2;
      lastSong = ""; lastArtist = "";
    }
    return;
  }

  if (np.songName != lastSong) {
    lv_label_set_text(songLabel, np.songName.c_str());
    lastSong = np.songName;
  }
  if (np.artistName != lastArtist) {
    lv_label_set_text(artistLabel, np.artistName.c_str());
    lastArtist = np.artistName;
  }
  int state = np.playing ? 1 : 0;
  if (state != lastState) {
    // Dim the title when paused so it's obvious at a glance.
    lv_obj_set_style_text_color(songLabel, lv_color_hex(np.playing ? 0xe8f2ec : 0x7a8a85), 0);
    lastState = state;
  }
}

// Background task (pinned to core 0, alongside the WiFi stack) that owns all
// Spotify networking, so a slow/failing TLS handshake never blocks the UI
// thread — see the comment above SpotifyNowPlaying.
//
// Backs off on repeated failures (3 fails -> 4x interval, 8+ -> 10x, resets
// the moment a poll succeeds) — see the comment on spotifyPollOnce(). A crash
// trail showed "SSL - Memory allocation failed" starting within ~7 seconds
// of boot and recurring on essentially every single 5s poll from then on, with
// nothing here ever slowing it down; that turned out to be the bigger
// contributor to the resource exhaustion behind a stack-overflow crash.
static void spotifyPollTask(void* pvParameters) {
  uint32_t lastPoll = 0;
  uint32_t failCount = 0;

  esp_task_wdt_add(NULL);   // see TASK_WDT_TIMEOUT_S

  for (;;) {
    esp_task_wdt_reset();
    if (deviceSleeping) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }   // screen asleep — no outbound network

    uint32_t now = millis();
    uint32_t interval = SPOTIFY_POLL_MS * (failCount >= 8 ? 10 : (failCount >= 3 ? 4 : 1));
    if (now - lastPoll >= interval) {
      lastPoll = now;
      bool ok = spotifyPollOnce();
      if (ok) {
        if (failCount > 0) Serial.printf("[spotify %8lu] poll OK, failCount %u -> 0\n", (unsigned long)millis(), (unsigned)failCount);
        failCount = 0;
      } else {
        failCount++;
        Serial.printf("[spotify %8lu] poll FAILED, failCount=%u, next interval=%lums\n",
          (unsigned long)millis(), (unsigned)failCount,
          (unsigned long)(SPOTIFY_POLL_MS * (failCount >= 8 ? 10 : (failCount >= 3 ? 4 : 1))));
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---------- WiFi setup portal ("Set WiFi" in the settings menu) ----------
// A minimal captive portal built on WebServer + DNSServer, both bundled with
// the ESP32 Arduino core — no third-party WiFi-manager library. That sidesteps
// the compatibility churn those libraries have had against newer core
// versions, and keeps this self-contained like the rest of the project's
// networking code. Credentials are stored in NVS via Preferences, under the
// "wifi" namespace ("ssid"/"pass" keys), and fall back to the WIFI_SSID /
// WIFI_PASSWORD macros above until the portal has been used once.
#define WIFI_SETUP_TIMEOUT_MS (3UL * 60UL * 1000UL)   // give up and return to normal operation
WebServer wifiSetupServer(80);
DNSServer wifiSetupDns;
static String wifiSetupPendingSsid, wifiSetupPendingPass;
static volatile bool wifiSetupSaved = false;

// Populated once in openWifiSetup(), before the AP goes up — see the comment there.
static String wifiSetupScannedOptionsHtml;

static void wifiSetupHandleRoot() {
  String html =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Clock WiFi Setup</title></head>"
    "<body style='font-family:sans-serif;max-width:360px;margin:24px auto;padding:0 16px;'>"
    "<h2>Set WiFi</h2><form method='POST' action='/save'>"
    "<label>Network</label><br><select name='ssid' style='width:100%;padding:8px;margin:8px 0;'>"
    "<option value=''>-- choose --</option>";
  html += wifiSetupScannedOptionsHtml;
  html +=
    "</select>"
    "<div style='font-size:12px;color:#666;margin:6px 0 0;'>Hidden network? Type it here instead:</div>"
    "<input type='text' name='ssid_manual' placeholder='Network name' style='width:100%;padding:8px;margin:4px 0 8px;box-sizing:border-box;'>"
    "<label>Password</label><br>"
    "<input type='password' name='pass' style='width:100%;padding:8px;margin:8px 0;box-sizing:border-box;'>"
    "<button type='submit' style='width:100%;padding:10px;margin-top:8px;'>Save &amp; Connect</button>"
    "</form></body></html>";
  wifiSetupServer.send(200, "text/html", html);
}

static void wifiSetupHandleSave() {
  String manual = wifiSetupServer.arg("ssid_manual");
  wifiSetupPendingSsid = manual.length() > 0 ? manual : wifiSetupServer.arg("ssid");
  wifiSetupPendingPass = wifiSetupServer.arg("pass");
  wifiSetupServer.send(200, "text/html",
    "<html><body style='font-family:sans-serif;'>Saved. Restarting the clock&hellip;</body></html>");
  wifiSetupSaved = true;   // picked up by the polling loop in openWifiSetup()
}

// Captive-portal probe paths (e.g. iOS/Android connectivity checks) all get
// redirected to "/" so the OS pops the setup page automatically.
static void wifiSetupHandleNotFound() {
  wifiSetupServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  wifiSetupServer.send(302, "text/plain", "");
}

void openWifiSetup() {
  // Blocks this task for its whole duration (up to the 3-min portal loop,
  // which never returns to loop()) — unsubscribe from the watchdog while it
  // runs, re-arm on the way out.
  esp_task_wdt_delete(NULL);

  lv_obj_add_flag(settingsCloseBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifiStatusLabel, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(wifiStatusLabel, "Scanning for networks...");
  lv_timer_handler();

  stopSettingsServer();   // free port 80 — the AP-mode portal below needs it

  // Scan *before* the AP goes up. WiFi.scanNetworks() while in AP-only mode is
  // a known ESP32 hang/crash — the radio can't scan and beacon at the same
  // time — so we do it now while still in normal STA mode and cache the
  // result; the portal page below is served from that cache instead of
  // re-scanning per request.
  //
  // If we got here because connectWifi() just failed, the driver can be left
  // mid-"connecting" (retrying in the background), and scanning in that state
  // reliably returns -2 (WIFI_SCAN_FAILED) rather than an empty list. Force
  // the STA idle first so the scan actually runs.
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  if (n < 0) {   // -1 = still running, -2 = failed — give it one more try
    delay(300);
    n = WiFi.scanNetworks();
  }
  wifiSetupScannedOptionsHtml = "";
  for (int i = 0; i < n; i++) {
    wifiSetupScannedOptionsHtml += "<option value=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + "</option>";
  }
  WiFi.scanDelete();
  Serial.printf("WiFi setup: found %d networks\n", n);

  lv_label_set_text(wifiStatusLabel, "Scan to join, or connect to WiFi\n\"ClockSetup\" manually, then open\nhttp://192.168.4.1");
  static const char wifiQrPayload[] = "WIFI:T:nopass;S:ClockSetup;;";   // standard WiFi QR format — joins the open AP, one scan
  lv_qrcode_update(wifiQr, wifiQrPayload, sizeof(wifiQrPayload) - 1);
  lv_obj_clear_flag(wifiQr, LV_OBJ_FLAG_HIDDEN);
  lv_timer_handler();   // flush that message to the panel before the blocking setup loop below

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ClockSetup");
  delay(200);   // let the AP interface come up before DNS/HTTP bind to its IP
  Serial.printf("WiFi setup: AP up at %s\n", WiFi.softAPIP().toString().c_str());

  wifiSetupDns.start(53, "*", WiFi.softAPIP());
  wifiSetupServer.on("/", wifiSetupHandleRoot);
  wifiSetupServer.on("/save", HTTP_POST, wifiSetupHandleSave);
  wifiSetupServer.onNotFound(wifiSetupHandleNotFound);
  wifiSetupServer.begin();

  wifiSetupSaved = false;
  uint32_t start = millis();
  while (!wifiSetupSaved && millis() - start < WIFI_SETUP_TIMEOUT_MS) {
    wifiSetupDns.processNextRequest();
    wifiSetupServer.handleClient();
    lv_timer_handler();
    delay(2);
  }

  wifiSetupServer.stop();
  wifiSetupDns.stop();

  if (wifiSetupSaved) {
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.putString("ssid", wifiSetupPendingSsid);
    prefs.putString("pass", wifiSetupPendingPass);
    prefs.end();
    // Restarting is simpler and more robust than tearing down AP mode and
    // reconnecting STA in place — connectWifi() picks the new credentials
    // up from Preferences on the next boot.
    lv_label_set_text(wifiStatusLabel, "Saved! Restarting...");
    lv_timer_handler();
    delay(1000);
    ESP.restart();
  }

  // Timed out — leave AP mode and resume normal operation.
  lv_label_set_text(wifiStatusLabel, "Setup timed out.");
  lv_timer_handler();
  delay(1200);
  WiFi.softAPdisconnect(true);
  connectWifi();
  startSettingsServer();   // resume now that port 80 is free again
  lv_label_set_text(wifiStatusLabel, "");
  lv_obj_add_flag(wifiStatusLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifiQr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(settingsCloseBtn, LV_OBJ_FLAG_HIDDEN);
  closeSettingsMenu();

  esp_task_wdt_add(NULL);   // re-arm (no-op if the TWDT isn't up yet during first setup)
}

void connectWifi() {
  WiFi.mode(WIFI_STA);

  Preferences prefs;
  prefs.begin("wifi", true);   // read-only
  String ssid = prefs.getString("ssid", WIFI_SSID);
  String pass = prefs.getString("pass", WIFI_PASSWORD);
  prefs.end();

  if (ssid.length() == 0) {
    Serial.println("No WiFi configured yet — use the on-screen Set WiFi menu.");
    return;
  }

  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.print("Connecting to WiFi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed, will retry in loop.");
  }
}

void loadRuntimeConfig() {
  Preferences prefs;
  prefs.begin("cfg", true);   // read-only
  String spotifyId     = prefs.getString("spotify_id",     SPOTIFY_CLIENT_ID);
  String spotifySecret = prefs.getString("spotify_secret", SPOTIFY_CLIENT_SECRET);
  String spotifyToken  = prefs.getString("spotify_token",  SPOTIFY_REFRESH_TOKEN);
  cfgLat = prefs.getString("lat", LAT);
  cfgLon = prefs.getString("lon", LON);
  prefs.end();

  // The cfgSpotify* strings are also read from spotifyPollTask() (core 0) —
  // guard the reassignment so it can't ever observe a half-written String
  // mid-reallocation.
  xSemaphoreTake(spotifyDataMutex, portMAX_DELAY);
  cfgSpotifyClientId     = spotifyId;
  cfgSpotifyClientSecret = spotifySecret;
  cfgSpotifyRefreshToken = spotifyToken;
  xSemaphoreGive(spotifyDataMutex);
}

// ---------- Settings page (Spotify + location) ----------
// Always-on, reachable at the clock's normal LAN IP (shown in the settings
// menu alongside the PIN) whenever it's connected to WiFi — unlike the setup
// portal above, this doesn't need AP mode since the device is already on the
// home network. Gated by HTTP Basic Auth using the random PIN generated at
// boot, so only someone who can read the clock's screen can get in. Saves go
// straight to NVS and take effect immediately, no reboot needed.
WebServer settingsServer(80);
bool settingsServerRunning = false;

// Minimal escaping for values embedded in an HTML attribute — cheap
// insurance against a token/secret containing a quote and breaking the form.
static String htmlAttrEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '"':  out += "&quot;"; break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      default:   out += c;
    }
  }
  return out;
}

// PIN gate: the settings form itself carries the PIN as a hidden field, so
// a correct submission re-proves it each time — no session/cookie needed.
static bool settingsPinOk() {
  return settingsServer.arg("pin") == settingsPin;
}

static void settingsSendPinForm() {
  settingsServer.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Clock Settings</title></head>"
    "<body style='font-family:sans-serif;max-width:320px;margin:24px auto;padding:0 16px;'>"
    "<h2>Clock Settings</h2><form method='GET' action='/'>"
    "<label>Enter the PIN shown on the clock</label><br>"
    "<input type='text' inputmode='numeric' pattern='[0-9]*' maxlength='4' name='pin' autofocus "
    "style='width:100%;padding:8px;margin:8px 0;font-size:20px;letter-spacing:6px;text-align:center;box-sizing:border-box;'>"
    "<button type='submit' style='width:100%;padding:10px;margin-top:8px;'>Unlock</button>"
    "</form></body></html>");
}

static void settingsHandleRoot() {
  if (!settingsPinOk()) { settingsSendPinForm(); return; }
  String html =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Clock Settings</title></head>"
    "<body style='font-family:sans-serif;max-width:400px;margin:24px auto;padding:0 16px;'>"
    "<h2>Spotify + Location</h2><form method='POST' action='/save'>"
    "<input type='hidden' name='pin' value='" + String(settingsPin) + "'>"
    "<label>Spotify Client ID</label><br>"
    "<input type='text' name='spotify_id' value='" + htmlAttrEscape(cfgSpotifyClientId) +
      "' style='width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box;'><br>"
    "<label>Spotify Client Secret</label><br>"
    "<input type='text' name='spotify_secret' value='" + htmlAttrEscape(cfgSpotifyClientSecret) +
      "' style='width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box;'><br>"
    "<label>Spotify Refresh Token</label><br>"
    "<input type='text' name='spotify_token' value='" + htmlAttrEscape(cfgSpotifyRefreshToken) +
      "' style='width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box;'><br>"
    "<label>Latitude</label><br>"
    "<input type='text' name='lat' value='" + htmlAttrEscape(cfgLat) +
      "' style='width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box;'><br>"
    "<label>Longitude</label><br>"
    "<input type='text' name='lon' value='" + htmlAttrEscape(cfgLon) +
      "' style='width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box;'><br>"
    "<button type='submit' style='width:100%;padding:10px;margin-top:8px;'>Save</button>"
    "</form></body></html>";
  settingsServer.send(200, "text/html", html);
}

static void settingsHandleSave() {
  if (!settingsPinOk()) { settingsSendPinForm(); return; }

  Preferences prefs;
  prefs.begin("cfg", false);
  prefs.putString("spotify_id",     settingsServer.arg("spotify_id"));
  prefs.putString("spotify_secret", settingsServer.arg("spotify_secret"));
  prefs.putString("spotify_token",  settingsServer.arg("spotify_token"));
  prefs.putString("lat",            settingsServer.arg("lat"));
  prefs.putString("lon",            settingsServer.arg("lon"));
  prefs.end();

  loadRuntimeConfig();   // apply immediately — next weather/Spotify poll picks it up, no reboot needed
  settingsServer.send(200, "text/html",
    "<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:400px;margin:24px auto;padding:0 16px;'>"
    "Saved.<br><a href='/?pin=" + String(settingsPin) + "'>Back</a></body></html>");
}

void startSettingsServer() {
  if (settingsServerRunning) return;
  settingsServer.on("/", settingsHandleRoot);
  settingsServer.on("/save", HTTP_POST, settingsHandleSave);
  settingsServer.begin();
  settingsServerRunning = true;
}

void stopSettingsServer() {
  if (!settingsServerRunning) return;
  settingsServer.stop();
  settingsServerRunning = false;
}

void syncTime() {
  // Uses NTP + POSIX TZ string so DST is handled automatically.
  configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");
}

void updateClock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    drawClockText("--:--:--");
    return;
  }
  char timeBuf[16];
  char dateBuf[40];
  // Zero-padded (always "HH:MM:SS", 8 chars) so the left-anchored canvas
  // draw never shifts width between redraws — no more leading-zero strip.
  strftime(timeBuf, sizeof(timeBuf), "%I:%M:%S", &timeinfo);
  strftime(dateBuf, sizeof(dateBuf), "%A, %B %d", &timeinfo);     // e.g. "Wednesday, July 15"
  drawClockText(timeBuf);
  lv_label_set_text(clockDateLabel, dateBuf);
}

void applyWeather(JsonDocument& doc) {
  JsonObject current = doc["current"];
  double temp    = current["temperature_2m"]         | 0.0;
  double feels   = current["apparent_temperature"]   | 0.0;
  double humid   = current["relative_humidity_2m"]   | 0.0;
  double wind    = current["wind_speed_10m"]          | 0.0;
  int code       = current["weather_code"]            | 0;

  double hi = doc["daily"]["temperature_2m_max"][0]   | 0.0;
  double lo = doc["daily"]["temperature_2m_min"][0]   | 0.0;

  lv_label_set_text_fmt(tempLabel, "%.0f\xC2\xB0", temp);
  lv_label_set_text(conditionLabel, weatherCodeToText(code));
  lv_label_set_text_fmt(hiLoLabel, "H: %.0f\xC2\xB0 / L: %.0f\xC2\xB0", hi, lo);

  // Sunrise/sunset from the same response. Open-Meteo returns them as local
  // ISO strings ("2026-07-30T05:42") because timezone=auto; the clock face is
  // 12-hour, so convert here rather than show "05:42" next to a 12h clock.
  // Index 0 = today (forecast_days=2 starts today, so this can't run off the
  // end mid-evening the way a "next sunrise" would).
  auto fmtSun = [](const char* iso, char* hm, size_t hn, char* ap, size_t an) {
    // strlen < 16 guards a missing/null field: dash the time, blank the suffix.
    if (!iso || strlen(iso) < 16) { snprintf(hm, hn, "--"); ap[0] = '\0'; return; }
    int h = (iso[11] - '0') * 10 + (iso[12] - '0');
    int m12 = h % 12; if (m12 == 0) m12 = 12;
    snprintf(hm, hn, "%d:%c%c", m12, iso[14], iso[15]);
    snprintf(ap, an, "%s", h >= 12 ? "PM" : "AM");
  };
  char hmBuf[8], apBuf[4];
  const char* riseIso = doc["daily"]["sunrise"][0] | "";
  const char* setIso  = doc["daily"]["sunset"][0]  | "";
  if (sunriseLabel && strlen(riseIso) >= 16) {
    fmtSun(riseIso, hmBuf, sizeof(hmBuf), apBuf, sizeof(apBuf));
    lv_label_set_text(sunriseLabel, hmBuf);
    lv_label_set_text(sunriseAmPmLabel, apBuf);
  }
  if (sunsetLabel && strlen(setIso) >= 16) {
    fmtSun(setIso, hmBuf, sizeof(hmBuf), apBuf, sizeof(apBuf));
    lv_label_set_text(sunsetLabel, hmBuf);
    lv_label_set_text(sunsetAmPmLabel, apBuf);
  }
  lv_label_set_text_fmt(feelsLikeLabel, "%.0f\xC2\xB0", feels);
  lv_label_set_text_fmt(humidityLabel, "%.0f%%", humid);
  lv_label_set_text_fmt(windLabel, "%.0f mph", wind);

  // Outdoor-temp mini graph. Dot 0 is the *live* current temperature (the same
  // `temp` shown big above, so the first dot always matches the reading);
  // dots 1-3 are the hourly forecast for the next 3 hours.
  //
  // We find "now" in the hourly arrays by matching the "YYYY-MM-DDTHH" prefix
  // of current.time against hourly.time, rather than parsing an hour and
  // assuming hourly[0] == today-midnight. That assumption is what sent the
  // trend the wrong way (a bad/empty index fell back to midnight, plotting the
  // overnight curve). Matching is exact, timezone-safe, and needs no NTP clock.
  JsonArray hrTemps = doc["hourly"]["temperature_2m"];
  JsonArray hrTimes = doc["hourly"]["time"];
  const char* nowStr = doc["current"]["time"] | "";
  if (!hrTemps.isNull() && strlen(nowStr) >= 13) {
    int nowIdx = -1;
    if (!hrTimes.isNull()) {
      for (int i = 0; i < (int)hrTimes.size(); i++) {
        const char* t = hrTimes[i] | "";
        if (strlen(t) >= 13 && strncmp(t, nowStr, 13) == 0) { nowIdx = i; break; }
      }
    }
    if (nowIdx < 0 && nowStr[10] == 'T') {            // fallback: HH as day-0 index
      nowIdx = (nowStr[11] - '0') * 10 + (nowStr[12] - '0');
    }
    if (nowIdx >= 0) {
      hourlyTempF[0] = (float)temp;                   // dot 0 = live current temp
      for (int i = 1; i < 4; i++) {
        hourlyTempF[i] = hrTemps[nowIdx + i] | (float)NAN;
      }
      updateTempChart();
    }
  }
}

bool pollWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (cfgLat.length() == 0 || cfgLon.length() == 0) return false;   // location not configured yet

  NetworkLock lock;   // see the comment above networkMutex's declaration
  if (!lock.ok) { Serial.println("net busy, request skipped"); return false; }
  // Plain HTTP (not HTTPS): Open-Meteo is a public, keyless API — there's no
  // secret to protect, so TLS bought us nothing here except a ~40KB heap
  // requirement for the mbedTLS handshake that this device can't meet
  // (~34KB free at poll time). Dropping to HTTP removes WiFiClientSecure
  // entirely and the GET fits comfortably. Built at runtime now since
  // lat/lon come from Preferences, not a compile-time macro.
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + cfgLat + "&longitude=" + cfgLon +
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
    "&hourly=temperature_2m"
    "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset&forecast_days=2&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto";

  // weatherClient is passed INTO HTTPClient so the by-IP connection opened
  // below is the one the GET reuses — http.begin(url) without a client would
  // spin up HTTPClient's own internal client and re-run getaddrinfo itself.
  WiFiClient weatherClient;
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(weatherClient, url);
  // setReuse(false) forces "Connection: close" — see the note in
  // spotifyFetchAccessToken(): a kept-alive socket from a one-shot client
  // would linger and slowly exhaust this device's small socket table.
  http.setReuse(false);

  // Bounded DNS + connect-by-IP. This is the UI-thread instance of the same
  // landmine the watchdog caught on spotifyPoll: a wedged getaddrinfo here
  // freezes lv_timer_handler() — i.e. the display — directly.
  {
    IPAddress meteoIp;
    if (!resolveWithTimeout("api.open-meteo.com", meteoIp, 15000)) { http.end(); return false; }
    if (!weatherClient.connect(meteoIp, 80)) { http.end(); return false; }
  }
  int code = http.GET();

  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      applyWeather(doc);
      lastGoodWeatherMs = millis();
      ok = true;
    } else {
      Serial.print("Weather JSON parse error: ");
      Serial.println(err.c_str());
    }
  } else {
    Serial.print("Weather GET failed, code: ");
    Serial.println(code);
  }
  http.end();
  return ok;
}

// ---------- Setup / Loop ----------
uint32_t lastWeatherPoll = 0;
bool weatherPollFailed = false;
#define WEATHER_RETRY_MS (30UL * 1000UL)  // on failure, retry sooner instead of waiting the full 10 min
uint32_t lastClockUpdate = 0;
uint32_t lastNtpResync = 0;
uint32_t lastWifiCheck = 0;

// Overrides the Arduino core's weak default (arduino-esp32's main.cpp calls
// this to size "loopTask" — the task that runs setup()/loop(), and so every
// bit of LVGL rendering via lv_timer_handler(), since we never created a
// separate task for that). Default is a modest 8192 bytes. Sized generous
// here (16KB) after chasing a stack-overflow crash: loopTask does the usual
// clock/weather/Spotify upkeep plus the settings overlays, and the stack had
// never been sized off real data — leaving it at the default would have left
// the one unverified stack in place.
size_t getArduinoLoopTaskStackSize(void) {
  return 16384;
}

void setup() {
  Serial.begin(115200);

  // First allocation of the boot, deliberately: the album-art tile + JPEG buffer
  // (one ~12KB block, never freed) should come out of the big contiguous block
  // while it is still big, for the same reason the poll-task stacks are placed
  // explicitly rather than left to drift. See allocAlbumBuffers().

  // Created first — loadRuntimeConfig() below (and spotifyPollTask() once it
  // starts) guard the cfg* strings and fetched-data snapshots with these.
  spotifyDataMutex = xSemaphoreCreateMutex();
  networkMutex = xSemaphoreCreateMutex();

  generateSettingsPin();
  loadRuntimeConfig();

  // Release the *entire* BT controller memory region (BTDM = Classic + BLE,
  // ~60KB+). With the Govee BLE scan gone, nothing on this device uses
  // Bluetooth at all, so we reclaim all of it for the heap — which is what
  // gives the Spotify TLS handshakes room to work. Must run before any
  // WiFi/BT init touches that region.
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

  display.init();
  display.setRotation(1);
  display.setBrightness(200);

  // First boot (or after an NVS wipe) — no saved calibration yet, so walk
  // through it now, before there's even an LVGL UI to interfere with it.
  // Redoing it later is available from the Settings menu (recalibrateTouchAndRedraw()).
  if (!loadTouchCalibration()) {
    runTouchCalibration();
  }

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, 480 * 8);   // must match buf1's size above

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 480;
  disp_drv.ver_res = 320;
  disp_drv.flush_cb = lvglFlushCb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lvglTouchReadCb;
  lv_indev_drv_register(&indev_drv);

  buildUi();

  // LVGL has its own fixed-size pool (LV_MEM_SIZE in lv_conf_c.h), separate
  // from the ESP32 heap that free-heap logging reports — so "free heap=98120"
  // says nothing about whether LVGL is nearly full. Worth logging because the
  // failure mode is silent and looks like a hang: with LV_USE_LOG 0, a failed
  // allocation trips LV_ASSERT_MALLOC, whose handler is a bare while(1). If
  // that happens during rendering rather than during buildUi(), setup finishes
  // normally and the background poll tasks keep logging from core 0 while the
  // screen never gets its first flush — i.e. a blank display with healthy-
  // looking serial output.
  //
  // LVGL now allocates from the system heap (LV_MEM_CUSTOM), so the number to
  // watch is the largest free block rather than a pool percentage: LVGL asks
  // for many small blocks, and fragmentation bites before exhaustion does.
#if LV_MEM_CUSTOM
  Serial.printf("LVGL on system heap: free=%u, largest block=%u after buildUi\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#else
  {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("LVGL mem after buildUi: %u%% used (%u of %u bytes), largest free block=%u, frag=%u%%\n",
                  (unsigned)mon.used_pct, (unsigned)(mon.total_size - mon.free_size),
                  (unsigned)mon.total_size, (unsigned)mon.free_biggest_size, (unsigned)mon.frag_pct);
  }
#endif

  connectWifi();
  if (WiFi.status() != WL_CONNECTED) {
    // Never configured, or the saved credentials no longer work — jump
    // straight into the setup portal (QR + instructions) instead of sitting
    // on a clock that can never get weather/time/Spotify. openWifiSetup()
    // itself returns to normal operation on save (it reboots) or on its
    // internal timeout.
    openSettingsMenu();
    openWifiSetup();
  }
  startSettingsServer();
  syncTime();
  weatherPollFailed = !pollWeather();

  // Both poll-task stacks are placed in .bss (static allocation) rather than
  // pulled from the heap. Measured: heap-allocated stacks land inside the big
  // contiguous DRAM block and carved it from ~73KB down to ~47KB, leaving the
  // Spotify TLS handshake (needs one ~45KB contiguous block) almost no margin —
  // it then only connected when fragmentation churn happened to leave the block
  // above ~40KB. .bss doesn't border that block (proven separately: shrinking a
  // .bss buffer didn't grow the block), so moving these 20KB of stacks out of
  // the heap keeps the block near 73KB. StackType_t is a byte on ESP-IDF, so
  // sizeof(stack) is the depth in bytes — matches the old numeric args.

  // Spotify networking on its own core-0 task so a dead/unreachable endpoint
  // can never block the UI thread — it does a TLS handshake every poll,
  // which is exactly the kind of call that can stall for a long time under
  // heap pressure. See the comment above SpotifyNowPlaying.
  static StackType_t  spotifyPollStack[12288];
  static StaticTask_t spotifyPollTcb;
  xTaskCreateStaticPinnedToCore(spotifyPollTask, "spotifyPoll", sizeof(spotifyPollStack), NULL, 1, spotifyPollStack, &spotifyPollTcb, 0);

  // Arm the watchdog and subscribe loopTask (poll tasks subscribe themselves).
  // Reconfigures the core's default TWDT in place; see TASK_WDT_TIMEOUT_S.
  esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  Serial.printf("Setup complete, free heap=%u\n", ESP.getFreeHeap());
}

#define SETTINGS_GEAR_TIMEOUT_MS (15UL * 1000UL)   // hide the gear icon this long after the last touch

void loop() {
  esp_task_wdt_reset();

  // Sleeping: panel off, LVGL idle, outbound network parked (the poll tasks
  // check deviceSleeping themselves). Still serve the web settings page and
  // watch the touch chip; poll fast enough that a tap feels instantly alive.
  if (deviceSleeping) {
    settingsServer.handleClient();
    uint16_t tx, ty;
    if (display.getTouch(&tx, &ty)) wakeScreen();
    vTaskDelay(pdMS_TO_TICKS(50));
    return;
  }

  lv_timer_handler();
  settingsServer.handleClient();

  uint32_t now = millis();

  // Settings gear icon: hidden until the screen is touched anywhere, then
  // hidden again after SETTINGS_GEAR_TIMEOUT_MS of no further touches.
  if (settingsGearBtn) {
    bool shouldShow = screenEverTouched && (now - lastTouchMs) < SETTINGS_GEAR_TIMEOUT_MS;
    bool isHidden = lv_obj_has_flag(settingsGearBtn, LV_OBJ_FLAG_HIDDEN);
    if (shouldShow && isHidden) {
      lv_obj_clear_flag(settingsGearBtn, LV_OBJ_FLAG_HIDDEN);
    } else if (!shouldShow && !isHidden) {
      lv_obj_add_flag(settingsGearBtn, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (now - lastWifiCheck >= 15000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) connectWifi();
  }

  if (now - lastClockUpdate >= 1000) {
    lastClockUpdate = now;
    updateClock();
  }

  // weatherForceRefresh (set in wakeScreen()) forces an immediate catch-up
  // poll right after waking from Sleep instead of waiting out the interval.
  {
    bool forcedWeather = weatherForceRefresh;
    if (forcedWeather) weatherForceRefresh = false;
    if (forcedWeather || now - lastWeatherPoll >= (weatherPollFailed ? WEATHER_RETRY_MS : WEATHER_POLL_MS)) {
      lastWeatherPoll = now;
      weatherPollFailed = !pollWeather();
    }
  }

  // Spotify: spotifyPollTask() (background task, core 0) does the actual
  // network fetches — this just picks up whatever it's finished and pushes
  // it to the widgets, which touches no network and is effectively instant.
  spotifyApplyIfDirty();

  if (now - lastNtpResync >= NTP_RESYNC_MS) {
    lastNtpResync = now;
    syncTime();
  }

  // (Removed: scheduled daily reboot.) It existed as a sledgehammer for heap
  // fragmentation caused by a fresh TLS handshake every Spotify poll. That
  // churn is gone — the now-playing connection is a persistent keep-alive,
  // album art (a per-track 14KB carve) is gone, and DNS/lock acquisition are
  // bounded. Anything that still wedges gets caught by the 90s task watchdog
  // (TASK_WDT_TIMEOUT_S) with a clean panic+reboot, which is a better failure
  // than a blind 2am restart — and it lets Sleep mode actually stay asleep.

  delay(5);
}