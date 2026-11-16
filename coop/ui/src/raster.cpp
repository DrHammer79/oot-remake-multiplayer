// SPDX-License-Identifier: GPL-3.0-or-later
// CoopRaster (coop/ui/raster.h): a CoopDrawList into a premultiplied RGBA picture, integer arithmetic only.
//
// Coordinates become 1/16 px integers once (Fix). A command is one or more convex parts (an outline: one band per
// edge); each part gives, per sub-row (4 per pixel row, at 1/8, 3/8, 5/8, 7/8), at most two spans [a, b) in 1/16 px,
// which add up to a coverage of 0..64 per pixel. Parts of one command combine by the largest coverage, so an outline's
// corners are not drawn twice. Text and images sample their pixels bilinearly in 1/256 texel steps.
#include "coop/ui/raster.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr int kMaxParts = COOP_DRAW_POLY_POINTS;
constexpr std::int32_t kOutlineHalf = 12;  // 0.75 px: the Longshot replay's outline bands are 1.5 px wide

// A float into 1/16 px, clamped to +-16384 px; false for NaN. v * 16 is exact, so FMA or not, the same result.
bool Fix(float v, std::int32_t& out) {
    if (!(v == v)) {
        return false;
    }
    if (v < -16384.0f) {
        v = -16384.0f;
    } else if (v > 16384.0f) {
        v = 16384.0f;
    }
    const float t = v * 16.0f + 0.5f;
    auto i = static_cast<std::int32_t>(t);
    if (static_cast<float>(i) > t) {
        --i;
    }
    out = i;
    return true;
}

// a / b rounded down (b > 0).
std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if (a % b != 0 && a < 0) {
        --q;
    }
    return q;
}

std::int64_t ISqrt(std::int64_t v) {
    if (v <= 0) {
        return 0;
    }
    auto x = static_cast<std::uint64_t>(v);
    std::uint64_t r = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<std::int64_t>(r);
}

std::uint32_t Div255(std::uint32_t x) {
    x += 128;
    return (x + (x >> 8)) >> 8;
}

std::uint32_t Chan(std::uint32_t c, int k) { return (c >> (8 * k)) & 0xFFu; }

// Premultiplied p over the pixel.
void Over(std::uint32_t& px, const std::uint32_t p[4]) {
    const std::uint32_t ia = 255 - p[3];
    std::uint32_t out = 0;
    for (int k = 0; k < 4; ++k) {
        std::uint32_t v = p[k] + Div255(Chan(px, k) * ia);
        if (v > 255) {
            v = 255;
        }
        out |= v << (8 * k);
    }
    px = out;
}

// Straight colour `rgba` at alpha `a` (0..255, the colour's own alpha already in it) over the pixel.
void OverStraight(std::uint32_t& px, std::uint32_t rgba, std::uint32_t a) {
    const std::uint32_t p[4] = {Div255(Chan(rgba, 0) * a), Div255(Chan(rgba, 1) * a), Div255(Chan(rgba, 2) * a), a};
    Over(px, p);
}

enum ShapeKind { kPoly, kRRect, kRing };

struct Shape {
    int kind = kPoly;
    int n = 0;                      // kPoly: points
    std::int32_t x[kMaxParts] = {};  // kPoly
    std::int32_t y[kMaxParts] = {};
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, rad = 0;  // kRRect
    std::int32_t cx = 0, cy = 0, ro = 0, ri = 0;           // kRing (ri <= 0: a disc)
};

