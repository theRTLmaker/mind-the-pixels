#include "zectrix_dashboard_trains.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "zectrix_http.h"

namespace {
constexpr char kTag[] = "dashboard_trains";
// Official Rail Data Marketplace feed (LDB staff version) when a key is
// configured; the public Huxley proxy otherwise, and as a fallback. Both
// return trainServices with std, etd, isCancelled and destination[].crs;
// the staff version gives times as "yyyy-mm-ddThh:mm:ss" instead of "hh:mm".
constexpr char kRdmUrl[] = CONFIG_ZECTRIX_TRAINS_RDM_URL;
constexpr char kRdmApiKey[] = CONFIG_ZECTRIX_TRAINS_RDM_API_KEY;
// Appended to kRdmUrl as /<crs>/<yyyymmddThhmmss>?<query>.
constexpr char kRdmStation[] = "SFA";
constexpr char kRdmQuery[] =
    "numRows=5&filterCRS=STP&filterType=to&timeWindow=120";
constexpr char kHuxleyUrl[] =
    "https://huxley2.azurewebsites.net/departures/SFA/to/STP/3";
constexpr size_t kResponseCapacity = 32768;
constexpr TickType_t kRefreshInterval = pdMS_TO_TICKS(60000);

zectrix_trains_data_t latest_data = {};
SemaphoreHandle_t data_mutex = nullptr;
bool task_started = false;

const cJSON *GetField(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

bool CopyTime(const cJSON *value, char out[6]) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        return false;
    }

    const char *time = value->valuestring;
    // Staff version: "2026-10-27T10:15:00" -> "10:15".
    char clock[6] = {};
    const char *t = strchr(time, 'T');
    if (t != nullptr && strlen(t + 1) >= 5) {
        memcpy(clock, t + 1, 5);
        clock[5] = '\0';
        time = clock;
    }
    if (strlen(time) != 5 || time[2] != ':' ||
        time[0] < '0' || time[0] > '9' ||
        time[1] < '0' || time[1] > '9' ||
        time[3] < '0' || time[3] > '9' ||
        time[4] < '0' || time[4] > '9') {
        return false;
    }

    memcpy(out, time, 5);
    out[5] = '\0';
    return true;
}

bool FetchTrainsFrom(const char *url, const char *api_key,
                     zectrix_trains_data_t *result) {
    char *body = zectrix_http_get(url, api_key, kResponseCapacity);
    if (body == nullptr) {
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == nullptr) {
        ESP_LOGW(kTag, "Could not parse train JSON");
        return false;
    }

    const cJSON *services = GetField(root, "trainServices");
    if (!cJSON_IsArray(services)) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Train JSON has no trainServices array");
        return false;
    }

    zectrix_trains_data_t parsed = {};
    const cJSON *service = nullptr;

    cJSON_ArrayForEach(service, services) {
        if (parsed.count >= ZECTRIX_TRAIN_COUNT) {
            break;
        }

        // Keep only services whose destination is St Pancras International.
        bool goes_to_st_pancras = false;
        const cJSON *destinations = GetField(service, "destination");
        const cJSON *destination = nullptr;
        cJSON_ArrayForEach(destination, destinations) {
            const cJSON *crs = GetField(destination, "crs");
            if (cJSON_IsString(crs) &&
                strcmp(crs->valuestring, "STP") == 0) {
                goes_to_st_pancras = true;
                break;
            }
        }
        if (!goes_to_st_pancras) {
            continue;
        }

        // Staff version keeps just-departed trains on the board; they carry
        // an actual departure time instead of an estimate.
        const cJSON *departure_type = GetField(service, "departureType");
        if (cJSON_IsString(GetField(service, "atd")) ||
            cJSON_IsTrue(GetField(service, "atdSpecified")) ||
            (cJSON_IsString(departure_type) &&
             strcmp(departure_type->valuestring, "Actual") == 0)) {
            continue;
        }

        zectrix_train_t train = {};
        if (!CopyTime(GetField(service, "std"), train.scheduled)) {
            continue;
        }

        const cJSON *cancelled = GetField(service, "isCancelled");
        train.cancelled = cJSON_IsTrue(cancelled);

        // Staff version only: formation.serviceLoading.loadingPercentage.
        const cJSON *loading = GetField(
            GetField(GetField(GetField(service, "formation"),
                              "serviceLoading"),
                     "loadingPercentage"),
            "Value");
        train.loading_percent = -1;
        if (cJSON_IsNumber(loading)) {
            train.loading_percent =
                loading->valueint < 0     ? 0
                : loading->valueint > 100 ? 100
                                          : loading->valueint;
        }

        const cJSON *expected = GetField(service, "etd");
        ESP_LOGI(kTag, "Train %s: etd='%s', cancelled=%s",
                 train.scheduled,
                 cJSON_IsString(expected) && expected->valuestring != nullptr
                     ? expected->valuestring : "(missing)",
                 train.cancelled ? "yes" : "no");

        // Staff version: delayed with no estimate has no etd at all.
        if (!cJSON_IsString(expected) && cJSON_IsString(departure_type) &&
            strcmp(departure_type->valuestring, "Delayed") == 0) {
            strncpy(train.expected, "-----", sizeof(train.expected) - 1);
        } else if (cJSON_IsString(expected) &&
                   expected->valuestring != nullptr &&
                   strcmp(expected->valuestring, "On time") != 0) {
            char revised[6] = {};
            if (CopyTime(expected, revised)) {
                memcpy(train.expected, revised, sizeof(train.expected));
            } else {
                // Keep non-time statuses (e.g. "Delayed") distinguishable.
                strncpy(train.expected, "-----", sizeof(train.expected) - 1);
            }
        } else {
            memcpy(train.expected, train.scheduled, sizeof(train.expected));
        }

        parsed.trains[parsed.count++] = train;
    }

    cJSON_Delete(root);

    if (parsed.count == 0) {
        ESP_LOGW(kTag, "No matching departures returned");
        return false;
    }

    parsed.valid = true;
    *result = parsed;
    ESP_LOGI(kTag, "Loaded %d departures", parsed.count);
    return true;
}

