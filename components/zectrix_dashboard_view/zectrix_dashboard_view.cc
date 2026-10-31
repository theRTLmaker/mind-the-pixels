#include "zectrix_dashboard_view.h"

#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "zectrix_ascii_font_8x16.h"

// Dashboard: wide weather, compact trains and clock, larger events.

namespace {

// Trains refresh every minute, so this many failures means stale times.
constexpr int kTrainsStaleAfterFailures = 3;

struct Point {
    int x;
    int y;
};

// Lightning bolt, relative to its top-left corner.
constexpr Point kBolt[] = {
    {6, 0}, {12, 0}, {8, 7}, {13, 7}, {3, 20}, {5, 10}, {0, 10}};

// Gray levels for icons (0 = black, 15 = white). On a black/white refresh
// the canvas dithers them.
constexpr uint8_t kInk = 1;
constexpr uint8_t kSoftInk = 4;

// Even-odd scanline fill of a closed polygon offset by (ox, oy).
template <size_t N>
void FillPolygon(ZectrixCanvas& canvas, int ox, int oy,
                 const Point (&points)[N], uint8_t level) {
    int top = points[0].y;
    int bottom = points[0].y;
    for (const Point& p : points) {
        top = std::min(top, p.y);
        bottom = std::max(bottom, p.y);
    }
    for (int y = top; y <= bottom; ++y) {
        int crossings[N];
        size_t count = 0;
        for (size_t i = 0; i < N; ++i) {
            const Point& a = points[i];
            const Point& b = points[(i + 1) % N];
            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
                crossings[count++] =
                    a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
            }
        }
        std::sort(crossings, crossings + count);
        for (size_t i = 0; i + 1 < count; i += 2) {
            canvas.FillRectGray(ox + crossings[i], oy + y,
                                crossings[i + 1] - crossings[i] + 1, 1,
                                level);
        }
    }
}

void FillCircle(ZectrixCanvas& canvas, int cx, int cy, int r, uint8_t level) {
    if (r < 0) {
        return;
    }
    for (int dy = -r; dy <= r; ++dy) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) {
            ++dx;
        }
        canvas.FillRectGray(cx - dx, cy + dy, dx * 2 + 1, 1, level);
    }
}

