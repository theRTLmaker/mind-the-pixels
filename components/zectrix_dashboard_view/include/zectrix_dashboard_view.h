#ifndef ZECTRIX_DASHBOARD_VIEW_H_
#define ZECTRIX_DASHBOARD_VIEW_H_

// Dashboard layout and drawing, shared by the firmware and the Mac simulator.
// Everything here draws into a ZectrixCanvas and must stay free of ESP-IDF
// runtime calls (networking, FreeRTOS, display refresh) so it builds on a host.

#include "zectrix_canvas.h"
#include "zectrix_dashboard_commute.h"
#include "zectrix_dashboard_events.h"
#include "zectrix_dashboard_outlook.h"
#include "zectrix_dashboard_trains.h"
#include "zectrix_dashboard_weather.h"

struct ZectrixDashboardRect {
    int x;
    int y;
    int width;
    int height;
};

// Byte-aligned partial-refresh regions covering the train card and clock.
// Battery state for the icon beside the clock (filled from the board's
// power snapshot in app_main; the view stays free of ESP-IDF calls).
struct ZectrixDashboardBattery {
    bool valid;
    int percent;
    bool charging;
    bool full;
};

inline constexpr ZectrixDashboardRect kDashboardTrainsRefreshRect = {
    248, 32, 152, 120};
inline constexpr ZectrixDashboardRect kDashboardClockRefreshRect = {
    248, 0, 152, 32};
// Clock and battery on the full-screen events view, and the whole view for
// page turns (it has no gray).
inline constexpr ZectrixDashboardRect kEventsClockRefreshRect = {
    296, 0, 104, 28};
inline constexpr ZectrixDashboardRect kEventsPageRefreshRect = {
    0, 0, 400, 300};
// Commute view: header (title, update time, weather, clock, battery) and
// the journey rows; both black and white so they can update partially.
inline constexpr ZectrixDashboardRect kCommuteHeaderRefreshRect = {
    72, 0, 328, 44};
inline constexpr ZectrixDashboardRect kCommuteRowsRefreshRect = {
    0, 48, 400, 252};

// Weather area. Pass nullptr when the weather fetch failed. On days with
// rain the hourly chart replaces humidity and wind; hours before
// current_hour are drawn lighter (-1 when unknown).
void ZectrixDashboardDrawWeather(ZectrixCanvas& canvas,
                                 const zectrix_weather_data_t* weather,
                                 int current_hour = -1);

// Train card. Clears its own area so it can be redrawn in place.
void ZectrixDashboardDrawTrains(ZectrixCanvas& canvas,
                                const zectrix_trains_data_t& trains);

// Events area (placeholder until the calendar is connected).
void ZectrixDashboardDrawEvents(ZectrixCanvas& canvas,
                                const zectrix_events_data_t* events);

// True when any hour today has a rain probability above 0%.
bool ZectrixDashboardHasRain(const zectrix_weather_data_t* weather);

// Hourly rain-probability chart in the lower part of the weather area.
// Hours before current_hour are drawn lighter; pass -1 when unknown.
void ZectrixDashboardDrawRainChart(ZectrixCanvas& canvas,
                                   const zectrix_weather_data_t& weather,
                                   int current_hour = -1);

// Clock in the top-right corner. Clears its own area; text such as "10:42".
void ZectrixDashboardDrawClock(ZectrixCanvas& canvas, const char* text,
                               const ZectrixDashboardBattery* battery);
// Full-screen "Coming up": the curated items with when, price and venue,
// kEventsPerPage per page (page is 0-based). Black and white only, so pages
// turn with a partial refresh of kEventsPageRefreshRect.
inline constexpr int kEventsPerPage = 6;
int ZectrixDashboardEventsPageCount(const zectrix_events_data_t* events);
void ZectrixDashboardDrawEventsFull(ZectrixCanvas& canvas,
                                    const zectrix_events_data_t* events,
                                    const ZectrixDashboardBattery* battery,
                                    const char* clock_text, int page);

// Margarida's commute: Stratford Intl <-> London <-> Cambridge connections
// with live and timetable times, plus a weather line.
void ZectrixDashboardDrawCommute(ZectrixCanvas& canvas,
                                 const zectrix_weather_data_t* weather,
                                 const zectrix_commute_data_t* commute,
                                 const ZectrixDashboardBattery* battery,
                                 const char* clock_text);

// The coming weekend: Saturday and Sunday weather, weekend picks, and
// today's sunrise and sunset. The clock window is
// kEventsClockRefreshRect, as on the events view.
void ZectrixDashboardDrawWeekend(ZectrixCanvas& canvas,
                                 const zectrix_outlook_data_t* outlook,
                                 const zectrix_events_data_t* events,
                                 const ZectrixDashboardBattery* battery,
                                 const char* clock_text);

// Boot screen: wordmark above a station platform edge, with a status line
// (e.g. "Connecting to Wi-Fi..."). Gray areas dither on a 1bpp refresh.
void ZectrixDashboardDrawSplash(ZectrixCanvas& canvas, const char* status);

// Screen left on the panel during the overnight deep sleep: weather (left),
// night status and battery (right), picks (bottom). updated_text is the
// local "HH:MM" of the last refresh.
void ZectrixDashboardDrawNight(ZectrixCanvas& canvas,
                               const zectrix_weather_data_t* weather,
                               const zectrix_events_data_t* events,
                               const ZectrixDashboardBattery* battery,
                               const char* wake_text,
                               const char* updated_text);

// Clears the canvas and draws the whole dashboard.
void ZectrixDashboardDrawAll(ZectrixCanvas& canvas,
                             const zectrix_weather_data_t* weather,
                             const zectrix_trains_data_t& trains,
                             const zectrix_events_data_t* events,
                             const ZectrixDashboardBattery* battery,
                             const char* clock_text);

#endif  // ZECTRIX_DASHBOARD_VIEW_H_
