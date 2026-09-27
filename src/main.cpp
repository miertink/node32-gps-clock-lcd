/*
  node32-gps-clock
  GPS clock: NodeMCU (ESP8266 / ESP-12E) + CN-06 GPS (u-blox NEO-6M) + round GC9A01 1.28" 240x240 display

  Wiring
  ---------------------------------------------------------------
  CN-06 GPS        NodeMCU
    VCC     ->     VIN (5V)   (module has its own regulator; 3V3 also works)
    GND     ->     GND
    TX      ->     D6 (GPIO12)   SoftwareSerial RX
    RX      ->     not connected (only needed to configure the GPS)

  GC9A01           NodeMCU
    VCC     ->     3V3
    GND     ->     GND
    SCL/SCK ->     D5 (GPIO14)   HSPI CLK (fixed)
    SDA/MOSI->     D7 (GPIO13)   HSPI MOSI (fixed)
    CS      ->     D1 (GPIO5)
    DC      ->     D2 (GPIO4)
    RST     ->     D0 (GPIO16)
    BLK     ->     3V3 (or leave open if the module has a pull-up)
  ---------------------------------------------------------------
  Deliberately avoided: D3/GPIO0, D4/GPIO2 and D8/GPIO15 are boot strapping pins;
  D9/D10 are UART0 (USB).

  No full framebuffer: the ESP8266 lacks RAM for 240x240x2 = 115 KB.
  Each field is redrawn only when it changes, through a small (190x46) RAM canvas.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <U8g2lib.h>              // fonts only (Logisoso, Helvetica)
#include <Arduino_GFX_Library.h>
#include <SoftwareSerial.h>
#include <TinyGPSPlus.h>

// ---------------------------- Configuration -----------------------------
constexpr int UTC_OFFSET_MIN = -3 * 60;   // Brasília; no daylight saving time since 2019

constexpr int8_t   PIN_GPS_RX = D6;
constexpr uint32_t GPS_BAUD   = 115200;    // CN-06 reconfigured (ex-drone); NEO-6M factory default is 9600

constexpr int8_t PIN_TFT_CS  = D1;
constexpr int8_t PIN_TFT_DC  = D2;
constexpr int8_t PIN_TFT_RST = D0;

constexpr uint32_t GPS_STALE_MS = 3000;    // no new sentence for longer than this = stale data

constexpr uint32_t SERIAL_BAUD     = 74880;  // same as the ROM boot log: everything readable in the monitor
constexpr uint32_t STATUS_EVERY_MS = 1000;   // GPS status interval on the serial monitor
constexpr bool     GPS_ECHO_NMEA   = false;  // true = forward raw NMEA sentences to the monitor

// ------------------------------- Display --------------------------------
constexpr int16_t CX = 120, CY = 120;

Arduino_DataBus *bus = new Arduino_ESP8266SPI(PIN_TFT_DC, PIN_TFT_CS);
Arduino_GFX     *gfx = new Arduino_GC9A01(bus, PIN_TFT_RST, 0 /*rotation*/, true /*IPS*/);

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t C_BG   = rgb(0, 0, 0);
constexpr uint16_t C_TIME = rgb(255, 255, 255);
constexpr uint16_t C_SEC  = rgb(0, 200, 255);
constexpr uint16_t C_DATE = rgb(200, 200, 200);
constexpr uint16_t C_DIM  = rgb(50, 50, 50);
constexpr uint16_t C_OK   = rgb(0, 220, 0);
constexpr uint16_t C_WARN = rgb(255, 170, 0);
constexpr uint16_t C_ERR  = rgb(255, 40, 40);

// --------------------------------- GPS ----------------------------------
TinyGPSPlus    gps;
SoftwareSerial gpsSerial;

// ------------------------------ Date/time -------------------------------
struct LocalTime {
  int year; uint8_t month, day, hour, minute, second, weekday;  // weekday 0 = Sunday
};

// H. Hinnant's algorithms (days since 1970-01-01, proleptic Gregorian calendar)
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static void civilFromDays(int32_t z, int &y, uint8_t &m, uint8_t &d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp  = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)yoe + era * 400 + (m <= 2);
}

// Converts GPS UTC to local time using the offset (handles day/month/year rollover)
static LocalTime utcToLocal(int y, int mo, int d, int h, int mi, int s) {
  int64_t t = (int64_t)daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
  t += (int64_t)UTC_OFFSET_MIN * 60;

  int32_t days = (int32_t)(t / 86400);
  int32_t secs = (int32_t)(t % 86400);
  if (secs < 0) { secs += 86400; days--; }

  LocalTime lt;
  civilFromDays(days, lt.year, lt.month, lt.day);
  lt.hour    = secs / 3600;
  lt.minute  = (secs / 60) % 60;
  lt.second  = secs % 60;
  lt.weekday = (uint8_t)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday (4)
  return lt;
}

