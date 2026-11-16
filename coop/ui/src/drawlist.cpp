// SPDX-License-Identifier: GPL-3.0-or-later
// CoopDrawList (coop/ui/drawlist.h): appending commands, text widths from advance tables.
#include "coop/ui/drawlist.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

CoopDrawCmd* Next(CoopDrawList* l, std::uint8_t kind) {
    if (l->n >= COOP_DRAW_MAX_CMDS) {
        ++l->dropped;
        return nullptr;
    }
    CoopDrawCmd* c = &l->cmd[l->n++];
    std::memset(c, 0, sizeof *c);
    c->kind = kind;
    return c;
}

// One UTF-8 codepoint from `s` (advanced past it); a bad byte is skipped alone and gives 0xFFFFFFFF.
std::uint32_t Decode(const unsigned char*& s) {
    const unsigned char b = *s++;
    if (b < 0x80) {
        return b;
    }
    int more = 0;
    std::uint32_t cp = 0;
    if ((b & 0xE0) == 0xC0) {
        more = 1, cp = b & 0x1Fu;
    } else if ((b & 0xF0) == 0xE0) {
        more = 2, cp = b & 0x0Fu;
    } else if ((b & 0xF8) == 0xF0) {
        more = 3, cp = b & 0x07u;
    } else {
        return 0xFFFFFFFFu;
    }
    for (int i = 0; i < more; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0xFFFFFFFFu;   // (s stays after the lead byte: the next byte is read on its own)
        }
    }
    for (int i = 0; i < more; ++i) {
        cp = cp << 6 | (s[i] & 0x3Fu);
    }
    s += more;
    return cp;
}

float Advance(const CoopFontAdvances* f, std::uint32_t cp) {
    std::uint32_t lo = 0, hi = f->n;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (f->codepoints[mid] < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < f->n && f->codepoints[lo] == cp ? f->advances[lo] : f->missing;
}

}  // namespace

