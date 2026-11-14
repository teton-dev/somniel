// CPU compositor for the dual-screen host: draws the layout ops of dsui::Host into an RGBA8 canvas.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "dsui.h"

namespace dsui {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba; // straight alpha
    mutable int8_t opaque = -1; // -1 unknown, 0 has transparent texels, 1 every texel opaque (a plain copy)
    bool is_opaque() const {
        if (opaque < 0) {
            opaque = 1;
            for (size_t i = 3; i < rgba.size(); i += 4)
                if (rgba[i] != 255) { opaque = 0; break; }
        }
        return opaque == 1;
    }
};

struct Resources {
    std::function<const Image *(const std::string &key)> image; // null: not available (yet)
    const Image *font_atlas = nullptr;                         // white glyphs, coverage in alpha
    const FontMetrics *font = nullptr;
};

// Draws `ops` over a cleared canvas of w x h pixels (rgba is resized). `fade` (0..1) dims the page in from black.
void render(const Manifest &man, const std::vector<Op> &ops, int w, int h, std::vector<uint8_t> &rgba, const Resources &res,
            double fade = 1.0);

// Text helpers (also used by hit-testing and tests).
int measure_text(const Resources &res, const std::string &text, int scale, bool markup);

} // namespace dsui
