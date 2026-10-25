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
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zectrix_board.h"
#include "zectrix_demo_ui.h"
#include "zectrix_epd.h"
#include "zectrix_self_test.h"
#include "zectrix_dashboard_wifi.h"
#include "zectrix_dashboard_weather.h"
#include "zectrix_dashboard_trains.h"
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
        ESP_ERROR_CHECK(ui_.ShowSplash());
        Wait(pdMS_TO_TICKS(1500), false);

        const esp_err_t wifi_err = zectrix_dashboard_wifi_connect();
        if (wifi_err != ESP_OK) {
            ESP_LOGW(kTag, "Dashboard Wi-Fi unavailable: %s",
                     esp_err_to_name(wifi_err));
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

    ControlResult RunDashboard() {
        ZectrixCanvas& canvas = ui_.canvas();

        zectrix_weather_data_t weather = {};
        const esp_err_t weather_err =
            zectrix_dashboard_weather_fetch(&weather);

        // Start background train updates.
        ESP_ERROR_CHECK(zectrix_dashboard_trains_start());

        zectrix_trains_data_t displayed_trains = {};

        // Layout and drawing live in zectrix_dashboard_view, shared with
        // the Mac simulator.
        ZectrixDashboardDrawAll(
            canvas, weather_err == ESP_OK ? &weather : nullptr,
            displayed_trains, "--:--");

        // 16-gray frame; the clock and train card then use partial
        // refreshes on top of it (experimental, see zectrix_epd.h).
        ESP_ERROR_CHECK(ui_.RefreshFullGray());

        StartDashboardClockSync();

        int last_displayed_minute = -1;

        while (true) {
            ZectrixButtonEvent event;

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

            // Refresh only when the displayed minute changes.
            if (local_time.tm_min != last_displayed_minute) {
                char clock_text[8];
                std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d",
                            local_time.tm_hour, local_time.tm_min);

                ZectrixDashboardDrawClock(canvas, clock_text);

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
