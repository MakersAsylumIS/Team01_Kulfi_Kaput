/*
  CrowPanel 3.5" "bridge" firmware for the CrowPanel Web Lab (localhost web page).

  Upload this once. Then open the web page (start.bat) in Chrome or Edge, click
  Connect, and control everything from the browser over USB. No recompiling.

  Serial protocol: one text command per line, 460800 baud.
    PING                         -> PONG
    INFO                         -> INFO <width> <height> <rotation>
    ROT <0-3>                    rotation (0/2 portrait, 1/3 landscape)
    BL <0-255>                   backlight
    FILL <rrggbb>                fill the screen
    TESTPAT                      color bars + gradient + border
    TEXT <mode> <font> <fg> <bg> <margin> <text...>
                                 mode: wrap | words | letters, font: 0-11, colors rrggbb
    PBEGIN <bg> <margin> <top|center|spread> <fit 0|1>
    PART <font> <px> <fg> <left|center|right> <spaceAbove> <text...>   (\n = line break)
    PEND                         -> OK PAGE scale=<s> h=<px>   (draws the parts stacked)
    IMG <x> <y> <w> <h>          -> READY, then w*h*2 bytes of RGB565 (big-endian),
                                 board answers K after every 4096 bytes, then OK IMG
    SERVO <deg>                  move servo (IO22)
    SERVOCFG <minUs> <maxUs>     servo pulse range
    DIST                         -> D <cm>        (-1 = no echo)
    STREAM <ms>                  send D <cm> every <ms> (0 = stop)
    SD <deg> <settleMs>          move servo, wait, measure -> SD <deg> <cm>
    PAINT <0|1> <rrggbb>         draw on the screen where it's touched
    CAL                          run touch calibration on the screen -> CALDONE
  The board also sends  T <x> <y>  while the screen is touched, and  TU  on release.

  Wiring: servo signal IO22, ultrasonic TRIG IO25, ECHO IO32 (via divider for 5 V sensors)

  Board:     "ESP32 Dev Module"
  Library:   LovyanGFX
*/

#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "fonts_custom.h"     // Sora and Poppins (generated from the Google Fonts TTFs)

// Set to 0 for boards older than v2.2.
#define BOARD_V22 1

#define BAUD 460800

#define PIN_SCLK   14
#define PIN_MOSI   13
#define PIN_LCD_DC  2
#define PIN_LCD_CS 15
#define PIN_LCD_BL 27
#if BOARD_V22
  #define PIN_MISO 33
  #define PIN_TP_CS 12
#else
  #define PIN_MISO 12
  #define PIN_TP_CS 33
#endif

#define PIN_SERVO  3
#define SERVO_CH   0
#define PIN_TRIG  25
#define PIN_ECHO  32
#define ECHO_TIMEOUT_US (400 * 58 + 2000)

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 27000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = PIN_SCLK;
      cfg.pin_mosi    = PIN_MOSI;
      cfg.pin_miso    = PIN_MISO;
      cfg.pin_dc      = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs        = PIN_LCD_CS;
      cfg.pin_rst       = -1;
      cfg.pin_busy      = -1;
      cfg.panel_width   = 320;
      cfg.panel_height  = 480;
      cfg.readable      = true;
      cfg.invert        = false;
      cfg.rgb_order     = false;
      cfg.dlen_16bit    = false;
      cfg.bus_shared    = true;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = PIN_LCD_BL;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min      = 0;
      cfg.x_max      = 319;
      cfg.y_min      = 0;
      cfg.y_max      = 479;
      cfg.pin_int    = -1;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host   = SPI2_HOST;
      cfg.freq       = 1000000;
      cfg.pin_sclk   = PIN_SCLK;
      cfg.pin_mosi   = PIN_MOSI;
      cfg.pin_miso   = PIN_MISO;
      cfg.pin_cs     = PIN_TP_CS;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

static LGFX lcd;
static Preferences prefs;

