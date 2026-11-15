// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_draw_hos.h"

#include <algorithm>
#include <cstddef>

namespace CoopDraw {

int HostFont(const Look* look, int font) {
    if (look == nullptr) {
        return 0;
    }
    switch (font) {
        case COOP_FONT_REGULAR: return look->regular;
        case COOP_FONT_MEDIUM: return look->medium;
        default: return 0;
    }
}

int HostImage(const Look* look, int image) {
    if (look == nullptr || image < COOP_IMAGE_HEART_0 || image > COOP_IMAGE_HEART_4) {
        return 0;
    }
    return look->hearts[static_cast<std::size_t>(image - COOP_IMAGE_HEART_0)];
}

float Measure::Width(int font, float px, const char* utf8) const {
    if (host == nullptr) {
        const int f = std::clamp(font, 0, COOP_FONT_COUNT - 1);
        return coop_text_width(tables[static_cast<std::size_t>(f)], px, utf8);
    }
    if (font >= 0 && host->use_font) {
        const int id = HostFont(look, font);
        if (id != host_font_) {
            host->use_font(host->user, id);
            host_font_ = id;
        }
    }
    return host->text_width(host->user, px, utf8);
}

void Measure::Restore() const {
    if (host != nullptr && host->use_font && host_font_ != 0) {
        host->use_font(host->user, 0);
        host_font_ = 0;
    }
}

void Replay(const CoopDrawList& list, const HosCoopDraw& d, const Look* look) {
    for (std::uint32_t i = 0; i < list.n; ++i) {
        const CoopDrawCmd& c = list.cmd[i];
        const float* v = c.v;
        switch (c.kind) {
            case COOP_DRAW_RECT:
                d.rect(d.user, v[0], v[1], v[2], v[3], c.rgba, v[4]);
                break;
            case COOP_DRAW_CIRCLE:
                d.circle(d.user, v[0], v[1], v[2], c.rgba, v[3]);
                break;
            case COOP_DRAW_TRIANGLE:
                d.triangle(d.user, v[0], v[1], v[2], v[3], v[4], v[5], c.rgba);
                break;
            case COOP_DRAW_TEXT:
                d.text(d.user, v[0], v[1], v[2], c.rgba, coop_draw_text_of(&list, &c));
                break;
            case COOP_DRAW_IMAGE:
                if (d.draw_image) {
                    d.draw_image(d.user, HostImage(look, c.id), v[0], v[1], v[2], v[3], c.rgba);
                }
                break;
            case COOP_DRAW_FONT:
                if (d.use_font) {
                    d.use_font(d.user, HostFont(look, c.id));
                }
                break;
            default:   // POLY, CLIP: nothing in this table draws them
                break;
        }
    }
}

}  // namespace CoopDraw
