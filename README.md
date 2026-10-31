# Mind the Pixels

*A little less screen time. A little more London.*

A quiet London dashboard for the ZECTRIX NOTE4 e-paper display: weather,
the next trains to St Pancras, and (later) what's coming up in the calendar.
It runs on the NOTE4's ESP32-S3 and draws on its 400 × 300 SSD2683 panel,
which supports full-screen black/white and 16-level grayscale refreshes.

Mind the Pixels is built on the
[ZECTRIX NOTE4 E-Paper Reference Demo](https://github.com/itopinion/zectrix-note4-epd-demo)
by Zectrix Lab, which provides the board support, display driver and bitmap
font. See [Credits and licence](#credits-and-licence).

> [!IMPORTANT]
> This targets the black-and-white ZECTRIX NOTE4, not the NOTE4C. Flashing
> replaces whatever firmware is on the device. Confirm the model and serial
> port before every flash, and keep a backup of the original flash outside
> this repository.

## Screens

UP and DOWN switch between four screens; OK refreshes the current one (on
"Coming up" it turns the page) and holding DOWN switches the board off. The board boots on a "Mind the Pixels"
splash, then the dashboard.

1. **Dashboard**: weather, clock, trains to St Pancras and four picks (below).
2. **Coming up**: up to 12 curated picks, six per page, with when, price and
   venue; pages turn every minute with a quick black-and-white refresh.
3. **This weekend**: Saturday and Sunday forecasts, up to four weekend
   picks, and today's sunrise and sunset ([Open-Meteo](https://open-meteo.com/)
   forecast).
4. **Commute**: Margarida's journey between Stratford International and
   Cambridge, changing at St Pancras (Thameslink) or King's Cross (Great
   Northern). Before 14:00 it shows the way there, after that the way home:
   up to four connections with live times (inverted when they differ from
   the timetable), the timetable underneath and the time to change. It
   needs the Rail Data Marketplace key and refreshes every 2 minutes.

### Dashboard

