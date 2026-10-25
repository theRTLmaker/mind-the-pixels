#ifndef ZECTRIX_DASHBOARD_VIEW_H_
#define ZECTRIX_DASHBOARD_VIEW_H_

// Dashboard layout and drawing, shared by the firmware and the Mac simulator.
// Everything here draws into a ZectrixCanvas and must stay free of ESP-IDF
// runtime calls (networking, FreeRTOS, display refresh) so it builds on a host.

#include "zectrix_canvas.h"
#include "zectrix_dashboard_trains.h"
#include "zectrix_dashboard_weather.h"

struct ZectrixDashboardRect {
    int x;
    int y;
    int width;
    int height;
};

// Byte-aligned partial-refresh regions covering the train card and clock.
inline constexpr ZectrixDashboardRect kDashboardTrainsRefreshRect = {
    248, 32, 152, 120};
inline constexpr ZectrixDashboardRect kDashboardClockRefreshRect = {
    248, 0, 152, 32};

// Weather panel. Pass nullptr when the weather fetch failed.
void ZectrixDashboardDrawWeather(ZectrixCanvas& canvas,
                                 const zectrix_weather_data_t* weather);

// Train card. Clears its own area so it can be redrawn in place.
void ZectrixDashboardDrawTrains(ZectrixCanvas& canvas,
                                const zectrix_trains_data_t& trains);

// Events panel (placeholder until the calendar is connected). Leaves its
// lower part free when the rain chart is drawn there.
void ZectrixDashboardDrawEvents(ZectrixCanvas& canvas,
                                bool leave_room_for_rain = false);

// True when any hour today has a rain probability above 0%.
bool ZectrixDashboardHasRain(const zectrix_weather_data_t* weather);

// Hourly rain-probability chart in the lower part of the events panel.
// Hours before current_hour are drawn lighter; pass -1 when unknown.
void ZectrixDashboardDrawRainChart(ZectrixCanvas& canvas,
                                   const zectrix_weather_data_t& weather,
                                   int current_hour = -1);

// Clock in the top-right corner. Clears its own area; text such as "10:42".
void ZectrixDashboardDrawClock(ZectrixCanvas& canvas, const char* text);

// Clears the canvas and draws the whole dashboard.
void ZectrixDashboardDrawAll(ZectrixCanvas& canvas,
                             const zectrix_weather_data_t* weather,
                             const zectrix_trains_data_t& trains,
                             const char* clock_text);

#endif  // ZECTRIX_DASHBOARD_VIEW_H_