// Two-pixel gray line, which holds up better than one pixel on the panel.
void ThickLine(ZectrixCanvas& canvas, int x0, int y0, int x1, int y1,
               uint8_t level = kInk) {
    const bool flat = std::abs(x1 - x0) >= std::abs(y1 - y0);
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        canvas.GrayPixel(x0, y0, level);
        canvas.GrayPixel(flat ? x0 : x0 + 1, flat ? y0 + 1 : y0, level);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

// Sun: shaded disc with a dark ring, a highlight and eight rays.
void DrawSun(ZectrixCanvas& canvas, int cx, int cy, int r) {
    FillCircle(canvas, cx, cy, r, kInk);
    FillCircle(canvas, cx, cy, r - 2, 9);
    FillCircle(canvas, cx - r / 4, cy - r / 4, r / 2, 12);
    const int inner = r + 4;
    const int outer = r + 9;
    const int inner_d = inner * 7 / 10;
    const int outer_d = outer * 7 / 10;
    ThickLine(canvas, cx, cy - inner, cx, cy - outer);
    ThickLine(canvas, cx, cy + inner, cx, cy + outer);
    ThickLine(canvas, cx - inner, cy, cx - outer, cy);
    ThickLine(canvas, cx + inner, cy, cx + outer, cy);
    ThickLine(canvas, cx - inner_d, cy - inner_d, cx - outer_d, cy - outer_d);
    ThickLine(canvas, cx + inner_d, cy - inner_d, cx + outer_d, cy - outer_d);
    ThickLine(canvas, cx - inner_d, cy + inner_d, cx - outer_d, cy + outer_d);
    ThickLine(canvas, cx + inner_d, cy + inner_d, cx + outer_d, cy + outer_d);
}

// Puffy cloud: three bumps on a flat base. (cx, cy) is the base's centre
// line; the cloud spans about 46 x 26 at size 10. grow widens the shape
// for outlines.
void CloudShape(ZectrixCanvas& canvas, int cx, int cy, int size, int grow,
                uint8_t level) {
    const int bump = size * 8 / 10;
    const int small = bump + grow;
    const int big = size + 2 + grow;
    FillCircle(canvas, cx - size - 3, cy, small, level);
    FillCircle(canvas, cx + size + 3, cy + 1, small - 1, level);
    FillCircle(canvas, cx - 1, cy - size / 2 - 2, big, level);
    canvas.FillRectGray(cx - size - 3, cy - grow, (size + 3) * 2,
                        bump + 1 + grow * 2, level);
}

// Cloud with a 2 px outline and a slightly darker underside. It is filled,
// so it hides whatever was drawn behind it.
void DrawCloud(ZectrixCanvas& canvas, int cx, int cy, int size, uint8_t fill,
               uint8_t outline = kInk) {
    CloudShape(canvas, cx, cy, size, 2, outline);
    CloudShape(canvas, cx, cy, size, 0, fill >= 2 ? fill - 2 : 0);
    CloudShape(canvas, cx, cy - 3, size, -1, fill);
}

// Raindrop with its round end at (x, y).
void DrawDrop(ZectrixCanvas& canvas, int x, int y, uint8_t level) {
    FillCircle(canvas, x, y, 3, level);
    canvas.FillRectGray(x - 2, y - 4, 5, 2, level);
    canvas.FillRectGray(x - 1, y - 6, 3, 2, level);
    canvas.FillRectGray(x, y - 8, 1, 2, level);
}

// Weather icon for an Open-Meteo WMO code, centred near (cx, cy) in a
// roughly 60 x 60 box.
void DrawWeatherIcon(ZectrixCanvas& canvas, int cx, int cy, int code) {
    const bool fog = (code == 45 || code == 48);
    const bool drizzle = (code >= 51 && code <= 57);
    const bool rain = (code >= 61 && code <= 67) ||
                      (code >= 80 && code <= 82);
    const bool snow = (code >= 71 && code <= 77) ||
                      (code >= 85 && code <= 86);
    const bool storm = (code >= 95);

    if (code == 0) {
        DrawSun(canvas, cx, cy, 11);
        return;
    }
    if (code == 1) {
        DrawSun(canvas, cx - 4, cy - 5, 10);
        DrawCloud(canvas, cx + 10, cy + 14, 6, 14);
        return;
    }
    if (code == 2) {
        DrawSun(canvas, cx - 10, cy - 10, 8);
        DrawCloud(canvas, cx + 5, cy + 9, 10, 14);
        return;
    }
    if (code == 3) {
        DrawCloud(canvas, cx + 8, cy - 4, 8, 9, kSoftInk);
        DrawCloud(canvas, cx - 3, cy + 8, 10, 13);
        return;
    }

    // Everything else: a cloud with something falling out of it. Rain and
    // storm clouds are darker.
    const int base = cy - 2;
    const uint8_t fill = storm ? 6 : (rain ? 10 : (drizzle ? 12 : 13));
    DrawCloud(canvas, cx, base, 10, fill);
    const int below = base + 14;

    if (fog) {
        ThickLine(canvas, cx - 22, below + 2, cx + 16, below + 2, 3);
        ThickLine(canvas, cx - 16, below + 8, cx + 22, below + 8, 6);
        ThickLine(canvas, cx - 20, below + 14, cx + 12, below + 14, 9);
    } else if (snow) {
        for (int i = 0; i < 3; ++i) {
            const int fx = cx - 14 + i * 14;
            const int fy = below + 6 + (i == 1 ? 7 : 0);
            ThickLine(canvas, fx - 5, fy, fx + 4, fy);
            ThickLine(canvas, fx - 3, fy - 5, fx + 2, fy + 4);
            ThickLine(canvas, fx + 2, fy - 5, fx - 3, fy + 4);
        }
    } else if (storm) {
        FillPolygon(canvas, cx - 5, below + 1, kBolt, 8);
        FillPolygon(canvas, cx - 6, below, kBolt, kInk);
        DrawDrop(canvas, cx - 16, below + 7, 5);
        DrawDrop(canvas, cx + 15, below + 7, 5);
    } else if (rain) {
        for (int i = 0; i < 3; ++i) {
            DrawDrop(canvas, cx - 12 + i * 12, below + 7 + (i == 1 ? 7 : 0),
                     4);
        }
    } else if (drizzle) {
        for (int i = 0; i < 6; ++i) {
            const int dx = cx - 16 + (i % 3) * 12 + (i / 3) * 5;
            const int dy = below + 3 + (i / 3) * 8;
            canvas.FillRectGray(dx, dy, 2, 4, 5);
        }
    }
}

// Gray for labels and secondary text, and for hairline dividers.
constexpr uint8_t kLabel = 3;
constexpr uint8_t kRule = 6;

// TRMNL16 text in a gray level. Only for areas outside the partial-refresh
// windows, which must stay pure black and white.
void TextGray(ZectrixCanvas& canvas, int x, int y, const char* text,
              uint8_t level) {
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(text);
         *cursor != '\0'; ++cursor) {
        unsigned char code = *cursor;
        if (code < 0x20 || code > 0x7e) {
            code = '?';
        }
        const uint16_t* glyph = kZectrixAsciiFont8x16[code - 0x20];
        const int width = kZectrixAsciiFontWidths[code - 0x20];
        for (int row = 0; row < 16; ++row) {
            for (int column = 0; column < width; ++column) {
                if (glyph[row] & (1U << (15 - column))) {
                    canvas.GrayPixel(x + column, y + row, level);
                }
            }
        }
        x += width;
    }
}