// ------------------------ Incremental field drawing ---------------------
// Each field remembers what is on screen and is redrawn only when its text changes.
// Text is rendered into a small RAM canvas and pushed to the display in one go:
// proportional fonts without flicker, and no full-screen framebuffer needed.
//
// Each field has its own width, centered on X. Because the screen is round, the free
// width inside the seconds ring shrinks away from the center: the time (middle) can
// be 190 px wide, the status line (bottom) only ~110 px.
constexpr int16_t FIELD_MAX_W = 190;
constexpr int16_t FIELD_MAX_H = 46;

Arduino_Canvas *fieldCanvas = new Arduino_Canvas(FIELD_MAX_W, FIELD_MAX_H, nullptr);   // ~17 KB

struct Field {
  int16_t        top, w, h;
  const uint8_t *font;      // U8g2 font
  const char    *layout;    // if set, center using this text instead (fixed position, no jitter)
  char           shown[24];
  uint16_t       shownColor;
};

enum Slot { F_TOP, F_DATE, F_MAIN, F_UTC, F_STATUS, F_COUNT };   // same order as the table below
static Field fields[F_COUNT] = {
  { 40, 140, 20, u8g2_font_helvR14_tr,    nullptr,    "", 0},   // weekday
  { 64, 150, 20, u8g2_font_helvR14_tr,    nullptr,    "", 0},   // date / message
  { 97, 190, 46, u8g2_font_logisoso42_tn, "88:88:88", "", 0},   // HH:MM:SS, centered on y=120
  {158, 170, 30, u8g2_font_logisoso20_tr, nullptr,    "", 0},   // UTC HH:MM
  {200, 110, 14, u8g2_font_helvB08_tr,    nullptr,    "", 0},   // status
};

static void setField(Slot slot, const char *txt, uint16_t color) {
  Field &f = fields[slot];
  if (strcmp(f.shown, txt) == 0 && f.shownColor == color) return;

  fieldCanvas->fillRect(0, 0, f.w, f.h, C_BG);
  fieldCanvas->setFont(f.font);
  fieldCanvas->setTextColor(color);

  int16_t x1, y1;
  uint16_t w, h;
  fieldCanvas->getTextBounds(f.layout ? f.layout : txt, 0, 0, &x1, &y1, &w, &h);
  fieldCanvas->setCursor((f.w - (int16_t)w) / 2 - x1, (f.h - (int16_t)h) / 2 - y1);  // center
  fieldCanvas->print(txt);

  // Push only the w x h rectangle of the canvas (row by row, since the canvas is wider)
  const uint16_t *fb = fieldCanvas->getFramebuffer();
  const int16_t   x  = CX - f.w / 2;
  for (int16_t row = 0; row < f.h; row++) {
    gfx->draw16bitRGBBitmap(x, f.top + row, (uint16_t *)fb + row * FIELD_MAX_W, f.w, 1);
  }

  strlcpy(f.shown, txt, sizeof(f.shown));
  f.shownColor = color;
}

static void clearFields() {
  gfx->fillScreen(C_BG);
  for (auto &f : fields) f.shown[0] = '\0';
}

// Ring of 60 dots along the edge; lit up to the current second
static int ringShown = -2;   // -2 = not drawn

static void drawRingDot(int i, uint16_t color) {
  constexpr float R = 112;
  const float a = (i * 6 - 90) * DEG_TO_RAD;
  gfx->fillCircle(CX + R * cosf(a), CY + R * sinf(a), (i % 5) == 0 ? 4 : 2, color);
}

static void setRing(int sec) {   // sec = -1 turns all dots off
  if (sec == ringShown) return;
  if (ringShown < -1 || sec < ringShown) {        // first draw or minute rollover
    for (int i = 0; i < 60; i++) drawRingDot(i, i <= sec ? C_SEC : C_DIM);
  } else {
    for (int i = ringShown + 1; i <= sec; i++) drawRingDot(i, C_SEC);
  }
  ringShown = sec;
}

// ------------------------------- Screens --------------------------------
enum class Screen { None, NoData, Waiting, Clock };
static Screen screen = Screen::None;

static void enterScreen(Screen s) {
  if (screen == s) return;
  screen = s;
  clearFields();
  ringShown = -2;
}

