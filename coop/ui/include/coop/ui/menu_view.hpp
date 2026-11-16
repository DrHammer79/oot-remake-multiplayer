// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's own menu screen (docs/ui.md "Own menu"): a tab strip, the page's rows, a help column with an optional
// picture, and a prompt strip, laid out into a CoopDrawList. Our own screen, never the game's menu state (the
// advice in the coordinator's v1 inputs). The rows are the ones Hookshot's menu shows on Longshot (hos_coop_menu.h:
// the same kinds and fields), so every platform shows the same page.
//
// Header only: DrawMenu takes the text measure as a template parameter (static dispatch; nothing indirect), any type
// with `float Width(int font, float px, const char* utf8) const`.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "coop/ui/drawlist.h"

namespace coop::ui {

// A row's kind: the numbers of hos_coop_menu.h's HOS_COOP_MENU_ACTION..NOTE.
enum class RowKind : std::uint8_t { action = 0, choice = 1, toggle = 2, heading = 3, note = 4 };

struct MenuRow {
    std::string name;
    std::string value;   // action, choice: the value cell's words ("" none)
    RowKind kind = RowKind::action;
    bool on = false;     // toggle
    bool left = false, right = false;   // choice: it can go that way
    bool fixed = false;  // cannot be used now: dim
};

// A convex shape of 3..8 points in the picture's square (0..1, y down), colour as the draw list's (0xAABBGGRR).
struct MenuShape {
    std::uint32_t rgba = 0;
    bool outline = false;
    std::vector<std::array<float, 2>> points;
};

struct MenuModel {
    std::vector<std::string> tabs;
    int tab = 0;
    std::vector<MenuRow> rows;
    int selected = -1;
    bool typing = false;
    std::string help_title, help, hint, gold;
    std::string change, confirm, back;   // the prompt strip's words ("" none)
    std::string off_word = "Off", on_word = "On";   // a toggle's halves, in the player's language
    std::vector<MenuShape> shapes;
    bool live = false;   // the picture's square is a clear window onto the game (no backing)
};

// Where DrawMenu put things, for the pointer.
struct MenuLayout {
    struct RowBox {
        int row = -1;
        float top = 0, bottom = 0;
    };
    std::vector<RowBox> rows;   // the rows shown, top down
    float rows_left = 0, rows_right = 0;
    float value_left = 0, value_right = 0;
    std::vector<std::array<float, 4>> tabs;   // each tab's box: x0 y0 x1 y1
    float u = 1;   // one of the game's 720 lines, in pixels
};

namespace menu_detail {

constexpr std::uint32_t Col(int r, int g, int b, int a) {
    return static_cast<std::uint32_t>(r & 0xFF) | static_cast<std::uint32_t>(g & 0xFF) << 8 |
           static_cast<std::uint32_t>(b & 0xFF) << 16 | static_cast<std::uint32_t>(a & 0xFF) << 24;
}
inline constexpr std::uint32_t kHighlight = Col(255, 206, 82, 255);
inline constexpr std::uint32_t kWhite = Col(255, 255, 255, 255);
inline constexpr std::uint32_t kDim = Col(170, 170, 170, 255);

inline std::uint32_t Faded(std::uint32_t rgba, bool fixed) {
    return fixed ? (rgba & 0x00FFFFFFu) | (110u << 24) : rgba;
}

// `s` cut to `room` pixels with "..." (never inside a UTF-8 character).
template <class M>
std::string Elided(const M& m, float px, const std::string& s, float room) {
    if (m.Width(COOP_FONT_UI, px, s.c_str()) <= room) {
        return s;
    }
    std::string t = s;
    while (!t.empty()) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) {
            t.pop_back();
        }
        if (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0xC0) {
            t.pop_back();
        }
        if (m.Width(COOP_FONT_UI, px, (t + "...").c_str()) <= room) {
            return t + "...";
        }
    }
    return "...";
}