// Degree sign: a small ring whose top-left is at (x, y).
void DrawDegree(ZectrixCanvas& canvas, int x, int y, int r, int thickness,
                uint8_t level) {
    FillCircle(canvas, x + r, y + r, r, level);
    FillCircle(canvas, x + r, y + r, r - thickness, 15);
}

// Small solid triangle, pointing up or down, centred on cx.
void DrawArrow(ZectrixCanvas& canvas, int cx, int top, bool up,
               uint8_t level) {
    for (int row = 0; row < 5; ++row) {
        const int half = up ? row : 4 - row;
        canvas.FillRectGray(cx - half, top + row, half * 2 + 1, 1, level);
    }
}

// Hour from "HH:MM", or -1 when the clock is not set yet.
int HourFromClock(const char* clock_text) {
    if (clock_text == nullptr || clock_text[0] < '0' || clock_text[0] > '9' ||
        clock_text[1] < '0' || clock_text[1] > '9') {
        return -1;
    }
    return (clock_text[0] - '0') * 10 + (clock_text[1] - '0');
}

// Train status, right-aligned to the card's inner edge.
void DrawStatus(ZectrixCanvas& canvas, int y, const char* text,
                int right = 384) {
    canvas.Text(right - canvas.TextWidth(text), y, text);
}

// Right edge of train-row text when the occupancy icon follows it.
constexpr int kTrainTextRight = 368;

// Typical busyness as three rising bars, filled up to the level: quiet
// (up to 30%), busy (up to 70%), very busy. Black and white only, like the
// rest of the train card.
void DrawLoading(ZectrixCanvas& canvas, int y, int percent) {
    if (percent < 0) {
        return;
    }
    const int level = percent <= 30 ? 1 : percent <= 70 ? 2 : 3;
    for (int bar = 0; bar < 3; ++bar) {
        const int height = 6 + bar * 3;
        const int x = 374 + bar * 6;
        const int top = y + 14 - height;
        if (bar < level) {
            canvas.FillRect(x, top, 4, height, true);
        } else {
            canvas.Rect(x, top, 4, height, true);
        }
    }
}

}  // namespace

void ZectrixDashboardDrawWeather(ZectrixCanvas& canvas,
                                 const zectrix_weather_data_t* weather,
                                 int current_hour) {
    // Top-left, no box: icon and temperature, then either the hourly rain
    // chart (rainy days) or humidity and wind (dry days).
    if (weather == nullptr) {
        canvas.Text(12, 40, "Weather unavailable");
        TextGray(canvas, 12, 60, "Check Wi-Fi", kLabel);
        return;
    }

    DrawWeatherIcon(canvas, 40, 44, weather->weather_code);

    // Current temperature, large, with the day's high over low beside it.
    char temperature[16];
    std::snprintf(temperature, sizeof(temperature), "%.1f",
                  weather->temperature_c);
    canvas.Text(78, 10, temperature, 2);
    const int degree_x = 78 + canvas.TextWidth(temperature, 2) + 2;
    DrawDegree(canvas, degree_x, 14, 3, 2, kInk);

    char high[8];
    char low[8];
    std::snprintf(high, sizeof(high), "%.0f", weather->max_temperature_c);
    std::snprintf(low, sizeof(low), "%.0f", weather->min_temperature_c);
    const int range_x = degree_x + 16;
    DrawArrow(canvas, range_x + 3, 14, true, kInk);
    canvas.Text(range_x + 10, 8, high);
    DrawArrow(canvas, range_x + 3, 32, false, kInk);
    canvas.Text(range_x + 10, 26, low);

    char feels[24];
    std::snprintf(feels, sizeof(feels), "%.1f", weather->feels_like_c);
    TextGray(canvas, 80, 48, "Feels like", kLabel);
    const int feels_x = 80 + canvas.TextWidth("Feels like ");
    canvas.Text(feels_x, 48, feels);
    DrawDegree(canvas, feels_x + canvas.TextWidth(feels) + 1, 50, 2, 1, kInk);

    if (ZectrixDashboardHasRain(weather)) {
        ZectrixDashboardDrawRainChart(canvas, *weather, current_hour);
        return;
    }

    char humidity[16];
    char wind[16];
    std::snprintf(humidity, sizeof(humidity), "%d%%",
                  weather->humidity_percent);
    std::snprintf(wind, sizeof(wind), "%.0f km/h", weather->wind_speed_kmh);
    canvas.Text(12, 88, "No rain today");
    TextGray(canvas, 12, 108, "Humidity", kLabel);
    canvas.Text(92, 108, humidity);
    TextGray(canvas, 12, 128, "Wind", kLabel);
    canvas.Text(92, 128, wind);
}

