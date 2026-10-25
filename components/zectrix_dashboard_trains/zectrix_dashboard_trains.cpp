#include "zectrix_dashboard_trains.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace {
constexpr char kTag[] = "dashboard_trains";
constexpr char kUrl[] =
    "https://huxley2.azurewebsites.net/departures/SFA/to/STP/3";
constexpr size_t kResponseCapacity = 16384;
constexpr TickType_t kRefreshInterval = pdMS_TO_TICKS(60000);

struct ResponseBuffer {
    char *data;
    size_t length;
    size_t capacity;
};

zectrix_trains_data_t latest_data = {};
SemaphoreHandle_t data_mutex = nullptr;
bool task_started = false;

esp_err_t HttpEventHandler(esp_http_client_event_t *event) {
    auto *response = static_cast<ResponseBuffer *>(event->user_data);

    if (event->event_id != HTTP_EVENT_ON_DATA ||
        response == nullptr || event->data_len <= 0) {
        return ESP_OK;
    }

    const size_t incoming = static_cast<size_t>(event->data_len);
    if (response->length + incoming >= response->capacity) {
        ESP_LOGE(kTag, "Train response is too large");
        return ESP_FAIL;
    }

    memcpy(response->data + response->length, event->data, incoming);
    response->length += incoming;
    response->data[response->length] = '\0';
    return ESP_OK;
}

const cJSON *GetField(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

bool CopyTime(const cJSON *value, char out[6]) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        return false;
    }

    const char *time = value->valuestring;
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

bool FetchTrains(zectrix_trains_data_t *result) {
    char *body = static_cast<char *>(malloc(kResponseCapacity));
    if (body == nullptr) {
        ESP_LOGE(kTag, "Out of memory for response");
        return false;
    }
    body[0] = '\0';

    ResponseBuffer response = {body, 0, kResponseCapacity};

    esp_http_client_config_t config = {};
    config.url = kUrl;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 10000;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        free(body);
        return false;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || response.length == 0) {
        ESP_LOGW(kTag, "Train request failed: %s, HTTP %d",
                 esp_err_to_name(err), status);
        free(body);
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

        zectrix_train_t train = {};
        if (!CopyTime(GetField(service, "std"), train.scheduled)) {
            continue;
        }

        const cJSON *cancelled = GetField(service, "isCancelled");
        train.cancelled = cJSON_IsTrue(cancelled);

        const cJSON *expected = GetField(service, "etd");
        ESP_LOGI(kTag, "Train %s: etd='%s', cancelled=%s",
                 train.scheduled,
                 cJSON_IsString(expected) && expected->valuestring != nullptr
                     ? expected->valuestring : "(missing)",
                 train.cancelled ? "yes" : "no");

        if (cJSON_IsString(expected) && expected->valuestring != nullptr &&
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