// The shape's spans on the sub-row at y (1/16 px): pairs into sp, returns how many (0..2).
int Spans(const Shape& s, std::int64_t y, std::int64_t sp[4]) {
    if (s.kind == kPoly) {
        bool any = false;
        std::int64_t lo = 0, hi = 0;
        for (int i = 0; i < s.n; ++i) {
            const int j = (i + 1) % s.n;
            std::int64_t xa = s.x[i], ya = s.y[i], xb = s.x[j], yb = s.y[j];
            if (ya == yb) {
                continue;
            }
            if (yb < ya) {
                std::int64_t t = xa;
                xa = xb, xb = t;
                t = ya;
                ya = yb, yb = t;
            }
            if (y < ya || y >= yb) {
                continue;
            }
            const std::int64_t x = xa + FloorDiv((y - ya) * (xb - xa), yb - ya);
            if (!any || x < lo) {
                lo = x;
            }
            if (!any || x > hi) {
                hi = x;
            }
            any = true;
        }
        if (!any || hi <= lo) {
            return 0;
        }
        sp[0] = lo, sp[1] = hi;
        return 1;
    }
    if (s.kind == kRRect) {
        if (y < s.y0 || y >= s.y1) {
            return 0;
        }
        const std::int64_t r = s.rad;
        std::int64_t dy = 0;
        if (y < s.y0 + r) {
            dy = s.y0 + r - y;
        } else if (y > s.y1 - r) {
            dy = y - (s.y1 - r);
        }
        const std::int64_t dx = dy > 0 ? ISqrt(r * r - dy * dy) : r;
        sp[0] = s.x0 + r - dx, sp[1] = s.x1 - r + dx;
        return sp[1] > sp[0] ? 1 : 0;
    }
    const std::int64_t dy = y - s.cy;
    const std::int64_t ro = s.ro, ri = s.ri;
    if (dy <= -ro || dy >= ro) {
        return 0;
    }
    const std::int64_t dxo = ISqrt(ro * ro - dy * dy);
    if (ri > 0 && dy > -ri && dy < ri) {
        const std::int64_t dxi = ISqrt(ri * ri - dy * dy);
        sp[0] = s.cx - dxo, sp[1] = s.cx - dxi;
        sp[2] = s.cx + dxi, sp[3] = s.cx + dxo;
        return 2;
    }
    sp[0] = s.cx - dxo, sp[1] = s.cx + dxo;
    return 1;
}

void Bounds(const Shape& s, std::int64_t& x0, std::int64_t& y0, std::int64_t& x1, std::int64_t& y1) {
    if (s.kind == kPoly) {
        x0 = x1 = s.x[0], y0 = y1 = s.y[0];
        for (int i = 1; i < s.n; ++i) {
            x0 = s.x[i] < x0 ? s.x[i] : x0, x1 = s.x[i] > x1 ? s.x[i] : x1;
            y0 = s.y[i] < y0 ? s.y[i] : y0, y1 = s.y[i] > y1 ? s.y[i] : y1;
        }
    } else if (s.kind == kRRect) {
        x0 = s.x0, y0 = s.y0, x1 = s.x1, y1 = s.y1;
    } else {
        x0 = s.cx - s.ro, x1 = s.cx + s.ro, y0 = s.cy - s.ro, y1 = s.cy + s.ro;
    }
}

// Adds span [a, b) (1/16 px) to the row's coverage, inside [lo, hi).
void Add(std::uint8_t* cov, std::int64_t a, std::int64_t b, std::int64_t lo, std::int64_t hi) {
    a = a < lo ? lo : a;
    b = b > hi ? hi : b;
    if (a >= b) {
        return;
    }
    const std::int64_t p0 = a / 16, p1 = (b - 1) / 16;
    if (p0 == p1) {
        cov[p0] = static_cast<std::uint8_t>(cov[p0] + (b - a));
        return;
    }
    cov[p0] = static_cast<std::uint8_t>(cov[p0] + (16 - (a - p0 * 16)));
    for (std::int64_t p = p0 + 1; p < p1; ++p) {
        cov[p] = static_cast<std::uint8_t>(cov[p] + 16);
    }
    cov[p1] = static_cast<std::uint8_t>(cov[p1] + (b - p1 * 16));
}

