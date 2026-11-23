// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_dbno_draw.h"

#include <algorithm>
#include <cmath>

#include "coop_draw.h"
#include "coop_i18n.h"

namespace CoopDbnoDraw {

std::string Caption(const CoopDbno::View& view) {
    using Kind = CoopDbno::View::Kind;
    // The table's words around the name (data): "You are down", "Saria is reviving you", "Hold A to revive Saria".
    if (view.kind == Kind::downed) {
        return view.name.empty() ? CoopI18n::Tr("You are down") : view.name + " " + CoopI18n::Tr("is reviving you");
    }
    if (view.kind == Kind::spectating) {
        return CoopI18n::Tr("Watching") + " " + view.name +
               (view.elsewhere ? " " + CoopI18n::Tr("(out of sight)") : std::string()) + "  " +
               CoopI18n::Tr("A: next player");
    }
    return CoopI18n::Tr(view.holding ? "Reviving" : "Hold A to revive") + " " + view.name;
}

std::string Progress(float progress) {
    if (!(progress > 0.0f)) {
        return {};
    }
    const int share = progress >= 1.0f ? 100 : static_cast<int>(progress * 100.0f);
    return std::to_string(share) + "%";
}

std::string Line(const CoopDbno::View& view) {
    std::string line = Caption(view);
    if (view.kind == CoopDbno::View::Kind::spectating) {
        return line;
    }
    line += "  " + CoopDbno::Clock(view.left_us);
    const bool reviving = view.kind == CoopDbno::View::Kind::downed ? !view.name.empty() : view.holding;
    if (const std::string p = Progress(view.progress); reviving && !p.empty()) {
        line += "  " + p;
    }
    return line;
}

float LinePx(float width, float height, float measured_at_px, float measured_width) {
    const float px = std::clamp(height * 0.03f, 14.0f, 40.0f);
    if (!(measured_at_px > 0.0f) || !(measured_width > 0.0f)) {
        return px;
    }
    const float fit = 0.9f * width * measured_at_px / measured_width;   // text width scales with its size
    return std::max(12.0f, std::min(px, fit));
}

void Draw(const HosCoopDraw& d, const CoopDbno::View& view) {
    using CoopDraw::Rgba;
    const std::string line = Line(view);
    const float probe = 20.0f;
    const float px = LinePx(d.width, d.height, probe, d.text_width(d.user, probe, line.c_str()));
    const float w = d.text_width(d.user, px, line.c_str());
    const float x = d.width * 0.5f - w * 0.5f;
    const float y = d.height * (view.kind == CoopDbno::View::Kind::spectating ? 0.86f : 0.78f);
    // The clock's last ten seconds in the game's warning red; otherwise white. A soft dark outline, as the game's
    // own HUD text has, so it reads on sky and on grass (no box).
    const bool late = view.kind == CoopDbno::View::Kind::downed && view.left_us < 10'000'000;
    const std::uint32_t fill = late ? Rgba(255, 110, 90, 255) : Rgba(255, 255, 255, 255);
    const float o = std::max(1.0f, px / 12.0f);
    static constexpr float kAround[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (const auto& k : kAround) {
        d.text(d.user, x + k[0] * o, y + k[1] * o, px, Rgba(0, 0, 0, 200), line.c_str());
    }
    d.text(d.user, x, y, px, fill, line.c_str());
}

}  // namespace CoopDbnoDraw
