#include "zectrix_dashboard_commute.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "cJSON.h"
#include "esp_log.h"
#include "zectrix_dashboard_trains.h"

namespace {

constexpr char kTag[] = "dashboard_commute";
// Outbound (Stratford Intl -> Cambridge) before this local hour, home after.
constexpr int kReturnFromHour = 14;

// One leg from a departure board: times in minutes since 1970 (local).
struct Leg {
    char from[4];
    char to[4];
    int64_t dep;
    int64_t dep_expected;
    int64_t arr;
    int64_t arr_expected;
};

// Minutes to change in London. St Pancras high level (HS1) to the
// Thameslink platforms below is quick; St Pancras <-> King's Cross means
// crossing the road.
int ChangeMinutes(const char* arrive_at, const char* depart_from) {
    return strcmp(arrive_at, depart_from) == 0 ? 8 : 12;
}

// "2026-11-02T07:31:30" -> minutes since 1970 (fields taken as given).
bool ParseMinutes(const cJSON* value, int64_t* out) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (sscanf(value->valuestring, "%d-%d-%dT%d:%d", &year, &month, &day,
               &hour, &minute) != 5) {
        return false;
    }
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int year_of_era = year - era * 400;
    const int day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int day_of_era = year_of_era * 365 + year_of_era / 4 -
                           year_of_era / 100 + day_of_year;
    const int64_t days = era * 146097LL + day_of_era - 719468;
    *out = days * 1440 + hour * 60 + minute;
    return true;
}

void FormatMinutes(int64_t minutes, char out[6]) {
    const int of_day = static_cast<int>(((minutes % 1440) + 1440) % 1440);
    snprintf(out, 6, "%02d:%02d", of_day / 60, of_day % 60);
}

const cJSON* Field(const cJSON* object, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

// Departures from board station `from` that call at one of `targets`
// (space-separated CRS codes), skipping cancelled and departed trains.
void ParseBoard(const char* body, const char* from, const char* targets,
                std::vector<Leg>* legs) {
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        ESP_LOGW(kTag, "Could not parse %s board", from);
        return;
    }
    const cJSON* service = nullptr;
    cJSON_ArrayForEach(service, Field(root, "trainServices")) {
        const cJSON* departure_type = Field(service, "departureType");
        if (cJSON_IsTrue(Field(service, "isCancelled")) ||
            cJSON_IsString(Field(service, "atd")) ||
            (cJSON_IsString(departure_type) &&
             strcmp(departure_type->valuestring, "Actual") == 0)) {
            continue;
        }
        Leg leg = {};
        snprintf(leg.from, sizeof(leg.from), "%s", from);
        if (!ParseMinutes(Field(service, "std"), &leg.dep)) {
            continue;
        }
        if (!ParseMinutes(Field(service, "etd"), &leg.dep_expected)) {
            leg.dep_expected = leg.dep;
        }

        bool found = false;
        const cJSON* location = nullptr;
        cJSON_ArrayForEach(location, Field(service, "subsequentLocations")) {
            const cJSON* crs = Field(location, "crs");
            if (!cJSON_IsString(crs) || strlen(crs->valuestring) != 3 ||
                strstr(targets, crs->valuestring) == nullptr ||
                cJSON_IsTrue(Field(location, "isCancelled"))) {
                continue;
            }
            if (!ParseMinutes(Field(location, "sta"), &leg.arr)) {
                break;
            }
            if (!ParseMinutes(Field(location, "eta"), &leg.arr_expected) &&
                !ParseMinutes(Field(location, "ata"), &leg.arr_expected)) {
                leg.arr_expected = leg.arr + (leg.dep_expected - leg.dep);
            }
            snprintf(leg.to, sizeof(leg.to), "%s", crs->valuestring);
            found = true;
            break;
        }
        if (found) {
            legs->push_back(leg);
        }
    }
    cJSON_Delete(root);
}

