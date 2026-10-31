#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One day of the weekend forecast.
typedef struct {
    char label[16];  // e.g. "Sat 7 Nov"
    int weather_code;
    float max_c;
    float min_c;
    int rain_percent;
    float wind_kmh;
} zectrix_day_forecast_t;

// The coming weekend plus today's sunrise and sunset.
typedef struct {
    zectrix_day_forecast_t days[2];  // Saturday and Sunday, in date order
    int day_count;
    char sunrise[6];                 // today, local "HH:MM"
    char sunset[6];
    int daylight_minutes;
    bool valid;
} zectrix_outlook_data_t;

// Fetches the Open-Meteo forecast (8 days) for central London. Needs a
// synced clock to find the weekend days.
esp_err_t zectrix_dashboard_outlook_fetch(zectrix_outlook_data_t *out);

#ifdef __cplusplus
}
#endif
