// Mind the Pixels: a personal London dashboard for the ZECTRIX NOTE4.
//
// Three screens, switched with UP/DOWN: the dashboard (weather, trains to
// St Pancras, curated picks), the full "Coming up" list, and Margarida's
// commute to Cambridge. OK refreshes the current screen; holding DOWN
// switches the board off. From 22:00 to 05:00 the board shows a night
// screen and deep-sleeps, waking hourly to refresh the weather.

#include <sys/time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtc_pcf8563.h"
#include "zectrix_board.h"
#include "zectrix_board_config.h"
#include "zectrix_dashboard_commute.h"
#include "zectrix_dashboard_events.h"
#include "zectrix_dashboard_trains.h"
#include "zectrix_dashboard_view.h"
#include "zectrix_dashboard_weather.h"
#include "zectrix_dashboard_wifi.h"
#include "zectrix_epd.h"
#include "zectrix_panel.h"

namespace {

constexpr char kTag[] = "mind_the_pixels";

enum class Screen : int {
    kDashboard = 0,
    kEvents,
    kCommute,
    kCount,
};

// Kept in RTC memory across the overnight deep sleeps: the last good
// weather and picks (so a failed hourly fetch leaves the night screen
// unchanged) and the screen to return to in the morning.
RTC_DATA_ATTR zectrix_weather_data_t g_night_weather;
RTC_DATA_ATTR bool g_night_weather_ok;
RTC_DATA_ATTR zectrix_events_data_t g_night_events;
RTC_DATA_ATTR bool g_night_events_ok;
RTC_DATA_ATTR int g_night_screen;

// UTC calendar time to time_t without touching TZ (days-from-civil).
time_t UtcToTime(const tm& utc) {
    int year = utc.tm_year + 1900;
    const int month = utc.tm_mon + 1;
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int year_of_era = year - era * 400;
    const int day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + utc.tm_mday - 1;
    const int day_of_era = year_of_era * 365 + year_of_era / 4 -
                           year_of_era / 100 + day_of_year;
    const int64_t days = era * 146097LL + day_of_era - 719468;
    return static_cast<time_t>(days * 86400 + utc.tm_hour * 3600 +
                               utc.tm_min * 60 + utc.tm_sec);
}

void StartClockSync() {
    static bool started = false;

    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
    tzset();

    if (!started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        started = true;
    }
}

zectrix_epd_rect_t ToEpdRect(const ZectrixDashboardRect& rect) {
    return {rect.x, rect.y, rect.width, rect.height};
}

class MindThePixels {
public:
    void Run() {
        esp_err_t err = board_.Init();
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "board initialization failed: %s",
                     esp_err_to_name(err));
        }

        zectrix_epd_config_t config = {};
        zectrix_epd_get_default_config(&config);
        err = zectrix_epd_new(&config, &epd_);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "EPD initialization failed: %s",
                     esp_err_to_name(err));
            return;
        }
        panel_.SetEpd(epd_);

        // Waking from the overnight sleep: an hourly alarm (or its backup
        // timer) refreshes the night screen and sleeps again; the OK button
        // or the 05:00 alarm returns to the screens. No splash either way.
        const esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
        const bool night_wake = wake == ESP_SLEEP_WAKEUP_EXT1 ||
                                wake == ESP_SLEEP_WAKEUP_TIMER;
        bool button_wake = false;
        if (night_wake) {
            button_wake = wake == ESP_SLEEP_WAKEUP_EXT1 &&
                          (esp_sleep_get_ext1_wakeup_status() &
                           (1ULL << ZECTRIX_BUTTON_OK)) != 0;
            RestoreTimeFromRtc();
            if (board_.rtc() != nullptr) {
                board_.rtc()->DisableAlarm();
                board_.rtc()->ClearAlarmFlag();
            }
            ESP_LOGI(kTag, "woke from night sleep (cause %d)",
                     static_cast<int>(wake));
        } else {
            ZectrixDashboardDrawSplash(panel_.canvas(),
                                       "Connecting to Wi-Fi...");
            ESP_ERROR_CHECK(panel_.RefreshFull());
        }

        const esp_err_t wifi_err = zectrix_dashboard_wifi_connect();
        if (wifi_err != ESP_OK) {
            ESP_LOGW(kTag, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_err));
        }
        StartClockSync();

        if (night_wake) {
            const time_t now = time(nullptr);
            tm local_time = {};
            localtime_r(&now, &local_time);
            if (!button_wake && now >= 1700000000 && IsNight(local_time)) {
                NightRefresh();
            }
            if (g_night_screen >= 0 &&
                g_night_screen < static_cast<int>(Screen::kCount)) {
                screen_ = static_cast<Screen>(g_night_screen);
            }
        }

        ESP_ERROR_CHECK(zectrix_dashboard_trains_start());
        FetchWeatherAndEvents();
        // Ignore the release of a button press that woke the board.
        board_.DrainButtons();
        last_input_us_ = esp_timer_get_time();
        ShowScreen();
        Loop();
    }

