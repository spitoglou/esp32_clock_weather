# Future Feature Ideas

A menu of features to pick from for future development. Each item lists
the hardware it needs and a rough effort hint (S = small, M = medium,
L = large). Free GPIOs on the WROOM-32: 4, 5, 13-17, 23, 25-27, 32, 33,
plus input-only 34, 35, 36, 39.

---

## Buttons & input

### Long-press dedicated button → enter config portal (S)
Add a dedicated tactile button on **GPIO27** (free, no strapping role,
supports internal pull-up — wire button between GPIO27 and GND,
configure `INPUT_PULLUP`). In `loop()`, on falling edge start a timer;
if still held after 3 s, call `runConfigPortal()`. OLED shows a
progress bar in the bottom row during the hold; cancels if released
early. BOOT button (GPIO0) stays as power-on-only force-config, as
today. If the paged UI is also built, this becomes the **SET** button
in the top-face button row (short-press = settings menu, long-press =
config portal).

Alternative pins if GPIO27 is needed elsewhere: 4, 5, 13, 14, 16, 17,
23, 25, 26, 32, 33.

### Paged OLED UI with 1-2 buttons (M)
Short-press = next page, long-press = back to main, auto-return after
20 s idle. Pages:
- Page 1 — current (today's screen, unchanged)
- Page 2 — daily forecast for next 3 days (Open-Meteo `daily=`)
- Page 3 — hourly precipitation bars (mini chart, next 6-12 h)
- Page 4 — sunrise / sunset + day length
- Page 5 — system info (uptime, RSSI, IP, free heap)

### Quality-of-life toggles (S, per toggle)
One dedicated button cycling through quick actions, or a few:
- Manual weather refresh (force fetch now)
- 12 h / 24 h time format toggle
- Brightness mode: auto / always-bright / always-dim
- Ambient LEDs on/off (if installed)

---

## LEDs

### Weather-aware ambient strip (M)
WS2812 strip or ring on a single GPIO. Visual cues:
- Temperature gradient — blue → green → amber → red mapped to °C
- Precipitation pulse — slow blue ripple on rain/snow codes
- Storm alert — red flash on codes 95-99 (thunderstorms)
- Sunrise simulation — gradual warm-white ramp before alarm time

### Status RGB LED (S)
Single common-anode RGB or one WS2812. Encodes state at a glance:
green = WiFi + fresh weather, amber = WiFi but weather stale,
red = offline, blue blink = config portal active.

---

## Sensors

### LDR auto-brightness (S)
Photoresistor on an ADC pin replaces the fixed-hour `BRIGHT_DAY/NIGHT`
schedule with true ambient-light response. Smooths with a moving
average so passing shadows don't flicker the display.

### PIR motion sensor (S)
Blank or dim both displays when no one is in the room; wake on motion.
Saves OLED burn-in over months of running.

### Indoor environment sensor — DHT22 or BME280 (M)
Show indoor temperature and humidity alongside outdoor. BME280 also
gives pressure (trend = weather prediction without an API). Enables:
- Dew-point display
- "Open the window?" hint when outdoor is more comfortable than indoor
- Indoor/outdoor delta on the OLED

---

## Clock / timer features

### Alarm clock (M)
Extend the captive portal with an "Alarm" page: time, weekdays, on/off.
Saved to Preferences. Snooze button silences for 5 min, optional LED
ring does pre-alarm sunrise ramp, passive piezo buzzer for the chime.

### Pomodoro / focus timer (S)
Button starts a 25-min timer. Countdown on TM1637 (mm:ss). If LED
strip installed, it depletes as a progress bar. Buzzer at end.

### Stopwatch mode (S)
Hold-to-start, tap-to-lap, double-tap-to-reset. Display on TM1637.

---

## Data / connectivity

### Web dashboard + mDNS (M)
Respond on `clock.local`. Small web UI shows current readings, lets
you change city, timezone, brightness, alarm — no need to re-flash for
config changes.

### MQTT / Home Assistant integration (M)
Publish weather + status to MQTT for HA. Optionally subscribe to a
topic for a "next calendar event" string to display on the OLED.

### Persistent settings in portal (S, per setting)
Extend the existing captive portal to also configure:
- City / coordinates (currently hardcoded to Athens)
- Timezone (currently hardcoded EET)
- Brightness schedule hours
- Units (°C/°F, km/h / mph)

---

## Use more of Open-Meteo

Already a free API call, just unused fields:
- Daily highs / lows
- Sunrise / sunset times
- UV index
- Hourly precipitation probability
- Apparent ("feels like") temperature
- Air quality (separate free Open-Meteo endpoint)
- Temperature trend arrows (↑/↓ vs. last fetch or vs. yesterday)

---

## Notes

- Avoid GPIO 6-11 (connected to onboard flash).
- GPIO 0, 2, 12, 15 are strapping pins — fine as outputs after boot,
  but don't tie them to anything that pulls them low at reset.
- GPIO 34, 35, 36, 39 are input-only and have no internal pull-up —
  good for sensors with external pull-ups (LDR, PIR).
- TM1637 (19, 18), I2C OLED (21, 22), onboard LED (2), BOOT (0)
  are already taken.