void Touch(CoopRasterRow& row, std::int32_t x0, std::int32_t x1) {
    if (row.x0 >= row.x1) {
        row.x0 = static_cast<std::int16_t>(x0), row.x1 = static_cast<std::int16_t>(x1);
        return;
    }
    if (x0 < row.x0) {
        row.x0 = static_cast<std::int16_t>(x0);
    }
    if (x1 > row.x1) {
        row.x1 = static_cast<std::int16_t>(x1);
    }
}

// The pixel rows/columns [p0, p1) a 1/16 px range touches, clipped to [0, side).
void PixelRange(std::int64_t a, std::int64_t b, std::int32_t side, std::int32_t& p0, std::int32_t& p1) {
    std::int64_t q0 = FloorDiv(a, 16), q1 = FloorDiv(b + 15, 16);
    q0 = q0 < 0 ? 0 : q0;
    q1 = q1 > side ? side : q1;
    p0 = static_cast<std::int32_t>(q0);
    p1 = static_cast<std::int32_t>(q1 < q0 ? q0 : q1);
}

struct ImageFill {
    const CoopRasterImage* img = nullptr;
    std::int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // 1/16 px, x1 > x0, y1 > y0
};

// The image's premultiplied colour at pixel (X, Y) of the picture, bilinear, edges clamped.
void SampleImage(const ImageFill& im, std::int32_t X, std::int32_t Y, std::uint32_t out[4]) {
    const CoopRasterImage& g = *im.img;
    const std::int64_t w = g.w, h = g.h;
    const std::int64_t u = FloorDiv((X * std::int64_t{16} + 8 - im.x0) * w * 256, im.x1 - im.x0) - 128;
    const std::int64_t v = FloorDiv((Y * std::int64_t{16} + 8 - im.y0) * h * 256, im.y1 - im.y0) - 128;
    const std::int64_t iu = FloorDiv(u, 256), iv = FloorDiv(v, 256);
    const std::int64_t fu = u - iu * 256, fv = v - iv * 256;
    std::uint64_t sum[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            std::int64_t tx = iu + i, ty = iv + j;
            tx = tx < 0 ? 0 : (tx >= w ? w - 1 : tx);
            ty = ty < 0 ? 0 : (ty >= h ? h - 1 : ty);
            const std::uint32_t t = g.rgba[ty * w + tx];
            const auto wt = static_cast<std::uint64_t>((i != 0 ? fu : 256 - fu) * (j != 0 ? fv : 256 - fv));
            const std::uint32_t a = Chan(t, 3);
            for (int k = 0; k < 3; ++k) {
                sum[k] += wt * Div255(Chan(t, k) * a);
            }
            sum[3] += wt * a;
        }
    }
    for (int k = 0; k < 4; ++k) {
        out[k] = static_cast<std::uint32_t>((sum[k] + 32768) >> 16);
    }
}