private:
    // Weather and picks are refetched this often on every screen.
    static constexpr int64_t kSlowRefreshUs = 15LL * 60 * 1000 * 1000;
    // Commute boards are refetched this often while that screen is shown.
    static constexpr int64_t kCommuteRefreshUs = 2LL * 60 * 1000 * 1000;

    // Overnight deep sleep from kNightStartHour to kNightEndHour local
    // time, once no button has been pressed for kNightAwakeUs.
    static constexpr int kNightStartHour = 22;
    static constexpr int kNightEndHour = 5;
    static constexpr int64_t kNightAwakeUs = 10LL * 60 * 1000 * 1000;

    static bool IsNight(const tm& local_time) {
        return local_time.tm_hour >= kNightStartHour ||
               local_time.tm_hour < kNightEndHour;
    }

    [[noreturn]] void Loop() {
        while (true) {
            ZectrixButtonEvent event;
            if (board_.WaitButton(&event, pdMS_TO_TICKS(1000))) {
                HandleButton(event);
            }

            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_slow_fetch_us_ >= kSlowRefreshUs &&
                FetchWeatherAndEvents()) {
                // Weather and picks sit in gray areas on every screen.
                ShowScreen();
            }

            if (screen_ == Screen::kDashboard) {
                UpdateDashboardTrains();
            } else if (screen_ == Screen::kCommute &&
                       now_us - last_commute_fetch_us_ >= kCommuteRefreshUs &&
                       FetchCommute()) {
                DrawScreen();
                RefreshPartial(kCommuteHeaderRefreshRect);
                RefreshPartial(kCommuteRowsRefreshRect);
            }

            // Wait until SNTP (or the RTC) has supplied a plausible time.
            const time_t now = time(nullptr);
            if (now < 1700000000) {
                continue;
            }
            tm local_time = {};
            localtime_r(&now, &local_time);

            if (IsNight(local_time) && now_us - last_input_us_ >= kNightAwakeUs) {
                g_night_weather = weather_;
                g_night_weather_ok = weather_ok_;
                g_night_events = events_;
                g_night_events_ok = events_ok_;
                g_night_screen = static_cast<int>(screen_);
                NightSleep(now);
            }

            if (local_time.tm_min != last_minute_) {
                last_minute_ = local_time.tm_min;
                std::snprintf(clock_text_, sizeof(clock_text_), "%02d:%02d",
                              local_time.tm_hour, local_time.tm_min);
                UpdateClock();
            }
        }
    }

    void HandleButton(const ZectrixButtonEvent& event) {
        last_input_us_ = esp_timer_get_time();
        if (event.button == ZectrixButton::kDown &&
            event.action == ZectrixButtonAction::kLongPress) {
            Shutdown();
        }
        if (event.action != ZectrixButtonAction::kClick) {
            return;
        }
        constexpr int kCount = static_cast<int>(Screen::kCount);
        int index = static_cast<int>(screen_);
        if (event.button == ZectrixButton::kDown) {
            index = (index + 1) % kCount;
        } else if (event.button == ZectrixButton::kUp) {
            index = (index + kCount - 1) % kCount;
        } else {
            // OK: refresh the current screen with fresh commute times.
            last_commute_fetch_us_ = INT64_MIN / 2;
        }
        screen_ = static_cast<Screen>(index);
        ShowScreen();
    }

    // Draws the current screen into the canvas from the cached data.
    void DrawScreen() {
        ZectrixCanvas& canvas = panel_.canvas();
        const zectrix_weather_data_t* weather =
            weather_ok_ ? &weather_ : nullptr;
        const zectrix_events_data_t* events = events_ok_ ? &events_ : nullptr;
        switch (screen_) {
            case Screen::kDashboard:
                ZectrixDashboardDrawAll(canvas, weather, displayed_trains_,
                                        events, &battery_, clock_text_);
                break;
            case Screen::kEvents:
                ZectrixDashboardDrawEventsFull(canvas, events, &battery_,
                                               clock_text_);
                break;
            case Screen::kCommute:
                ZectrixDashboardDrawCommute(canvas, weather,
                                            commute_ok_ ? &commute_ : nullptr,
                                            &battery_, clock_text_);
                break;
            case Screen::kCount:
                break;
        }
    }

    // Full 16-gray redraw of the current screen with fresh data.
    void ShowScreen() {
        battery_ = ReadBattery();
        if (screen_ == Screen::kDashboard) {
            zectrix_dashboard_trains_get(&displayed_trains_);
        } else if (screen_ == Screen::kCommute &&
                   esp_timer_get_time() - last_commute_fetch_us_ >=
                       kCommuteRefreshUs) {
            FetchCommute();
        }
        DrawScreen();
        const esp_err_t err = panel_.RefreshFullGray();
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "full refresh failed: %s", esp_err_to_name(err));
        }
    }

    void RefreshPartial(const ZectrixDashboardRect& rect) {
        const esp_err_t err = panel_.RefreshPartial(ToEpdRect(rect));
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "partial refresh failed: %s", esp_err_to_name(err));
        }
    }

    void UpdateClock() {
        battery_ = ReadBattery();
        switch (screen_) {
            case Screen::kDashboard:
                ZectrixDashboardDrawClock(panel_.canvas(), clock_text_,
                                          &battery_);
                RefreshPartial(kDashboardClockRefreshRect);
                break;
            case Screen::kEvents:
                DrawScreen();
                RefreshPartial(kEventsClockRefreshRect);
                break;
            case Screen::kCommute:
                DrawScreen();
                RefreshPartial(kCommuteHeaderRefreshRect);
                break;
            case Screen::kCount:
                break;
        }
    }

    // The train card redraws on any change, including failures while
    // still invalid, so it can show "No train data".
    void UpdateDashboardTrains() {
        zectrix_trains_data_t current = {};
        zectrix_dashboard_trains_get(&current);
        if (std::memcmp(&current, &displayed_trains_, sizeof(current)) == 0) {
            return;
        }
        displayed_trains_ = current;
        ZectrixDashboardDrawTrains(panel_.canvas(), displayed_trains_);
        RefreshPartial(kDashboardTrainsRefreshRect);
    }

    // Returns true when the weather or the picks changed. On failure the
    // last good data stays.
    bool FetchWeatherAndEvents() {
        last_slow_fetch_us_ = esp_timer_get_time();
        bool changed = false;
        zectrix_weather_data_t weather = {};
        if (zectrix_dashboard_weather_fetch(&weather) == ESP_OK &&
            (!weather_ok_ ||
             std::memcmp(&weather, &weather_, sizeof(weather)) != 0)) {
            weather_ = weather;
            weather_ok_ = true;
            changed = true;
        }
        zectrix_events_data_t events = {};
        if (zectrix_dashboard_events_fetch(&events) == ESP_OK &&
            (!events_ok_ ||
             std::memcmp(&events, &events_, sizeof(events)) != 0)) {
            events_ = events;
            events_ok_ = true;
            changed = true;
        }
        return changed;
    }

    // Returns true when the journeys changed.
    bool FetchCommute() {
        last_commute_fetch_us_ = esp_timer_get_time();
        zectrix_commute_data_t commute = {};
        const esp_err_t err = zectrix_dashboard_commute_fetch(&commute);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            return false;
        }
        if (commute_ok_ &&
            std::memcmp(&commute, &commute_, sizeof(commute)) == 0) {
            return false;
        }
        commute_ = commute;
        commute_ok_ = true;
        return true;
    }

    ZectrixDashboardBattery ReadBattery() {
        const ZectrixPowerSnapshot power = board_.ReadPowerSnapshot();
        return {power.battery_valid, power.battery_percent,
                power.charge.charging, power.charge.full};
    }

    // The PCF8563 keeps crystal time (UTC) across deep sleep; the ESP32's
    // own RC clock can drift by minutes overnight.
    void RestoreTimeFromRtc() {
        RtcPcf8563* rtc = board_.rtc();
        tm utc = {};
        if (rtc == nullptr || !rtc->GetTime(utc)) {
            return;
        }
        const time_t now = UtcToTime(utc);
        if (now < 1700000000) {
            return;
        }
        const timeval tv = {now, 0};
        settimeofday(&tv, nullptr);
    }

    // Hourly night wake: fetch the weather and picks (keeping the last good
    // ones on failure), redraw the night screen and sleep again.
    [[noreturn]] void NightRefresh() {
        zectrix_weather_data_t weather = {};
        if (zectrix_dashboard_weather_fetch(&weather) == ESP_OK) {
            g_night_weather = weather;
            g_night_weather_ok = true;
        }
        zectrix_events_data_t events = {};
        if (zectrix_dashboard_events_fetch(&events) == ESP_OK) {
            g_night_events = events;
            g_night_events_ok = true;
        }
        NightSleep(time(nullptr));
    }

    // Draws the night screen (weather, picks, battery) and deep-sleeps
    // until the next full hour, or the end of the night. The PCF8563 alarm
    // (crystal, accurate) wakes the board on RTC_INT; a backup timer on the
    // RC clock follows 10 minutes later; the OK button wakes it any time.
    [[noreturn]] void NightSleep(time_t now) {
        tm end_local = {};
        localtime_r(&now, &end_local);
        if (end_local.tm_hour >= kNightEndHour) {
            ++end_local.tm_mday;
        }
        end_local.tm_hour = kNightEndHour;
        end_local.tm_min = 0;
        end_local.tm_sec = 0;
        end_local.tm_isdst = -1;
        const time_t night_end = mktime(&end_local);
        const time_t next_hour = now - now % 3600 + 3600;
        const time_t wake_at = std::min(next_hour, night_end);
        const int64_t sleep_s = std::max<int64_t>(60, wake_at - now);

        tm local_now = {};
        localtime_r(&now, &local_now);
        char updated[8];
        std::snprintf(updated, sizeof(updated), "%02d:%02d",
                      local_now.tm_hour, local_now.tm_min);
        char wake_text[24];
        std::snprintf(wake_text, sizeof(wake_text), "Back at %02d:00",
                      kNightEndHour);
        const ZectrixDashboardBattery battery = ReadBattery();
        ZectrixDashboardDrawNight(
            panel_.canvas(), g_night_weather_ok ? &g_night_weather : nullptr,
            g_night_events_ok ? &g_night_events : nullptr, &battery,
            wake_text, updated);
        const esp_err_t refresh = panel_.RefreshFullGray();
        if (refresh != ESP_OK) {
            ESP_LOGW(kTag, "night screen refresh failed: %s",
                     esp_err_to_name(refresh));
        }
        zectrix_epd_power_off(epd_);
        board_.SetPowerLed(false);
        board_.SetAudioPower(false);

        // The PCF8563 runs in UTC so the alarm ignores daylight saving.
        uint64_t wake_pins = 1ULL << ZECTRIX_BUTTON_OK;
        RtcPcf8563* rtc = board_.rtc();
        if (rtc != nullptr) {
            tm utc_now = {};
            tm utc_wake = {};
            gmtime_r(&now, &utc_now);
            gmtime_r(&wake_at, &utc_wake);
            if (rtc->SetTime(utc_now) && rtc->SetAlarm(utc_wake)) {
                wake_pins |= 1ULL << ZECTRIX_RTC_INT;
                rtc_gpio_pullup_en(ZECTRIX_RTC_INT);
                rtc_gpio_pulldown_dis(ZECTRIX_RTC_INT);
            } else {
                ESP_LOGW(kTag, "RTC alarm unavailable; using the timer only");
            }
        }
        rtc_gpio_pullup_en(ZECTRIX_BUTTON_OK);
        rtc_gpio_pulldown_dis(ZECTRIX_BUTTON_OK);
        // RTC pull-ups only hold while the RTC peripherals stay powered.
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_sleep_enable_ext1_wakeup(wake_pins, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_sleep_enable_timer_wakeup(
            static_cast<uint64_t>(sleep_s + 10 * 60) * 1000000ULL);

        // Keep the battery latch (and other held pins) on while asleep.
        gpio_deep_sleep_hold_en();
        ESP_LOGI(kTag, "night sleep for %lld s",
                 static_cast<long long>(sleep_s));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_deep_sleep_start();
    }

    // Clears the panel and releases the battery latch; on USB power the
    // board falls back to deep sleep.
    [[noreturn]] void Shutdown() {
        ESP_LOGI(kTag, "clearing display before shutdown");
        const esp_err_t clear = panel_.Clear();
        if (clear != ESP_OK) {
            ESP_LOGW(kTag, "display clear failed: %s", esp_err_to_name(clear));
        }
        board_.SetPowerLed(false);
        board_.SetAudioPower(false);
        ESP_LOGI(kTag, "cutting battery latch");
        vTaskDelay(pdMS_TO_TICKS(100));
        board_.CutBatteryPower();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_deep_sleep_start();
    }

    ZectrixBoard board_;
    zectrix_epd_handle_t epd_ = nullptr;
    ZectrixPanel panel_;
    Screen screen_ = Screen::kDashboard;

    zectrix_weather_data_t weather_ = {};
    bool weather_ok_ = false;
    zectrix_events_data_t events_ = {};
    bool events_ok_ = false;
    zectrix_trains_data_t displayed_trains_ = {};
    zectrix_commute_data_t commute_ = {};
    bool commute_ok_ = false;
    ZectrixDashboardBattery battery_ = {};

    char clock_text_[8] = "--:--";
    int last_minute_ = -1;
    int64_t last_input_us_ = 0;
    int64_t last_slow_fetch_us_ = 0;
    int64_t last_commute_fetch_us_ = INT64_MIN / 2;
};

}  // namespace

extern "C" void app_main(void) {
    static MindThePixels app;
    app.Run();
}
