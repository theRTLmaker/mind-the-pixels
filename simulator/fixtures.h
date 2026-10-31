#ifndef SIMULATOR_FIXTURES_H_
#define SIMULATOR_FIXTURES_H_

// Sample data for the Mac simulator. Add a scenario here to preview a new
// weather or train condition without waiting for the live APIs.

#include <cstdio>
#include <vector>

#include "zectrix_dashboard_events.h"
#include "zectrix_dashboard_trains.h"
#include "zectrix_dashboard_view.h"
#include "zectrix_dashboard_weather.h"

struct Scenario {
    const char* name;
    const char* description;
    bool weather_ok;
    zectrix_weather_data_t weather;
    zectrix_trains_data_t trains;
    const char* clock;
    const zectrix_events_data_t* events = nullptr;
    ZectrixDashboardBattery battery = {true, 72, false, false};
    bool night = false;
};

inline zectrix_weather_data_t Weather(int code, float temp, float feels,
                                      float low, float high, int rain,
                                      int humidity, float wind) {
    zectrix_weather_data_t w = {};
    w.temperature_c = temp;
    w.feels_like_c = feels;
    w.wind_speed_kmh = wind;
    w.min_temperature_c = low;
    w.max_temperature_c = high;
    w.humidity_percent = humidity;
    w.rain_probability_percent = rain;
    w.weather_code = code;
    w.daily_weather_code = code;
    // A smooth daily profile peaking mid-afternoon at the daily maximum.
    for (int hour = 0; hour < ZECTRIX_WEATHER_HOURS; ++hour) {
        const int distance = hour > 15 ? hour - 15 : 15 - hour;
        const int value = rain - distance * rain / 8;
        w.hourly_rain_percent[hour] = value > 0 ? value : 0;
    }
    return w;
}

// Replaces the generated hourly rain profile with real-looking values.
inline zectrix_weather_data_t WithHourlyRain(zectrix_weather_data_t w,
                                             std::vector<int> hourly) {
    for (int hour = 0; hour < ZECTRIX_WEATHER_HOURS; ++hour) {
        w.hourly_rain_percent[hour] =
            hour < static_cast<int>(hourly.size()) ? hourly[hour] : 0;
    }
    return w;
}

inline zectrix_train_t Train(const char* scheduled, const char* expected,
                             bool cancelled = false,
                             int loading_percent = -1) {
    zectrix_train_t t = {};
    for (int i = 0; i < 5; ++i) {
        t.scheduled[i] = scheduled[i];
        t.expected[i] = expected[i];
    }
    t.cancelled = cancelled;
    t.loading_percent = loading_percent;
    return t;
}

inline zectrix_trains_data_t Trains(std::vector<zectrix_train_t> list,
                                    const char* updated = "09:57",
                                    int failed_fetches = 0) {
    zectrix_trains_data_t data = {};
    data.valid = true;
    for (int i = 0; i < 5 && updated[i] != '\0'; ++i) {
        data.updated[i] = updated[i];
    }
    data.failed_fetches = failed_fetches;
    for (const zectrix_train_t& t : list) {
        if (data.count < ZECTRIX_TRAIN_COUNT) {
            data.trains[data.count++] = t;
        }
    }
    return data;
}

inline const zectrix_trains_data_t kOnTime = Trains({
    Train("10:01", "10:01", false, 12), Train("10:11", "10:11", false, 48),
    Train("10:21", "10:21", false, 91)});
inline const zectrix_trains_data_t kMixed = Trains({
    Train("10:01", "10:03", false, 85), Train("10:11", "-----", false, 40),
    Train("10:21", "10:21", true, 20)});
inline const zectrix_trains_data_t kLoading = {};
inline const zectrix_trains_data_t kUnavailable = [] {
    zectrix_trains_data_t data = {};
    data.failed_fetches = 4;
    return data;
}();
inline const zectrix_trains_data_t kStale = Trains({
    Train("10:01", "10:01"), Train("10:11", "10:14"),
    Train("10:21", "10:21")}, "09:51", 7);
inline const zectrix_trains_data_t kOneTrain = Trains({
    Train("23:41", "23:41", false, 7)});

inline zectrix_event_line_t Pick(const char* when, const char* text,
                                 const char* tag = "") {
    zectrix_event_line_t line = {};
    std::snprintf(line.when, sizeof(line.when), "%s", when);
    std::snprintf(line.text, sizeof(line.text), "%s", text);
    std::snprintf(line.tag, sizeof(line.tag), "%s", tag);
    return line;
}