// Fills the parts' union (largest coverage) with a straight colour, or with an image times a tint (im).
void Fill(CoopRaster* r, const Shape* parts, int np, std::uint32_t rgba, const ImageFill* im) {
    if (np <= 0) {
        return;
    }
    std::int64_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    Bounds(parts[0], bx0, by0, bx1, by1);
    for (int p = 1; p < np; ++p) {
        std::int64_t a0 = 0, b0 = 0, a1 = 0, b1 = 0;
        Bounds(parts[p], a0, b0, a1, b1);
        bx0 = a0 < bx0 ? a0 : bx0, by0 = b0 < by0 ? b0 : by0;
        bx1 = a1 > bx1 ? a1 : bx1, by1 = b1 > by1 ? b1 : by1;
    }
    std::int32_t X0 = 0, X1 = 0, Y0 = 0, Y1 = 0;
    PixelRange(bx0, bx1, r->width, X0, X1);
    PixelRange(by0, by1, r->height, Y0, Y1);
    if (X0 >= X1 || Y0 >= Y1) {
        return;
    }
    const std::int64_t lo = X0 * std::int64_t{16}, hi = X1 * std::int64_t{16};
    for (std::int32_t Y = Y0; Y < Y1; ++Y) {
        std::memset(r->cov + X0, 0, static_cast<std::size_t>(X1 - X0));
        for (int p = 0; p < np; ++p) {
            std::uint8_t* acc = np > 1 ? r->part : r->cov;
            if (np > 1) {
                std::memset(r->part + X0, 0, static_cast<std::size_t>(X1 - X0));
            }
            for (int s = 0; s < 4; ++s) {
                std::int64_t sp[4];
                const int k = Spans(parts[p], Y * std::int64_t{16} + 2 + 4 * s, sp);
                for (int i = 0; i < k; ++i) {
                    Add(acc, sp[2 * i], sp[2 * i + 1], lo, hi);
                }
            }
            if (np > 1) {
                for (std::int32_t x = X0; x < X1; ++x) {
                    r->cov[x] = r->part[x] > r->cov[x] ? r->part[x] : r->cov[x];
                }
            }
        }
        std::uint32_t* row = r->pixels + static_cast<std::ptrdiff_t>(Y) * r->stride;
        std::int32_t t0 = X1, t1 = X0;
        for (std::int32_t x = X0; x < X1; ++x) {
            const std::uint32_t c = r->cov[x];
            if (c == 0) {
                continue;
            }
            if (im != nullptr) {
                std::uint32_t p[4];
                SampleImage(*im, x, Y, p);
                const std::uint32_t ta = Chan(rgba, 3);
                for (int k = 0; k < 3; ++k) {
                    p[k] = Div255(Div255(p[k] * Chan(rgba, k)) * ta);
                }
                p[3] = Div255(p[3] * ta);
                for (int k = 0; k < 4; ++k) {
                    p[k] = (p[k] * c + 32) / 64;
                }
                if (p[3] == 0 && p[0] == 0 && p[1] == 0 && p[2] == 0) {
                    continue;
                }
                Over(row[x], p);
            } else {
                const std::uint32_t a = (Chan(rgba, 3) * c + 32) / 64;
                if (a == 0) {
                    continue;
                }
                OverStraight(row[x], rgba, a);
            }
            t0 = x < t0 ? x : t0;
            t1 = x + 1;
        }
        if (t0 < t1) {
            Touch(r->rows[Y], t0, t1);
        }
    }
}

// ---- text ----

std::uint32_t Decode(const unsigned char*& s, const unsigned char* end) {
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
        if (s >= end || (*s & 0xC0) != 0x80) {
            return 0xFFFFFFFFu;
        }
        cp = (cp << 6) | (*s++ & 0x3Fu);
    }
    return cp;
}

