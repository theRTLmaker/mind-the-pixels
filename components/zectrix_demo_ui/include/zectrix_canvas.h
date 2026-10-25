#ifndef ZECTRIX_CANVAS_H_
#define ZECTRIX_CANVAS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zectrix_epd.h"

class ZectrixCanvas {
public:
    static constexpr int kWidth = ZECTRIX_EPD_PANEL_WIDTH;
    static constexpr int kHeight = ZECTRIX_EPD_PANEL_HEIGHT;
    static constexpr int kStride = kWidth / 8;

    void Clear(bool white = true);
    void Pixel(int x, int y, bool black);
    void FillRect(int x, int y, int width, int height, bool black);
    void Rect(int x, int y, int width, int height, bool black = true);
    void Line(int x0, int y0, int x1, int y1, bool black = true);
    void Text(int x, int y, const char* text, int scale = 1,
              bool inverted = false);
    void TextCentered(int y, const char* text, int scale = 1,
                      bool inverted = false);
    int TextWidth(const char* text, int scale = 1) const;

    // 16-level gray, 0 = black and 15 = white. The 4bpp frame gets the exact
    // level; the 1bpp frame gets an ordered dither so it still reads as gray
    // on a black/white refresh. Black/white drawing updates both frames.
    void GrayPixel(int x, int y, uint8_t level);
    void FillRectGray(int x, int y, int width, int height, uint8_t level);

    // 1bpp frame: row-major, MSB first, 1 = white.
    uint8_t* data() { return pixels_.data(); }
    const uint8_t* data() const { return pixels_.data(); }
    size_t size() const { return pixels_.size(); }

    // 4bpp frame: two pixels per byte, left pixel in the high nibble.
    const uint8_t* gray_data() const { return gray_.data(); }
    size_t gray_size() const { return gray_.size(); }

private:
    void SetGray(int x, int y, uint8_t level);

    std::array<uint8_t, ZECTRIX_EPD_1BPP_FRAME_BYTES> pixels_ = {};
    // Heap-allocated so it lands in PSRAM on the device.
    std::vector<uint8_t> gray_ =
        std::vector<uint8_t>(ZECTRIX_EPD_4BPP_FRAME_BYTES, 0x00);
};

#endif  // ZECTRIX_CANVAS_H_
