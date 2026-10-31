// Mind the Pixels Mac simulator.
//
// Renders the dashboard with the firmware's own drawing code (ZectrixCanvas
// and zectrix_dashboard_view) and writes each scenario to PNG, plus an
// index.html contact sheet. Physical refresh, ghosting and panel tone are not
// simulated; check those on the NOTE4 itself.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "fixtures.h"
#include "png_writer.h"
#include "zectrix_canvas.h"
#include "zectrix_dashboard_view.h"

namespace {

void PrintUsage() {
    std::printf(
        "usage: mtp_sim [--out DIR] [--scale N] [--list] [scenario ...]\n"
        "  Renders every scenario when none is named.\n");
}

// Unpacks the 1bpp canvas (1=white) into 8-bit gray, enlarged by scale.
std::vector<uint8_t> ToGray(const ZectrixCanvas& canvas, int scale) {
    const int width = ZectrixCanvas::kWidth * scale;
    const int height = ZectrixCanvas::kHeight * scale;
    std::vector<uint8_t> gray(static_cast<size_t>(width) * height);
    const uint8_t* bits = canvas.data();
    for (int y = 0; y < height; ++y) {
        const int sy = y / scale;
        for (int x = 0; x < width; ++x) {
            const int sx = x / scale;
            const uint8_t byte = bits[sy * ZectrixCanvas::kStride + sx / 8];
            const bool white = (byte & (1U << (7 - (sx & 7)))) != 0;
            gray[static_cast<size_t>(y) * width + x] = white ? 255 : 0;
        }
    }
    return gray;
}

// Unpacks the 4bpp canvas (0=black, 15=white) into 8-bit gray, enlarged by
// scale. This is what a full 16-gray refresh puts on the panel.
std::vector<uint8_t> ToGray4(const ZectrixCanvas& canvas, int scale) {
    const int width = ZectrixCanvas::kWidth * scale;
    const int height = ZectrixCanvas::kHeight * scale;
    std::vector<uint8_t> gray(static_cast<size_t>(width) * height);
    const uint8_t* nibbles = canvas.gray_data();
    for (int y = 0; y < height; ++y) {
        const int sy = y / scale;
        for (int x = 0; x < width; ++x) {
            const size_t index =
                static_cast<size_t>(sy) * ZectrixCanvas::kWidth + x / scale;
            const uint8_t pair = nibbles[index / 2];
            const uint8_t level = (index & 1U) ? pair & 0x0F : pair >> 4;
            gray[static_cast<size_t>(y) * width + x] =
                static_cast<uint8_t>(level * 17);
        }
    }
    return gray;
}

bool Render(const Scenario& scenario, const std::string& out_dir, int scale) {
    static ZectrixCanvas canvas;
    if (std::strcmp(scenario.name, "splash") == 0) {
        ZectrixDashboardDrawSplash(canvas, "Connecting to Wi-Fi...");
    } else if (std::strcmp(scenario.name, "commute") == 0) {
        ZectrixDashboardDrawCommute(canvas, &scenario.weather, &kCommuteAm,
                                    &scenario.battery, scenario.clock);
    } else if (std::strcmp(scenario.name, "events-full") == 0) {
        ZectrixDashboardDrawEventsFull(canvas, scenario.events,
                                       &scenario.battery, scenario.clock);
    } else if (scenario.night) {
        ZectrixDashboardDrawNight(
            canvas, scenario.weather_ok ? &scenario.weather : nullptr,
            scenario.events, &scenario.battery, "Back at 05:00",
            scenario.clock);
    } else {
        ZectrixDashboardDrawAll(
            canvas, scenario.weather_ok ? &scenario.weather : nullptr,
            scenario.trains, scenario.events, &scenario.battery,
            scenario.clock);
    }

    const std::string base = out_dir + "/" + scenario.name;
    // name.png is the 16-gray frame; name-mono.png is the 1bpp frame used
    // by black/white and partial refreshes (gray shows as dither there).
    bool ok = WriteGrayPng(base + ".png", ZectrixCanvas::kWidth,
                           ZectrixCanvas::kHeight, ToGray4(canvas, 1));
    ok = ok && WriteGrayPng(base + "-mono.png", ZectrixCanvas::kWidth,
                            ZectrixCanvas::kHeight, ToGray(canvas, 1));
    if (scale > 1) {
        ok = ok && WriteGrayPng(base + "@" + std::to_string(scale) + "x.png",
                                ZectrixCanvas::kWidth * scale,
                                ZectrixCanvas::kHeight * scale,
                                ToGray4(canvas, scale));
    }
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", (base + ".png").c_str());
    return ok;
}

void WriteIndex(const std::string& out_dir,
                const std::vector<const Scenario*>& rendered) {
    FILE* file = std::fopen((out_dir + "/index.html").c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file,
        "<!doctype html><meta charset=utf-8><title>Mind the Pixels "
        "preview</title>\n<style>body{font:14px -apple-system,sans-serif;"
        "background:#2b2b2b;color:#eee;margin:24px}figure{display:inline-"
        "block;margin:0 24px 24px 0}img{width:800px;image-rendering:"
        "pixelated;display:block;box-shadow:0 2px 12px #0008}figcaption"
        "{margin-top:6px}small{color:#aaa}</style>\n"
        "<h1>Mind the Pixels</h1><p>400 &times; 300 shown at 2&times;. "
        "Rendered by the firmware drawing code.</p>\n");
    for (const Scenario* s : rendered) {
        std::fprintf(file,
            "<figure><img src=\"%s.png\" alt=\"%s\"><figcaption><b>%s"
            "</b> <small>%s</small></figcaption></figure>\n",
            s->name, s->name, s->name, s->description);
    }
    std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = "out";
    int scale = 3;
    std::vector<std::string> wanted;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--scale" && i + 1 < argc) {
            scale = std::atoi(argv[++i]);
            if (scale < 1 || scale > 8) {
                std::fprintf(stderr, "--scale must be 1..8\n");
                return 2;
            }
        } else if (arg == "--list") {
            for (const Scenario& s : Scenarios()) {
                std::printf("%-14s %s\n", s.name, s.description);
            }
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            PrintUsage();
            return 2;
        } else {
            wanted.push_back(arg);
        }
    }

    mkdir(out_dir.c_str(), 0755);

    std::vector<const Scenario*> rendered;
    bool ok = true;
    for (const Scenario& s : Scenarios()) {
        bool selected = wanted.empty();
        for (const std::string& name : wanted) {
            selected = selected || name == s.name;
        }
        if (selected) {
            ok = Render(s, out_dir, scale) && ok;
            rendered.push_back(&s);
        }
    }
    if (rendered.size() != (wanted.empty() ? Scenarios().size()
                                            : wanted.size())) {
        std::fprintf(stderr, "unknown scenario name; try --list\n");
        ok = false;
    }
    WriteIndex(out_dir, rendered);
    return ok ? 0 : 1;
}
