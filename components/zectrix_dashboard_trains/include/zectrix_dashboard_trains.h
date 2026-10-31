#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZECTRIX_TRAIN_COUNT 3

typedef struct {
    char scheduled[6];
    char expected[6];
    bool cancelled;
    int loading_percent;  // typical busyness 0-100, -1 when unknown
} zectrix_train_t;

typedef struct {
    zectrix_train_t trains[ZECTRIX_TRAIN_COUNT];
    int count;
    bool valid;
    // Local "HH:MM" of the last successful fetch; empty when unknown.
    char updated[6];
    // Consecutive failed fetches since then (or since start if never valid).
    int failed_fetches;
} zectrix_trains_data_t;

esp_err_t zectrix_dashboard_trains_start(void);

// Raw Rail Data Marketplace GetDepBoardWithDetails JSON for station crs at
// the current time, with query appended (e.g. "numRows=6&filterCRS=CBG&
// filterType=to"). Returns a malloc'd body the caller frees, or NULL when no
// API key is set, the clock is not synced or the request fails.
char *zectrix_rdm_board_json(const char *crs, const char *query);
bool zectrix_dashboard_trains_get(zectrix_trains_data_t *out);

#ifdef __cplusplus
}
#endif
