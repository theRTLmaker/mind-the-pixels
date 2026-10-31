#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <ctime>
#include <cstdlib>
#include "esp_sntp.h"

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <sys/time.h>
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zectrix_board.h"
#include "zectrix_board_config.h"
#include "rtc_pcf8563.h"
#include "zectrix_demo_ui.h"
#include "zectrix_epd.h"
#include "zectrix_self_test.h"
#include "zectrix_dashboard_wifi.h"
#include "zectrix_dashboard_weather.h"
#include "zectrix_dashboard_trains.h"
#include "zectrix_dashboard_events.h"
#include "zectrix_dashboard_view.h"

extern "C" {
extern const uint8_t kLighthouse1bppStart[]
    asm("_binary_lighthouse_400x300_1bpp_bin_start");
extern const uint8_t kLighthouse1bppEnd[]
    asm("_binary_lighthouse_400x300_1bpp_bin_end");
extern const uint8_t kSnowPath1bppStart[]
    asm("_binary_snow_path_400x300_1bpp_bin_start");
extern const uint8_t kSnowPath1bppEnd[]
    asm("_binary_snow_path_400x300_1bpp_bin_end");
extern const uint8_t kFootprintAnimationStart[]
    asm("_binary_footprint_animation_bin_start");
extern const uint8_t kFootprintAnimationEnd[]
    asm("_binary_footprint_animation_bin_end");
extern const uint8_t kMountain4bppStart[]
    asm("_binary_mountain_400x300_4bpp_bin_start");
extern const uint8_t kMountain4bppEnd[]
    asm("_binary_mountain_400x300_4bpp_bin_end");
}

namespace {

constexpr char kTag[] = "epd_showcase";
constexpr uint8_t kFootprintMagic[] = {'Z', 'F', 'P', '1'};
constexpr size_t kAnimationHeaderSize = 8;
constexpr size_t kStepHeaderSize = 12;
constexpr TickType_t kHomeIdleTimeout = pdMS_TO_TICKS(15000);

enum class ControlResult {
    kContinue,
    kSelected,
    kBack,
    kShutdown,
};

struct SceneResult {
    esp_err_t error = ESP_OK;
    int64_t elapsed_ms = 0;
    ControlResult control = ControlResult::kContinue;
};

uint16_t ReadLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1] << 8);
}

// Last good weather and picks, kept in RTC memory across the overnight
// deep sleeps so a failed hourly fetch leaves the night screen unchanged.
RTC_DATA_ATTR zectrix_weather_data_t g_night_weather;
RTC_DATA_ATTR bool g_night_weather_ok;
RTC_DATA_ATTR zectrix_events_data_t g_night_events;
RTC_DATA_ATTR bool g_night_events_ok;

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

static void StartDashboardClockSync() {
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

class DemoApp {
public:
    DemoApp() : ui_(nullptr), tests_(&board_) {
        test_states_.fill(ZectrixTestState::kWait);
    }

    void Run() {
        esp_err_t err = board_.Init();
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "board initialization failed: %s", esp_err_to_name(err));
        }

        zectrix_epd_config_t config = {};
        zectrix_epd_get_default_config(&config);
        err = zectrix_epd_new(&config, &epd_);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "EPD initialization failed: %s", esp_err_to_name(err));
            return;
        }
        ui_.SetEpd(epd_);

        // Waking from the overnight sleep: an hourly alarm (or its backup
        // timer) refreshes the night screen and sleeps again; the OK button
        // or the 05:00 alarm goes straight to the dashboard. No splash.
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
            ESP_ERROR_CHECK(ui_.ShowSplash());
            Wait(pdMS_TO_TICKS(1500), false);
        }

        const esp_err_t wifi_err = zectrix_dashboard_wifi_connect();
        if (wifi_err != ESP_OK) {
            ESP_LOGW(kTag, "Dashboard Wi-Fi unavailable: %s",
                     esp_err_to_name(wifi_err));
        }

        if (night_wake) {
            StartDashboardClockSync();
            const time_t now = time(nullptr);
            tm local_time = {};
            localtime_r(&now, &local_time);
            if (!button_wake && now >= 1700000000 && IsNight(local_time)) {
                NightRefresh();
            }
            // Ignore the release of the OK press that woke the board.
            board_.DrainButtons();
            const ControlResult result = RunDashboard();
            ui_.EndGrayMode();
            if (result == ControlResult::kShutdown) {
                Shutdown();
            }
        }

        while (true) {
            const ControlResult result = RunHome();
            if (result == ControlResult::kShutdown) {
                Shutdown();
            }
        }
    }

