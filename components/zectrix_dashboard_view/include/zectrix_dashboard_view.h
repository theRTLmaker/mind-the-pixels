#ifndef ZECTRIX_DASHBOARD_VIEW_H_
#define ZECTRIX_DASHBOARD_VIEW_H_

// Dashboard layout and drawing, shared by the firmware and the Mac simulator.
// Everything here draws into a ZectrixCanvas and must stay free of ESP-IDF
// runtime calls (networking, FreeRTOS, display refresh) so it builds on a host.

#include "zectrix_canvas.h"
#include "zectrix_dashboard_events.h"
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
