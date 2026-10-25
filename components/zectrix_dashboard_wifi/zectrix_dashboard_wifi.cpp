#include "zectrix_dashboard_wifi.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "dashboard_wifi";
static constexpr EventBits_t CONNECTED_BIT = BIT0;
static constexpr EventBits_t FAILED_BIT = BIT1;

static EventGroupHandle_t wifi_events;
static esp_netif_t *wifi_netif;

static void wifi_event_handler(void *, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *event =
            static_cast<wifi_event_sta_disconnected_t *>(event_data);

        ESP_LOGW(TAG, "Disconnected: reason=%d, RSSI=%d",
                event->reason, event->rssi);

        xEventGroupClearBits(wifi_events, CONNECTED_BIT);

        static int retry_count = 0;
        if (retry_count < 5) {
            ++retry_count;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection (%d/5)", retry_count);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Wi-Fi retries exhausted");
            xEventGroupSetBits(wifi_events, FAILED_BIT);
        }
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected; IP address acquired");
        xEventGroupSetBits(wifi_events, CONNECTED_BIT);
    }
}

extern "C" esp_err_t zectrix_dashboard_wifi_connect(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS needs recovery; refusing to erase existing settings");
        return err;
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    wifi_netif = esp_netif_create_default_wifi_sta();
    if (wifi_netif == nullptr) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    wifi_events = xEventGroupCreate();
    if (wifi_events == nullptr) return ESP_ERR_NO_MEM;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, nullptr);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler, nullptr);
    if (err != ESP_OK) return err;

    wifi_config_t config = {};
    const char *ssid = CONFIG_ZECTRIX_DASHBOARD_WIFI_SSID;
    const char *password = CONFIG_ZECTRIX_DASHBOARD_WIFI_PASSWORD;

    if (ssid[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty; configure it in menuconfig");
        return ESP_ERR_INVALID_ARG;
    }

    std::strncpy(reinterpret_cast<char *>(config.sta.ssid), ssid,
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(config.sta.password), password,
                 sizeof(config.sta.password) - 1);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    EventBits_t bits = xEventGroupWaitBits(
        wifi_events, CONNECTED_BIT | FAILED_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & CONNECTED_BIT) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "Connected; IP address: " IPSTR,
                     IP2STR(&ip_info.ip));
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi connection failed or timed out");
    return ESP_FAIL;
}