extern "C" {

void coop_draw_begin(CoopDrawList* l, float width, float height) {
    l->width = width;
    l->height = height;
    l->n = 0;
    l->text_used = 0;
    l->dropped = 0;
    l->font = COOP_FONT_UI;
    l->clip_depth = 0;
    l->last_text = 0;
}

int coop_draw_rect(CoopDrawList* l, float x0, float y0, float x1, float y1, std::uint32_t rgba, float rounding) {
    CoopDrawCmd* c = Next(l, COOP_DRAW_RECT);
    if (c == nullptr) {
        return 0;
    }
    c->rgba = rgba;
    c->v[0] = x0, c->v[1] = y0, c->v[2] = x1, c->v[3] = y1, c->v[4] = rounding;
    return 1;
}

int coop_draw_circle(CoopDrawList* l, float x, float y, float radius, std::uint32_t rgba, float thickness) {
    CoopDrawCmd* c = Next(l, COOP_DRAW_CIRCLE);
    if (c == nullptr) {
        return 0;
    }
    c->rgba = rgba;
    c->v[0] = x, c->v[1] = y, c->v[2] = radius, c->v[3] = thickness;
    return 1;
}

int coop_draw_triangle(CoopDrawList* l, float x0, float y0, float x1, float y1, float x2, float y2,
                       std::uint32_t rgba) {
    CoopDrawCmd* c = Next(l, COOP_DRAW_TRIANGLE);
    if (c == nullptr) {
        return 0;
    }
    c->rgba = rgba;
    c->v[0] = x0, c->v[1] = y0, c->v[2] = x1, c->v[3] = y1, c->v[4] = x2, c->v[5] = y2;
    return 1;
}

int coop_draw_poly(CoopDrawList* l, const float* xy, int points, std::uint32_t rgba, int outline) {
    if (xy == nullptr || points < 3 || points > COOP_DRAW_POLY_POINTS) {
        return 0;   // not a shape: nothing (not counted as dropped)
    }
    CoopDrawCmd* c = Next(l, COOP_DRAW_POLY);
    if (c == nullptr) {
        return 0;
    }
    c->rgba = rgba;
    c->n = static_cast<std::uint8_t>(points);
    c->id = outline ? 1 : 0;
    std::memcpy(c->v, xy, sizeof(float) * 2 * static_cast<std::size_t>(points));
    return 1;
}

int coop_draw_text(CoopDrawList* l, float x, float y, float px, std::uint32_t rgba, const char* utf8) {
    if (utf8 == nullptr) {
        utf8 = "";
    }
    const std::size_t len = std::strlen(utf8);
    std::uint32_t off = 0;
    if (l->last_text != 0) {   // the same words as the last text (an outline's copies): stored once
        const char* last = l->text + (l->last_text - 1);
        if (std::strlen(last) == len && std::memcmp(last, utf8, len) == 0) {
            off = l->last_text;
        }
    }
    if (off == 0) {
        if (len + 1 > COOP_DRAW_MAX_TEXT - l->text_used || l->n >= COOP_DRAW_MAX_CMDS) {
            ++l->dropped;
            return 0;
        }
        std::memcpy(l->text + l->text_used, utf8, len + 1);
        off = l->text_used + 1;
        l->text_used += static_cast<std::uint32_t>(len + 1);
    }
    CoopDrawCmd* c = Next(l, COOP_DRAW_TEXT);
    if (c == nullptr) {
        return 0;
    }
    l->last_text = off;
    c->rgba = rgba;
    c->text_off = off - 1;
    c->text_len = static_cast<std::uint32_t>(len);
    c->v[0] = x, c->v[1] = y, c->v[2] = px;
    return 1;
}

int coop_draw_image(CoopDrawList* l, int image, float x0, float y0, float x1, float y1, std::uint32_t tint) {
    CoopDrawCmd* c = Next(l, COOP_DRAW_IMAGE);
    if (c == nullptr) {
        return 0;
    }
    c->rgba = tint;
    c->id = static_cast<std::uint16_t>(image);
    c->v[0] = x0, c->v[1] = y0, c->v[2] = x1, c->v[3] = y1;
    return 1;
}

int coop_draw_font(CoopDrawList* l, int font) {
    CoopDrawCmd* c = Next(l, COOP_DRAW_FONT);
    if (c == nullptr) {
        return 0;
    }
    c->id = static_cast<std::uint16_t>(font);
    l->font = static_cast<std::uint16_t>(font);
    return 1;
}

int coop_draw_clip_push(CoopDrawList* l, float x0, float y0, float x1, float y1) {
    CoopDrawCmd* c = Next(l, COOP_DRAW_CLIP_PUSH);
    if (c == nullptr) {
        return 0;
    }
    c->v[0] = x0, c->v[1] = y0, c->v[2] = x1, c->v[3] = y1;
    ++l->clip_depth;
    return 1;
}

int coop_draw_clip_pop(CoopDrawList* l) {
    if (l->clip_depth == 0) {
        return 0;   // nothing pushed: nothing to pop
    }
    CoopDrawCmd* c = Next(l, COOP_DRAW_CLIP_POP);
    if (c == nullptr) {
        return 0;
    }
    --l->clip_depth;
    return 1;
}

const char* coop_draw_text_of(const CoopDrawList* l, const CoopDrawCmd* c) {
    if (c == nullptr || c->kind != COOP_DRAW_TEXT || c->text_off >= COOP_DRAW_MAX_TEXT) {
        return "";
    }
    return l->text + c->text_off;
}

float coop_text_width(const CoopFontAdvances* f, float px, const char* utf8) {
    if (f == nullptr || utf8 == nullptr) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (const auto* s = reinterpret_cast<const unsigned char*>(utf8); *s != 0;) {
        sum += Advance(f, Decode(s));
    }
    return sum * px;
}

}  // extern "C"