static void render() {
  static const char *WEEKDAYS[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
  char buf[24];

  const bool noData    = millis() > 5000 && gps.charsProcessed() < 10;
  const bool timeValid = gps.time.isValid() && gps.date.isValid() &&
                         gps.date.year() >= 2020 && gps.time.age() < GPS_STALE_MS;
  const bool hasFix    = gps.location.isValid() && gps.location.age() < GPS_STALE_MS;

  if (!timeValid) {
    enterScreen(noData ? Screen::NoData : Screen::Waiting);
    setRing(-1);
    setField(F_MAIN, "--:--:--", C_DIM);
    if (noData) {
      setField(F_DATE, "NO GPS DATA", C_ERR);
      setField(F_STATUS, "check TX -> D6", C_ERR);
    } else {
      setField(F_DATE, "WAITING", C_WARN);
      snprintf(buf, sizeof(buf), "SAT %lu", (unsigned long)gps.satellites.value());
      setField(F_STATUS, buf, C_WARN);
    }
    return;
  }

  enterScreen(Screen::Clock);

  const LocalTime lt = utcToLocal(gps.date.year(), gps.date.month(), gps.date.day(),
                                  gps.time.hour(), gps.time.minute(), gps.time.second());

  setRing(lt.second);
  setField(F_TOP, WEEKDAYS[lt.weekday], C_DATE);

  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", lt.hour, lt.minute, lt.second);
  setField(F_MAIN, buf, C_TIME);

  snprintf(buf, sizeof(buf), "UTC %02u:%02u", gps.time.hour(), gps.time.minute());
  setField(F_UTC, buf, C_SEC);

  snprintf(buf, sizeof(buf), "%02u/%02u/%04d", lt.day, lt.month, lt.year);
  setField(F_DATE, buf, C_DATE);

  // Before a fix, time comes from the GPS internal RTC and may be off by a few seconds
  snprintf(buf, sizeof(buf), "%s  SAT %lu", hasFix ? "FIX" : "NO FIX",
           (unsigned long)gps.satellites.value());
  setField(F_STATUS, buf, hasFix ? C_OK : C_WARN);
}

// ---------------------------- Serial status -----------------------------
// One line per second. Quick diagnosis:
//   chars not increasing       -> nothing arrives from the GPS (TX->D6 wiring / power)
//   chars up, ok=0, err up     -> garbage arrives (wrong baud rate or noise)
//   ok up, no fix              -> GPS works, needs open sky / time
static void printStatus() {
  Serial.printf("[GPS] chars=%lu ok=%lu err=%lu | sat=%lu hdop=%.1f | ",
                (unsigned long)gps.charsProcessed(), (unsigned long)gps.passedChecksum(),
                (unsigned long)gps.failedChecksum(), (unsigned long)gps.satellites.value(),
                gps.hdop.isValid() ? gps.hdop.hdop() : 99.9);

  if (gps.location.isValid() && gps.location.age() < GPS_STALE_MS) {
    Serial.printf("FIX %.6f,%.6f alt=%.0fm | ", gps.location.lat(), gps.location.lng(),
                  gps.altitude.meters());
  } else {
    Serial.print(F("NO FIX | "));
  }

  if (gps.time.isValid() && gps.date.isValid()) {
    Serial.printf("UTC %04u-%02u-%02u %02u:%02u:%02u (age %lums)\n",
                  gps.date.year(), gps.date.month(), gps.date.day(),
                  gps.time.hour(), gps.time.minute(), gps.time.second(),
                  (unsigned long)gps.time.age());
  } else {
    Serial.println(F("time invalid"));
  }
}

// -------------------------------- Setup ---------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  Serial.println(F("node32-gps-clock started"));

  // Wi-Fi is not used: turning it off frees heap and removes RF noise near the GPS antenna
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  if (!gfx->begin()) {
    Serial.println("Display init failed");
    while (true) delay(1000);
  }
  if (!fieldCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("Not enough RAM for the field canvas");
    while (true) delay(1000);
  }
  gfx->fillScreen(C_BG);
  setField(F_MAIN, "GPS", C_TIME);

  // Must come AFTER gfx->begin(): SPI.begin() switches GPIO12 (D6) to its MISO function
  // and SoftwareSerial stops receiving. The display is write-only and doesn't need MISO.
  // RX only. 512 B buffer (~45 ms at 115200): each field is pushed in a few ms.
  // SoftwareSerial's internal edge buffer is ~5x this size in uint32, so be careful when increasing it.
  gpsSerial.begin(GPS_BAUD, SWSERIAL_8N1, PIN_GPS_RX, -1, false, 512);

  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
}

// --------------------------------- Loop ---------------------------------
void loop() {
  while (gpsSerial.available()) {
    const char c = gpsSerial.read();
    gps.encode(c);
    if (GPS_ECHO_NMEA) Serial.write(c);
  }

  // Redraw when a new GPS time arrives (1 Hz) or every 1 s to refresh the status
  static uint32_t lastDraw = 0;
  const bool newTime = gps.time.isUpdated();
  if (newTime) gps.time.value();   // clear the flag even if render() doesn't read the time
  if (newTime || millis() - lastDraw > 1000) {
    lastDraw = millis();
    render();
  }

  // After render(): printStatus() reads the time and would clear the isUpdated flag
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= STATUS_EVERY_MS) {
    lastStatus = millis();
    printStatus();
  }
}
