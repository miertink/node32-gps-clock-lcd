# node32-gps-clock (LCD)

A GPS-disciplined clock on a 20x4 character LCD. It shows local time, date, weekday
and UTC. The time comes from a CN-06 (u-blox NEO-6M) GPS receiver, and a NodeMCU (ESP8266)
drives the LCD through a PCF8574 I2C backpack.

```
+--------------------+
|SATURDAY  27/09/2026|
|     BR 18:42:10    |   <- local time
|    UTC 21:42:10    |
|FIX SAT 7   HDOP 1.1|   <- fix status / satellites / HDOP
+--------------------+
```

## Features

- Local time `HH:MM:SS`, date `DD/MM/YYYY`, weekday and UTC `HH:MM:SS`.
- Fixed UTC offset (default UTC-3, Brasília). Day, month and year rollover is handled.
- Only the characters that changed are sent to the LCD: no flicker and minimal I2C
  traffic, so the SoftwareSerial GPS input does not lose bytes.
- I2C address (0x27, 0x3F, ...) and backpack pin mapping are auto-detected.
- Clear on-screen states: waiting for GPS, no GPS data (wiring), no fix, fix.
- One-line GPS status per second on the serial monitor for troubleshooting.
- Wi-Fi turned off: it frees heap and keeps RF noise away from the GPS antenna.

## Hardware

| Part | Notes |
|---|---|
| NodeMCU v2/v3 (ESP8266, ESP-12E) | |
| CN-06 GPS receiver (u-blox NEO-6M) | e.g. Crius CN-06. Any NMEA GPS should work. |
| 20x4 HD44780 LCD + PCF8574 I2C backpack | The common "LCD2004 + IIC/I2C" module |

## Wiring

| CN-06 GPS | NodeMCU | |
|---|---|---|
| VCC | VIN (5V) | The module has its own regulator. 3V3 also works, with less margin. |
| GND | GND | |
| TX | D6 (GPIO12) | SoftwareSerial RX |
| RX | D7 (GPIO13) | SoftwareSerial TX. Optional: only needed to configure the GPS from the ESP |

| LCD backpack | NodeMCU | |
|---|---|---|
| VCC | VIN (5V) | The HD44780 needs 5V for usable contrast |
| GND | GND | |
| SDA | D2 (GPIO4) | |
| SCL | D1 (GPIO5) | |

**Level warning:** most backpacks have 4.7k-10k pull-ups from SDA/SCL to their VCC (5V),
which puts 5V on the ESP8266 pins. Either use a bidirectional level shifter, or remove
the backpack pull-ups and fit 4.7k to 3V3. Many people run it directly and it works, but
it is outside the ESP8266 specification.

If the screen lights up but shows nothing (or only blocks), adjust the contrast
potentiometer on the backpack.

D3, D4 and D8 are ESP8266 boot strapping pins and are deliberately not used.

**Why D7 for the GPS RX line:** it is free, it is not a boot strapping pin (the pull-up
on the GPS RX input could hold D8 high and stop the ESP8266 from booting), and it sits
next to D6. No level shifter is needed: the NEO-6M I/O is 3.3V even when the CN-06 board
is powered from 5V. The clock itself only reads from the GPS; D7 idles high when unused.

## Build and upload

This is a [PlatformIO](https://platformio.org/) project. The libraries are installed
automatically:

- [hd44780](https://github.com/duinoWitchery/hd44780) (Bill Perry): LCD driver, `hd44780_I2Cexp` class
- [TinyGPSPlus](https://github.com/mikalhart/TinyGPSPlus): NMEA parser

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
| `PIN_GPS_RX` / `PIN_GPS_TX` | `D6` / `D7` | GPS serial pins (ESP side) |
| `GPS_BAUD` | `115200` | GPS serial baud rate (the NEO-6M factory default is **9600**) |
| `PIN_I2C_SDA` / `PIN_I2C_SCL` | `D2` / `D1` | I2C pins |
| `I2C_CLOCK` | `100000` | I2C clock (the PCF8574 is specified for 100 kHz) |
| `GPS_ECHO_NMEA` | `false` | Forward raw NMEA sentences to the serial monitor |
| `STATUS_EVERY_MS` | `1000` | Serial status interval |

The screen layout is in `render()`. Each row is written with either an alignment or
an absolute start column (0-based); the rest of the row is blanked:

```cpp
setRow(2, buf, Align::Center);   // Align::Left | Align::Center | Align::Right
setRow(2, buf, 8);               // start at column 8, clipped at column 20
```

## HDOP

HDOP (Horizontal Dilution of Precision) describes how good the satellite geometry is
for a horizontal position. Satellites spread across the sky give a low HDOP; satellites
bunched together (e.g. only one side of the sky visible from a window) give a high HDOP.
It is a multiplier, not a distance: the horizontal error is roughly HDOP x the receiver's
base error (a few meters for the NEO-6M).

| HDOP | Rating | Meaning |
|---|---|---|
| < 1 | Ideal | Best possible geometry |
| 1 - 2 | Excellent | Typical with open sky and 7+ satellites |
| 2 - 5 | Good | Normal for navigation |
| 5 - 10 | Moderate | Position usable, but noticeably off |
| 10 - 20 | Fair | Rough position only |
| > 20 | Poor | Position unreliable |
| 99.9 | No value | No HDOP received yet (no fix) |

HDOP only shows up on the status line once there is a fix. It describes the position,
not the time: the displayed time is accurate to the second whenever there is a fix,
even with a high HDOP.

## Serial diagnostics

```
[GPS] chars=13194 ok=486 err=0 | sat=7 hdop=1.1 | FIX -23.03,-46.97 alt=690m | UTC 2026-09-27 21:42:10 (age 68ms)
```

| Symptom | Likely cause |
|---|---|
| `LCD init failed (status ...)`, built-in LED blinking | No I2C device found: check SDA/SCL (not swapped), 5V, GND and pull-ups |
| `chars` stays at 0 | Nothing reaches D6: check the wiring (GPS **TX** -> D6), GND and power |
| `chars` goes up, `ok=0`, `err` goes up | Wrong baud rate, or the GPS is set to UBX binary only |
| `ok` goes up, `sat=0`, no fix | The GPS works; it needs open sky (a cold start can take minutes) |

## Notes and lessons learned

- **The CN-06 may come preconfigured.** Modules taken from drones are often set to
  38400/57600/115200 baud and may not keep new settings (no flash on the NEO-6M, a dead
  backup battery, or an onboard EEPROM that won't write). This clock simply uses the
  baud rate the module already has. If the module does not keep its settings, the ESP
  can send the configuration (UBX/NMEA commands) on every boot through D7.
- **The time before a fix may be off by a few seconds.** After a cold start, the NEO-6M
  can use a default leap-second value until it receives the UTC parameters, which can
  take up to ~12.5 minutes. The status line shows `NO FIX` until then.
- **Keep LCD writes small.** SoftwareSerial at 115200 baud is interrupt driven and the
  ESP8266 I2C is bit-banged. Rewriting all 80 characters every second works, but
  sending only the changed characters keeps the GPS input safe.