const CoopRasterGlyph* Find(const CoopRasterFont& f, std::uint32_t cp) {
    std::uint32_t lo = 0, hi = f.n;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (f.glyphs[mid].codepoint < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < f.n && f.glyphs[lo].codepoint == cp ? &f.glyphs[lo] : nullptr;
}

bool FontOk(const CoopRasterFont* f) {
    return f != nullptr && f->alpha != nullptr && f->size > 0 && f->size <= 4096 && (f->n == 0 || f->glyphs != nullptr);
}

// One glyph with its top left at (gx, gy) (1/1024 px); px16: the text's px in 1/16.
void Glyph(CoopRaster* r, const CoopRasterFont& f, const CoopRasterGlyph& g, std::int64_t gx, std::int64_t gy,
           std::int64_t px16, std::uint32_t rgba) {
    if (g.w == 0 || g.h == 0 || std::uint32_t{g.x} + g.w > f.atlas_w || std::uint32_t{g.y} + g.h > f.atlas_h) {
        return;
    }
    const std::int64_t size = f.size;
    const std::int64_t gw = FloorDiv(std::int64_t{g.w} * 64 * px16, size);
    const std::int64_t gh = FloorDiv(std::int64_t{g.h} * 64 * px16, size);
    // Bilinear sampling fades half a texel past the glyph's edges, on every side alike.
    const std::int64_t half = FloorDiv(32 * px16, size);
    std::int64_t X0 = FloorDiv(gx - half, 1024), X1 = FloorDiv(gx + gw + half + 1023, 1024);
    std::int64_t Y0 = FloorDiv(gy - half, 1024), Y1 = FloorDiv(gy + gh + half + 1023, 1024);
    X0 = X0 < 0 ? 0 : X0, Y0 = Y0 < 0 ? 0 : Y0;
    X1 = X1 > r->width ? r->width : X1, Y1 = Y1 > r->height ? r->height : Y1;
    const std::uint32_t ca = Chan(rgba, 3);
    for (std::int64_t Y = Y0; Y < Y1; ++Y) {
        const std::int64_t v = FloorDiv((Y * 1024 + 512 - gy) * 4 * size, px16) - 128;
        const std::int64_t iv = FloorDiv(v, 256), fv = v - iv * 256;
        std::uint32_t* row = r->pixels + static_cast<std::ptrdiff_t>(Y) * r->stride;
        std::int64_t t0 = X1, t1 = X0;
        for (std::int64_t X = X0; X < X1; ++X) {
            const std::int64_t u = FloorDiv((X * 1024 + 512 - gx) * 4 * size, px16) - 128;
            const std::int64_t iu = FloorDiv(u, 256), fu = u - iu * 256;
            std::int64_t sum = 0;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const std::int64_t tx = iu + i, ty = iv + j;
                    if (tx < 0 || ty < 0 || tx >= g.w || ty >= g.h) {
                        continue;
                    }
                    const std::uint8_t t = f.alpha[(g.y + ty) * std::int64_t{f.atlas_w} + g.x + tx];
                    sum += t * (i != 0 ? fu : 256 - fu) * (j != 0 ? fv : 256 - fv);
                }
            }
            const auto cov = static_cast<std::uint32_t>((sum + 32768) >> 16);
            const std::uint32_t a = Div255(ca * cov);
            if (a == 0) {
                continue;
            }
            OverStraight(row[X], rgba, a);
            t0 = X < t0 ? X : t0;
            t1 = X + 1;
        }
        if (t0 < t1) {
            Touch(r->rows[Y], static_cast<std::int32_t>(t0), static_cast<std::int32_t>(t1));
        }
    }
}

void Text(CoopRaster* r, const CoopRasterFont& f, std::int32_t x16, std::int32_t y16, std::int32_t px16,
          std::uint32_t rgba, const char* text, std::uint32_t len) {
    const std::int64_t size = f.size;
    const std::int64_t start = std::int64_t{x16} * 64;
    std::int64_t pen = start, top = std::int64_t{y16} * 64;
    const auto* s = reinterpret_cast<const unsigned char*>(text);
    const unsigned char* end = s + len;
    while (s < end) {
        const std::uint32_t cp = Decode(s, end);
        if (cp == '\n') {
            pen = start;
            top += FloorDiv(std::int64_t{f.line64} * px16, size);
            continue;
        }
        const CoopRasterGlyph* g = Find(f, cp);
        if (g == nullptr) {
            pen += FloorDiv(std::int64_t{f.missing64} * px16, size);
            continue;
        }
        Glyph(r, f, *g, pen + FloorDiv(std::int64_t{g->ox} * 64 * px16, size),
              top + FloorDiv(std::int64_t{g->oy} * 64 * px16, size), px16, rgba);
        pen += FloorDiv(std::int64_t{g->advance64} * px16, size);
    }
}

// ---- commands ----

bool FixAll(const float* v, int n, std::int32_t* out) {
    for (int i = 0; i < n; ++i) {
        if (!Fix(v[i], out[i])) {
            return false;
        }
    }
    return true;
}

