#include "zectrix_dashboard_outlook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_log.h"
#include "zectrix_http.h"

namespace {

constexpr char kTag[] = "dashboard_outlook";

// Central London, times in Europe/London.
constexpr char kForecastUrl[] =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=51.5072&longitude=-0.1276"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
    "precipitation_probability_max,wind_speed_10m_max,sunrise,sunset,"
    "daylight_duration"
    "&forecast_days=8&timezone=Europe%2FLondon";

const cJSON* Field(const cJSON* object, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

double NumberAt(const cJSON* array, int index, double fallback) {
    const cJSON* item = cJSON_GetArrayItem(array, index);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

// "2026-11-07" -> weekday (0 = Sunday) and a "Sat 7 Nov" label.
bool ParseDay(const cJSON* value, int* weekday, char* label, size_t size) {
    if (!cJSON_IsString(value)) {
        return false;
    }
    tm date = {};
    if (sscanf(value->valuestring, "%d-%d-%d", &date.tm_year, &date.tm_mon,
               &date.tm_mday) != 3) {
        return false;
    }
    date.tm_year -= 1900;
    date.tm_mon -= 1;
    date.tm_hour = 12;
    date.tm_isdst = -1;
    mktime(&date);
    *weekday = date.tm_wday;
    strftime(label, size, "%a %e %b", &date);
    // "%e" pads single-digit days with a space: "Sat  7 Nov" -> "Sat 7 Nov".
    char* gap = strstr(label, "  ");
    if (gap != nullptr) {
        memmove(gap, gap + 1, strlen(gap));
    }
    return true;
}

void CopyClock(const cJSON* value, char out[6]) {
    out[0] = '\0';
    if (cJSON_IsString(value)) {
        const char* t = strchr(value->valuestring, 'T');
        if (t != nullptr && strlen(t + 1) >= 5) {
            memcpy(out, t + 1, 5);
            out[5] = '\0';
        }
    }
}

bool ParseForecast(const char* body, zectrix_outlook_data_t* out) {
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        ESP_LOGW(kTag, "Could not parse forecast JSON");
        return false;
    }
    const cJSON* daily = Field(root, "daily");
    const cJSON* dates = Field(daily, "time");
    const int count = cJSON_GetArraySize(dates);

    // The first Saturday and Sunday from today; on a Sunday that is today
    // and the next Saturday.
    int saturday = -1;
    int sunday = -1;
    for (int i = 0; i < count; ++i) {
        int weekday = 0;
        char label[16];
        if (!ParseDay(cJSON_GetArrayItem(dates, i), &weekday, label,
                      sizeof(label))) {
            continue;
        }
        if (weekday == 6 && saturday < 0) saturday = i;
        if (weekday == 0 && sunday < 0) sunday = i;
    }
    int order[2] = {saturday, sunday};
    if (saturday >= 0 && sunday >= 0 && sunday < saturday) {
        order[0] = sunday;
        order[1] = saturday;
    }
    for (int index : order) {
        if (index < 0) {
            continue;
        }
        zectrix_day_forecast_t& day = out->days[out->day_count++];
        int weekday = 0;
        ParseDay(cJSON_GetArrayItem(dates, index), &weekday, day.label,
                 sizeof(day.label));
        day.weather_code = static_cast<int>(
            NumberAt(Field(daily, "weather_code"), index, 0));
        day.max_c = NumberAt(Field(daily, "temperature_2m_max"), index, 0);
        day.min_c = NumberAt(Field(daily, "temperature_2m_min"), index, 0);
        day.rain_percent = static_cast<int>(
            NumberAt(Field(daily, "precipitation_probability_max"), index, 0));
        day.wind_kmh = NumberAt(Field(daily, "wind_speed_10m_max"), index, 0);
    }

    CopyClock(cJSON_GetArrayItem(Field(daily, "sunrise"), 0), out->sunrise);
    CopyClock(cJSON_GetArrayItem(Field(daily, "sunset"), 0), out->sunset);
    out->daylight_minutes = static_cast<int>(
        NumberAt(Field(daily, "daylight_duration"), 0, 0) / 60.0 + 0.5);
    cJSON_Delete(root);
    return out->day_count > 0;
}

}  // namespace

extern "C" esp_err_t
zectrix_dashboard_outlook_fetch(zectrix_outlook_data_t* out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        return ESP_ERR_INVALID_STATE;
    }

    zectrix_outlook_data_t parsed = {};

    char* body = zectrix_http_get(kForecastUrl, nullptr, 8192);
    if (body == nullptr) {
        return ESP_FAIL;
    }
    const bool ok = ParseForecast(body, &parsed);
    free(body);
    if (!ok) {
        ESP_LOGW(kTag, "Forecast has no weekend days");
        return ESP_FAIL;
    }

    parsed.valid = true;
    *out = parsed;
    ESP_LOGI(kTag, "Weekend %s / %s, sun %s-%s", parsed.days[0].label,
             parsed.day_count > 1 ? parsed.days[1].label : "-",
             parsed.sunrise, parsed.sunset);
    return ESP_OK;
}