static const lgfx::IFont* FONTS[] = {
  &fonts::FreeSansBold24pt7b,   // 0
  &fonts::FreeSans24pt7b,       // 1
  &fonts::FreeSerifBold24pt7b,  // 2
  &fonts::FreeSerif24pt7b,      // 3
  &fonts::FreeMonoBold24pt7b,   // 4
  &fonts::Orbitron_Light_32,    // 5
  &fonts::Satisfy_24,           // 6
  &fonts::Yellowtail_32,        // 7
  &Sora_64,                     // 8
  &SoraBold_64,                 // 9
  &Poppins_64,                  // 10
  &PoppinsBold_64,              // 11
};
static const int N_FONTS = sizeof(FONTS) / sizeof(FONTS[0]);

static int servoMinUs = 500, servoMaxUs = 2500;
static uint32_t streamMs = 0, lastStream = 0;
static bool paintOn = false;
static uint16_t paintColor = TFT_WHITE;

// ---------------------------------------------------------------------------
// Helpers

uint16_t hexColor(const String& s) {
  uint32_t v = strtoul(s.c_str(), nullptr, 16);
  return lcd.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// Splits off the first space-separated token of `rest`
String nextToken(String& rest) {
  rest.trim();
  int sp = rest.indexOf(' ');
  String tok = sp < 0 ? rest : rest.substring(0, sp);
  rest = sp < 0 ? "" : rest.substring(sp + 1);
  return tok;
}

// Reads the next token as a number and clamps it. Don't put nextToken() inside
// constrain(): it's a macro and would read several tokens.
long argInt(String& rest, long lo, long hi) { long v = nextToken(rest).toInt(); return v < lo ? lo : v > hi ? hi : v; }
float argFloat(String& rest, float lo, float hi) { float v = nextToken(rest).toFloat(); return v < lo ? lo : v > hi ? hi : v; }

void servoBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttachChannel(PIN_SERVO, 50, 16, SERVO_CH);
#else
  ledcSetup(SERVO_CH, 50, 16);
  ledcAttachPin(PIN_SERVO, SERVO_CH);
#endif
}

void servoWrite(int deg) {
  deg = constrain(deg, 0, 180);
  uint32_t us = servoMinUs + (uint32_t)(servoMaxUs - servoMinUs) * deg / 180;
  uint32_t duty = us * 65535UL / 20000UL;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_SERVO, duty);
#else
  ledcWrite(SERVO_CH, duty);
#endif
}

float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, ECHO_TIMEOUT_US);
  return us == 0 ? -1 : us / 58.0f;
}

float readDistanceMedian() {
  float v[3];
  int n = 0;
  for (int i = 0; i < 3; i++) {
    float d = readDistanceCm();
    if (d > 0) v[n++] = d;
    delay(10);
  }
  if (n == 0) return -1;
  if (n == 1) return v[0];
  if (n == 2) return (v[0] + v[1]) / 2;
  float a = v[0], b = v[1], c = v[2];
  return max(min(a, b), min(max(a, b), c));
}

// ---------------------------------------------------------------------------
// Text layouts (same logic as the CrowPanel35_Text sketches)

static const int MAX_WORDS = 48;
static String words[MAX_WORDS];
static String lines[MAX_WORDS];
static int nWords = 0, nLines = 0;

void splitWords(const String& text) {
  nWords = 0;
  int i = 0, len = text.length();
  while (i < len && nWords < MAX_WORDS) {
    while (i < len && text[i] == ' ') i++;
    int j = i;
    while (j < len && text[j] != ' ') j++;
    if (j > i) words[nWords++] = text.substring(i, j);
    i = j;
  }
}

bool wrapWords(int maxW) {
  nLines = 0;
  String line = "";
  for (int k = 0; k < nWords; k++) {
    if (lcd.textWidth(words[k]) > maxW) return false;
    String t = line.length() ? line + " " + words[k] : words[k];
    if (lcd.textWidth(t) <= maxW) line = t;
    else { lines[nLines++] = line; line = words[k]; }
  }
  if (line.length()) lines[nLines++] = line;
  return true;
}

int blockHeight() {
  int lh = lcd.fontHeight() * 1.05f;
  return lh * nLines - lcd.fontHeight() * 0.05f;
}

void drawLines() {
  int lh = lcd.fontHeight() * 1.05f;
  int y = (lcd.height() - blockHeight()) / 2;
  lcd.setTextDatum(top_center);
  for (int i = 0; i < nLines; i++) lcd.drawString(lines[i], lcd.width() / 2, y + i * lh);
}