// One outline band: edge a-b widened by kOutlineHalf both sides. False for a zero-length edge.
bool Band(std::int64_t ax, std::int64_t ay, std::int64_t bx, std::int64_t by, Shape& s) {
    const std::int64_t dx = bx - ax, dy = by - ay;
    const std::int64_t len = ISqrt(dx * dx + dy * dy);
    if (len == 0) {
        return false;
    }
    const std::int64_t nx = -dy * kOutlineHalf / len, ny = dx * kOutlineHalf / len;
    s.kind = kPoly;
    s.n = 4;
    s.x[0] = static_cast<std::int32_t>(ax + nx), s.y[0] = static_cast<std::int32_t>(ay + ny);
    s.x[1] = static_cast<std::int32_t>(bx + nx), s.y[1] = static_cast<std::int32_t>(by + ny);
    s.x[2] = static_cast<std::int32_t>(bx - nx), s.y[2] = static_cast<std::int32_t>(by - ny);
    s.x[3] = static_cast<std::int32_t>(ax - nx), s.y[3] = static_cast<std::int32_t>(ay - ny);
    return true;
}

// Draws one command; false when it could not be drawn (counted in `skipped`).
bool Command(CoopRaster* r, const CoopDrawList* l, const CoopDrawCmd& c, int& font) {
    std::int32_t q[2 * COOP_DRAW_POLY_POINTS];
    Shape parts[kMaxParts];
    switch (c.kind) {
    case COOP_DRAW_RECT: {
        if (!FixAll(c.v, 5, q)) {
            return false;
        }
        Shape& s = parts[0];
        s.kind = kRRect;
        s.x0 = q[0], s.y0 = q[1], s.x1 = q[2], s.y1 = q[3];
        if (s.x1 <= s.x0 || s.y1 <= s.y0) {
            return true;
        }
        std::int32_t rad = q[4] < 0 ? 0 : q[4];
        const std::int32_t half = (s.x1 - s.x0 < s.y1 - s.y0 ? s.x1 - s.x0 : s.y1 - s.y0) / 2;
        s.rad = rad > half ? half : rad;
        Fill(r, parts, 1, c.rgba, nullptr);
        return true;
    }
    case COOP_DRAW_CIRCLE: {
        if (!FixAll(c.v, 4, q)) {
            return false;
        }
        Shape& s = parts[0];
        s.kind = kRing;
        s.cx = q[0], s.cy = q[1];
        if (q[2] <= 0) {
            return true;
        }
        if (q[3] > 0) {
            s.ro = q[2] + q[3] / 2;
            s.ri = q[2] - q[3] / 2;
        } else {
            s.ro = q[2];
        }
        Fill(r, parts, 1, c.rgba, nullptr);
        return true;
    }
    case COOP_DRAW_TRIANGLE:
    case COOP_DRAW_POLY: {
        const int n = c.kind == COOP_DRAW_TRIANGLE ? 3 : c.n;
        if (n < 3 || n > COOP_DRAW_POLY_POINTS || !FixAll(c.v, 2 * n, q)) {
            return false;
        }
        if (c.kind == COOP_DRAW_POLY && c.id != 0) {
            int np = 0;
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                if (Band(q[2 * i], q[2 * i + 1], q[2 * j], q[2 * j + 1], parts[np])) {
                    ++np;
                }
            }
            Fill(r, parts, np, c.rgba, nullptr);
            return true;
        }
        Shape& s = parts[0];
        s.kind = kPoly;
        s.n = n;
        for (int i = 0; i < n; ++i) {
            s.x[i] = q[2 * i], s.y[i] = q[2 * i + 1];
        }
        Fill(r, parts, 1, c.rgba, nullptr);
        return true;
    }
    case COOP_DRAW_TEXT: {
        if (font < 0 || font >= COOP_FONT_COUNT || !FontOk(r->font[font]) || !FixAll(c.v, 3, q) ||
            c.text_off >= COOP_DRAW_MAX_TEXT || c.text_len > COOP_DRAW_MAX_TEXT - c.text_off) {
            return false;
        }
        const std::int32_t px16 = q[2] < 1 ? 1 : (q[2] > 512 * 16 ? 512 * 16 : q[2]);
        Text(r, *r->font[font], q[0], q[1], px16, c.rgba, l->text + c.text_off, c.text_len);
        return true;
    }
    case COOP_DRAW_IMAGE: {
        if (c.id >= COOP_IMAGE_COUNT || !FixAll(c.v, 4, q)) {
            return false;
        }
        const CoopRasterImage* img = r->image[c.id];
        if (img == nullptr || img->rgba == nullptr || img->w == 0 || img->h == 0 || img->w > COOP_RASTER_MAX_SIDE ||
            img->h > COOP_RASTER_MAX_SIDE) {
            return false;
        }
        if (q[2] <= q[0] || q[3] <= q[1]) {
            return true;
        }
        Shape& s = parts[0];
        s.kind = kRRect;
        s.x0 = q[0], s.y0 = q[1], s.x1 = q[2], s.y1 = q[3];
        ImageFill im;
        im.img = img;
        im.x0 = q[0], im.y0 = q[1], im.x1 = q[2], im.y1 = q[3];
        Fill(r, parts, 1, c.rgba, &im);
        return true;
    }
    case COOP_DRAW_FONT:
        font = c.id;
        return font < COOP_FONT_COUNT;
    default:
        return false;
    }
}

