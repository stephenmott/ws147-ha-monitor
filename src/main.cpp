#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "secrets.h"

// ── Display config ────────────────────────────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _bl;
public:
  LGFX() {
    { auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST; cfg.freq_write = 40000000;
      cfg.pin_sclk = 7; cfg.pin_mosi = 6; cfg.pin_miso = -1; cfg.pin_dc = 15;
      _bus.config(cfg); _panel.setBus(&_bus); }
    { auto cfg = _panel.config();
      cfg.pin_cs = 14; cfg.pin_rst = 21;
      cfg.memory_width = 172; cfg.memory_height = 320;
      cfg.panel_width  = 172; cfg.panel_height  = 320;
      cfg.offset_x = 34; cfg.invert = true;
      _panel.config(cfg); }
    { auto cfg = _bl.config();
      cfg.pin_bl = 22; cfg.invert = false;
      _bl.config(cfg); _panel.setLight(&_bl); }
    setPanel(&_panel);
  }
};
static LGFX lcd;

// ── Home Assistant ────────────────────────────────────────────────────────────
#define HA_BASE  "http://192.168.1.29:8123"
#define POLL_MS  5000UL

struct State {
  float grid = 0, solar = 0, temp = 0;
  bool  valid = false;
  int   dbg_grid = 0, dbg_solar = 0, dbg_temp = 0;
  char  dbg_grid_s[32] = "", dbg_solar_s[32] = "", dbg_temp_s[32] = "";
};
static State state;
static unsigned long lastPoll = 0;

bool haGet(const char* id, float& out, int& code, char* s, size_t slen) {
  if (WiFi.status() != WL_CONNECTED) { code=-1; strncpy(s,"no wifi",slen); return false; }
  HTTPClient http;
  http.begin(String(HA_BASE) + "/api/states/" + id);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  http.addHeader("Content-Type", "application/json");
  code = http.GET();
  if (code != 200) { snprintf(s,slen,"http %d",code); http.end(); return false; }
  String body = http.getString(); http.end();
  StaticJsonDocument<64> filter; filter["state"] = true;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
    { strncpy(s,"json err",slen); return false; }
  const char* v = doc["state"] | "";
  strncpy(s, *v ? v : "(empty)", slen);
  if (!*v || strcmp(v,"unavailable")==0 || strcmp(v,"unknown")==0) return false;
  out = atof(v); return true;
}

void pollHA() {
  bool ok = false;
  state.dbg_grid = state.dbg_solar = state.dbg_temp = 0;
  ok |= haGet("sensor.power_emonpi_power1",   state.grid,  state.dbg_grid,  state.dbg_grid_s,  sizeof(state.dbg_grid_s));
  ok |= haGet("sensor.power_emonpi_power2",   state.solar, state.dbg_solar, state.dbg_solar_s, sizeof(state.dbg_solar_s));
  ok |= haGet("sensor.temperature_emonpi_t2", state.temp,  state.dbg_temp,  state.dbg_temp_s,  sizeof(state.dbg_temp_s));
  if (ok) state.valid = true;
}

// ── Layout constants ──────────────────────────────────────────────────────────
// 3 sections × 96px + 2×2px gaps = 292px, centred in 320px → 14px top + 14px bottom
static const int16_t SEC_H   = 96;    // section height
static const int16_t TOP_Y   = 14;    // top margin (clears rounded corners)
static const int16_t GAP     = 2;     // gap between sections
static const int16_t BORDER  = 2;     // thin coloured border around each section

// ── LCARS colour palette ──────────────────────────────────────────────────────
static const uint16_t C_BG  = 0x0000;  // black
static const uint16_t C_SUN = 0xFD00;  // LCARS orange  — solar
static const uint16_t C_IMP = 0xCACC;  // LCARS salmon  — grid import
static const uint16_t C_EXP = 0x4FE0;  // green         — grid export
static const uint16_t C_TMP = 0x9C9F;  // LCARS lavender — temperature

// ── Icons (drawn with primitives, ~24px tall) ─────────────────────────────────

// ☀  sun: filled circle + 8 rays
void iconSun(int16_t cx, int16_t cy, uint16_t c) {
  lcd.fillCircle(cx, cy, 7, c);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4.0f;
    int16_t x1 = cx + cosf(a) * 10, y1 = cy + sinf(a) * 10;
    int16_t x2 = cx + cosf(a) * 14, y2 = cy + sinf(a) * 14;
    lcd.drawLine(x1, y1, x2, y2, c);
    lcd.drawLine(x1+1, y1, x2+1, y2, c);  // 2px thick
  }
}

// ⚡  lightning bolt: two filled triangles
void iconBolt(int16_t cx, int16_t cy, uint16_t c) {
  lcd.fillTriangle(cx+7, cy-11, cx-3, cy+3, cx+5, cy+3, c);
  lcd.fillTriangle(cx-5, cy-3,  cx+3, cy-3, cx-7, cy+11, c);
}

// 🌡  thermometer: rectangle stem + circular bulb
void iconThermo(int16_t cx, int16_t cy, uint16_t c) {
  lcd.fillCircle(cx, cy + 7, 7, c);          // bulb
  lcd.fillRect(cx - 3, cy - 11, 7, 16, c);   // stem
  // hollow tube centre for realism
  lcd.fillRect(cx - 1, cy - 10, 3, 12, C_BG);
  lcd.fillRect(cx - 1, cy + 2,  3,  6, c);   // mercury fill
}