float drawText(const String& mode, int font, uint16_t fg, uint16_t bg, int margin, const String& text) {
  int maxW = lcd.width() - 2 * margin, maxH = lcd.height() - 2 * margin;
  lcd.fillScreen(bg);
  lcd.setFont(FONTS[constrain(font, 0, N_FONTS - 1)]);
  lcd.setTextColor(fg);
  splitWords(text);
  if (nWords == 0) return 0;

  float best = 0.3f;
  if (mode == "letters") {
    int maxLen = 0;
    for (int i = 0; i < nWords; i++) maxLen = max(maxLen, (int)words[i].length());
    for (float s = 10.0f; s >= 0.3f; s -= 0.05f) {
      lcd.setTextSize(s);
      int colW = lcd.textWidth("W"), gap = colW / 3, rowH = lcd.fontHeight() * 0.85f;
      if (nWords * colW + (nWords - 1) * gap <= maxW && maxLen * rowH <= maxH) { best = s; break; }
    }
    lcd.setTextSize(best);
    int colW = lcd.textWidth("W"), gap = colW / 3, rowH = lcd.fontHeight() * 0.85f;
    int x0 = (lcd.width() - (nWords * colW + (nWords - 1) * gap)) / 2 + colW / 2;
    lcd.setTextDatum(top_center);
    for (int c = 0; c < nWords; c++) {
      int len = words[c].length();
      int y0 = (lcd.height() - len * rowH) / 2;
      for (int r = 0; r < len; r++) {
        char ch[2] = { words[c][r], 0 };
        lcd.drawString(ch, x0 + c * (colW + gap), y0 + r * rowH);
      }
    }
    return best;
  }

  for (float s = 10.0f; s >= 0.3f; s -= 0.05f) {
    lcd.setTextSize(s);
    bool ok;
    if (mode == "words") {
      nLines = nWords;
      for (int i = 0; i < nWords; i++) lines[i] = words[i];
      ok = true;
      for (int i = 0; ok && i < nLines; i++) if (lcd.textWidth(lines[i]) > maxW) ok = false;
    } else {
      ok = wrapWords(maxW);
    }
    if (ok && blockHeight() <= maxH) { best = s; break; }
  }
  lcd.setTextSize(best);
  if (mode == "words") { nLines = nWords; for (int i = 0; i < nWords; i++) lines[i] = words[i]; }
  else wrapWords(maxW);
  drawLines();
  return best;
}

// ---------------------------------------------------------------------------
// Page layout: several parts (title, heading, body...) stacked top to bottom.
// Each part has its own font family, size in pixels, color and alignment.

// Font families in up to 5 sizes, smallest to largest. The closest size is
// picked so text stays sharp; it is then scaled to the exact pixel size.
// The index of each family is the font number used by the web page.
static const int FONT_SIZES = 5;
static const lgfx::IFont* FAMILIES[][FONT_SIZES] = {
  {&fonts::FreeSansBold9pt7b,  &fonts::FreeSansBold12pt7b,  &fonts::FreeSansBold18pt7b,  &fonts::FreeSansBold24pt7b,  &fonts::FreeSansBold24pt7b},   // 0
  {&fonts::FreeSans9pt7b,      &fonts::FreeSans12pt7b,      &fonts::FreeSans18pt7b,      &fonts::FreeSans24pt7b,      &fonts::FreeSans24pt7b},       // 1
  {&fonts::FreeSerifBold9pt7b, &fonts::FreeSerifBold12pt7b, &fonts::FreeSerifBold18pt7b, &fonts::FreeSerifBold24pt7b, &fonts::FreeSerifBold24pt7b},  // 2
  {&fonts::FreeSerif9pt7b,     &fonts::FreeSerif12pt7b,     &fonts::FreeSerif18pt7b,     &fonts::FreeSerif24pt7b,     &fonts::FreeSerif24pt7b},      // 3
  {&fonts::FreeMonoBold9pt7b,  &fonts::FreeMonoBold12pt7b,  &fonts::FreeMonoBold18pt7b,  &fonts::FreeMonoBold24pt7b,  &fonts::FreeMonoBold24pt7b},   // 4
  {&fonts::Orbitron_Light_24,  &fonts::Orbitron_Light_24,   &fonts::Orbitron_Light_32,   &fonts::Orbitron_Light_32,   &fonts::Orbitron_Light_32},    // 5
  {&fonts::Satisfy_24,         &fonts::Satisfy_24,          &fonts::Satisfy_24,          &fonts::Satisfy_24,          &fonts::Satisfy_24},           // 6
  {&fonts::Yellowtail_32,      &fonts::Yellowtail_32,       &fonts::Yellowtail_32,       &fonts::Yellowtail_32,       &fonts::Yellowtail_32},        // 7
  {&Sora_14,        &Sora_20,        &Sora_28,        &Sora_40,        &Sora_64},          // 8  Sora
  {&SoraBold_14,    &SoraBold_20,    &SoraBold_28,    &SoraBold_40,    &SoraBold_64},      // 9  Sora Bold
  {&Poppins_14,     &Poppins_20,     &Poppins_28,     &Poppins_40,     &Poppins_64},       // 10 Poppins
  {&PoppinsBold_14, &PoppinsBold_20, &PoppinsBold_28, &PoppinsBold_40, &PoppinsBold_64},   // 11 Poppins Bold
};
static const int N_FAMILIES = sizeof(FAMILIES) / sizeof(FAMILIES[0]);