std::uint64_t RowHash(const CoopRasterRow& row, const std::uint32_t* px) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const std::uint64_t prime = 0x100000001b3ull;
    h = (h ^ static_cast<std::uint16_t>(row.x0)) * prime;
    h = (h ^ static_cast<std::uint16_t>(row.x1)) * prime;
    for (std::int32_t x = row.x0; x < row.x1; ++x) {
        h = (h ^ px[x]) * prime;
    }
    return h | 1;
}

std::size_t Align8(std::size_t n) { return (n + 7) & ~static_cast<std::size_t>(7); }

}  // namespace

extern "C" {

std::size_t coop_raster_work_bytes(int width, int height) {
    if (width <= 0 || height <= 0 || width > COOP_RASTER_MAX_SIDE || height > COOP_RASTER_MAX_SIDE) {
        return 0;
    }
    return static_cast<std::size_t>(height) * sizeof(CoopRasterRow) + 2 * Align8(static_cast<std::size_t>(width));
}

int coop_raster_init(CoopRaster* r, std::uint32_t* pixels, int width, int height, int stride, void* work,
                     std::size_t work_bytes) {
    const std::size_t need = coop_raster_work_bytes(width, height);
    if (r == nullptr || pixels == nullptr || work == nullptr || need == 0 || work_bytes < need || stride < width ||
        (reinterpret_cast<std::uintptr_t>(work) & 7u) != 0) {
        return 0;
    }
    std::memset(r, 0, sizeof *r);
    r->pixels = pixels;
    r->width = width, r->height = height, r->stride = stride;
    auto* bytes = static_cast<std::uint8_t*>(work);
    std::memset(bytes, 0, need);
    r->rows = reinterpret_cast<CoopRasterRow*>(bytes);
    r->cov = bytes + static_cast<std::size_t>(height) * sizeof(CoopRasterRow);
    r->part = r->cov + Align8(static_cast<std::size_t>(width));
    for (int y = 0; y < height; ++y) {
        std::memset(pixels + static_cast<std::ptrdiff_t>(y) * stride, 0,
                    static_cast<std::size_t>(width) * sizeof(std::uint32_t));
    }
    return 1;
}

void coop_raster_set_font(CoopRaster* r, int font, const CoopRasterFont* f) {
    if (r != nullptr && font >= 0 && font < COOP_FONT_COUNT) {
        r->font[font] = f;
    }
}

void coop_raster_set_image(CoopRaster* r, int image, const CoopRasterImage* img) {
    if (r != nullptr && image >= 0 && image < COOP_IMAGE_COUNT) {
        r->image[image] = img;
    }
}

void coop_raster_draw(CoopRaster* r, const CoopDrawList* l) {
    if (r == nullptr || r->pixels == nullptr) {
        return;
    }
    ++r->frames;
    r->skipped = 0;
    for (std::int32_t y = 0; y < r->height; ++y) {
        CoopRasterRow& row = r->rows[y];
        if (row.x0 < row.x1) {
            std::memset(r->pixels + static_cast<std::ptrdiff_t>(y) * r->stride + row.x0, 0,
                        static_cast<std::size_t>(row.x1 - row.x0) * sizeof(std::uint32_t));
        }
        row.last_x0 = row.x0, row.last_x1 = row.x1;
        row.x0 = row.x1 = 0;
    }
    if (l != nullptr) {
        int font = COOP_FONT_UI;
        const std::uint32_t n = l->n < COOP_DRAW_MAX_CMDS ? l->n : COOP_DRAW_MAX_CMDS;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!Command(r, l, l->cmd[i], font)) {
                ++r->skipped;
            }
        }
    }
    for (std::int32_t y = 0; y < r->height; ++y) {
        CoopRasterRow& row = r->rows[y];
        const std::uint64_t h = row.x0 < row.x1 ? RowHash(row, r->pixels + static_cast<std::ptrdiff_t>(y) * r->stride) : 0;
        row.dirty = static_cast<std::uint8_t>(h != row.hash ? 1 : 0);
        row.hash = h;
    }
}

