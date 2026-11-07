/*
  node32-gps-clock (LCD)
  GPS clock: NodeMCU (ESP8266 / ESP-12E) + ATGM336H GNSS receiver (GPS + BeiDou) + 20x4
  HD44780 LCD with a PCF8574 I2C backpack. The PPS output aligns the seconds.

  Wiring
  ---------------------------------------------------------------
  ATGM336H         NodeMCU
    VCC     ->     3V3        (safe for both bare modules and breakouts with a regulator)
    GND     ->     GND
    TX      ->     D6 (GPIO12)   SoftwareSerial RX
    RX      ->     D7 (GPIO13)   SoftwareSerial TX (optional: only to configure the GPS)
    PPS     ->     D5 (GPIO14)   interrupt, rising edge = start of the UTC second

  LCD backpack     NodeMCU
    VCC     ->     VIN (5V)   (HD44780 needs 5V for usable contrast)
    GND     ->     GND
    SDA     ->     D2 (GPIO4)
    SCL     ->     D1 (GPIO5)
  ---------------------------------------------------------------
  Most backpacks pull SDA/SCL up to VCC (5V). The ESP8266 is a 3.3V part: use a level
  shifter, or remove the backpack pull-ups and add 4.7k to 3V3. With 3V3 pull-ups the
  PCF8574 at 5V sees 3.3V as HIGH (V_IH = 0.7 x VCC = 3.5V: marginal, works in practice
  with short wires).

  Deliberately avoided: D3/GPIO0, D4/GPIO2 and D8/GPIO15 are boot strapping pins;
  D9/D10 are UART0 (USB); D0/GPIO16 has no interrupt (cannot take PPS).

  How the time is kept
    NMEA tells WHICH second it is, but arrives 100-500 ms after the second started (and
    the delay varies). PPS marks EXACTLY when the second starts, but carries no label.
    With a fix, each PPS edge shows (time of the last RMC sentence + 1 s), so the LCD
    turns over at the true second. Without PPS (no fix, or not wired) the clock falls
    back to updating on NMEA arrival.

  Screen
    +--------------------+
    |SATURDAY  27/09/2026|
    |     BR 18:42:10    |
    |    UTC 21:42:10    |
    |PPS SAT 9  HDOP  1.1|
    +--------------------+
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>   // PCF8574 backpack, address and pin map auto-detected
#include <SoftwareSerial.h>
#include <TinyGPSPlus.h>

// ---------------------------- Configuration -----------------------------
constexpr int UTC_OFFSET_MIN = -3 * 60;   // Brasília; no daylight saving time since 2019

constexpr int8_t   PIN_GPS_RX  = D6;         // ESP RX <- GPS TX
constexpr int8_t   PIN_GPS_TX  = D7;         // ESP TX -> GPS RX (configuration only; idles high)
constexpr int8_t   PIN_GPS_PPS = D5;         // GPS PPS, 1 pulse per second once there is a fix
constexpr uint32_t GPS_BAUD    = 9600;       // ATGM336H factory default

constexpr int8_t   PIN_I2C_SDA = D2;
constexpr int8_t   PIN_I2C_SCL = D1;
constexpr uint32_t I2C_CLOCK   = 100000;   // PCF8574 is specified for 100 kHz

constexpr uint32_t GPS_STALE_MS = 3000;    // no new sentence for longer than this = stale data

constexpr uint32_t SERIAL_BAUD     = 74880;  // same as the ROM boot log: everything readable in the monitor
constexpr uint32_t STATUS_EVERY_MS = 1000;   // GPS status interval on the serial monitor
constexpr bool     GPS_ECHO_NMEA   = false;  // true = forward raw NMEA sentences to the monitor

// ------------------------------- Display --------------------------------
constexpr uint8_t LCD_COLS = 20, LCD_ROWS = 4;

enum class Align { Left, Center, Right };
constexpr Align CLOCK_ALIGN = Align::Right;   // BR / UTC time rows: Align::Left, Align::Center or Align::Right

hd44780_I2Cexp lcd;

// --------------------------------- GPS ----------------------------------
TinyGPSPlus    gps;
SoftwareSerial gpsSerial;

// ------------------------------ Date/time -------------------------------
struct CivilTime {
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

// Seconds since 1970-01-01 00:00:00
static int64_t toEpoch(int y, int mo, int d, int h, int mi, int s) {
  return (int64_t)daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

// Splits epoch seconds into date and time (handles day/month/year rollover)
static CivilTime fromEpoch(int64_t t) {
  int32_t days = (int32_t)(t / 86400);
  int32_t secs = (int32_t)(t % 86400);
  if (secs < 0) { secs += 86400; days--; }

  CivilTime ct;
  civilFromDays(days, ct.year, ct.month, ct.day);
  ct.hour    = secs / 3600;
  ct.minute  = (secs / 60) % 60;
  ct.second  = secs % 60;
  ct.weekday = (uint8_t)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday (4)
  return ct;
}

// -------------------------------- Timing --------------------------------
// NMEA: the last RMC sentence (date + time in one sentence, so no midnight mix-ups)
static bool     nmeaValid = false;
static int64_t  nmeaEpoch = 0;     // UTC
static uint32_t nmeaUs    = 0;     // micros() when it was parsed

// PPS: written by the interrupt
static volatile uint32_t ppsEdgeUs = 0;
static volatile uint32_t ppsEdges  = 0;

static IRAM_ATTR void onPps() {
  ppsEdgeUs = micros();
  ppsEdges++;
}

// PPS: processed in loop()
static bool     ppsValid      = false;   // ppsEpoch labels the last edge correctly
static int64_t  ppsEpoch      = 0;       // UTC second that started at the last edge
static uint32_t lastEdgeUs    = 0;
static uint32_t lastEdgeCount = 0;
static uint32_t ppsMismatches = 0;       // times NMEA disagreed with the PPS count
static int32_t  nmeaDelayMs   = -1;      // last PPS edge -> RMC parsed (diagnostics)

static bool ppsLocked() {
  return ppsValid && micros() - lastEdgeUs < 1100000;
}

// Called for every new PPS edge. Returns true if the edge is a valid second boundary.
static bool processPpsEdge(uint32_t edgeUs, uint32_t edgeCount) {
  const uint32_t interval = edgeUs - lastEdgeUs;
  const uint32_t prevUs   = lastEdgeUs;
  // Exactly one edge, ~1 s after the previous one: rejects noise and a floating input
  const bool regular = edgeCount - lastEdgeCount == 1 && interval > 900000 && interval < 1100000;
  lastEdgeUs    = edgeUs;
  lastEdgeCount = edgeCount;

  if (!regular) { ppsValid = false; return false; }

  if (ppsValid) ppsEpoch++;   // flywheel: one more second

  // The RMC that arrived after the previous edge described that edge's second, so this
  // edge is that second + 1. Check (and if needed correct) the running count.
  if (nmeaValid && nmeaUs - prevUs < interval) {
    const int64_t expected = nmeaEpoch + 1;
    if (ppsValid && ppsEpoch != expected) ppsMismatches++;
    ppsEpoch = expected;
    ppsValid = true;
  }
  return ppsValid;
}

// ------------------------- Incremental row drawing ----------------------
// The LCD keeps its own content, so only the characters that differ from what is on
// screen are sent. A normal second changes 1-2 characters per row: little I2C traffic,
// so SoftwareSerial (interrupt driven) does not lose GPS bytes during an update.
static char shown[LCD_ROWS][LCD_COLS];

// Writes txt starting at column col (0-based); the rest of the row is blanked.
// Text that does not fit is clipped at the right edge.
static void setRow(uint8_t row, const char *txt, uint8_t col) {
  char line[LCD_COLS];
  memset(line, ' ', LCD_COLS);

  if (col < LCD_COLS) {
    memcpy(line + col, txt, min(strlen(txt), (size_t)(LCD_COLS - col)));
  }

  int cursor = -1;   // column the LCD cursor is at, -1 = unknown
  for (uint8_t c = 0; c < LCD_COLS; c++) {
    if (line[c] == shown[row][c]) continue;
    if (cursor != c) lcd.setCursor(c, row);
    lcd.write(line[c]);
    cursor = c + 1;
  }
  memcpy(shown[row], line, LCD_COLS);
}

static void setRow(uint8_t row, const char *txt, Align align = Align::Left) {
  const size_t len = min(strlen(txt), (size_t)LCD_COLS);
  setRow(row, txt, (uint8_t)(align == Align::Center ? (LCD_COLS - len) / 2
                           : align == Align::Right  ? LCD_COLS - len
                                                    : 0));
}

static void clearRows() {
  lcd.clear();
  memset(shown, ' ', sizeof(shown));
}

// ------------------------------- Screens --------------------------------
enum class Screen { None, NoData, Waiting, Clock };
static Screen screen = Screen::None;

static void enterScreen(Screen s) {
  if (screen == s) return;
  screen = s;
  clearRows();
}

// haveTime = utcEpoch is a valid current UTC time
static void render(bool haveTime, int64_t utcEpoch) {
  static const char *WEEKDAYS[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
  char buf[32];

  const bool noData = millis() > 5000 && gps.charsProcessed() < 10;
  const bool hasFix = gps.location.isValid() && gps.location.age() < GPS_STALE_MS;

  if (!haveTime) {
    enterScreen(noData ? Screen::NoData : Screen::Waiting);
    setRow(1, "--:--:--", Align::Center);
    if (noData) {
      setRow(0, "NO GPS DATA", Align::Center);
      setRow(3, "check TX -> D6", Align::Center);
    } else {
      setRow(0, "WAITING FOR GPS", Align::Center);
      snprintf(buf, sizeof(buf), "SAT %lu", (unsigned long)gps.satellites.value());
      setRow(3, buf, Align::Center);
    }
    return;
  }

  enterScreen(Screen::Clock);

  const CivilTime lt  = fromEpoch(utcEpoch + (int64_t)UTC_OFFSET_MIN * 60);
  const CivilTime utc = fromEpoch(utcEpoch);

  // Weekday (max 9 chars) + space + date (10 chars) = exactly 20
  snprintf(buf, sizeof(buf), "%-9s %02u/%02u/%04d", WEEKDAYS[lt.weekday], lt.day, lt.month, lt.year);
  setRow(0, buf);

  snprintf(buf, sizeof(buf), " BR %02u:%02u:%02u", lt.hour, lt.minute, lt.second);
  setRow(1, buf, Align::Center);

  snprintf(buf, sizeof(buf), "UTC %02u:%02u:%02u", utc.hour, utc.minute, utc.second);
  setRow(2, buf, Align::Center);

  // PPS = fix and seconds aligned to the pulse. FIX = fix, NMEA timing only.
  // Before a fix, time comes from the GPS internal RTC and may be off by a few seconds.
  // Fixed width (20): "PPS SAT 9  HDOP  1.1" / "FIX SAT 12 HDOP 12.3"
  if (hasFix) {
    const double hdop = gps.hdop.isValid() ? min(gps.hdop.hdop(), 99.9) : 99.9;
    snprintf(buf, sizeof(buf), "%s SAT %-2lu HDOP %4.1f", ppsLocked() ? "PPS" : "FIX",
             (unsigned long)min(gps.satellites.value(), (uint32_t)99), hdop);
  } else {
    snprintf(buf, sizeof(buf), "NO FIX  SAT %lu", (unsigned long)gps.satellites.value());
  }
  setRow(3, buf, Align::Center);
}

// Redraw with the best time available: PPS-aligned if locked, else the last RMC
static void renderNow() {
  if (ppsLocked()) {
    render(true, ppsEpoch);
  } else {
    render(nmeaValid && micros() - nmeaUs < GPS_STALE_MS * 1000, nmeaEpoch);
  }
}

// ---------------------------- Serial status -----------------------------
// One line per second. Quick diagnosis:
//   chars not increasing       -> nothing arrives from the GPS (TX->D6 wiring / power)
//   chars up, ok=0, err up     -> garbage arrives (wrong baud rate or noise)
//   ok up, no fix              -> GPS works, needs open sky / time
//   fix, pps=off, edges=0      -> check PPS -> D5 (the pulse only starts after a fix)
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

  Serial.printf("pps=%s edges=%lu nmea+%ldms mism=%lu | ", ppsLocked() ? "LOCK" : "off",
                (unsigned long)lastEdgeCount, (long)nmeaDelayMs, (unsigned long)ppsMismatches);

  if (nmeaValid) {
    const CivilTime utc = fromEpoch(nmeaEpoch);
    Serial.printf("UTC %04d-%02u-%02u %02u:%02u:%02u (age %lums)\n",
                  utc.year, utc.month, utc.day, utc.hour, utc.minute, utc.second,
                  (unsigned long)((micros() - nmeaUs) / 1000));
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

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK);

  // begin() returns 0 on success; error codes are in hd44780.h (e.g. no I2C device found)
  const int status = lcd.begin(LCD_COLS, LCD_ROWS);
  if (status) {
    Serial.printf("LCD init failed (status %d): check SDA->D2, SCL->D1, 5V and pull-ups\n", status);
    hd44780::fatalError(status);   // blinks the built-in LED, never returns
  }
  clearRows();
  setRow(1, "GPS CLOCK", Align::Center);

  // 512 B RX buffer (~0.5 s at 9600), far longer than an LCD update takes.
  // TX is only used to send configuration commands to the GPS.
  gpsSerial.begin(GPS_BAUD, SWSERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX, false, 512);

  // Pull-up keeps the input quiet if PPS is not wired; the module drives it push-pull
  pinMode(PIN_GPS_PPS, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_GPS_PPS), onPps, RISING);

  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
}

// --------------------------------- Loop ---------------------------------
void loop() {
  while (gpsSerial.available()) {
    const char c = gpsSerial.read();
    gps.encode(c);
    if (GPS_ECHO_NMEA) Serial.write(c);
  }

  static uint32_t lastDraw = 0;

  // New RMC: date and time are updated together only by RMC (GGA has no date)
  if (gps.date.isUpdated()) {
    gps.date.value();   // clear the flags
    gps.time.value();
    if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2020) {
      nmeaEpoch   = toEpoch(gps.date.year(), gps.date.month(), gps.date.day(),
                            gps.time.hour(), gps.time.minute(), gps.time.second());
      nmeaUs      = micros();
      nmeaValid   = true;
      nmeaDelayMs = ppsLocked() ? (int32_t)((nmeaUs - lastEdgeUs) / 1000) : -1;
      // Without PPS the NMEA arrival is the best second boundary we have
      if (!ppsLocked()) { lastDraw = millis(); renderNow(); }
    }
  }

  // New PPS edge: the second starts now
  noInterrupts();
  const uint32_t edgeUs = ppsEdgeUs, edgeCount = ppsEdges;
  interrupts();
  if (edgeCount != lastEdgeCount && processPpsEdge(edgeUs, edgeCount)) {
    lastDraw = millis();
    render(true, ppsEpoch);
  }

  // Fallback: keep status and state changes (lost fix, no data) on screen
  if (millis() - lastDraw > 1100) {
    lastDraw = millis();
    renderNow();
  }

  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= STATUS_EVERY_MS) {
    lastStatus = millis();
    printStatus();
  }
}