void pickFont(int family, float px) {
  family = constrain(family, 0, N_FAMILIES - 1);
  for (int v = 0; v < FONT_SIZES; v++) {
    lcd.setFont(FAMILIES[family][v]);
    lcd.setTextSize(1);
    int h = lcd.fontHeight();
    if (h >= px * 0.95f || v == FONT_SIZES - 1) { lcd.setTextSize(max(0.2f, px / h)); return; }
  }
}

struct Part {
  int font;
  float px;
  uint16_t fg;
  char align;          // 'l', 'c', 'r'
  int spaceAbove;
  String text;
};
static const int MAX_PARTS = 10;
static Part parts[MAX_PARTS];
static int nParts = 0;
static uint16_t pageBg = TFT_BLACK;
static int pageMargin = 24;
static String pageValign = "top";
static bool pageFit = true;

static const int MAX_PLINES = 64;
static String plines[MAX_PLINES];
static int nPlines = 0;

// Wraps a part's text (current font) into plines[]; "\n" forces a new line
void wrapPart(const String& text, int maxW) {
  nPlines = 0;
  int start = 0;
  while (start <= (int)text.length() && nPlines < MAX_PLINES) {
    int nl = text.indexOf("\\n", start);
    String para = nl < 0 ? text.substring(start) : text.substring(start, nl);
    start = nl < 0 ? text.length() + 1 : nl + 2;

    splitWords(para);
    if (nWords == 0) { plines[nPlines++] = ""; continue; }   // empty line
    String line = "";
    for (int k = 0; k < nWords && nPlines < MAX_PLINES; k++) {
      String t = line.length() ? line + " " + words[k] : words[k];
      if (line.length() && lcd.textWidth(t) > maxW) { plines[nPlines++] = line; line = words[k]; }
      else line = t;
    }
    if (line.length() && nPlines < MAX_PLINES) plines[nPlines++] = line;
  }
}

// Measures (draw=false) or draws (draw=true) all parts at the given scale
int layoutPage(float scale, bool draw, int startY) {
  int maxW = lcd.width() - 2 * pageMargin;
  int y = startY;
  int total = 0;
  int gapSlots = max(1, nParts - 1);
  int extraGap = 0;
  if (draw && pageValign == "spread" && nParts > 1) {
    int used = layoutPage(scale, false, 0);
    extraGap = max(0, (int)(lcd.height() - 2 * pageMargin - used) / gapSlots);
  }
  for (int i = 0; i < nParts; i++) {
    Part& p = parts[i];
    if (i > 0) { int s = (int)(p.spaceAbove * scale) + extraGap; y += s; total += s; }
    pickFont(p.font, p.px * scale);
    wrapPart(p.text, maxW);
    int lineH = lcd.fontHeight() * 1.1f;
    if (draw) {
      lcd.setTextColor(p.fg);
      int x = p.align == 'c' ? lcd.width() / 2 : p.align == 'r' ? lcd.width() - pageMargin : pageMargin;
      lcd.setTextDatum(p.align == 'c' ? top_center : p.align == 'r' ? top_right : top_left);
      for (int k = 0; k < nPlines; k++) lcd.drawString(plines[k], x, y + k * lineH);
    }
    int h = nPlines * lineH;
    y += h; total += h;
  }
  return total;
}

