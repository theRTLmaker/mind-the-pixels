#include "zectrix_dashboard_events.h"

#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "dashboard_events";
constexpr char kUrl[] = CONFIG_ZECTRIX_EVENTS_URL;
// events.json carries the full item list too; only "screen" is drawn.
constexpr size_t kResponseCapacity = 32768;

struct ResponseBuffer {
    char *data;
    size_t length;
    size_t capacity;
};

esp_err_t HttpEventHandler(esp_http_client_event_t *event) {
    auto *response = static_cast<ResponseBuffer *>(event->user_data);

    if (event->event_id != HTTP_EVENT_ON_DATA ||
        response == nullptr || event->data_len <= 0) {
        return ESP_OK;
    }

    const size_t incoming = static_cast<size_t>(event->data_len);
    if (response->length + incoming >= response->capacity) {
        ESP_LOGE(kTag, "Events response is too large");
        return ESP_FAIL;
    }

    memcpy(response->data + response->length, event->data, incoming);
    response->length += incoming;
    response->data[response->length] = '\0';
    return ESP_OK;
}

// Copies a JSON string, cutting it to fit without splitting a UTF-8 "£".
void CopyString(const cJSON *object, const char *name, char *out,
                size_t size) {
    out[0] = '\0';
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        return;
    }
    size_t length = strlen(value->valuestring);
    if (length >= size) {
        length = size - 1;
        if (length > 0 &&
            static_cast<unsigned char>(value->valuestring[length - 1]) ==
                0xC2) {
            --length;
        }
    }
    memcpy(out, value->valuestring, length);
    out[length] = '\0';
}

}  // namespace

extern "C" esp_err_t
zectrix_dashboard_events_fetch(zectrix_events_data_t *out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (kUrl[0] == '\0') {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char *body = static_cast<char *>(malloc(kResponseCapacity));
    if (body == nullptr) {
        ESP_LOGE(kTag, "Out of memory for response");
        return ESP_ERR_NO_MEM;
    }
    body[0] = '\0';
    ResponseBuffer response = {body, 0, kResponseCapacity};

    esp_http_client_config_t config = {};
    config.url = kUrl;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 15000;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        free(body);
        return ESP_FAIL;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || response.length == 0) {
        ESP_LOGW(kTag, "Events request failed: %s, HTTP %d",
                 esp_err_to_name(err), status);
        free(body);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == nullptr) {
        ESP_LOGW(kTag, "Could not parse events JSON");
        return ESP_FAIL;
    }

    const cJSON *screen = cJSON_GetObjectItemCaseSensitive(root, "screen");
    if (!cJSON_IsArray(screen)) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Events JSON has no screen array");
        return ESP_FAIL;
    }

    zectrix_events_data_t parsed = {};
    const cJSON *line = nullptr;
    cJSON_ArrayForEach(line, screen) {
        if (parsed.count >= ZECTRIX_EVENT_LINES) {
            break;
        }
        zectrix_event_line_t &entry = parsed.lines[parsed.count];
        CopyString(line, "when", entry.when, sizeof(entry.when));
        CopyString(line, "text", entry.text, sizeof(entry.text));
        CopyString(line, "tag", entry.tag, sizeof(entry.tag));
        if (entry.text[0] != '\0') {
            ++parsed.count;
        }
    }

    // Items for the full-screen view, best first as curated.
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
        if (parsed.item_count >= ZECTRIX_EVENT_ITEMS) {
            break;
        }
        zectrix_event_item_t &entry = parsed.items[parsed.item_count];
        CopyString(item, "when", entry.when, sizeof(entry.when));
        CopyString(item, "title", entry.title, sizeof(entry.title));
        CopyString(item, "venue", entry.venue, sizeof(entry.venue));
        CopyString(item, "price", entry.price, sizeof(entry.price));
        const bool free = strncasecmp(entry.price, "free", 4) == 0;
        const char *tag =
            free ? "FREE"
            : cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "deal"))
                ? "DEAL"
            : cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "book_now"))
                ? "BOOK"
                : "";
        strncpy(entry.tag, tag, sizeof(entry.tag) - 1);
        if (entry.title[0] != '\0') {
            ++parsed.item_count;
        }
    }
    cJSON_Delete(root);

    parsed.valid = parsed.count > 0;
    *out = parsed;
    ESP_LOGI(kTag, "Loaded %d event lines, %d items", parsed.count,
             parsed.item_count);
    return parsed.valid ? ESP_OK : ESP_FAIL;
}