void ZectrixDashboardDrawTrains(ZectrixCanvas& canvas,
                                const zectrix_trains_data_t& trains) {
    // Top-right: train departures. Black and white only: this area is
    // updated with partial refreshes.
    canvas.FillRect(248, 34, 144, 114, false);
    canvas.Text(256, 40, "To St Pancras");
    canvas.Line(256, 58, 384, 58);

    if (!trains.valid) {
        if (trains.failed_fetches == 0) {
            canvas.Text(256, 64, "Loading...");
        } else {
            canvas.Text(256, 64, "No train data");
            canvas.Text(256, 86, "Retrying...");
        }
        return;
    }

    // Footer: when the times were fetched. After repeated failures the
    // cached times may be wrong, so flag them in an inverted bar.
    if (trains.updated[0] != '\0') {
        char footer[24];
        if (trains.failed_fetches >= kTrainsStaleAfterFailures) {
            std::snprintf(footer, sizeof(footer), "OLD - from %s",
                          trains.updated);
            canvas.FillRect(250, 129, 140, 18, true);
            canvas.Text(256, 130, footer, 1, true);
        } else {
            std::snprintf(footer, sizeof(footer), "Updated %s",
                          trains.updated);
            DrawStatus(canvas, 130, footer);
        }
    }

    for (int i = 0; i < ZECTRIX_TRAIN_COUNT; ++i) {
        const int y = 64 + i * 22;

        if (i >= trains.count) {
            continue;
        }

        const zectrix_train_t& train = trains.trains[i];

        char scheduled[6] = {};
        std::snprintf(scheduled, sizeof(scheduled), "%c%ch%c%c",
                      train.scheduled[0], train.scheduled[1],
                      train.scheduled[3], train.scheduled[4]);

        if (train.cancelled) {
            canvas.Text(256, y, scheduled);
            DrawStatus(canvas, y, "Cancelled");
            continue;
        }

        DrawLoading(canvas, y, train.loading_percent);
        if (std::strcmp(train.expected, "-----") == 0) {
            canvas.Text(256, y, scheduled);
            DrawStatus(canvas, y, "Delayed", kTrainTextRight);
        } else if (std::strcmp(train.scheduled, train.expected) == 0) {
            canvas.Text(256, y, scheduled);
        } else {
            char expected[6] = {};
            std::snprintf(expected, sizeof(expected), "%c%ch%c%c",
                          train.expected[0], train.expected[1],
                          train.expected[3], train.expected[4]);

            // Struck-out timetabled time -> predicted time. The strike is
            // diagonal: a horizontal one joins the top stroke of each "1"
            // into a "4" ("10h11" read as "10h44").
            canvas.Text(256, y, scheduled);
            canvas.Line(255, y + 13, 257 + canvas.TextWidth(scheduled), y + 2);
            canvas.Text(302, y, "->");
            canvas.Text(325, y, expected);
        }
    }
}