void drawPage() {
  int maxH = lcd.height() - 2 * pageMargin;
  float scale = 1.0f;
  int h = layoutPage(scale, false, 0);
  if (pageFit) {
    for (int i = 0; i < 80 && h > maxH; i++) {   // shrink everything until it fits
      scale *= 0.96f;
      h = layoutPage(scale, false, 0);
    }
  }
  lcd.fillScreen(pageBg);
  int y = pageMargin;
  if (pageValign == "center") y = pageMargin + max(0, (maxH - h) / 2);
  layoutPage(scale, true, y);
  Serial.printf("OK PAGE scale=%.2f h=%d max=%d\n", scale, h, maxH);
}

// ---------------------------------------------------------------------------
// Image upload: RGB565 big-endian, pushed row by row

void receiveImage(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0 || w > 480 || h > 480) { Serial.println("ERR IMG size"); return; }
  static uint8_t row[480 * 2];
  const uint32_t total = (uint32_t)w * h * 2;
  const int rowBytes = w * 2;
  uint32_t got = 0, sinceAck = 0;
  int rowFill = 0, rowIdx = 0;
  uint32_t lastData = millis();

  Serial.println("READY");
  lcd.startWrite();
  while (got < total) {
    int avail = Serial.available();
    if (avail <= 0) {
      if (millis() - lastData > 3000) { lcd.endWrite(); Serial.println("ERR IMG timeout"); return; }
      delay(0);
      continue;
    }
    lastData = millis();
    int want = min((int)(rowBytes - rowFill), avail);
    want = min(want, (int)(total - got));
    int n = Serial.readBytes(row + rowFill, want);
    rowFill += n; got += n; sinceAck += n;
    if (rowFill == rowBytes) {
      lcd.pushImage(x, y + rowIdx, w, 1, (const lgfx::swap565_t*)row);
      rowIdx++;
      rowFill = 0;
    }
    if (sinceAck >= 4096 || got == total) { Serial.println("K"); sinceAck = 0; }
  }
  lcd.endWrite();
  Serial.println("OK IMG");
}

void testPattern() {
  int w = lcd.width(), h = lcd.height();
  const uint16_t bars[] = {TFT_WHITE, TFT_YELLOW, TFT_CYAN, TFT_GREEN, TFT_MAGENTA, TFT_RED, TFT_BLUE, TFT_BLACK};
  int bh = h * 2 / 3;
  for (int i = 0; i < 8; i++) lcd.fillRect(i * w / 8, 0, w / 8 + 1, bh, bars[i]);
  for (int x = 0; x < w; x++) {
    uint8_t v = x * 255 / (w - 1);
    lcd.drawFastVLine(x, bh, h - bh, lcd.color565(v, v, v));
  }
  lcd.drawRect(0, 0, w, h, TFT_RED);
}

