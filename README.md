# node32-gps-clock

A GPS-disciplined clock for a round 1.28" display. It shows local time, date, weekday
and UTC. The time comes from a CN-06 (u-blox NEO-6M) GPS receiver, and a NodeMCU (ESP8266)
drives the display.

```
        SATURDAY
       27/09/2026
     ·            ·
   ·   18:42:10     ·     <- local time, centered
     ·  UTC 21:42   ·
       FIX  SAT 7         <- fix status / satellites
```

A ring of 60 dots along the edge fills up as the seconds go by.

## Features

- Local time `HH:MM:SS`, date `DD/MM/YYYY`, weekday and UTC `HH:MM`.
- Fixed UTC offset (default UTC-3, Brasília). Day, month and year rollover is handled.
- Smooth U8g2 fonts (Logisoso, Helvetica) without flicker. Each field is rendered into
  a small RAM canvas and pushed only when it changes, because the ESP8266 has no RAM
  for a 115 KB full-screen framebuffer.
- Clear on-screen states: waiting for GPS, no GPS data (wiring), no fix, fix.
- One-line GPS status per second on the serial monitor for troubleshooting.
- Wi-Fi turned off: it frees heap and keeps RF noise away from the GPS antenna.

## Hardware

| Part | Notes |
|---|---|
| NodeMCU v2/v3 (ESP8266, ESP-12E) | |
| CN-06 GPS receiver (u-blox NEO-6M) | e.g. Crius CN-06. Any NMEA GPS should work. |
| GC9A01 round IPS display, 1.28", 240x240, SPI | Its SPI pins are often labeled SDA/SCL. It is **not** I2C. |

## Wiring

| CN-06 GPS | NodeMCU | |
|---|---|---|
| VCC | VIN (5V) | The module has its own regulator. 3V3 also works, with less margin. |
| GND | GND | |
| TX | D6 (GPIO12) | SoftwareSerial RX |
| RX | not connected | Only needed to configure the GPS |

| GC9A01 | NodeMCU | |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| SCL | D5 (GPIO14) | SPI clock (fixed) |
| SDA | D7 (GPIO13) | SPI MOSI (fixed) |
| CS | D1 (GPIO5) | |
| DC | D2 (GPIO4) | |
| RST | D0 (GPIO16) | |
| BLK | 3V3 | Or leave it open if the module has a pull-up |

D3, D4 and D8 are ESP8266 boot strapping pins and are deliberately not used.

## Build and upload

This is a [PlatformIO](https://platformio.org/) project. The libraries are installed
automatically:

- [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) 1.4.9: GC9A01 driver and canvas
- [TinyGPSPlus](https://github.com/mikalhart/TinyGPSPlus): NMEA parser
- [U8g2](https://github.com/olikraus/u8g2): fonts only

```sh
pio run -t upload     # build and flash
pio device monitor    # serial monitor (74880 baud)
```

Set `upload_port` / `monitor_port` in [platformio.ini](platformio.ini) to your port, or
remove them for auto-detection.

## Configuration

All settings are at the top of [src/main.cpp](src/main.cpp):

| Constant | Default | Meaning |
|---|---|---|
| `UTC_OFFSET_MIN` | `-180` | Local offset from UTC in minutes |
| `GPS_BAUD` | `115200` | GPS serial baud rate (the NEO-6M factory default is **9600**) |
| `GPS_ECHO_NMEA` | `false` | Forward raw NMEA sentences to the serial monitor |
| `STATUS_EVERY_MS` | `1000` | Serial status interval |

The screen layout (position, size and font of each field) is the `fields[]` table:

```cpp
// top, width, height, font, layout reference
{ 97, 190, 46, u8g2_font_logisoso42_tn, "88:88:88", "", 0},   // HH:MM:SS
```

Fields are centered horizontally. Keep each field inside the seconds ring and within
the 190x46 canvas (`FIELD_MAX_W` / `FIELD_MAX_H`).

## Serial diagnostics

```
[GPS] chars=13194 ok=486 err=0 | sat=7 hdop=1.1 | FIX -23.03,-46.97 alt=690m | UTC 2026-09-27 21:42:10 (age 68ms)
```

| Symptom | Likely cause |
|---|---|
| `chars` stays at 0 | Nothing reaches D6: check the wiring (GPS **TX** -> D6), GND and power |
| `chars` goes up, `ok=0`, `err` goes up | Wrong baud rate, or the GPS is set to UBX binary only |
| `ok` goes up, `sat=0`, no fix | The GPS works; it needs open sky (a cold start can take minutes) |

## Notes and lessons learned

- **SPI takes over D6.** On the ESP8266, `SPI.begin()` switches GPIO12 to its MISO
  function. SoftwareSerial must be started *after* the display, or it never receives
  anything.
- **The CN-06 may come preconfigured.** Modules taken from drones are often set to
  38400/57600/115200 baud and may not keep new settings (no flash on the NEO-6M, a dead
  backup battery, or an onboard EEPROM that won't write). This clock simply uses the
  baud rate the module already has.
- **The time before a fix may be off by a few seconds.** After a cold start, the NEO-6M
  can use a default leap-second value until it receives the UTC parameters, which can
  take up to ~12.5 minutes. The status line shows `NO FIX` in orange until then.
- The GFX library is pinned to 1.4.9. Newer releases target ESP32 Arduino core 3.x.