| Region | Content | Source |
| --- | --- | --- |
| Top left | Temperature, low/high, rain chance, feels-like, humidity, wind, condition icon | [Open-Meteo](https://open-meteo.com/) (London, HTTPS) |
| Top right | Clock (UK time via SNTP) and next three departures Stratford → St Pancras, with revised times, delays, cancellations and typical busyness | [Rail Data Marketplace](https://raildata.org.uk/) Live Departure Board (Staff Version), falling back to [Huxley2](https://huxley2.azurewebsites.net/); both National Rail Darwin |
| Bottom | "Coming up": four curated London picks (exhibitions, jazz, classical, theatre deals, markets) with FREE/DEAL/BOOK tags | An `events.json` you publish (e.g. a secret gist), see [Events](#events) |

The weather and events are fetched at start and every 15 minutes after
that; trains refresh in the background every 60 seconds and keep the last
good data if a refresh fails. The dashboard is drawn with a 16-gray full
refresh; the clock and train card update with black-and-white partial
refreshes.

A battery icon sits beside the clock on every screen (a bolt while
charging). From 22:00 to 05:00, after 10 minutes without a button press,
the board switches to a night screen (weather, picks, battery) and
deep-sleeps, waking once an hour on the board's RTC alarm to refresh the
weather; press OK to bring the screens back for 10 minutes. At 05:00 it
returns to the screen it was on.

## Hardware

- ZECTRIX NOTE4 DevKit (ESP32-S3, 16 MB flash, 8 MB octal PSRAM)
- 4.2-inch 400 × 300 SSD2683 e-paper, 1bpp full/partial and 4bpp (16-gray)
  full refresh
- Three buttons, RTC, NFC, speaker/microphone (used by the self-tests)

## Firmware setup

Requires ESP-IDF **5.5.0**. ESP-IDF 6.1 fails static assertions in the
display waveform tables, so stay on 5.5.x unless you have tested otherwise.

```bash
conda deactivate 2>/dev/null || true
source ~/.espressif/tools/activate_idf_v5.5.sh
idf.py --version
```

### Wi-Fi

Set the dashboard's Wi-Fi network in `idf.py menuconfig` under
**Component config → Dashboard Wi-Fi**. These are stored in your local
`sdkconfig`, which is gitignored. Never commit `sdkconfig`, `sdkconfig.old`
or any file containing the password.

### Trains

Without configuration the train card uses the public Huxley2 server. For the
official feed, register at [raildata.org.uk](https://raildata.org.uk/),
subscribe to **Live Departure Board – Staff Version**, and enter its consumer
key in `idf.py menuconfig` under **Component config → Dashboard trains**. The
key is stored only in your gitignored `sdkconfig`. The firmware requests
`GetDepBoardWithDetails/SFA/<current UK time>` filtered to St Pancras, skips
trains that have already left, and shows the feed's typical busyness as
one to three bars. If that request fails it falls back to Huxley2.

### Build and flash

```bash
idf.py set-target esp32s3   # first time only
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

Replace the port with your device's (`ls /dev/cu.usbmodem*`). Exit the
monitor with `Ctrl+]`. The main task stack is raised to 12 KB in
`sdkconfig.defaults` because HTTPS requests overflowed the default.

### Events

The "Coming up" panel draws the `screen` lines of a small JSON file fetched
from `CONFIG_ZECTRIX_EVENTS_URL` (**Component config → Dashboard events**,
stored in the gitignored `sdkconfig`). How the file is produced is up to
you; the author's is written each morning by a scheduled Claude task that
searches London listings against a private taste profile and updates a
secret gist. Minimal format:

```json
{
  "screen": [
    {"when": "Fri 30", "text": "Free jazz, Oxford House E2", "tag": "FREE"},
    {"when": "13-22 Nov", "text": "London Jazz Festival, from £5", "tag": "BOOK"}
  ]
}
```

Up to four lines; `tag` is empty or a short word drawn inverted on the
right. Optional `weekend` (same shape, up to four lines) feeds the weekend
screen, and `items` (up to 12, with `title`, `when`, `venue`, `price`,
`deal`, `book_now`) feeds the full-screen "Coming up". Text is ASCII plus "£"; anything too long is cut with "...".

## Mac simulator

Iterate on the layout without flashing. The simulator compiles the
firmware's own drawing code (`ZectrixCanvas`, the TRMNL16 bitmap font and
`components/zectrix_dashboard_view`) for macOS and renders sample scenarios
to PNG.

```bash
simulator/run.sh                 # build, render all scenarios, open the preview
simulator/run.sh partly-cloudy   # render one scenario
simulator/build/mtp_sim --list   # list scenarios
```

Output goes to `simulator/out/`: a 1:1 PNG per scenario, a 3× enlargement,
and `index.html` showing them side by side. Scenarios live in
`simulator/fixtures.h` (sunny, partly cloudy, fog, rain, snow, storm, train
delays and cancellations, late night, offline). Needs Xcode command-line
tools and CMake (`brew install cmake`); no ESP-IDF required.

The simulator shows exact pixels, not the panel: refresh flashing, ghosting,
contrast and paper tone still need checking on the device.

## Repository layout

```text
main/app_main.cc                    Screens, buttons, refresh loop, night sleep
components/zectrix_dashboard_view/  Dashboard layout and drawing (shared with the simulator)
components/zectrix_dashboard_wifi/  Wi-Fi station connection
components/zectrix_dashboard_weather/ Open-Meteo client
components/zectrix_dashboard_trains/  Departure board client and background refresh
components/zectrix_dashboard_events/  Curated events.json client
components/zectrix_dashboard_commute/ Stratford <-> Cambridge connections
components/zectrix_dashboard_outlook/ Weekend forecast, daylight, air and pollen
components/zectrix_http/            Shared HTTPS GET helper
components/zectrix_demo_ui/         Canvas, bitmap font, panel refresh  (upstream, trimmed)
components/zectrix_epd/             SSD2683 display driver              (upstream)
components/zectrix_board/           Board pins and peripherals          (upstream)
simulator/                          Mac simulator
docs/                               Upstream hardware and API guides
tools/                              Bitmap font generator               (upstream)
```

The display driver API is documented in [docs/EPD_API.md](docs/EPD_API.md)
and the hardware in [docs/HARDWARE.md](docs/HARDWARE.md).

## Roadmap

- Switching screens with NFC (e.g. phone + tags through a small web endpoint)

## Credits and licence

Mind the Pixels is a modified version of the ZECTRIX NOTE4 E-Paper
Reference Demo, Copyright (c) 2026 Zectrix Lab. Upstream code and the
changes below (Copyright (c) 2026 Francisco Mendes) are released under the
[MIT License](LICENSE); upstream notices are kept unchanged.
The original README is preserved at
[docs/UPSTREAM_README.md](docs/UPSTREAM_README.md)
([中文](docs/UPSTREAM_README_zh.md)); its demo menu, image gallery and
hardware self-tests were removed here and remain in the upstream repository.

Changes from upstream: the screens and night mode, the Wi-Fi, weather,
train, events and commute components,
the shared dashboard view, the Mac simulator, and related edits to
`main/app_main.cc` and the build configuration.

Third-party components keep their own licences; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). In particular, the bitmap
font is derived from TRMNL16 Regular under the SIL Open Font License 1.1
([licenses/TRMNL_FONT_LICENSE.txt](licenses/TRMNL_FONT_LICENSE.txt)).

Weather data by [Open-Meteo](https://open-meteo.com/) (CC BY 4.0). Train
data from National Rail Darwin via the Rail Data Marketplace or Huxley2.

This is an independent hobby project, not affiliated with ZECTRIX,
Transport for London or National Rail.
