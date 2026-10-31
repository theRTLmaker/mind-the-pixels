# AGENTS.md — Mind the Pixels

Guidance for coding agents working in this repository. Read README.md first
for what the project is and how to build it.

## Project

Mind the Pixels is a personal London dashboard (weather, trains to St
Pancras, curated London events) for the ZECTRIX NOTE4 DevKit: ESP32-S3, 16 MB
flash, 8 MB PSRAM, 400 × 300 SSD2683 e-paper with 1bpp full/partial and
4bpp (16-gray) full refresh. It is built on Zectrix Lab's MIT-licensed
reference demo (upstream: https://github.com/itopinion/zectrix-note4-epd-demo).
Intended public repository name: `mind-the-pixels`.

## Toolchain

- macOS on Apple Silicon, ESP-IDF **5.5.0** only (6.1 breaks the waveform
  static assertions). Activate with
  `source ~/.espressif/tools/activate_idf_v5.5.sh` after `conda deactivate`.
- Firmware: `idf.py build`. Simulator: `simulator/run.sh` (CMake + clang,
  no ESP-IDF).
- Serial port is usually `/dev/cu.usbmodem1101`, but check.

## Hard rules

- **Never flash the device without the user's explicit go-ahead** for that
  flash, and confirm the port first. Flashing replaces the firmware.
- Never run `git reset --hard`, `git clean`, checkout-over or anything else
  that discards local work. The worktree is the source of truth.
- Never print, commit or ask for the Wi-Fi password. It lives only in the
  gitignored `sdkconfig`. Never commit `sdkconfig`, `sdkconfig.old`, keys or
  local paths.
- The original flash backup lives in `~/Documents/Projects/note4/backups/`,
  outside the repo. Keep it there.
- Keep upstream `LICENSE`, `THIRD_PARTY_NOTICES.md` and `licenses/` intact.
  Do not assume every asset or dependency is covered by the main MIT licence.
- Don't push or create the GitHub repository unless asked.
- Don't revert the `components/zectrix_epd/CMakeLists.txt` REQUIRES change;
  it is needed for ESP-IDF 5.5.
- The events taste profile, curation output and gist link live outside the
  repo in `~/Documents/Projects/note4/events/` (personal data). Never copy
  them, or the gist URL, into the repository.

## Architecture

- `main/app_main.cc`: app flow and menus. `RunDashboard()` fetches data,
  calls the view, refreshes the panel and runs the clock/train update loop.
- `components/zectrix_dashboard_view/`: **all dashboard layout and drawing**.
  It only touches `ZectrixCanvas` and the weather/train data structs, so it
  compiles both for the ESP32 and for the Mac. Keep ESP-IDF runtime calls
  (networking, FreeRTOS, logging, refresh) out of it.
- `components/zectrix_demo_ui/zectrix_canvas.*`: 1bpp canvas (1 = white,
  MSB first) with pixel, rect, Bresenham line and TRMNL16 bitmap text
  (proportional 16 px font, ASCII only, integer scaling).
- `components/zectrix_dashboard_{wifi,weather,trains}/`: data sources.
  Weather is Open-Meteo for 51.5072, -0.1276, Europe/London. Trains come
  from the Rail Data Marketplace Live Departure Board (Staff Version,
  `GetDepBoardWithDetails/SFA/<yyyymmddThhmmss>`, ISO times, departed
  trains skipped, typical busyness from
  `formation.serviceLoading`) when `CONFIG_ZECTRIX_TRAINS_RDM_API_KEY` is set
  (menuconfig, gitignored `sdkconfig`; never print or commit it), falling
  back to
  `https://huxley2.azurewebsites.net/departures/SFA/to/STP/3`. Refreshed
  every 60 s, cached on failure.
- `components/zectrix_dashboard_events/`: fetches `events.json` from
  `CONFIG_ZECTRIX_EVENTS_URL` (a secret gist, gitignored `sdkconfig`) with
  the weather every 15 min and keeps up to four `screen` lines. The file is
  written daily by the local scheduled task `mind-the-pixels-events`.
- `ZectrixCanvas::Text` also draws UTF-8 "£" (hand-drawn glyph in
  `zectrix_canvas.cc`); `TextGray` in the view is ASCII only.
- Partial refresh regions are byte-aligned and exported from the view header
  (`kDashboardTrainsRefreshRect`, `kDashboardClockRefreshRect`). If you move
  the clock or train card, update them.
- `simulator/`: host build of the canvas, font and view, with
  `host_include/` stand-ins for `esp_err.h` and the SPI/GPIO driver headers,
  fixtures in `fixtures.h`, PNG output in `simulator/out/`.

Layout today: weather (8, 8, 232×140), clock top-right centred on x=312
with the battery icon at (366, 12) in the same partial window, trains
(248, 34, 144×114), events (8, 158, 384×132).

Night mode (`main/app_main.cc`): from 22:00 to 05:00, once the dashboard
has been open 10 minutes, the board draws the night screen
(`ZectrixDashboardDrawNight`: weather, picks, battery) and deep-sleeps to
the next full hour. The PCF8563 (set to UTC) alarm on RTC_INT wakes it, with
an RC-timer backup 10 minutes later; each night wake restores the time from
the PCF8563, fetches weather and picks (last good copies live in
`RTC_DATA_ATTR` globals) and sleeps again. OK (GPIO0) wakes into the
dashboard; the 05:00 wake goes to the dashboard too. Deep sleep drops the
USB serial port, so flashing during the night needs a button wake first.

## Working style

1. Inspect the relevant files and `git status --short` before editing.
2. Briefly explain the intended change, then make a small, testable change.
3. For visual changes: render in the simulator first (`simulator/run.sh`),
   then run `idf.py build`. Flash only when the user asks.
4. Preserve unrelated work. Prefer focused edits over rewrites.
5. Give the user copy-pasteable terminal commands.

The simulator reproduces pixels, not physics: refresh flashing, ghosting and
panel contrast must be validated on the device.

## Known issues / next steps

- Weather is re-fetched every 15 minutes on the dashboard; a change redraws
  the whole screen with a gray full refresh (~7 s), which also resets the
  partial-refresh counter.
- The dashboard opens with a 16-gray refresh (`RefreshFullGray`), then
  updates the clock and train card with 1bpp partials on top of it via
  `zectrix_epd_set_partial_base_1bpp`. Keep gray out of those partial
  windows. The canvas keeps both frames; the simulator writes `name.png`
  (gray) and `name-mono.png` (1bpp, gray dithered).
