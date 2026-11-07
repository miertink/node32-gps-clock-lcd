# node32-gps-clock (LCD)

A GPS-disciplined clock on a 20x4 character LCD. It shows local time, date, weekday
and UTC. The time comes from an ATGM336H GNSS receiver (GPS + BeiDou), its PPS output
aligns the seconds, and a NodeMCU (ESP8266) drives the LCD through a PCF8574 I2C backpack.

```
+--------------------+
|SATURDAY  27/09/2026|
|     BR 18:42:10    |   <- local time
|    UTC 21:42:10    |
|PPS SAT 9  HDOP  1.1|   <- timing source / satellites / HDOP
+--------------------+
```

## Features

- Local time `HH:MM:SS`, date `DD/MM/YYYY`, weekday and UTC `HH:MM:SS`.
- Fixed UTC offset (default UTC-3, Brasília). Day, month and year rollover is handled.
- PPS-aligned seconds: the display turns over at the true start of the second, not
  when the NMEA sentence happens to arrive (see [How the time is kept](#how-the-time-is-kept)).
- Only the characters that changed are sent to the LCD: no flicker and minimal I2C
  traffic, so the SoftwareSerial GPS input does not lose bytes.
- I2C address (0x27, 0x3F, ...) and backpack pin mapping are auto-detected.
- Clear on-screen states: waiting for GPS, no GPS data (wiring), no fix, fix, PPS lock.
- One-line GPS status per second on the serial monitor for troubleshooting.
- Wi-Fi turned off: it frees heap and keeps RF noise away from the GPS antenna.

## Hardware

| Part | Notes |
|---|---|
| NodeMCU v2/v3 (ESP8266, ESP-12E) | |
| ATGM336H GNSS receiver (AT6558 chip) | GPS + BeiDou, NMEA at 9600 baud, PPS output. Any NMEA GPS works; PPS is optional. |
| 20x4 HD44780 LCD + PCF8574 I2C backpack | The common "LCD2004 + IIC/I2C" module |

## Wiring

| ATGM336H | NodeMCU | |
|---|---|---|
| VCC | 3V3 | Safe for the bare module (3.3V) and for breakouts with a regulator |
| GND | GND | |
| TX | D6 (GPIO12) | SoftwareSerial RX |
| RX | D7 (GPIO13) | SoftwareSerial TX. Optional: only needed to configure the GPS from the ESP |
| PPS | D5 (GPIO14) | Interrupt. Optional: without it the clock uses NMEA timing |

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

**Pin choices.** D3, D4 and D8 are ESP8266 boot strapping pins and are deliberately not
used: a pull-up on the GPS RX input, for example, could hold D8 high and stop the ESP8266
from booting. D0 (GPIO16) has no interrupt, so it cannot take PPS. That leaves D5 for PPS
and D7 for the GPS RX line. All GPS signals are 3.3V: no level shifter is needed. D7 idles
high when unused, and D5 has a pull-up so it stays quiet if PPS is not wired.

## How the time is kept

The two GPS outputs complement each other:

| Signal | Tells | Timing |
|---|---|---|
| NMEA (`$GNRMC`) | **Which** second it is (date and time) | Arrives 100-500 ms after the second started, with a varying delay (worse at 9600 baud) |
| PPS | **When** the second starts | Rising edge aligned to the UTC second (well under 1 ms) |

With a fix, each PPS edge shows *time of the last RMC + 1 s*, so the LCD turns over at
the true second. If an RMC sentence is missed, the clock counts the PPS edges on its own
(flywheel); the next RMC confirms or corrects the count (`mism` on the serial monitor).
An edge only counts if it arrives about 1 s after the previous one, which rejects noise.

Without PPS (no fix yet, or PPS not wired) the clock falls back to updating when the RMC
sentence arrives, and the status line shows `FIX` instead of `PPS`.

This assumes the receiver sends each RMC *after* the pulse it describes, which is the
usual convention. Check it once against a reference clock (e.g. time.is): the seconds
must turn over together, not 1 s apart.

## Build and upload

This is a [PlatformIO](https://platformio.org/) project. The libraries are installed
automatically:

- [hd44780](https://github.com/duinoWitchery/hd44780) (Bill Perry): LCD driver, `hd44780_I2Cexp` class
- [TinyGPSPlus](https://github.com/mikalhart/TinyGPSPlus): NMEA parser (accepts `$GP`, `$GN`, `$GB`... talkers)

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
| `PIN_GPS_PPS` | `D5` | GPS PPS input |
| `GPS_BAUD` | `9600` | GPS serial baud rate (ATGM336H factory default) |
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

## Status line

| Text | Meaning |
|---|---|
| `PPS SAT 9  HDOP  1.1` | Fix, seconds aligned to the PPS pulse |
| `FIX SAT 9  HDOP  1.1` | Fix, but no PPS: seconds follow NMEA arrival (100-500 ms late) |
| `NO FIX  SAT 3` | No fix: time comes from the receiver's internal clock |

The line is always 20 characters wide; HDOP is capped at 99.9 and satellites at 99.

## HDOP

HDOP (Horizontal Dilution of Precision) describes how good the satellite geometry is
for a horizontal position. Satellites spread across the sky give a low HDOP; satellites
bunched together (e.g. only one side of the sky visible from a window) give a high HDOP.
It is a multiplier, not a distance: the horizontal error is roughly HDOP x the receiver's
base error (a few meters for this class of receiver).

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
[GPS] chars=13194 ok=486 err=0 | sat=9 hdop=1.1 | FIX -23.03,-46.97 alt=690m | pps=LOCK edges=412 nmea+310ms mism=0 | UTC 2026-09-27 21:42:10 (age 312ms)
```

`nmea+310ms` is how long after the PPS edge the RMC sentence was parsed: this is the
delay the PPS removes from the display.

| Symptom | Likely cause |
|---|---|
| `LCD init failed (status ...)`, built-in LED blinking | No I2C device found: check SDA/SCL (not swapped), 5V, GND and pull-ups |
| `chars` stays at 0 | Nothing reaches D6: check the wiring (GPS **TX** -> D6), GND and power |
| `chars` goes up, `ok=0`, `err` goes up | Wrong baud rate (the ATGM336H default is 9600) |
| `ok` goes up, `sat=0`, no fix | The GPS works; it needs open sky (a cold start can take minutes) |
| Fix, `pps=off`, `edges=0` | PPS does not reach D5: check the wiring (the pulse only starts after a fix) |
| `mism` keeps increasing | PPS edges and NMEA disagree: noise on the PPS line, or a loose wire |

## Notes and lessons learned

- **The time before a fix may be off by a few seconds.** After a cold start, a GNSS
  receiver can use a default leap-second value until it receives the UTC parameters
  from the satellites, which can take up to ~12.5 minutes. The status line shows
  `NO FIX` until there is a fix.
- **Configuring the receiver.** The ATGM336H (AT6558) is configured with proprietary
  `$PCAS` NMEA commands (CASIC protocol): baud rate, update rate, sentences, GNSS
  constellations. The ESP can send them through D7.
- **Keep LCD writes small.** SoftwareSerial is interrupt driven and the ESP8266 I2C is
  bit-banged. Rewriting all 80 characters every second works, but sending only the
  changed characters keeps the GPS input safe.
- **Earlier hardware.** The first version used a CN-06 (u-blox NEO-6M) at 115200 baud,
  without PPS, and a GC9A01 round display. See the commit history.