// ---------------------------------------------------------------------------

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  String rest = line;
  String cmd = nextToken(rest);
  cmd.toUpperCase();

  if (cmd == "PING") Serial.println("PONG CrowPanel35Bridge");
  else if (cmd == "INFO") Serial.printf("INFO %d %d %d\n", lcd.width(), lcd.height(), lcd.getRotation());
  else if (cmd == "ROT") { lcd.setRotation(nextToken(rest).toInt() & 3); Serial.printf("INFO %d %d %d\n", lcd.width(), lcd.height(), lcd.getRotation()); }
  else if (cmd == "BL") { lcd.setBrightness(argInt(rest, 0, 255)); Serial.println("OK"); }
  else if (cmd == "FILL") { lcd.fillScreen(hexColor(nextToken(rest))); Serial.println("OK"); }
  else if (cmd == "TESTPAT") { testPattern(); Serial.println("OK"); }
  else if (cmd == "TEXT") {
    String mode = nextToken(rest);
    int font = nextToken(rest).toInt();
    uint16_t fg = hexColor(nextToken(rest));
    uint16_t bg = hexColor(nextToken(rest));
    int margin = nextToken(rest).toInt();
    float s = drawText(mode, font, fg, bg, margin, rest);
    Serial.printf("OK TEXT size=%.2f lines=%d\n", s, mode == "letters" ? 0 : nLines);
  }
  else if (cmd == "PBEGIN") {
    pageBg = hexColor(nextToken(rest));
    pageMargin = argInt(rest, 0, 150);
    pageValign = nextToken(rest);
    pageFit = nextToken(rest).toInt() != 0;
    nParts = 0;
    Serial.println("OK");
  }
  else if (cmd == "PART") {
    if (nParts >= MAX_PARTS) { Serial.println("ERR too many parts"); return; }
    Part& p = parts[nParts++];
    p.font = nextToken(rest).toInt();
    p.px = argFloat(rest, 6.0f, 300.0f);
    p.fg = hexColor(nextToken(rest));
    String a = nextToken(rest);
    p.align = a.length() ? a[0] : 'l';
    p.spaceAbove = argInt(rest, 0, 300);
    p.text = rest;
    Serial.println("OK");
  }
  else if (cmd == "PEND") drawPage();
  else if (cmd == "IMG") {
    int x = nextToken(rest).toInt(), y = nextToken(rest).toInt();
    int w = nextToken(rest).toInt(), h = nextToken(rest).toInt();
    receiveImage(x, y, w, h);
  }
  else if (cmd == "SERVO") { servoWrite(nextToken(rest).toInt()); Serial.println("OK"); }
  else if (cmd == "SERVOCFG") {
    servoMinUs = argInt(rest, 300, 3000);
    servoMaxUs = argInt(rest, 300, 3000);
    Serial.println("OK");
  }
  else if (cmd == "DIST") Serial.printf("D %.1f\n", readDistanceMedian());
  else if (cmd == "STREAM") { streamMs = max(0L, nextToken(rest).toInt()); if (streamMs) streamMs = max(streamMs, (uint32_t)60); Serial.println("OK"); }
  else if (cmd == "SD") {
    int deg = nextToken(rest).toInt();
    int settle = argInt(rest, 0, 1000);
    servoWrite(deg);
    delay(settle);
    Serial.printf("SD %d %.1f\n", deg, readDistanceCm());
  }
  else if (cmd == "PAINT") {
    paintOn = nextToken(rest).toInt() != 0;
    String c = nextToken(rest);
    if (c.length()) paintColor = hexColor(c);
    Serial.println("OK");
  }
  else if (cmd == "CAL") {
    uint16_t cal[8];
    lcd.fillScreen(TFT_BLACK);
    lcd.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 20);
    prefs.putBytes("cal", cal, sizeof(cal));
    lcd.setTouchCalibrate(cal);
    lcd.fillScreen(TFT_BLACK);
    Serial.println("CALDONE");
  }
  else Serial.printf("ERR unknown command: %s\n", cmd.c_str());
}

void pollTouch() {
  static bool wasDown = false;
  static uint32_t lastSent = 0;
  int32_t x, y;
  bool down = lcd.getTouch(&x, &y);
  if (down) {
    if (paintOn) lcd.fillCircle(x, y, 3, paintColor);
    if (!wasDown || millis() - lastSent > 30) {
      Serial.printf("T %ld %ld\n", (long)x, (long)y);
      lastSent = millis();
    }
  } else if (wasDown) {
    Serial.println("TU");
  }
  wasDown = down;
}

void setup() {
  Serial.setRxBufferSize(16384);
  Serial.begin(BAUD);

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);
  servoBegin();
  servoWrite(90);

  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(255);

  prefs.begin("crowtest", false);
  uint16_t cal[8];
  if (prefs.getBytes("cal", cal, sizeof(cal)) == sizeof(cal)) lcd.setTouchCalibrate(cal);

  lcd.fillScreen(TFT_BLACK);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawString("CrowPanel Web Lab", lcd.width() / 2, lcd.height() / 2 - 20);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(TFT_LIGHTGREY);
  lcd.drawString("Waiting for the browser...", lcd.width() / 2, lcd.height() / 2 + 25);

  Serial.println("HELLO CrowPanel35Bridge");
}

void loop() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { handleCommand(buf); buf = ""; }
    else if (c != '\r' && buf.length() < 1024) buf += c;
  }

  pollTouch();

  if (streamMs && millis() - lastStream >= streamMs) {
    lastStream = millis();
    Serial.printf("D %.1f\n", readDistanceCm());
  }
  delay(2);
}
