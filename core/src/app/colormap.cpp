#include "app/colormap.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace SharkSDR {
namespace {

struct Stop {
    float pos;
    float r, g, b;  // 0..255
};

uint32_t pack(float r, float g, float b) {
    auto c = [](float v) { return int(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
    return IM_COL32(c(r), c(g), c(b), 255);
}

Colormap fromStops(const char* name, std::vector<Stop> stops) {
    Colormap cm;
    cm.name = name;
    for (int i = 0; i < 256; i++) {
        float t = float(i) / 255.0f;
        size_t k = 1;
        while (k < stops.size() - 1 && stops[k].pos < t) k++;
        const Stop& a = stops[k - 1];
        const Stop& b = stops[k];
        float u = b.pos > a.pos ? std::clamp((t - a.pos) / (b.pos - a.pos), 0.0f, 1.0f) : 0.0f;
        cm.lut[size_t(i)] = pack(a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u);
    }
    return cm;
}

// Google "Turbo" colormap, polynomial approximation.
Colormap turbo() {
    Colormap cm;
    cm.name = "Turbo";
    for (int i = 0; i < 256; i++) {
        double x = double(i) / 255.0;
        double r = 0.13572138 + x * (4.61539260 + x * (-42.66032258 + x * (132.13108234 + x * (-152.94239396 + x * 59.28637943))));
        double g = 0.09140261 + x * (2.19418839 + x * (4.84296658 + x * (-14.18503333 + x * (4.27729857 + x * 2.82956604))));
        double b = 0.10667330 + x * (12.64194608 + x * (-60.58204836 + x * (110.36276771 + x * (-89.90310912 + x * 27.34824973))));
        cm.lut[size_t(i)] = pack(float(r * 255.0), float(g * 255.0), float(b * 255.0));
    }
    // Darken the lower quarter so that the noise floor stays calm.
    for (int i = 0; i < 64; i++) {
        float f = 0.2f + 0.8f * float(i) / 64.0f;
        uint32_t c = cm.lut[size_t(i)];
        cm.lut[size_t(i)] = pack(float(c & 0xff) * f, float((c >> 8) & 0xff) * f, float((c >> 16) & 0xff) * (0.3f + 0.7f * f));
    }
    return cm;
}

}  // namespace

const std::vector<Colormap>& colormaps() {
    static const std::vector<Colormap> maps = [] {
        std::vector<Colormap> m;
        m.push_back(turbo());
        m.push_back(fromStops("Classic", {{0.00f, 0, 0, 16},
                                          {0.20f, 0, 0, 120},
                                          {0.40f, 0, 110, 255},
                                          {0.60f, 0, 230, 200},
                                          {0.75f, 250, 250, 0},
                                          {0.90f, 255, 90, 0},
                                          {1.00f, 255, 0, 0}}));
        m.push_back(fromStops("Viridis", {{0.00f, 68, 1, 84},
                                          {0.25f, 59, 82, 139},
                                          {0.50f, 33, 145, 140},
                                          {0.75f, 94, 201, 98},
                                          {1.00f, 253, 231, 37}}));
        m.push_back(fromStops("Inferno", {{0.00f, 0, 0, 4},
                                          {0.20f, 66, 10, 104},
                                          {0.40f, 147, 38, 103},
                                          {0.60f, 221, 81, 58},
                                          {0.80f, 252, 165, 10},
                                          {1.00f, 252, 255, 164}}));
        m.push_back(fromStops("Grayscale", {{0.0f, 0, 0, 0}, {1.0f, 255, 255, 255}}));
        return m;
    }();
    return maps;
}

}  // namespace SharkSDR
