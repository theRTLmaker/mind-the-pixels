#ifndef ZECTRIX_PANEL_H_
#define ZECTRIX_PANEL_H_

#include <array>
#include <cstdint>

#include "esp_err.h"
#include "zectrix_canvas.h"
#include "zectrix_epd.h"

// Owns the canvas and pushes it to the e-paper: 1bpp or 16-gray full
// refreshes, and 1bpp partial refreshes of byte-aligned windows on top.
class ZectrixPanel {
public:
    void SetEpd(zectrix_epd_handle_t epd) { epd_ = epd; }
    ZectrixCanvas& canvas() { return canvas_; }

    esp_err_t RefreshFull();
    // 16-gray full refresh; later partials draw over it (black/white only).
    esp_err_t RefreshFullGray();
    esp_err_t RefreshPartial(const zectrix_epd_rect_t& rect);
    esp_err_t Clear();

private:
    zectrix_epd_handle_t epd_ = nullptr;
    ZectrixCanvas canvas_;
    std::array<uint8_t, 50 * 300> partial_buffer_ = {};
    int partial_count_ = 0;
    bool gray_base_ = false;
};

#endif  // ZECTRIX_PANEL_H_
