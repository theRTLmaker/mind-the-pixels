#include "zectrix_http.h"

#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "http";

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
        ESP_LOGE(kTag, "Response is larger than %u bytes",
                 static_cast<unsigned>(response->capacity));
        return ESP_FAIL;
    }

    memcpy(response->data + response->length, event->data, incoming);
    response->length += incoming;
    response->data[response->length] = '\0';
    return ESP_OK;
}

}  // namespace

extern "C" char *zectrix_http_get(const char *url, const char *api_key,
                                  size_t capacity) {
    char *body = static_cast<char *>(malloc(capacity));
    if (body == nullptr) {
        ESP_LOGE(kTag, "Out of memory for response");
        return nullptr;
    }
    body[0] = '\0';

    ResponseBuffer response = {body, 0, capacity};

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 15000;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        free(body);
        return nullptr;
    }

    if (api_key != nullptr && api_key[0] != '\0') {
        esp_http_client_set_header(client, "x-apikey", api_key);
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || response.length == 0) {
        ESP_LOGW(kTag, "Request failed: %s, HTTP %d", esp_err_to_name(err),
                 status);
        free(body);
        return nullptr;
    }
    return body;
}
