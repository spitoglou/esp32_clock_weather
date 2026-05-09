# ESP32 NTP Clock + Weather

ESP32-WROOM-32 firmware that drives a **TM1637 4-digit 7-segment** display
(HH:MM with a blinking colon) and a **128x64 SSD1306 OLED** (date, temperature,
condition, wind, humidity, WiFi info). Time comes from NTP, weather from
[Open-Meteo](https://open-meteo.com) (free, no API key). WiFi credentials are
provisioned via a captive-portal AP — no hard-coded SSIDs in the firmware.

---

## Features

- HH:MM on 7-segment, colon blinks at exactly 1 Hz.
- OLED layout split for dual-color (yellow/blue) panels: small text in the top
  yellow band, big temperature + status rows in the blue band.
- Captive-portal WiFi config (first boot, on saved-cred failure, or after
  10 min offline mid-run).
- Async network scan with 30 s cache so the captive page lands fast and
  rescanning is instant.
- Greek/EU timezone with automatic DST (configurable, POSIX `TZ` syntax).
- Persistent credentials in NVS (`Preferences`).
- Auto-reconnect with on-screen countdown to the fallback portal.
- Onboard LED blinks at 1 Hz only while the portal is up — calm in normal use.
- Hold the BOOT button at power-on to force re-configuration.

---

## Hardware

### Bill of materials

| Part                       | Notes                                              |
| -------------------------- | -------------------------------------------------- |
| ESP32-WROOM-32 dev board   | Onboard LED on GPIO2 (active HIGH on most boards)  |
| TM1637 4-digit 7-seg       | Runs at 3.3 V                                      |
| SSD1306 OLED 128x64, I2C   | Default address `0x3C` (some are `0x3D`)           |

The BOOT pushbutton (GPIO0) is already on every ESP32 dev board.

### Wiring

```
ESP32           Module
-----           ------
3V3     -----   TM1637 VCC + OLED VCC
GND     -----   TM1637 GND + OLED GND
GPIO19  -----   TM1637 CLK
GPIO18  -----   TM1637 DIO
GPIO21  -----   OLED SDA
GPIO22  -----   OLED SCL
GPIO2   -----   onboard LED (no wiring needed)
GPIO0   -----   BOOT button (no wiring needed)
```

If the digits look dim and you want them brighter, the TM1637 will also run
from 5 V (VIN) — the 3.3 V GPIO drive on CLK/DIO is still accepted by the
module. VIN is only present when powered via USB/barrel.

Pin assignments are constants near the top of [src/main.cpp](src/main.cpp):
- TM1637 — [main.cpp:53-54](src/main.cpp#L53-L54)
- OLED I2C bus — see `Wire.begin(21, 22)` inside `initOLED()`
- LED — [main.cpp:40](src/main.cpp#L40)
- BOOT button — [main.cpp:78](src/main.cpp#L78)

---

## Build and flash

### Prerequisites

- [PlatformIO](https://platformio.org) (CLI or VS Code extension)
- USB-serial driver for your dev board (CP210x or CH340)

### Build

```sh
pio run
```

Library deps are declared in [platformio.ini](platformio.ini) and pulled
automatically on first build.

### Flash

```sh
pio run -t upload
```

Pass `--upload-port COMx` if PlatformIO can't auto-detect the board.

### Serial monitor

```sh
pio device monitor -b 115200
```

### Erase NVS (wipe stored creds)

```sh
pio run -t erase
```

This clears the `clockcfg` namespace among other things; the device will
re-enter the portal on the next boot.

---

## First-boot configuration

1. After flashing, the device boots into AP mode (no saved creds).
2. The OLED shows `== SETUP MODE ==` plus the AP SSID
   `ESP32-Clock-XXXX`, where `XXXX` is the last 4 hex chars of the MAC.
3. The onboard LED blinks at 1 Hz to signal portal mode.
4. From a phone or laptop, join the open AP. Modern OSes (iOS 14+, Android 8+,
   Windows 11) will auto-pop the captive page; otherwise browse to
   `http://192.168.4.1`.
5. The page shows nearby networks (live async scan), a manual SSID field for
   hidden networks, and a password field.
6. Hit **Save & Connect**. Creds are persisted to NVS, the device reboots and
   joins the chosen network.
7. **Cancel & reboot** is also available — useful if you entered the portal by
   mistake and want to fall back to the previously saved network.

To re-enter the portal later: hold the BOOT button while powering on.

---

## Operation

### Displays

**TM1637** (24-hour `HH:MM`):
- Colon blinks at 1 Hz (driven by [main.cpp:75](src/main.cpp#L75) `COLON_BLINK_MS`).
- Shows dashes (`----`) before NTP has synced.

**OLED** layout (top to bottom):

| Row range  | Content                                                          |
| ---------- | ---------------------------------------------------------------- |
| 0–7        | Date on the left (`Mon 09 Mar 2026`), humidity `%` on the right  |
| 16–39      | Big temperature, centered, with degree-C glyph                   |
| 44–51      | Weather condition on the left, wind `km/h` on the right          |
| 56–63      | When online: IP + RSSI. When offline: `Retry M:SS` and `->cfg`   |

The dual-color SSD1306 yellow band covers approximately rows 0–15. The big
temperature deliberately starts at row 16 so it sits entirely in the blue
section.

### State machine

```
                          boot
                            |
              +-------------+-------------+
              |             |             |
       BOOT held?       no creds?     creds + connect ok?
              |             |             |
              v             v             v
           PORTAL        PORTAL          RUN
              ^             ^             |
              |             |             | offline > 10 min
              |             |             v
              |             +---------- PORTAL
              |
              +---- /save or /cancel ----> reboot
```

- **RUN** — clock + weather refresh, retry WiFi every 30 s on drop, OLED shows
  the offline countdown.
- **PORTAL** — AP up at 192.168.4.1, captive HTTP server, segment clock keeps
  ticking on the free-running RTC, LED blinks. Exits only by `ESP.restart()`
  (from `/save` or `/cancel`).

### LED conventions

| State  | LED                         |
| ------ | --------------------------- |
| RUN    | Off                         |
| PORTAL | Blinks at 1 Hz (500 ms on / 500 ms off) |

---

## Configuration knobs

All editable in [src/main.cpp](src/main.cpp):

### Timezone — [main.cpp:43](src/main.cpp#L43)

```cpp
const char* TZ_INFO = "EET-2EEST,M3.5.0/3,M10.5.0/4";
```

POSIX `TZ` string. Examples:
- `EET-2EEST,M3.5.0/3,M10.5.0/4` — Eastern Europe (Greece) with DST
- `CET-1CEST,M3.5.0,M10.5.0/3` — Central Europe with DST
- `PST8PDT,M3.2.0,M11.1.0` — US Pacific
- `UTC0` — UTC, no DST

### Weather location — [main.cpp:46-47](src/main.cpp#L46-L47)

```cpp
const float LATITUDE  = 37.9838f;
const float LONGITUDE = 23.7275f;
```

### Tunable timings

| Constant                  | Default | Where                                                        | Purpose                                                  |
| ------------------------- | ------- | ------------------------------------------------------------ | -------------------------------------------------------- |
| `WEATHER_INTERVAL_MS`     | 15 min  | [main.cpp:48](src/main.cpp#L48)                              | Refresh cadence after a successful fetch                 |
| `WEATHER_RETRY_MS`        | 60 s    | [main.cpp:49](src/main.cpp#L49)                              | Retry cadence after a failed fetch                       |
| `WEATHER_FIRST_DELAY_MS`  | 3 s     | [main.cpp:50](src/main.cpp#L50)                              | Settle DNS after WiFi up before first fetch              |
| `WIFI_BOOT_TIMEOUT_MS`    | 20 s    | [main.cpp:71](src/main.cpp#L71)                              | Initial connect window at boot                           |
| `WIFI_RETRY_INTERVAL_MS`  | 30 s    | [main.cpp:72](src/main.cpp#L72)                              | Time between mid-run reconnect attempts                  |
| `OFFLINE_THRESHOLD_MS`    | 10 min  | [main.cpp:73](src/main.cpp#L73)                              | Drop into portal after this long offline                 |
| `AP_LED_BLINK_MS`         | 500     | [main.cpp:74](src/main.cpp#L74)                              | LED half-period in portal mode                           |
| `COLON_BLINK_MS`          | 500     | [main.cpp:75](src/main.cpp#L75)                              | Colon half-period                                        |
| `SCAN_CACHE_MS`           | 30 s    | [main.cpp:96](src/main.cpp#L96)                              | How long the portal reuses the previous scan             |

### TM1637 brightness schedule

The 7-segment display dims at night so it isn't blinding in a dark room.
Constants live near the TM1637 declaration in
[src/main.cpp](src/main.cpp):

| Constant         | Default       | Purpose                                      |
| ---------------- | ------------- | -------------------------------------------- |
| `BRIGHT_DAY`     | 7 (max)       | Brightness during day window                 |
| `BRIGHT_NIGHT`   | 3             | Brightness outside day window                |
| `DAY_START_HOUR` | 7  (inclusive)| First "day" hour                             |
| `DAY_END_HOUR`   | 22 (exclusive)| First "night" hour after day                 |

Schedule: 07:00–21:59 → `BRIGHT_DAY`; 22:00–06:59 → `BRIGHT_NIGHT`. The
brightness is recomputed every render and only takes effect once NTP has
synced (before that, the display shows dashes at the boot brightness =
`BRIGHT_DAY`). The OLED is not dimmed by this schedule.

### NVS namespace

Credentials live under namespace `clockcfg` with keys `ssid` and `pass`. See
`loadCredentials` / `saveCredentials`.

---

## Architecture

### Project layout

```
260509-150708-esp32dev/
  platformio.ini       PlatformIO project config + lib_deps
  src/main.cpp         entire firmware
  README.md            this file
  include/, lib/, test/ unused (PlatformIO defaults)
```

### Module map (within `main.cpp`)

| Section            | Anchors                                  | Responsibility                                  |
| ------------------ | ---------------------------------------- | ----------------------------------------------- |
| Headers + globals  | top of file                              | Constants, pin defs, state, weather struct      |
| Display init       | `initSegment` / `initOLED`               | TM1637 + I2C + SSD1306 startup                  |
| Credentials        | `loadCredentials` / `saveCredentials` / `makeApSsid` | Preferences I/O, AP SSID derivation |
| WiFi mgmt          | `connectSavedWiFi` / `manageWiFi`        | Boot connect + reconnect loop + offline trigger |
| Segment render     | `renderSegment`                          | HH:MM with colon                                |
| Portal HTML        | `PAGE_CSS` + `handleRoot` / `handleSave` / `handleCancel` / `handleNotFound` | UI |
| Portal scan        | `ensureScan`                             | Async + cached WiFi scan                        |
| Portal loop        | `runConfigPortal`                        | AP + DNS + HTTP + clock keep-running            |
| Time               | `initTime`                               | NTP servers + TZ                                |
| Weather            | `weatherDescription` / `fetchWeather` / `manageWeather` | Open-Meteo client + cadence       |
| OLED render        | `drawDegreeAndC` / `renderOLED`          | Main display layout                             |
| Arduino entry      | `setup` / `loop`                         | Wiring it together                              |

### Main loop pacing

- `delay(20)` between iterations.
- Colon flips every 500 ms; segment is redrawn on flip.
- OLED redraws when `tm_sec` changes (effectively 1 Hz).
- WiFi reconnect attempt at most once every 30 s.
- Weather refresh on its own schedule (15 min after success, 60 s after fail).

### Portal loop pacing

- `delay(2)` between iterations so DNS + HTTP stay snappy.
- LED + colon both 1 Hz.
- OLED refreshed once per second (shows count of connected stations).
- WiFi scan is async; `WiFi.scanComplete()` is polled per request, results
  cached for 30 s.

### External services

- **NTP** — `pool.ntp.org`, `time.google.com` via `configTime`.
- **Weather** — `https://api.open-meteo.com/v1/forecast`. Free tier, no API
  key. Current params requested: `temperature_2m,relative_humidity_2m,
  weather_code,wind_speed_10m`.
- **TLS** — `client.setInsecure()`. The app does no auth and the data is
  unsensitive; pin a CA bundle if you want stricter integrity (see Extending).

### Captive-portal HTTP routes

| Method | Path     | Purpose                                                |
| ------ | -------- | ------------------------------------------------------ |
| GET    | `/`      | Setup page: scan + manual SSID + password + cancel     |
| POST   | `/save`  | Persist creds and reboot                               |
| GET    | `/cancel`| Reboot without changes                                 |
| any    | `*`      | 302 redirect to `http://192.168.4.1` (captive probe)   |

The DNS server replies with the AP IP for *every* hostname, so any HTTP
request lands on this device and triggers the OS-level captive-portal popup.

---

## Resource usage

Approximate, from the latest build:

- Flash: ~1.01 MB / 1.31 MB (~77 %)
- RAM:   ~48.8 KB / 327.7 KB (~15 %)

Most of the flash goes to the WiFi/TLS stack, the SSD1306 framebuffer (1 KB),
and Adafruit GFX glyph data.

---

## Troubleshooting

### `SSD1306 not found` on the serial log

- Check SDA/SCL wiring (GPIO21 SDA, GPIO22 SCL).
- A few panels use `0x3D` instead of `0x3C` — change `OLED_ADDR`
  ([main.cpp:61](src/main.cpp#L61)).
- The device deliberately halts (`while (true) delay(100);`) so you notice the
  problem instead of running blind. Fix the wiring and reboot.

### Captive page never opens after joining the AP

- Manually browse to `http://192.168.4.1`. Some Android builds suppress the
  popup.
- Confirm the LED is blinking (otherwise the device is not in portal mode).

### Time stuck on `--:--`

- NTP needs working DNS plus outbound UDP/123 — both are sometimes blocked on
  hotel/work WiFi.
- Wrong timezone shows the wrong time, *not* dashes. Dashes mean NTP itself
  hasn't synced.

### Weather stays on `Loading...`

- Verify RSSI > -75 dBm.
- TLS handshakes can fail on captive-restricted networks. Try a different
  network.
- If your link is flaky, raise `WEATHER_RETRY_MS` so failed fetches don't hammer
  the radio.

### Display shows `Retry 0:00 ->cfg` and then freezes there

- Expected: after 10 min offline the device drops into the portal and the main
  loop stops. Either save new creds or hit Cancel.

### Build error: TM1637 library not found

- The `lib_deps` entry pulls TM1637 from
  `https://github.com/avishorp/TM1637.git`. Check that GitHub is reachable;
  otherwise drop a copy into `lib/` and remove the URL.

### "I want to start fresh"

- `pio run -t erase` wipes NVS. The next boot will land in the portal.

---

## Extending

A few directions if you want to keep playing:

- **Brightness scheduling** — call `segDisplay.setBrightness()` and
  `oled.dim()` based on local time. Hook into the second-change branch in
  `loop`.
- **MQTT / Home Assistant** — add `knolleary/PubSubClient` and publish
  `weather` + `WiFi.RSSI()` once per minute.
- **Forecast / hourly view** — pass `&hourly=...` to Open-Meteo and rotate
  display "pages" every N seconds.
- **TLS root CA pinning** — replace `client.setInsecure()` with
  `client.setCACert(...)`. Open-Meteo uses Let's Encrypt ISRG Root X1.
- **Settings UI** — extend the portal with a second page that edits TZ / lat /
  lon and persists to NVS, so the firmware itself is location-agnostic.
- **Sensor integration** — wire a BME280 to the same I2C bus, swap "Loading..."
  for indoor temp + humidity, keep Open-Meteo as the outdoor reading.

---

## Library dependencies

Declared in [platformio.ini](platformio.ini):

| Library             | Source                              | Notes                                |
| ------------------- | ----------------------------------- | ------------------------------------ |
| TM1637              | github.com/avishorp/TM1637          | 4-digit 7-segment driver             |
| Adafruit SSD1306    | PlatformIO registry, ^2.5.13        | Pulls in Adafruit GFX + BusIO        |
| ArduinoJson         | PlatformIO registry, ^7.0.0         | Used by the weather parser           |

Everything else (`WiFi`, `WebServer`, `DNSServer`, `Preferences`,
`HTTPClient`, `WiFiClientSecure`, `Wire`) is part of the Arduino-ESP32 core.
