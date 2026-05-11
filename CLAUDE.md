# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build / flash / monitor

PlatformIO project, single environment `esp32dev`. On this Windows machine the
`pio` CLI is **not on PATH** — call the absolute path:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor -b 115200
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev -t erase   # wipe NVS / saved WiFi creds
```

`pio run` alone (no target) is the build-only check — fast feedback after edits
and the standard sanity check before claiming a code change is done. There are
no tests, no linter — the build is the only automated check.

## Architecture

**Single-file firmware** — everything lives in [src/main.cpp](src/main.cpp).
`include/`, `lib/`, `test/` are unused PlatformIO defaults. Don't add files
unless there's a clear reason; this project's value is being readable in one
scroll.

**Two display devices, independent timing:**
- TM1637 7-segment shows `HH:MM`; redrawn on colon flip (1 Hz) and on minute
  change. Keeps ticking even in portal mode.
- SSD1306 OLED runs a **page rotation** (`PAGE_MAIN` 10 s, four secondary
  pages 6 s each). State: `currentPage`, `pageEnteredAt`, `PAGE_DURATIONS_MS[]`.
  Pages render through `renderOLEDDispatch()`. Page advance forces an immediate
  re-render so the screen never stays stale across a transition.

**Two top-level modes:**
- **RUN** — normal `loop()`: WiFi manage, weather fetch, render. The `loop()`
  body is short — most work happens inside `manageWiFi` / `manageWeather` /
  the render dispatcher.
- **PORTAL** — `runConfigPortal()` is a **blocking infinite loop** with its own
  AP + DNS + HTTP server. It only exits via `ESP.restart()` triggered by
  `/save` or `/cancel`. So anything you add that needs to run during portal
  mode (e.g. button polling for a future "exit portal" feature) must be added
  inside that loop, not in `loop()`.

**WiFi creds** persist in NVS namespace `clockcfg`. On boot, fail-to-connect
or holding GPIO0 (BOOT) drops to portal. Mid-run, `OFFLINE_THRESHOLD_MS` of
no connection also drops to portal — by design, since the captive portal is
the only way to fix creds without re-flashing.

**Weather** is one struct (`WeatherData`) populated by `fetchWeather()` from
Open-Meteo. The query bundles `current`, `daily` (3 days), and `hourly`
(`precipitation_probability`, 12 hours) into one HTTPS call with `timezone=auto`.
All page renderers read from this struct; they don't make their own network
calls. Page renderers must handle `!weather.valid` / `!dailyValid` /
`!hourlyValid` and show `Loading...`.

## OLED dual-color bands — important for layout

The SSD1306 panel used here is **bicolor**: rows **0–15 yellow**, **16–63 blue**.
Any text that straddles row 15/16 looks split. Rules when adding pages or
moving content:
- Page headers (size-1 text at y=0) sit fully in yellow — good for titles.
- Body content must start at **y ≥ 16**. With 8-px size-1 chars and 10-px
  pitch, the standard body row positions are 16, 26, 36, 46, 56.
- The big size-3 temperature on the main page starts at y=16 deliberately.

Past bug: an earlier system page had its first body row at y=14, which put the
top half of the row in the yellow band. Fix was to shift body rows to start at
y=16.

## Free GPIOs and hardware notes

In use today: 19, 18 (TM1637), 21, 22 (I2C OLED), 2 (onboard LED), 0 (BOOT).
Available for buttons/LEDs/sensors: 4, 5, 13-17, 23, 25-27, 32, 33, plus
input-only 34-39 (no internal pull-ups). Avoid GPIO 6-11 (flash) and treat
strapping pins (0, 2, 12, 15) carefully — fine as outputs after boot but don't
tie to anything that pulls them low at reset.

## Feature backlog

[IDEAS.md](IDEAS.md) holds the user's curated list of future features with
hardware needs and effort hints (S/M/L). When the user picks an item to
implement, that file is the source of truth for what was proposed — read it
before planning. Do not add or strike items unsolicited.

## Branch workflow

Work happens on `dev`. The user's standard ship sequence is: commit on dev →
push dev → fast-forward `main` from `dev` → push main → back to dev. Main is
never edited directly. The user will say "commit push, merge to main and
checkout dev again" or similar.