// `s` in lines of at most `room` pixels, broken at spaces and at '\n' (a word wider than a line is cut).
template <class M>
std::vector<std::string> Wrapped(const M& m, float px, const std::string& s, float room) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    while (at <= s.size()) {
        const std::size_t end = std::min(s.find_first_of(" \n", at), s.size());
        const std::string word = s.substr(at, end - at);
        const std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && m.Width(COOP_FONT_UI, px, tried.c_str()) > room) {
            lines.push_back(line);
            line = word;
        } else {
            line = tried;
        }
        if (end < s.size() && s[end] == '\n') {
            lines.push_back(line);
            line.clear();
        }
        at = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    for (auto& l : lines) {
        l = Elided(m, px, l, room);
    }
    return lines;
}

// A triangle pointing left (dir -1) or right (1) centred on (x, y).
inline void Arrow(CoopDrawList& l, float x, float y, float r, int dir, std::uint32_t c) {
    const float d = static_cast<float>(dir);
    coop_draw_triangle(&l, x + d * r, y, x - d * r * 0.6f, y - r, x - d * r * 0.6f, y + r, c);
}

}  // namespace menu_detail

// Draws the menu over the whole picture (`l` begun for its size) and says where the rows went.
template <class M>
MenuLayout DrawMenu(CoopDrawList& l, const M& measure, const MenuModel& m) {
    using namespace menu_detail;
    MenuLayout out;
    const float w = l.width, h = l.height;
    const float s = std::min(w / 16.0f, h / 9.0f);
    const float u = 9.0f * s / 720.0f;
    out.u = u;
    const float pic_left = (w - 16.0f * s) / 2, pic_top = (h - 9.0f * s) / 2;
    const float x0 = pic_left + 80 * u, x1 = pic_left + 16.0f * s - 80 * u;
    const float y0 = pic_top + 50 * u, y1 = pic_top + 9.0f * s - 50 * u;
    coop_draw_font(&l, COOP_FONT_UI);
    coop_draw_rect(&l, 0, 0, w, h, Col(0, 0, 0, 140), 0);
    coop_draw_rect(&l, x0, y0, x1, y1, Col(14, 16, 22, 235), 12 * u);

    // The tab strip.
    const float tab_px = 26 * u;
    float tx = x0 + 30 * u;
    const float ty = y0 + 18 * u;
    for (std::size_t i = 0; i < m.tabs.size(); ++i) {
        const float tw = measure.Width(COOP_FONT_UI, tab_px, m.tabs[i].c_str());
        const std::array<float, 4> box{tx, ty, tx + tw + 32 * u, ty + tab_px + 16 * u};
        out.tabs.push_back(box);
        const bool active = static_cast<int>(i) == m.tab;
        if (active) {
            coop_draw_rect(&l, box[0], box[1], box[2], box[3], kHighlight, 8 * u);
        }
        coop_draw_text(&l, tx + 16 * u, ty + 8 * u, tab_px, active ? Col(20, 20, 20, 255) : kDim, m.tabs[i].c_str());
        tx = box[2] + 10 * u;
    }
    const float strip_bottom = ty + tab_px + 26 * u;
    coop_draw_rect(&l, x0 + 20 * u, strip_bottom, x1 - 20 * u, strip_bottom + 2 * u, Col(255, 255, 255, 50), 0);

    // The rows, scrolled so the selected one shows.
    const float split = x0 + (x1 - x0) * 0.6f;
    const float rl = x0 + 30 * u, rr = split - 10 * u;
    out.rows_left = rl;
    out.rows_right = rr;
    const float vl = rl + (rr - rl) * 0.55f, vr = rr - 10 * u;
    out.value_left = vl;
    out.value_right = vr;
    const float rows_top = strip_bottom + 16 * u, rows_bottom = y1 - 64 * u;
    const float line = 44 * u, px = 24 * u;
    const int n = static_cast<int>(m.rows.size());
    const int visible = std::max(1, static_cast<int>((rows_bottom - rows_top) / line));
    int first = 0;
    if (m.selected >= 0 && n > visible) {
        first = std::clamp(m.selected - visible / 2, 0, n - visible);
    }
    const int last = std::min(n, first + visible);
    for (int i = first; i < last; ++i) {
        const MenuRow& r = m.rows[static_cast<std::size_t>(i)];
        const float top = rows_top + static_cast<float>(i - first) * line;
        const float text_y = top + (line - px) / 2;
        out.rows.push_back({i, top, top + line});
        if (i == m.selected) {
            coop_draw_rect(&l, rl - 10 * u, top + 2 * u, rr, top + line - 2 * u, Col(255, 206, 82, 60), 6 * u);
            coop_draw_rect(&l, rl - 10 * u, top + 2 * u, rl - 6 * u, top + line - 2 * u, kHighlight, 0);
        }
        const std::uint32_t text = Faded(kWhite, r.fixed);
        switch (r.kind) {
            case RowKind::heading:
                coop_draw_text(&l, rl, text_y, px, kHighlight, Elided(measure, px, r.name, rr - rl).c_str());
                continue;
            case RowKind::note:
                coop_draw_text(&l, rl, text_y + 2 * u, px * 0.85f, kDim,
                               Elided(measure, px * 0.85f, r.name, rr - rl).c_str());
                continue;
            default:
                break;
        }
        coop_draw_text(&l, rl, text_y, px, text, Elided(measure, px, r.name, vl - rl - 12 * u).c_str());
        if (r.kind == RowKind::toggle) {
            const float mid = (vl + vr) / 2;
            const float hx0 = r.on ? mid : vl, hx1 = r.on ? vr : mid;
            coop_draw_rect(&l, hx0, top + 6 * u, hx1, top + line - 6 * u, Faded(Col(255, 206, 82, 110), r.fixed), 6 * u);
            const auto half = [&](const std::string& word, float a, float b, bool lit) {
                const std::string t = Elided(measure, px, word, b - a - 8 * u);
                const float tw = measure.Width(COOP_FONT_UI, px, t.c_str());
                coop_draw_text(&l, (a + b - tw) / 2, text_y, px, Faded(lit ? kWhite : kDim, r.fixed), t.c_str());
            };
            half(m.off_word, vl, mid, !r.on);
            half(m.on_word, mid, vr, r.on);
            continue;
        }
        const float ar = 8 * u;
        const float in_l = r.kind == RowKind::choice ? vl + 3 * ar : vl;
        const float in_r = r.kind == RowKind::choice ? vr - 3 * ar : vr;
        if (!r.value.empty()) {
            const std::string v = Elided(measure, px, r.value, in_r - in_l);
            const float vw = measure.Width(COOP_FONT_UI, px, v.c_str());
            const float vx = r.kind == RowKind::choice ? (in_l + in_r - vw) / 2 : in_r - vw;
            coop_draw_text(&l, vx, text_y, px, text, v.c_str());
        }
        if (r.kind == RowKind::choice) {
            const float cy = top + line / 2;
            Arrow(l, vl + ar, cy, ar, -1, Faded(r.left ? kWhite : Col(255, 255, 255, 60), r.fixed));
            Arrow(l, vr - ar, cy, ar, 1, Faded(r.right ? kWhite : Col(255, 255, 255, 60), r.fixed));
        }
    }
    if (first > 0) {   // more above / below
        coop_draw_triangle(&l, (rl + rr) / 2, rows_top - 10 * u, (rl + rr) / 2 - 8 * u, rows_top - 2 * u,
                           (rl + rr) / 2 + 8 * u, rows_top - 2 * u, kDim);
    }
    if (last < n) {
        const float b = rows_top + static_cast<float>(visible) * line;
        coop_draw_triangle(&l, (rl + rr) / 2, b + 10 * u, (rl + rr) / 2 - 8 * u, b + 2 * u, (rl + rr) / 2 + 8 * u,
                           b + 2 * u, kDim);
    }

    // The help column: the picture's square, then the help.
    const float hl = split + 20 * u, hr = x1 - 30 * u;
    float hy = rows_top;
    if (!m.shapes.empty() || m.live) {
        const float side = std::min(hr - hl, 300 * u);
        const float sx = hl + (hr - hl - side) / 2;
        if (!m.live) {
            coop_draw_rect(&l, sx, hy, sx + side, hy + side, Col(0, 0, 0, 120), 6 * u);
        }
        for (const MenuShape& shape : m.shapes) {
            const std::size_t pts = std::min<std::size_t>(shape.points.size(), COOP_DRAW_POLY_POINTS);
            if (pts < 3) {
                continue;
            }
            std::array<float, 2 * COOP_DRAW_POLY_POINTS> xy{};
            for (std::size_t k = 0; k < pts; ++k) {
                xy[2 * k] = sx + shape.points[k][0] * side;
                xy[2 * k + 1] = hy + shape.points[k][1] * side;
            }
            coop_draw_poly(&l, xy.data(), static_cast<int>(pts), shape.rgba, shape.outline ? 1 : 0);
        }
        hy += side + 16 * u;
    }
    const float help_px = 20 * u, help_line = 26 * u;
    const auto lines = [&](const std::string& words, std::uint32_t colour, float lpx) {
        for (const std::string& t : Wrapped(measure, lpx, words, hr - hl)) {
            if (hy + lpx > rows_bottom) {
                return;
            }
            coop_draw_text(&l, hl, hy, lpx, colour, t.c_str());
            hy += help_line * lpx / help_px;
        }
    };
    if (!m.help_title.empty()) {
        lines(m.help_title, kHighlight, 24 * u);
        hy += 4 * u;
    }
    lines(m.help, kWhite, help_px);
    if (!m.hint.empty()) {
        hy += 8 * u;
        lines(m.hint, kDim, help_px);
    }
    if (!m.gold.empty()) {
        hy += 8 * u;
        lines(m.gold, kHighlight, help_px);
    }

    // The prompt strip, bottom right: "(<>) change   (A) confirm   (B) back".
    std::string prompt;
    const auto add = [&](const char* button, const std::string& word) {
        if (!word.empty()) {
            prompt += (prompt.empty() ? "" : "    ") + std::string(button) + " " + word;
        }
    };
    add("(<>)", m.change);
    add("(A)", m.confirm);
    add("(B)", m.back);
    if (!prompt.empty()) {
        const float ppx = 20 * u;
        const std::string p = Elided(measure, ppx, prompt, x1 - x0 - 60 * u);
        const float pw = measure.Width(COOP_FONT_UI, ppx, p.c_str());
        coop_draw_text(&l, x1 - 30 * u - pw, y1 - 40 * u, ppx, kDim, p.c_str());
    }
    coop_draw_font(&l, COOP_FONT_UI);
    return out;
}

