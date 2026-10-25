#include "zectrix_dashboard_weather.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "dashboard_weather";
constexpr size_t kResponseCapacity = 4096;

struct HttpResponse {
    char body[kResponseCapacity] = {};
    size_t length = 0;
    bool overflow = false;
};

esp_err_t HttpEventHandler(esp_http_client_event_t *event) {
    if (event == nullptr || event->user_data == nullptr) {
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }

    auto *response = static_cast<HttpResponse *>(event->user_data);
    const size_t incoming = static_cast<size_t>(event->data_len);

    if (incoming >= sizeof(response->body) - response->length) {
        response->overflow = true;
        return ESP_FAIL;
    }

    std::memcpy(response->body + response->length,
                event->data, incoming);
    response->length += incoming;
    response->body[response->length] = '\0';

    return ESP_OK;
}

bool ReadNumber(cJSON *object, const char *name, double *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out = item->valuedouble;
    return true;
}

}  // namespace

extern "C" esp_err_t
zectrix_dashboard_weather_fetch(zectrix_weather_data_t *out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // Central London coordinates; times are returned in Europe/London.
    constexpr char kUrl[] =
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=51.5072&longitude=-0.1276"
        "&current=temperature_2m,relative_humidity_2m,"
        "apparent_temperature,weather_code,wind_speed_10m"
        "&daily=temperature_2m_min,temperature_2m_max,"
        "precipitation_probability_max,weather_code"
        "&hourly=precipitation_probability"
        "&forecast_days=1&timezone=Europe%2FLondon";

    HttpResponse response;

    esp_http_client_config_t config = {};
    config.url = kUrl;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 20000;
    config.buffer_size = 1024;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(kTag, "Failed to initialise HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "HTTPS request failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    if (response.overflow) {
        ESP_LOGE(kTag, "Weather response exceeded buffer capacity");
        return ESP_ERR_INVALID_SIZE;
    }

    if (status != 200) {
        ESP_LOGE(kTag, "Weather server returned HTTP %d", status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response.body);
    if (root == nullptr) {
        ESP_LOGE(kTag, "Could not parse weather JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");

    double temperature = 0;
    double feels_like = 0;
    double wind = 0;
    double humidity = 0;
    double weather_code = 0;
    double min_temperature = 0;
    double max_temperature = 0;
    double daily_weather_code = 0;

    const bool valid =
        cJSON_IsObject(current) &&
        cJSON_IsObject(daily) &&
        ReadNumber(current, "temperature_2m", &temperature) &&
        ReadNumber(current, "apparent_temperature", &feels_like) &&
        ReadNumber(current, "wind_speed_10m", &wind) &&
        ReadNumber(current, "relative_humidity_2m", &humidity) &&
        ReadNumber(current, "weather_code", &weather_code);

    cJSON *min_array =
        cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    cJSON *max_array =
        cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    cJSON *rain_array =
        cJSON_GetObjectItemCaseSensitive(
            daily, "precipitation_probability_max");
    cJSON *code_array =
        cJSON_GetObjectItemCaseSensitive(daily, "weather_code");

    cJSON *min_value = cJSON_GetArrayItem(min_array, 0);
    cJSON *max_value = cJSON_GetArrayItem(max_array, 0);
    cJSON *rain_value = cJSON_GetArrayItem(rain_array, 0);
    cJSON *daily_code_value = cJSON_GetArrayItem(code_array, 0);

    if (!valid ||
        !cJSON_IsNumber(min_value) ||
        !cJSON_IsNumber(max_value) ||
        !cJSON_IsNumber(rain_value) ||
        !cJSON_IsNumber(daily_code_value)) {
        cJSON_Delete(root);
        ESP_LOGE(kTag, "Weather response is missing expected fields");
        return ESP_ERR_INVALID_RESPONSE;
    }

    min_temperature = min_value->valuedouble;
    max_temperature = max_value->valuedouble;

    if (!valid || !cJSON_IsNumber(rain_value)) {
        cJSON_Delete(root);
        ESP_LOGE(kTag, "Weather response is missing expected fields");
        return ESP_ERR_INVALID_RESPONSE;
    }

    double rain_probability = rain_value->valuedouble;

    out->temperature_c = static_cast<float>(temperature);
    out->feels_like_c = static_cast<float>(feels_like);
    out->wind_speed_kmh = static_cast<float>(wind);
    out->humidity_percent = static_cast<int>(humidity);
    out->rain_probability_percent = static_cast<int>(rain_probability);
    out->weather_code = static_cast<int>(weather_code);
    out->min_temperature_c = static_cast<float>(min_temperature);
    out->max_temperature_c = static_cast<float>(max_temperature);
    out->daily_weather_code =
        static_cast<int>(daily_weather_code);

    // Hourly rain is optional: missing values stay at 0 so the chart hides.
    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    cJSON *hourly_rain = cJSON_GetObjectItemCaseSensitive(
        hourly, "precipitation_probability");
    for (int hour = 0; hour < ZECTRIX_WEATHER_HOURS; ++hour) {
        cJSON *value = cJSON_GetArrayItem(hourly_rain, hour);
        out->hourly_rain_percent[hour] =
            cJSON_IsNumber(value) ? static_cast<int>(value->valuedouble)
                                  : 0;
    }

    cJSON_Delete(root);

    ESP_LOGI(kTag,
             "London: %.1f C, feels like %.1f C, humidity %d%%, "
             "wind %.1f km/h, daily rain probability %d%%, code %d",
             out->temperature_c, out->feels_like_c,
             out->humidity_percent, out->wind_speed_kmh,
             out->rain_probability_percent, out->weather_code);

    return ESP_OK;
}