namespace {

// Copies text into out, cut with "..." so it fits max_width pixels. Never
// splits the two-byte UTF-8 "£".
void FitText(const ZectrixCanvas& canvas, const char* text, int max_width,
             char* out, size_t size) {
    std::snprintf(out, size, "%s", text);
    if (canvas.TextWidth(out) <= max_width) {
        return;
    }
    size_t length = std::strlen(out);
    while (length > 0) {
        --length;
        if (length > 0 &&
            static_cast<unsigned char>(out[length - 1]) == 0xC2) {
            --length;
        }
        while (length > 0 &&
               (out[length - 1] == ' ' || out[length - 1] == ',')) {
            --length;
        }
        if (length + 4 > size) {
            continue;
        }
        std::memcpy(out + length, "...", 4);
        if (canvas.TextWidth(out) <= max_width) {
            return;
        }
    }
    out[0] = '\0';
}

}  // namespace

void ZectrixDashboardDrawEvents(ZectrixCanvas& canvas,
                                const zectrix_events_data_t* events) {
    // Bottom: curated picks, below a hairline. One line each: when (gray),
    // what, and an inverted FREE/DEAL/BOOK tag on the right.
    TextGray(canvas, 12, 166, "Coming up", kLabel);

    if (events == nullptr || !events->valid) {
        TextGray(canvas, 12, 190, "No picks yet", kLabel);
        return;
    }

    for (int i = 0; i < events->count && i < ZECTRIX_EVENT_LINES; ++i) {
        const zectrix_event_line_t& line = events->lines[i];
        const int y = 190 + i * 24;

        int text_right = 388;
        if (line.tag[0] != '\0') {
            const int tag_width = canvas.TextWidth(line.tag) + 6;
            canvas.FillRect(388 - tag_width, y, tag_width, 16, true);
            canvas.Text(388 - tag_width + 3, y, line.tag, 1, true);
            text_right = 388 - tag_width - 6;
        }

        char when[16];
        FitText(canvas, line.when, 80, when, sizeof(when));
        TextGray(canvas, 12, y, when, kLabel);

        char text[48];
        FitText(canvas, line.text, text_right - 96, text, sizeof(text));
        canvas.Text(96, y, text);
    }
}

bool ZectrixDashboardHasRain(const zectrix_weather_data_t* weather) {
    if (weather == nullptr) {
        return false;
    }
    for (int hour = 0; hour < ZECTRIX_WEATHER_HOURS; ++hour) {
        if (weather->hourly_rain_percent[hour] > 0) {
            return true;
        }
    }
    return false;
}

void ZectrixDashboardDrawRainChart(ZectrixCanvas& canvas,
                                   const zectrix_weather_data_t& weather,
                                   int current_hour) {
    // Lower part of the weather area: one bar per hour, 0-100% over
    // kBarMax pixels, labelled every 6 h.
    constexpr int kLeft = 12;
    constexpr int kPitch = 9;
    constexpr int kBarWidth = 7;
    constexpr int kRight = kLeft + 23 * kPitch + kBarWidth;
    constexpr int kBaseline = 128;
    constexpr int kBarMax = 28;

    int peak = 0;
    int peak_hour = 0;
    for (int hour = 0; hour < ZECTRIX_WEATHER_HOURS; ++hour) {
        if (weather.hourly_rain_percent[hour] > peak) {
            peak = weather.hourly_rain_percent[hour];
            peak_hour = hour;
        }
    }
    char peak_text[24];
    std::snprintf(peak_text, sizeof(peak_text), "%d%% at %02dh", peak,
                  peak_hour);
    // Open-Meteo's probability covers any precipitation; call it snow on
    // snowy days.
    const int code = weather.weather_code;
    const char* label = ((code >= 71 && code <= 77) ||
                         (code >= 85 && code <= 86)) ? "Snow" : "Rain";
    TextGray(canvas, kLeft, 76, label, kLabel);
    canvas.Text(kLeft + canvas.TextWidth(label) + 4, 76, peak_text);

    // Dotted 50% guide, then the baseline.
    for (int x = kLeft; x < kRight; x += 3) {
        canvas.GrayPixel(x, kBaseline - kBarMax / 2, kRule);
    }
    canvas.FillRectGray(kLeft, kBaseline, kRight - kLeft, 1, kSoftInk);

    for (int hour = 0; hour < ZECTRIX_WEATHER_HOURS; ++hour) {
        const int x = kLeft + hour * kPitch;
        const int percent =
            std::clamp(weather.hourly_rain_percent[hour], 0, 100);
        if (percent > 0) {
            const int height = std::max(2, percent * kBarMax / 100);
            const bool past = current_hour >= 0 && hour < current_hour;
            canvas.FillRectGray(x, kBaseline - height, kBarWidth, height,
                                past ? 10 : (percent >= 50 ? 3 : 7));
        }
        if (hour == current_hour) {
            // "Now" marker: a small triangle under the baseline.
            DrawArrow(canvas, x + kBarWidth / 2, kBaseline + 2, true, kInk);
        }
        if (hour % 6 == 0) {
            char label[4];
            std::snprintf(label, sizeof(label), "%02d", hour);
            TextGray(canvas, x, kBaseline + 4, label, kLabel);
        }
    }
}

