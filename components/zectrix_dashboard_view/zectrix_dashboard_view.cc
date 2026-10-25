#include "zectrix_dashboard_view.h"

#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <cstring>

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

// Minutes since midnight for an "HH:MM" string.
int MinutesOfDay(const char* time) {
    return ((time[0] - '0') * 10 + (time[1] - '0')) * 60 +
           (time[3] - '0') * 10 + (time[4] - '0');
}

// Train status, right-aligned to the card's inner edge.
void DrawStatus(ZectrixCanvas& canvas, int y, const char* text) {
    canvas.Text(384 - canvas.TextWidth(text), y, text);
}

}  // namespace

void ZectrixDashboardDrawWeather(ZectrixCanvas& canvas,
                                 const zectrix_weather_data_t* weather) {
    // Top-left: weather.
    canvas.Rect(8, 8, 232, 140);
    canvas.Text(16, 15, "WEATHER");

    if (weather == nullptr) {
        canvas.Text(16, 55, "Weather unavailable");
        canvas.Text(16, 77, "Check Wi-Fi");
        return;
    }

    DrawWeatherIcon(canvas, 44, 66, weather->weather_code);

    char temperature[24];
    char range[32];
    char rain[24];
    char feels_like[32];
    char conditions[32];

    std::snprintf(temperature, sizeof(temperature),
                  "%.1f C", weather->temperature_c);
    std::snprintf(range, sizeof(range), "L %.0f H %.0f C",
                  weather->min_temperature_c,
                  weather->max_temperature_c);
    std::snprintf(rain, sizeof(rain), "Rain today %d%%",
                  weather->rain_probability_percent);
    std::snprintf(feels_like, sizeof(feels_like),
                  "Feels %.1f C", weather->feels_like_c);
    std::snprintf(conditions, sizeof(conditions),
                  "RH %d%% Wind %.0fkm/h",
                  weather->humidity_percent, weather->wind_speed_kmh);

    canvas.Text(82, 43, temperature, 2);
    canvas.Text(82, 79, range);
    canvas.Text(82, 95, rain);
    canvas.Text(82, 111, feels_like);
    canvas.Text(82, 127, conditions);
}

void ZectrixDashboardDrawTrains(ZectrixCanvas& canvas,
                                const zectrix_trains_data_t& trains) {
    // Top-right: train departures.
    canvas.FillRect(248, 34, 144, 114, false);
    canvas.Rect(248, 34, 144, 114);
    canvas.Text(256, 41, "TRAINS -> ST PAN");
    canvas.Line(256, 59, 384, 59);

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
            canvas.FillRect(249, 129, 142, 18, true);
            canvas.Text(256, 130, footer, 1, true);
        } else {
            std::snprintf(footer, sizeof(footer), "Updated %s",
                          trains.updated);
            canvas.Line(256, 128, 384, 128);
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

        canvas.FillRect(256, y, 128, 18, false);

        if (train.cancelled) {
            canvas.Text(256, y, scheduled);
            DrawStatus(canvas, y, "Cancelled");
        } else if (std::strcmp(train.expected, "-----") == 0) {
            canvas.Text(256, y, scheduled);
            DrawStatus(canvas, y, "Delayed");
        } else if (std::strcmp(train.scheduled, train.expected) == 0) {
            canvas.Text(256, y, scheduled);
        } else {
            char expected[6] = {};
            std::snprintf(expected, sizeof(expected), "%c%ch%c%c",
                          train.expected[0], train.expected[1],
                          train.expected[3], train.expected[4]);

            // Lead with the time it will actually leave; a strikethrough
            // made "10h11" read as "10h44".
            int late = MinutesOfDay(train.expected) -
                       MinutesOfDay(train.scheduled);
            if (late < -720) {
                late += 1440;
            } else if (late > 720) {
                late -= 1440;
            }
            char status[16];
            std::snprintf(status, sizeof(status), "%+d min", late);

            canvas.Text(256, y, expected);
            DrawStatus(canvas, y, status);
        }
    }
}

void ZectrixDashboardDrawEvents(ZectrixCanvas& canvas,
                                bool leave_room_for_rain) {
    // Bottom: events and recommendations.
    canvas.Rect(8, 158, 384, 132);
    canvas.Text(16, 166, "COMING UP");
    canvas.Line(16, 185, 384, 185);
    canvas.Text(16, 194, "Events for you and Margarida");
    canvas.Text(16, 216, "Calendar not connected");
    if (!leave_room_for_rain) {
        canvas.Text(16, 238, "Recommendations not connected");
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
                                   const zectrix_weather_data_t& weather) {
    // One bar per hour, 0-100% over kBarMax pixels, labelled every 6 h.
    constexpr int kLeft = 72;
    constexpr int kPitch = 13;
    constexpr int kBarWidth = 10;
    constexpr int kBaseline = 269;
    constexpr int kBarMax = 30;

    canvas.Line(16, 236, 384, 236);

    int peak = 0;
    for (int hour = 0; hour < ZECTRIX_WEATHER_HOURS; ++hour) {
        peak = std::max(peak, weather.hourly_rain_percent[hour]);
    }
    char peak_text[8];
    std::snprintf(peak_text, sizeof(peak_text), "%d%%", peak);
    canvas.Text(16, 242, "RAIN");
    canvas.Text(16, 260, peak_text);

    // Dotted 50% guide, then the baseline.
    for (int x = kLeft; x < 384; x += 3) {
        canvas.Pixel(x, kBaseline - kBarMax / 2, true);
    }
    canvas.Line(kLeft, kBaseline, 383, kBaseline);

    for (int hour = 0; hour < ZECTRIX_WEATHER_HOURS; ++hour) {
        const int x = kLeft + hour * kPitch;
        const int percent =
            std::clamp(weather.hourly_rain_percent[hour], 0, 100);
        if (percent > 0) {
            const int height = std::max(2, percent * kBarMax / 100);
            canvas.FillRectGray(x, kBaseline - height, kBarWidth, height,
                                percent >= 50 ? 3 : 7);
        }
        if (hour % 6 == 0) {
            char label[4];
            std::snprintf(label, sizeof(label), "%02d", hour);
            canvas.Text(x + kBarWidth / 2 - canvas.TextWidth(label) / 2,
                        kBaseline + 2, label);
        }
    }
}

void ZectrixDashboardDrawClock(ZectrixCanvas& canvas, const char* text) {
    // Clock above the train card, centred in the right column.
    canvas.FillRect(248, 0, 144, 32, false);
    const int clock_x = 320 - canvas.TextWidth(text, 2) / 2;
    canvas.Text(clock_x, 4, text, 2);
}

void ZectrixDashboardDrawAll(ZectrixCanvas& canvas,
                             const zectrix_weather_data_t* weather,
                             const zectrix_trains_data_t& trains,
                             const char* clock_text) {
    canvas.Clear();
    ZectrixDashboardDrawWeather(canvas, weather);
    ZectrixDashboardDrawTrains(canvas, trains);
    const bool rain = ZectrixDashboardHasRain(weather);
    ZectrixDashboardDrawEvents(canvas, rain);
    if (rain) {
        ZectrixDashboardDrawRainChart(canvas, *weather);
    }
    ZectrixDashboardDrawClock(canvas, clock_text);
}
