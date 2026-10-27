#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZECTRIX_EVENT_LINES 4

// One line of the "Coming up" panel, as curated into events.json "screen".
// Text is ASCII plus UTF-8 "£"; longer values are cut to fit.
typedef struct {
    char when[16];
    char text[48];
    char tag[8];  // "", "FREE", "DEAL" or "BOOK"
} zectrix_event_line_t;

typedef struct {
    zectrix_event_line_t lines[ZECTRIX_EVENT_LINES];
    int count;
    bool valid;
} zectrix_events_data_t;

// Fetches events.json from CONFIG_ZECTRIX_EVENTS_URL. Returns
// ESP_ERR_NOT_SUPPORTED when no URL is configured.
esp_err_t zectrix_dashboard_events_fetch(zectrix_events_data_t *out);

#ifdef __cplusplus
}
#endif
