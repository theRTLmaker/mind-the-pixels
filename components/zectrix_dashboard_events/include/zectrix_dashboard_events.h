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

#define ZECTRIX_EVENT_ITEMS 12
#define ZECTRIX_EVENT_WEEKEND_LINES 4

// One entry of events.json "items", for the full-screen events view.
typedef struct {
    char when[24];
    char title[48];
    char venue[40];
    char price[24];
    char tag[8];  // derived: FREE, DEAL or BOOK
} zectrix_event_item_t;

typedef struct {
    zectrix_event_line_t lines[ZECTRIX_EVENT_LINES];
    int count;
    zectrix_event_item_t items[ZECTRIX_EVENT_ITEMS];
    int item_count;
    // events.json "weekend": picks for the coming Saturday and Sunday.
    zectrix_event_line_t weekend[ZECTRIX_EVENT_WEEKEND_LINES];
    int weekend_count;
    bool valid;
} zectrix_events_data_t;

// Fetches events.json from CONFIG_ZECTRIX_EVENTS_URL. Returns
// ESP_ERR_NOT_SUPPORTED when no URL is configured.
esp_err_t zectrix_dashboard_events_fetch(zectrix_events_data_t *out);

#ifdef __cplusplus
}
#endif
