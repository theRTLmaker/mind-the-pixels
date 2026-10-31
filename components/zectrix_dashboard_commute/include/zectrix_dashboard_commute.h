#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZECTRIX_COMMUTE_JOURNEYS 4

// One two-leg journey between Stratford International and Cambridge,
// changing in London. Times are local "HH:MM"; "expected" times are live
// estimates and equal the timetable when on time.
typedef struct {
    char leg1_dep[6];
    char leg1_dep_expected[6];
    char change_at[4];        // "STP" or "KGX"
    char change_arr[6];       // timetable arrival at the change station
    char change_arr_expected[6];
    char leg2_dep[6];
    char leg2_dep_expected[6];
    char final_arr[6];        // timetable arrival at the destination
    char final_arr_expected[6];
    int change_minutes;       // live time to make the connection
} zectrix_journey_t;

typedef struct {
    bool outbound;  // true: Stratford Intl -> Cambridge; false: home
    zectrix_journey_t journeys[ZECTRIX_COMMUTE_JOURNEYS];
    int count;
    bool valid;
    char updated[6];
} zectrix_commute_data_t;

// Fetches the boards for the current direction (outbound before 14:00,
// home after) and builds the best connections. Blocking: three requests
// to the Rail Data Marketplace; needs the RDM API key and a synced clock.
esp_err_t zectrix_dashboard_commute_fetch(zectrix_commute_data_t *out);

#ifdef __cplusplus
}
#endif
