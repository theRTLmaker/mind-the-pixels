#include "zectrix_panel.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr char kTag[] = "panel";
// Partials before a 1bpp full refresh clears ghosting.
constexpr int kPartialRefreshLimit = 8;
// Over a gray frame a full refresh is a ~7 s gray sequence, so allow more
// partials first: the clock and train card take about two per minute, so
// this is roughly every 30 minutes. Lower it if ghosting builds up.
constexpr int kGrayPartialRefreshLimit = 60;

}  // namespace

esp_err_t ZectrixPanel::RefreshFull() {
    if (epd_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err == ESP_OK) {
        err = zectrix_epd_refresh_full_1bpp(epd_, canvas_.data(), canvas_.size());
    }
    const esp_err_t off = zectrix_epd_power_off(epd_);
    partial_count_ = 0;
    gray_base_ = false;
    return err == ESP_OK ? off : err;
}

esp_err_t ZectrixPanel::RefreshFullGray() {
    if (epd_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t start = esp_timer_get_time();
    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err == ESP_OK) {
        err = zectrix_epd_refresh_full_4bpp(epd_, canvas_.gray_data(),
                                            canvas_.gray_size());
    }
    ESP_LOGD(kTag, "gray full refresh: %s, %lld ms", esp_err_to_name(err),
             (esp_timer_get_time() - start) / 1000);
    if (err == ESP_OK) {
        // Gray areas are dithered in the 1bpp frame, but partial windows
        // only cover black/white content, where it matches the panel.
        err = zectrix_epd_set_partial_base_1bpp(epd_, canvas_.data(),
                                                canvas_.size());
    }
    const esp_err_t off = zectrix_epd_power_off(epd_);
    partial_count_ = 0;
    gray_base_ = err == ESP_OK;
    return err == ESP_OK ? off : err;
}

esp_err_t ZectrixPanel::RefreshPartial(const zectrix_epd_rect_t& rect) {
    if (epd_ == nullptr || rect.x < 0 || rect.y < 0 || rect.width <= 0 ||
        rect.height <= 0 || (rect.x & 7) != 0 || (rect.width & 7) != 0 ||
        rect.x + rect.width > 400 || rect.y + rect.height > 300) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gray_base_ && partial_count_ >= kGrayPartialRefreshLimit) {
        return RefreshFullGray();
    }
    if (!gray_base_ && partial_count_ >= kPartialRefreshLimit) {
        return RefreshFull();
    }
    const size_t row_bytes = static_cast<size_t>(rect.width / 8);
    const size_t required = row_bytes * rect.height;
    if (required > partial_buffer_.size()) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (int row = 0; row < rect.height; ++row) {
        const uint8_t* source = canvas_.data() +
            static_cast<size_t>(rect.y + row) * ZectrixCanvas::kStride + rect.x / 8;
        std::memcpy(partial_buffer_.data() + static_cast<size_t>(row) * row_bytes,
                    source, row_bytes);
    }
    const int64_t start = esp_timer_get_time();
    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err == ESP_OK) {
        err = zectrix_epd_refresh_partial_1bpp(
            epd_, &rect, partial_buffer_.data(), required);
    }
    ESP_LOGD(kTag, "partial %d,%d %dx%d (#%d, gray base %d): %s, %lld ms",
             rect.x, rect.y, rect.width, rect.height, partial_count_ + 1,
             gray_base_, esp_err_to_name(err),
             (esp_timer_get_time() - start) / 1000);
    const esp_err_t off = zectrix_epd_power_off(epd_);
    if (err == ESP_OK) {
        ++partial_count_;
        err = off;
    }
    return err;
}

esp_err_t ZectrixPanel::Clear() {
    canvas_.Clear();
    return RefreshFull();
}