// The selectable row under (x, y), or -1; `half`: -1 the value's left half, 1 its right half, 0 the name (as
// hos_coop_menu.h's CLICK arg2).
inline int RowAt(const MenuLayout& lay, const MenuModel& m, float x, float y, int* half) {
    if (half != nullptr) {
        *half = 0;
    }
    if (x < lay.rows_left - 10 * lay.u || x > lay.rows_right) {
        return -1;
    }
    for (const auto& b : lay.rows) {
        if (y < b.top || y >= b.bottom || b.row < 0 || b.row >= static_cast<int>(m.rows.size())) {
            continue;
        }
        const RowKind k = m.rows[static_cast<std::size_t>(b.row)].kind;
        if (k == RowKind::heading || k == RowKind::note) {
            return -1;
        }
        if (half != nullptr && x >= lay.value_left && x <= lay.value_right) {
            *half = x < (lay.value_left + lay.value_right) / 2 ? -1 : 1;
        }
        return b.row;
    }
    return -1;
}

// The tab under (x, y), or -1.
inline int TabAt(const MenuLayout& lay, float x, float y) {
    for (std::size_t i = 0; i < lay.tabs.size(); ++i) {
        const auto& b = lay.tabs[i];
        if (x >= b[0] && x < b[2] && y >= b[1] && y < b[3]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace coop::ui
