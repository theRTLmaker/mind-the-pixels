#include "zectrix_dashboard_commute.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "board_stream.h"
#include "esp_log.h"
#include "zectrix_dashboard_trains.h"

namespace {

constexpr char kTag[] = "dashboard_commute";
// Outbound (Stratford Intl -> Cambridge) before this local hour, home after.
constexpr int kReturnFromHour = 14;

using Leg = BoardLeg;

// Minutes to change in London. St Pancras high level (HS1) to the
// Thameslink platforms below is quick; St Pancras <-> King's Cross means
// crossing the road.
int ChangeMinutes(const char* arrive_at, const char* depart_from) {
    return strcmp(arrive_at, depart_from) == 0 ? 8 : 12;
}

void FormatMinutes(int64_t minutes, char out[6]) {
    const int of_day = static_cast<int>(((minutes % 1440) + 1440) % 1440);
    snprintf(out, 6, "%02d:%02d", of_day / 60, of_day % 60);
}

void FeedBoard(void* context, const char* data, size_t length) {
    static_cast<BoardStream*>(context)->Feed(data, length);
}

// Departures from board station `from` that call at one of `targets`
// (space-separated CRS codes), read as the response streams in.
bool FetchBoard(const char* from, const char* filter, const char* targets,
                int rows, std::vector<Leg>* legs) {
    char query[96];
    snprintf(query, sizeof(query),
             "numRows=%d&filterCRS=%s&filterType=to&timeWindow=150", rows,
             filter);
    BoardStream stream(from, targets);
    if (!zectrix_rdm_board_stream(from, query, FeedBoard, &stream) ||
        !stream.complete()) {
        ESP_LOGW(kTag, "%s board incomplete", from);
        return false;
    }
    legs->insert(legs->end(), stream.legs().begin(), stream.legs().end());
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
        // Thameslink and Great Northern boards are ~16 KB per train with
        // details; they are streamed, but fewer rows still means less
        // waiting.
        ok &= FetchBoard("SFA", "STP", "STP", 6, &first);
        ok &= FetchBoard("STP", "CBG", "CBG", 6, &second);
        ok &= FetchBoard("KGX", "CBG", "CBG", 8, &second);
    } else {
        ok &= FetchBoard("CBG", "STP", "STP", 6, &first);
        ok &= FetchBoard("CBG", "KGX", "KGX", 6, &first);
        ok &= FetchBoard("STP", "SFA", "SFA", 10, &second);
    }
    if (!ok) {
        ESP_LOGW(kTag, "A commute board request failed (%d + %d legs so far)",
                 static_cast<int>(first.size()),
                 static_cast<int>(second.size()));
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