bool FetchBoard(const char* from, const char* filter, const char* targets,
                int rows, std::vector<Leg>* legs) {
    char query[96];
    snprintf(query, sizeof(query),
             "numRows=%d&filterCRS=%s&filterType=to&timeWindow=180", rows,
             filter);
    char* body = zectrix_rdm_board_json(from, query);
    if (body == nullptr) {
        return false;
    }
    ParseBoard(body, from, targets, legs);
    free(body);
    return true;
}

}  // namespace

extern "C" esp_err_t
zectrix_dashboard_commute_fetch(zectrix_commute_data_t* out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        return ESP_ERR_INVALID_STATE;
    }
    struct tm local_time = {};
    localtime_r(&now, &local_time);
    const bool outbound = local_time.tm_hour < kReturnFromHour;

    std::vector<Leg> first;
    std::vector<Leg> second;
    bool ok = true;
    if (outbound) {
        ok &= FetchBoard("SFA", "STP", "STP", 6, &first);
        ok &= FetchBoard("STP", "CBG", "CBG", 10, &second);
        ok &= FetchBoard("KGX", "CBG", "CBG", 10, &second);
    } else {
        ok &= FetchBoard("CBG", "STP", "STP", 6, &first);
        ok &= FetchBoard("CBG", "KGX", "KGX", 6, &first);
        ok &= FetchBoard("STP", "SFA", "SFA", 10, &second);
    }
    if (!ok) {
        ESP_LOGW(kTag, "A commute board request failed");
        return ESP_FAIL;
    }

    std::sort(first.begin(), first.end(), [](const Leg& a, const Leg& b) {
        return a.dep_expected < b.dep_expected;
    });

    zectrix_commute_data_t parsed = {};
    parsed.outbound = outbound;
    const Leg* previous_second = nullptr;
    for (const Leg& leg1 : first) {
        // Best connection: earliest live arrival, then earliest departure.
        const Leg* best = nullptr;
        for (const Leg& leg2 : second) {
            const int64_t ready =
                leg1.arr_expected + ChangeMinutes(leg1.to, leg2.from);
            if (leg2.dep_expected < ready) {
                continue;
            }
            if (best == nullptr || leg2.arr_expected < best->arr_expected ||
                (leg2.arr_expected == best->arr_expected &&
                 leg2.dep_expected < best->dep_expected)) {
                best = &leg2;
            }
        }
        if (best == nullptr) {
            continue;
        }
        // Two first legs reaching the same connection: keep the later one,
        // it arrives just the same.
        if (best == previous_second && parsed.count > 0) {
            --parsed.count;
        }
        if (parsed.count >= ZECTRIX_COMMUTE_JOURNEYS) {
            break;
        }
        zectrix_journey_t& journey = parsed.journeys[parsed.count++];
        FormatMinutes(leg1.dep, journey.leg1_dep);
        FormatMinutes(leg1.dep_expected, journey.leg1_dep_expected);
        snprintf(journey.change_at, sizeof(journey.change_at), "%s",
                 outbound ? best->from : leg1.to);
        FormatMinutes(leg1.arr, journey.change_arr);
        FormatMinutes(leg1.arr_expected, journey.change_arr_expected);
        FormatMinutes(best->dep, journey.leg2_dep);
        FormatMinutes(best->dep_expected, journey.leg2_dep_expected);
        FormatMinutes(best->arr, journey.final_arr);
        FormatMinutes(best->arr_expected, journey.final_arr_expected);
        journey.change_minutes =
            static_cast<int>(best->dep_expected - leg1.arr_expected);
        previous_second = best;
    }

    parsed.valid = parsed.count > 0;
    strftime(parsed.updated, sizeof(parsed.updated), "%H:%M", &local_time);
    *out = parsed;
    ESP_LOGI(kTag, "%s: %d journeys from %d + %d legs",
             outbound ? "To Cambridge" : "Home", parsed.count,
             static_cast<int>(first.size()), static_cast<int>(second.size()));
    return parsed.valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}