inline zectrix_events_data_t Picks(std::vector<zectrix_event_line_t> lines) {
    zectrix_events_data_t data = {};
    for (const zectrix_event_line_t& line : lines) {
        if (data.count < ZECTRIX_EVENT_LINES) {
            data.lines[data.count++] = line;
        }
    }
    data.valid = data.count > 0;
    return data;
}

inline const zectrix_events_data_t kPicks = Picks({
    Pick("Fri 30", "Free jazz, Oxford House E2", "FREE"),
    Pick("Sat-Sun", "East Asian Market, Canary Wharf"),
    Pick("Fri 6 Nov", "Rachmaninoff 2, Barbican", "BOOK"),
    Pick("13-22 Nov", "London Jazz Festival, from \u00a3" "5", "BOOK")});
inline const zectrix_events_data_t kLongPicks = Picks({
    Pick("Tonight", "Sample musical day seats \u00a3" "25, a very long venue name", "DEAL"),
    Pick("Wednesday 4 Nov", "Sample fado night with a long artist and venue name"),
    Pick("to 12 Apr", "Sample exhibition with a long title here", "BOOK")});

inline const std::vector<Scenario>& Scenarios() {
    static const std::vector<Scenario> scenarios = {
        {"sunny", "Clear sky, trains on time", true,
         Weather(0, 18.4f, 18.0f, 11, 21, 0, 52, 9), kOnTime, "10:42", &kPicks},
        {"partly-cloudy", "Weather code 2, one late and one cancelled", true,
         Weather(2, 12.3f, 10.8f, 9, 14, 20, 71, 14), kMixed, "09:58", &kPicks},
        {"overcast", "Weather code 3", true,
         Weather(3, 11.0f, 9.6f, 8, 12, 35, 80, 18), kOnTime, "13:07", &kPicks},
        {"fog", "Weather code 45", true,
         Weather(45, 6.2f, 4.1f, 4, 9, 10, 96, 4), kOnTime, "07:15", &kPicks},
        {"rain", "Weather code 61, disrupted trains", true,
         WithHourlyRain(Weather(61, 9.7f, 7.2f, 7, 11, 90, 93, 24),
                        {5, 5, 3, 0, 0, 0, 10, 35, 60, 45, 20, 10,
                         15, 40, 75, 90, 85, 60, 30, 15, 10, 5, 0, 0}),
         kMixed, "17:32", &kPicks},
        {"drizzle", "Weather code 53", true,
         Weather(53, 10.4f, 8.9f, 8, 12, 60, 90, 12), kOnTime, "11:26", &kPicks},
        {"snow", "Weather code 71", true,
         Weather(71, -1.5f, -5.8f, -3, 1, 70, 88, 16), kOnTime, "08:04", &kPicks},
        {"storm", "Weather code 95", true,
         Weather(95, 14.9f, 13.0f, 12, 17, 100, 85, 41), kMixed, "19:20", &kPicks,
         {true, 8, false, false}},
        {"late-night", "Only one departure left", true,
         Weather(1, 7.0f, 5.5f, 5, 12, 0, 77, 6), kOneTrain, "23:30", &kPicks,
         {true, 55, true, false}},
        {"trains-stale", "Train fetches failing for 7 min, cached times", true,
         Weather(3, 11.0f, 9.6f, 8, 12, 35, 80, 18), kStale, "09:58", &kPicks},
        {"trains-unavailable", "Trains never loaded, still retrying", true,
         Weather(2, 12.3f, 10.8f, 9, 14, 20, 71, 14), kUnavailable, "09:58", &kPicks},
        {"events-long", "Long picks are cut with ... before the tag", true,
         Weather(3, 11.0f, 9.6f, 8, 12, 35, 80, 18), kOnTime, "18:05",
         &kLongPicks},
        {"night", "22:00-05:00 screen, weather refreshed hourly", true,
         Weather(3, 11.0f, 9.6f, 8, 12, 35, 80, 18), kOnTime, "02:00",
         &kPicks, {true, 64, false, false}, true},
        {"offline", "No Wi-Fi: weather failed, trains still loading", false,
         {}, kLoading, "--:--"},
    };
    return scenarios;
}

#endif  // SIMULATOR_FIXTURES_H_