int coop_raster_dirty(const CoopRaster* r, CoopRasterBand* out, int max) {
    if (r == nullptr || r->rows == nullptr || out == nullptr || max <= 0) {
        return 0;
    }
    int n = 0;
    for (std::int32_t y = 0; y < r->height; ++y) {
        if (r->rows[y].dirty == 0) {
            continue;
        }
        if (n > 0 && (out[n - 1].y1 == y || n == max)) {
            out[n - 1].y1 = static_cast<std::uint16_t>(y + 1);
            continue;
        }
        out[n].y0 = static_cast<std::uint16_t>(y);
        out[n].y1 = static_cast<std::uint16_t>(y + 1);
        ++n;
    }
    return n;
}

void coop_raster_to_straight(const std::uint32_t* src, std::uint32_t* dst, std::size_t n) {
    if (src == nullptr || dst == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t p = src[i];
        const std::uint32_t a = Chan(p, 3);
        if (a == 0) {
            dst[i] = 0;
            continue;
        }
        std::uint32_t out = a << 24;
        for (int k = 0; k < 3; ++k) {
            std::uint32_t v = (Chan(p, k) * 255 + a / 2) / a;
            out |= (v > 255 ? 255 : v) << (8 * k);
        }
        dst[i] = out;
    }
}

void coop_raster_font_advances(const CoopRasterFont* f, std::uint32_t* codepoints, float* advances,
                               CoopFontAdvances* out) {
    if (out == nullptr) {
        return;
    }
    std::memset(out, 0, sizeof *out);
    if (f == nullptr || f->size == 0 || (f->n > 0 && (f->glyphs == nullptr || codepoints == nullptr || advances == nullptr))) {
        return;
    }
    const float scale = 1.0f / (64.0f * static_cast<float>(f->size));
    for (std::uint32_t i = 0; i < f->n; ++i) {
        codepoints[i] = f->glyphs[i].codepoint;
        advances[i] = static_cast<float>(f->glyphs[i].advance64) * scale;
    }
    out->n = f->n;
    out->codepoints = codepoints;
    out->advances = advances;
    out->missing = static_cast<float>(f->missing64) * scale;
}

}  // extern "C"
