#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZECTRIX_WEATHER_HOURS 24

typedef struct {
    float temperature_c;
    float feels_like_c;
    float wind_speed_kmh;
    float min_temperature_c;
    float max_temperature_c;
    int humidity_percent;
    int rain_probability_percent;
    int weather_code;
    int daily_weather_code;
    // Rain probability for each local hour of today (index 0 = 00:00).
    int hourly_rain_percent[ZECTRIX_WEATHER_HOURS];
} zectrix_weather_data_t;

esp_err_t zectrix_dashboard_weather_fetch(zectrix_weather_data_t *out);

#ifdef __cplusplus
}
#endif