// The staff board takes the board time in the path, so it needs the real
// (UK local) time from SNTP; until then only Huxley is used.
bool BuildRdmUrl(char *out, size_t size) {
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        return false;
    }
    struct tm local_time = {};
    localtime_r(&now, &local_time);
    char board_time[16] = {};
    strftime(board_time, sizeof(board_time), "%Y%m%dT%H%M%S", &local_time);
    const int written = snprintf(out, size, "%s/%s/%s?%s", kRdmUrl,
                                 kRdmStation, board_time, kRdmQuery);
    return written > 0 && static_cast<size_t>(written) < size;
}

bool FetchTrains(zectrix_trains_data_t *result) {
    if (kRdmApiKey[0] != '\0') {
        char url[384];
        if (BuildRdmUrl(url, sizeof(url)) &&
            FetchTrainsFrom(url, kRdmApiKey, result)) {
            return true;
        }
        ESP_LOGW(kTag, "Rail Data Marketplace failed; trying Huxley");
    }
    return FetchTrainsFrom(kHuxleyUrl, nullptr, result);
}

void TrainRefreshTask(void *) {
    while (true) {
        zectrix_trains_data_t updated = {};
        if (FetchTrains(&updated)) {
            // Stamp the fetch time once SNTP has supplied a real clock.
            const time_t now = time(nullptr);
            if (now >= 1700000000) {
                struct tm local_time = {};
                localtime_r(&now, &local_time);
                strftime(updated.updated, sizeof(updated.updated), "%H:%M",
                         &local_time);
            }
            xSemaphoreTake(data_mutex, portMAX_DELAY);
            latest_data = updated;
            xSemaphoreGive(data_mutex);
        } else {
            // Keep the cached departures but count the failure so the view
            // can mark them as old.
            xSemaphoreTake(data_mutex, portMAX_DELAY);
            ++latest_data.failed_fetches;
            xSemaphoreGive(data_mutex);
        }

        vTaskDelay(kRefreshInterval);
    }
}
}  // namespace

extern "C" bool zectrix_rdm_board_stream(const char *crs, const char *query,
                                         zectrix_http_chunk_cb on_chunk,
                                         void *context) {
    if (kRdmApiKey[0] == '\0' || crs == nullptr || query == nullptr) {
        return false;
    }
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        return false;
    }
    struct tm local_time = {};
    localtime_r(&now, &local_time);
    char board_time[16] = {};
    strftime(board_time, sizeof(board_time), "%Y%m%dT%H%M%S", &local_time);
    char url[384];
    const int written = snprintf(url, sizeof(url), "%s/%s/%s?%s", kRdmUrl,
                                 crs, board_time, query);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(url)) {
        return false;
    }
    return zectrix_http_get_stream(url, kRdmApiKey, on_chunk, context);
}

extern "C" esp_err_t zectrix_dashboard_trains_start(void) {
    if (task_started) {
        return ESP_OK;
    }

    data_mutex = xSemaphoreCreateMutex();
    if (data_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(TrainRefreshTask, "train_refresh", 8192, nullptr, 3,
                    nullptr) != pdPASS) {
        vSemaphoreDelete(data_mutex);
        data_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }

    task_started = true;
    return ESP_OK;
}

extern "C" bool zectrix_dashboard_trains_get(zectrix_trains_data_t *out) {
    if (out == nullptr || data_mutex == nullptr) {
        return false;
    }

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    *out = latest_data;
    xSemaphoreGive(data_mutex);
    return out->valid;
}