namespace {

// Battery outline with a level bar; a lightning bolt while charging.
// Black and white only: it shares the clock's partial-refresh window.
void DrawBattery(ZectrixCanvas& canvas, int x, int y,
                 const ZectrixDashboardBattery& battery) {
    constexpr int kWidth = 20;
    constexpr int kHeight = 11;
    canvas.Rect(x, y, kWidth, kHeight, true);
    canvas.FillRect(x + kWidth, y + 3, 2, kHeight - 6, true);

    if (battery.charging && !battery.full) {
        // Bolt: two strokes meeting in the middle, drawn twice for weight.
        for (int dx = 0; dx < 2; ++dx) {
            canvas.Line(x + 11 + dx, y + 2, x + 8 + dx, y + 5, true);
            canvas.Line(x + 8 + dx, y + 5, x + 12 + dx, y + 5, true);
            canvas.Line(x + 12 + dx, y + 5, x + 9 + dx, y + 8, true);
        }
        return;
    }

    const int percent =
        battery.full ? 100 : std::clamp(battery.percent, 0, 100);
    const int inner = kWidth - 4;
    // Never fully empty while valid, so a low battery still shows a sliver.
    const int fill = std::max(1, (inner * percent + 50) / 100);
    canvas.FillRect(x + 2, y + 2, fill, kHeight - 4, true);
}

}  // namespace

void ZectrixDashboardDrawClock(ZectrixCanvas& canvas, const char* text,
                               const ZectrixDashboardBattery* battery) {
    // Clock above the train card, centred in the right column, with the
    // battery icon at the right edge.
    canvas.FillRect(248, 0, 152, 32, false);
    const int clock_x = 312 - canvas.TextWidth(text, 2) / 2;
    canvas.Text(clock_x, 4, text, 2);
    if (battery != nullptr && battery->valid) {
        DrawBattery(canvas, 366, 12, *battery);
    }
}

void ZectrixDashboardDrawNight(ZectrixCanvas& canvas,
                               const zectrix_weather_data_t* weather,
                               const zectrix_events_data_t* events,
                               const ZectrixDashboardBattery* battery,
                               const char* wake_text,
                               const char* updated_text) {
    canvas.Clear();
    ZectrixDashboardDrawWeather(canvas, weather, HourFromClock(updated_text));
    ZectrixDashboardDrawEvents(canvas, events);

    // Right column instead of clock and trains: the panel only changes
    // once an hour at night, so no clock.
    canvas.Text(256, 8, "Good night");
    if (battery != nullptr && battery->valid) {
        DrawBattery(canvas, 366, 12, *battery);
    }
    canvas.Line(256, 30, 384, 30);
    TextGray(canvas, 256, 40, wake_text, kLabel);
    TextGray(canvas, 256, 62, "OK to wake now", kLabel);
    if (updated_text != nullptr && updated_text[0] != '\0') {
        char updated[24];
        std::snprintf(updated, sizeof(updated), "Updated %s", updated_text);
        TextGray(canvas, 384 - canvas.TextWidth(updated), 130, updated,
                 kLabel);
    }

    canvas.FillRectGray(243, 12, 1, 136, kRule);
    canvas.FillRectGray(12, 156, 376, 1, kRule);
}

void ZectrixDashboardDrawAll(ZectrixCanvas& canvas,
                             const zectrix_weather_data_t* weather,
                             const zectrix_trains_data_t& trains,
                             const zectrix_events_data_t* events,
                             const ZectrixDashboardBattery* battery,
                             const char* clock_text) {
    canvas.Clear();
    ZectrixDashboardDrawWeather(canvas, weather, HourFromClock(clock_text));
    ZectrixDashboardDrawTrains(canvas, trains);
    ZectrixDashboardDrawEvents(canvas, events);
    ZectrixDashboardDrawClock(canvas, clock_text, battery);

    // Hairlines instead of boxes, kept outside the partial-refresh windows.
    canvas.FillRectGray(243, 12, 1, 136, kRule);
    canvas.FillRectGray(12, 156, 376, 1, kRule);
}