private:
    ControlResult Wait(TickType_t duration, bool any_click_returns) {
        const TickType_t start = xTaskGetTickCount();
        while (xTaskGetTickCount() - start < duration) {
            ZectrixButtonEvent event;
            const TickType_t elapsed = xTaskGetTickCount() - start;
            const TickType_t remaining = duration > elapsed ? duration - elapsed : 0;
            if (!board_.WaitButton(&event,
                                   std::min(remaining, pdMS_TO_TICKS(100)))) {
                continue;
            }
            if (event.button == ZectrixButton::kDown &&
                event.action == ZectrixButtonAction::kLongPress) {
                return ControlResult::kShutdown;
            }
            if (event.button == ZectrixButton::kOk &&
                event.action == ZectrixButtonAction::kLongPress) {
                return ControlResult::kBack;
            }
            if (any_click_returns &&
                event.action == ZectrixButtonAction::kClick) {
                return ControlResult::kBack;
            }
        }
        return ControlResult::kContinue;
    }

    ControlResult RunMenu(const char* title, const char* const* items,
                          size_t count, const char* footer,
                          size_t* selected, TickType_t idle_timeout = portMAX_DELAY) {
        ESP_ERROR_CHECK(ui_.ShowMenu(title, items, count, *selected, footer, true));
        while (true) {
            ZectrixButtonEvent event;
            if (!board_.WaitButton(&event, idle_timeout)) {
                return ControlResult::kContinue;
            }
            if (event.button == ZectrixButton::kDown &&
                event.action == ZectrixButtonAction::kLongPress) {
                return ControlResult::kShutdown;
            }
            if (event.button == ZectrixButton::kOk &&
                event.action == ZectrixButtonAction::kLongPress) {
                return ControlResult::kBack;
            }
            if (event.action != ZectrixButtonAction::kClick) {
                continue;
            }
            if (event.button == ZectrixButton::kUp) {
                *selected = (*selected + count - 1) % count;
                ESP_ERROR_CHECK(ui_.ShowMenu(title, items, count, *selected,
                                             footer, false));
            } else if (event.button == ZectrixButton::kDown) {
                *selected = (*selected + 1) % count;
                ESP_ERROR_CHECK(ui_.ShowMenu(title, items, count, *selected,
                                             footer, false));
            } else if (event.button == ZectrixButton::kOk) {
                return ControlResult::kSelected;
            }
        }
    }

    ControlResult RunHome() {
        static constexpr const char* kItems[] = {
            "DASHBOARD",
            "AUTO SHOWCASE",
            "DISPLAY GALLERY",
            "HARDWARE TESTS",
            "DEVICE INFO",
            "ABOUT & LICENSE"
        };
        size_t selected = 0;
        while (true) {
            const ControlResult menu = RunMenu(
                "ZECTRIX | HARDWARE SHOWCASE", kItems, std::size(kItems),
                "UP/DOWN Move  OK Select  Hold DOWN Off", &selected,
                kHomeIdleTimeout);
            if (menu == ControlResult::kShutdown) {
                return menu;
            }
            if (menu == ControlResult::kBack) {
                continue;
            }
            if (menu == ControlResult::kContinue) {
                selected = 0;
            }
            // A timeout intentionally starts the auto showcase.
            if (menu == ControlResult::kContinue) {
                selected = 1;
            }

            if (selected == 0) {
                const ControlResult result = RunDashboard();
                ui_.EndGrayMode();
                if (result == ControlResult::kShutdown) return result;
            } else if (selected == 1) {
                const ControlResult result = RunAutoShowcase();
                if (result == ControlResult::kShutdown) return result;
            } else if (selected == 2) {
                const ControlResult result = RunDisplayGallery();
                if (result == ControlResult::kShutdown) return result;
            } else if (selected == 3) {
                const ControlResult result = RunHardwareTests();
                if (result == ControlResult::kShutdown) return result;
            } else if (selected == 4) {
                const ControlResult result = RunDeviceInfo();
                if (result == ControlResult::kShutdown) return result;
            } else if (selected == 5) {
                ESP_ERROR_CHECK(ui_.ShowAbout());
                const ControlResult result = Wait(portMAX_DELAY, false);
                if (result == ControlResult::kShutdown) return result;
            }
        }
    }

    SceneResult RunLighthouse() {
        SceneResult result;
        const size_t size = static_cast<size_t>(kLighthouse1bppEnd -
                                                kLighthouse1bppStart);
        if (size != ZECTRIX_EPD_1BPP_FRAME_BYTES) {
            result.error = ESP_ERR_INVALID_SIZE;
            return result;
        }
        const int64_t start = esp_timer_get_time();
        result.error = zectrix_epd_power_on(epd_);
        if (result.error == ESP_OK) {
            result.error = zectrix_epd_refresh_full_1bpp(
                epd_, kLighthouse1bppStart, size);
        }
        zectrix_epd_power_off(epd_);
        result.elapsed_ms = (esp_timer_get_time() - start) / 1000;
        if (result.error == ESP_OK) {
            result.control = Wait(pdMS_TO_TICKS(2500), true);
        }
        return result;
    }

    SceneResult RunFootprints() {
        SceneResult result;
        const size_t background_size = static_cast<size_t>(
            kSnowPath1bppEnd - kSnowPath1bppStart);
        const size_t animation_size = static_cast<size_t>(
            kFootprintAnimationEnd - kFootprintAnimationStart);
        if (background_size != ZECTRIX_EPD_1BPP_FRAME_BYTES ||
            animation_size < kAnimationHeaderSize ||
            std::memcmp(kFootprintAnimationStart, kFootprintMagic,
                        sizeof(kFootprintMagic)) != 0) {
            result.error = ESP_ERR_INVALID_SIZE;
            return result;
        }

        const int64_t start = esp_timer_get_time();
        result.error = zectrix_epd_power_on(epd_);
        if (result.error == ESP_OK) {
            result.error = zectrix_epd_refresh_full_1bpp(
                epd_, kSnowPath1bppStart, background_size);
        }
        if (result.error == ESP_OK) {
            result.control = Wait(pdMS_TO_TICKS(900), true);
        }

        const uint16_t count = ReadLe16(kFootprintAnimationStart + 4);
        const uint8_t* cursor = kFootprintAnimationStart + kAnimationHeaderSize;
        for (uint16_t index = 0;
             result.error == ESP_OK &&
             result.control == ControlResult::kContinue && index < count;
             ++index) {
            if (static_cast<size_t>(kFootprintAnimationEnd - cursor) <
                kStepHeaderSize) {
                result.error = ESP_ERR_INVALID_SIZE;
                break;
            }
            const uint16_t x = ReadLe16(cursor);
            const uint16_t y = ReadLe16(cursor + 2);
            const uint16_t width = ReadLe16(cursor + 4);
            const uint16_t height = ReadLe16(cursor + 6);
            const uint16_t data_size = ReadLe16(cursor + 10);
            cursor += kStepHeaderSize;
            const size_t expected = static_cast<size_t>((width + 7) / 8) * height;
            if (data_size != expected ||
                data_size > static_cast<size_t>(kFootprintAnimationEnd - cursor)) {
                result.error = ESP_ERR_INVALID_SIZE;
                break;
            }
            const zectrix_epd_rect_t rect = {
                static_cast<int>(x), static_cast<int>(y),
                static_cast<int>(width), static_cast<int>(height)};
            result.error = zectrix_epd_refresh_partial_1bpp(
                epd_, &rect, cursor, data_size);
            cursor += data_size;
            if (result.error == ESP_OK) {
                result.control = Wait(pdMS_TO_TICKS(400), true);
            }
        }
        zectrix_epd_power_off(epd_);
        result.elapsed_ms = (esp_timer_get_time() - start) / 1000;
        if (result.error == ESP_OK &&
            result.control == ControlResult::kContinue) {
            result.control = Wait(pdMS_TO_TICKS(2200), true);
        }
        return result;
    }

    SceneResult RunMountain() {
        SceneResult result;
        const size_t size = static_cast<size_t>(kMountain4bppEnd -
                                                kMountain4bppStart);
        if (size != ZECTRIX_EPD_4BPP_FRAME_BYTES) {
            result.error = ESP_ERR_INVALID_SIZE;
            return result;
        }
        const int64_t start = esp_timer_get_time();
        // A true white 1bpp full refresh is deliberately performed before
        // every gray refresh to minimize visible history on this panel.
        result.error = ui_.ClearDisplay();
        if (result.error == ESP_OK) {
            result.error = zectrix_epd_power_on(epd_);
        }
        if (result.error == ESP_OK) {
            result.error = zectrix_epd_refresh_full_4bpp(
                epd_, kMountain4bppStart, size);
        }
        zectrix_epd_power_off(epd_);
        result.elapsed_ms = (esp_timer_get_time() - start) / 1000;
        if (result.error == ESP_OK) {
            result.control = Wait(pdMS_TO_TICKS(5000), true);
        }
        return result;
    }

    SceneResult RunScene(size_t scene) {
        ESP_LOGI(kTag, "running display scene %u", static_cast<unsigned>(scene));
        if (scene == 0) return RunLighthouse();
        if (scene == 1) return RunFootprints();
        return RunMountain();
    }

    static constexpr int64_t kDashboardWeatherRefreshUs =
        15LL * 60 * 1000 * 1000;

    // Overnight deep sleep: from kNightStartHour to kNightEndHour local
    // time. After a wake (OK button) or opening the dashboard at night, it
    // stays on for kNightAwakeUs before sleeping again.
    static constexpr int kNightStartHour = 22;
    static constexpr int kNightEndHour = 5;
    static constexpr int64_t kNightAwakeUs = 10LL * 60 * 1000 * 1000;

    static bool IsNight(const tm& local_time) {
        return local_time.tm_hour >= kNightStartHour ||
               local_time.tm_hour < kNightEndHour;
    }

    ZectrixDashboardBattery ReadDashboardBattery() {
        const ZectrixPowerSnapshot power = board_.ReadPowerSnapshot();
        return {power.battery_valid, power.battery_percent,
                power.charge.charging, power.charge.full};
    }

    ControlResult RunDashboard() {
        ZectrixCanvas& canvas = ui_.canvas();
        const int64_t dashboard_started_us = esp_timer_get_time();
        ZectrixDashboardBattery battery = ReadDashboardBattery();

        zectrix_weather_data_t weather = {};
        bool weather_ok = zectrix_dashboard_weather_fetch(&weather) == ESP_OK;
        int64_t last_weather_fetch_us = esp_timer_get_time();

        // Curated picks for the "Coming up" panel (events.json, updated
        // daily); fetched together with the weather.
        zectrix_events_data_t events = {};
        bool events_ok = zectrix_dashboard_events_fetch(&events) == ESP_OK;

        // Start background train updates.
        ESP_ERROR_CHECK(zectrix_dashboard_trains_start());

        zectrix_trains_data_t displayed_trains = {};

        // Layout and drawing live in zectrix_dashboard_view, shared with
        // the Mac simulator.
        char clock_text[8] = "--:--";
        ZectrixDashboardDrawAll(canvas, weather_ok ? &weather : nullptr,
                                displayed_trains,
                                events_ok ? &events : nullptr, &battery,
                                clock_text);

        // 16-gray frame; the clock and train card then use partial
        // refreshes on top of it (experimental, see zectrix_epd.h).
        ESP_ERROR_CHECK(ui_.RefreshFullGray());

        StartDashboardClockSync();

        int last_displayed_minute = -1;

        while (true) {
            ZectrixButtonEvent event;

            // Re-fetch the weather and events every 15 minutes. Those
            // panels contain gray, which partial refreshes can't draw, so a
            // change redraws everything with a gray full refresh. On
            // failure the last good data stays on screen.
            if (esp_timer_get_time() - last_weather_fetch_us >=
                kDashboardWeatherRefreshUs) {
                last_weather_fetch_us = esp_timer_get_time();
                bool changed = false;
                zectrix_weather_data_t fresh = {};
                if (zectrix_dashboard_weather_fetch(&fresh) == ESP_OK &&
                    (!weather_ok ||
                     std::memcmp(&fresh, &weather, sizeof(fresh)) != 0)) {
                    weather = fresh;
                    weather_ok = true;
                    changed = true;
                }
                zectrix_events_data_t fresh_events = {};
                if (zectrix_dashboard_events_fetch(&fresh_events) == ESP_OK &&
                    (!events_ok || std::memcmp(&fresh_events, &events,
                                               sizeof(fresh_events)) != 0)) {
                    events = fresh_events;
                    events_ok = true;
                    changed = true;
                }
                if (changed) {
                    ZectrixDashboardDrawAll(
                        canvas, weather_ok ? &weather : nullptr,
                        displayed_trains, events_ok ? &events : nullptr,
                        &battery, clock_text);
                    ESP_ERROR_CHECK(ui_.RefreshFullGray());
                }
            }

            zectrix_trains_data_t current_trains = {};

            // Redraw on any change, including failures while still
            // invalid, so the card can show "No train data".
            zectrix_dashboard_trains_get(&current_trains);
            if (std::memcmp(&current_trains, &displayed_trains,
                            sizeof(current_trains)) != 0) {
                displayed_trains = current_trains;
                ZectrixDashboardDrawTrains(canvas, displayed_trains);

                // Aligned partial-refresh region covering the train card.
                const zectrix_epd_rect_t trains_rect = {
                    kDashboardTrainsRefreshRect.x,
                    kDashboardTrainsRefreshRect.y,
                    kDashboardTrainsRefreshRect.width,
                    kDashboardTrainsRefreshRect.height};
                ESP_ERROR_CHECK(ui_.RefreshPartial(trains_rect));
            }

            // Check buttons once per second so the clock can update.
            const bool got_event =
                board_.WaitButton(&event, pdMS_TO_TICKS(1000));

            if (got_event) {
                if (event.button == ZectrixButton::kDown &&
                    event.action == ZectrixButtonAction::kLongPress) {
                    return ControlResult::kShutdown;
                }

                if (event.button == ZectrixButton::kOk &&
                    event.action == ZectrixButtonAction::kLongPress) {
                    return ControlResult::kBack;
                }

                if (event.action == ZectrixButtonAction::kClick) {
                    return ControlResult::kBack;
                }
            }

            // Wait until SNTP has supplied a plausible current time.
            const time_t now = time(nullptr);
            if (now < 1700000000) {
                continue;
            }

            struct tm local_time = {};
            localtime_r(&now, &local_time);

            if (IsNight(local_time) &&
                esp_timer_get_time() - dashboard_started_us >= kNightAwakeUs) {
                g_night_weather = weather;
                g_night_weather_ok = weather_ok;
                g_night_events = events;
                g_night_events_ok = events_ok;
                NightSleep(now);
            }

            // Refresh only when the displayed minute changes; the battery
            // icon shares the clock's partial-refresh window.
            if (local_time.tm_min != last_displayed_minute) {
                std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d",
                            local_time.tm_hour, local_time.tm_min);

                battery = ReadDashboardBattery();
                ZectrixDashboardDrawClock(canvas, clock_text, &battery);

                const zectrix_epd_rect_t clock_rect = {
                    kDashboardClockRefreshRect.x,
                    kDashboardClockRefreshRect.y,
                    kDashboardClockRefreshRect.width,
                    kDashboardClockRefreshRect.height};
                ESP_ERROR_CHECK(ui_.RefreshPartial(clock_rect));

                last_displayed_minute = local_time.tm_min;
            }
        }
    }

    ControlResult RunAutoShowcase() {
        while (true) {
            for (size_t scene = 0; scene < 3; ++scene) {
                const SceneResult result = RunScene(scene);
                if (result.error != ESP_OK) {
                    ESP_LOGE(kTag, "scene failed: %s",
                             esp_err_to_name(result.error));
                    return ControlResult::kBack;
                }
                if (result.control != ControlResult::kContinue) {
                    return result.control;
                }
            }
        }
    }

    ControlResult RunDisplayGallery() {
        static constexpr const char* kItems[] = {
            "FULL REFRESH / 1BPP", "PARTIAL / FOOTPRINTS",
            "16-GRAY / 4BPP", "RUN ALL SCENES"};
        static constexpr const char* kTitles[] = {
            "LIGHTHOUSE", "FOOTPRINTS", "MOUNTAIN LANDSCAPE"};
        static constexpr const char* kModes[] = {
            "FULL", "PARTIAL", "FULL + PRE-CLEAR"};
        static constexpr const char* kFormats[] = {
            "1BPP B/W", "1BPP B/W", "4BPP / 16 GRAY"};
        size_t selected = 0;
        while (true) {
            const ControlResult menu = RunMenu(
                "DISPLAY GALLERY", kItems, std::size(kItems),
                "UP/DOWN Move  OK Run  Hold OK Back", &selected);
            if (menu == ControlResult::kShutdown) return menu;
            if (menu == ControlResult::kBack) {
                return ControlResult::kBack;
            }
            if (selected == 3) {
                for (size_t scene = 0; scene < 3; ++scene) {
                    const SceneResult result = RunScene(scene);
                    if (result.control == ControlResult::kShutdown) return result.control;
                    if (result.control == ControlResult::kBack) break;
                }
                continue;
            }
            const SceneResult result = RunScene(selected);
            if (result.control == ControlResult::kShutdown) return result.control;
            ESP_ERROR_CHECK(ui_.ShowSceneInfo(
                kTitles[selected], kModes[selected], kFormats[selected],
                selected == 2 ? ZECTRIX_EPD_4BPP_FRAME_BYTES
                              : ZECTRIX_EPD_1BPP_FRAME_BYTES,
                result.elapsed_ms, result.error));
            while (true) {
                ZectrixButtonEvent event;
                if (!board_.WaitButton(&event, portMAX_DELAY)) continue;
                if (event.button == ZectrixButton::kDown &&
                    event.action == ZectrixButtonAction::kLongPress) {
                    return ControlResult::kShutdown;
                }
                if (event.button == ZectrixButton::kOk &&
                    event.action == ZectrixButtonAction::kClick) {
                    break;
                }
                if (event.button == ZectrixButton::kOk &&
                    event.action == ZectrixButtonAction::kLongPress) {
                    break;
                }
            }
        }
    }

    ControlResult RunHardwareTests() {
        static constexpr const char* kItems[] = {
            "RUN ALL TESTS", "SELECT INDIVIDUAL TEST"};
        size_t selected = 0;
        const ControlResult menu = RunMenu(
            "HARDWARE TESTS", kItems, std::size(kItems),
            "UP/DOWN Move  OK Select  Hold OK Back", &selected);
        if (menu == ControlResult::kShutdown) return menu;
        if (menu == ControlResult::kBack) return menu;
        return selected == 0 ? RunAllTests() : RunIndividualTests();
    }

    ZectrixTestResult ExecuteTest(ZectrixTestId id) {
        test_states_[static_cast<size_t>(id)] = ZectrixTestState::kRunning;
        return tests_.Run(id, [this](const ZectrixTestUpdate& update) {
            test_states_[static_cast<size_t>(update.id)] = update.state;
            const esp_err_t err = ui_.ShowTestUpdate(update, test_states_);
            if (err != ESP_OK) {
                ESP_LOGE(kTag, "test UI refresh failed: %s", esp_err_to_name(err));
            }
        });
    }

    ControlResult RunAllTests() {
        test_states_.fill(ZectrixTestState::kWait);
        for (size_t i = 0; i < static_cast<size_t>(ZectrixTestId::kCount); ++i) {
            const ZectrixTestId id = static_cast<ZectrixTestId>(i);
            ESP_ERROR_CHECK(ui_.ShowTestMenu(i, test_states_, true));
            const ZectrixTestResult result = ExecuteTest(id);
            if (result == ZectrixTestResult::kShutdown) {
                return ControlResult::kShutdown;
            }
            if (result == ZectrixTestResult::kCancelled) {
                break;
            }
            test_states_[i] = result == ZectrixTestResult::kPass
                                  ? ZectrixTestState::kPass
                                  : ZectrixTestState::kFail;
            const ControlResult pause = Wait(pdMS_TO_TICKS(800), false);
            if (pause == ControlResult::kShutdown) return pause;
        }
        ESP_ERROR_CHECK(ui_.ShowTestSummary(test_states_));
        return Wait(portMAX_DELAY, true);
    }

    ControlResult RunIndividualTests() {
        size_t selected = 0;
        ESP_ERROR_CHECK(ui_.ShowTestMenu(selected, test_states_, true));
        while (true) {
            ZectrixButtonEvent event;
            if (!board_.WaitButton(&event, portMAX_DELAY)) continue;
            if (event.button == ZectrixButton::kDown &&
                event.action == ZectrixButtonAction::kLongPress) {
                return ControlResult::kShutdown;
            }
            if (event.button == ZectrixButton::kOk &&
                event.action == ZectrixButtonAction::kLongPress) {
                return ControlResult::kBack;
            }
            if (event.action != ZectrixButtonAction::kClick) continue;
            if (event.button == ZectrixButton::kUp) {
                selected = (selected + 6) % 7;
                ESP_ERROR_CHECK(ui_.ShowTestMenu(selected, test_states_, false));
            } else if (event.button == ZectrixButton::kDown) {
                selected = (selected + 1) % 7;
                ESP_ERROR_CHECK(ui_.ShowTestMenu(selected, test_states_, false));
            } else if (event.button == ZectrixButton::kOk) {
                const ZectrixTestId id = static_cast<ZectrixTestId>(selected);
                const ZectrixTestResult result = ExecuteTest(id);
                if (result == ZectrixTestResult::kShutdown) {
                    return ControlResult::kShutdown;
                }
                test_states_[selected] = result == ZectrixTestResult::kPass
                                             ? ZectrixTestState::kPass
                                             : (result == ZectrixTestResult::kFail
                                                    ? ZectrixTestState::kFail
                                                    : test_states_[selected]);
                const ControlResult pause = Wait(pdMS_TO_TICKS(1200), true);
                if (pause == ControlResult::kShutdown) {
                    return pause;
                }
                ESP_ERROR_CHECK(ui_.ShowTestMenu(selected, test_states_, true));
            }
        }
    }

    ControlResult RunDeviceInfo() {
        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac[24];
        std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac_bytes[0], mac_bytes[1], mac_bytes[2], mac_bytes[3],
                      mac_bytes[4], mac_bytes[5]);
        uint32_t flash_bytes = 0;
        esp_flash_get_size(nullptr, &flash_bytes);
        const uint32_t psram_bytes = static_cast<uint32_t>(
            heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        ESP_ERROR_CHECK(ui_.ShowDeviceInfo(
            board_.ReadPowerSnapshot(), board_.rtc() != nullptr,
            board_.nfc() != nullptr, mac, flash_bytes / (1024 * 1024),
            psram_bytes / (1024 * 1024)));
        return Wait(portMAX_DELAY, false);
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
        const ZectrixDashboardBattery battery = ReadDashboardBattery();
        ZectrixDashboardDrawNight(
            ui_.canvas(), g_night_weather_ok ? &g_night_weather : nullptr,
            g_night_events_ok ? &g_night_events : nullptr, &battery,
            wake_text, updated);
        const esp_err_t refresh = ui_.RefreshFullGray();
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
        ESP_LOGI(kTag, "night sleep for %lld s", static_cast<long long>(sleep_s));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_deep_sleep_start();
    }

    [[noreturn]] void Shutdown() {
        ESP_LOGI(kTag, "clearing display before shutdown");
        const esp_err_t clear = ui_.ClearDisplay();
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
    ZectrixDemoUi ui_;
    ZectrixSelfTest tests_;
    std::array<ZectrixTestState,
               static_cast<size_t>(ZectrixTestId::kCount)> test_states_;
};

}  // namespace

extern "C" void app_main(void) {
    static DemoApp app;
    app.Run();
}