// ── Section renderer ──────────────────────────────────────────────────────────
// Thin coloured border, icon on the left, Font7 number right-aligned.
// Coloured left accent strip (icon in black), black number area with thin border.
static const int16_t STRIP_W = 24;   // width of left coloured strip

void drawSection(int16_t y, uint16_t c,
                 void(*icon)(int16_t,int16_t,uint16_t), const char* value) {
  // Full section background in colour (acts as border on top/bottom/right)
  lcd.fillRect(0, y, 172, SEC_H, c);
  // Black fill for number area (1px colour border on right, 2px top/bottom)
  lcd.fillRect(STRIP_W, y + BORDER, 172 - STRIP_W - 1, SEC_H - 2*BORDER, C_BG);
  // Icon drawn in black over the coloured strip
  icon(STRIP_W / 2, y + SEC_H / 2, C_BG);
  // Number: Font7, tall but narrower so 4 digits fit, vertically centred
  lcd.setFont(&lgfx::fonts::Font7);
  lcd.setTextSize(1.1f, 1.35f);
  lcd.setTextColor(0xFFFF, C_BG);
  lcd.setTextDatum(lgfx::middle_left);
  lcd.drawString(value, STRIP_W + 1, y + SEC_H / 2);
  lcd.setTextSize(1);
}

// ── Full display update ───────────────────────────────────────────────────────
void updateDisplay() {
  if (!state.valid) {
    lcd.fillScreen(C_BG);
    lcd.setTextDatum(lgfx::top_left);
    lcd.setFont(&lgfx::fonts::FreeSans9pt7b);
    int16_t y = 10;
    auto line = [&](uint16_t col, const char* txt) {
      lcd.setTextColor(col, C_BG); lcd.drawString(txt, 8, y); y += 18; };
    line(C_TMP, "-- NO DATA --"); y += 4;
    line(WiFi.status()==WL_CONNECTED ? (uint16_t)TFT_GREEN : (uint16_t)TFT_RED,
         WiFi.status()==WL_CONNECTED ? "WiFi: OK" : "WiFi: FAILED");
    if (WiFi.status()==WL_CONNECTED) line(0x4208, WiFi.localIP().toString().c_str());
    y += 4;
    char buf[40];
    snprintf(buf,sizeof(buf),"Grid:  \"%s\"",  state.dbg_grid_s);  line(state.dbg_grid ==200?(uint16_t)TFT_GREEN:(uint16_t)TFT_RED,buf);
    snprintf(buf,sizeof(buf),"Solar: \"%s\"", state.dbg_solar_s);  line(state.dbg_solar==200?(uint16_t)TFT_GREEN:(uint16_t)TFT_RED,buf);
    snprintf(buf,sizeof(buf),"Temp:  \"%s\"",  state.dbg_temp_s);  line(state.dbg_temp ==200?(uint16_t)TFT_GREEN:(uint16_t)TFT_RED,buf);
    return;
  }


  static char prev_solar[16] = "", prev_grid[16] = "", prev_temp[16] = "";
  static bool prev_exporting = false;

  char val[16];
  int16_t y0 = TOP_Y;
  int16_t y1 = TOP_Y + SEC_H + GAP;
  int16_t y2 = TOP_Y + (SEC_H + GAP) * 2;

  // ① Solar
  float solar = state.solar < 10.0f ? 0.0f : state.solar;
  snprintf(val, sizeof(val), "%.0f", solar);
  if (strcmp(val, prev_solar) != 0) {
    drawSection(y0, C_SUN, iconSun, val);
    strcpy(prev_solar, val);
  }

  // ② Grid — colour flips between import (salmon) and export (green)
  bool exporting = state.grid < 0.0f;
  snprintf(val, sizeof(val), "%.0f", fabsf(state.grid));
  if (strcmp(val, prev_grid) != 0 || exporting != prev_exporting) {
    drawSection(y1, exporting ? C_EXP : C_IMP, iconBolt, val);
    strcpy(prev_grid, val);
    prev_exporting = exporting;
  }

  // ③ Temperature
  snprintf(val, sizeof(val), "%.1f", state.temp);
  if (strcmp(val, prev_temp) != 0) {
    drawSection(y2, C_TMP, iconThermo, val);
    strcpy(prev_temp, val);
  }
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
void connectWiFi() {
  lcd.fillScreen(C_BG);
  lcd.setFont(&lgfx::fonts::FreeSans9pt7b);
  lcd.setTextColor(C_TMP, C_BG);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.drawString("CONNECTING...", 86, 160);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(250);
}

// ── Entry points ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);
  lcd.init(); lcd.setRotation(0); lcd.setBrightness(60);
  lcd.fillScreen(C_BG);
  // Test: show 9999 in all sections for 3 seconds
  { int16_t y0=TOP_Y, y1=TOP_Y+SEC_H+GAP, y2=TOP_Y+(SEC_H+GAP)*2;
    lcd.fillScreen(C_BG);
    drawSection(y0, C_SUN, iconSun,    "9999");
    drawSection(y1, C_IMP, iconBolt,   "9999");
    drawSection(y2, C_TMP, iconThermo, "9999");
    delay(3000); }
  connectWiFi();
  pollHA();
  updateDisplay();
  lastPoll = millis();
}

void loop() {
  if (millis() - lastPoll >= POLL_MS) {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    pollHA();
    updateDisplay();
    lastPoll = millis();
  }
  delay(1000);
}
