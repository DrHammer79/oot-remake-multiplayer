// SPDX-License-Identifier: GPL-3.0-or-later
// CoopRaster (coop/ui/raster.h): exact pixels for each command kind, blending, text from an atlas, images, clipping,
// dirty rows, and golden pictures (the same bytes on every compiler CI builds with).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "coop/ui/drawlist.h"
#include "coop/ui/raster.h"

namespace {

std::unique_ptr<CoopDrawList> NewList(float w, float h) {
    auto l = std::make_unique<CoopDrawList>();
    coop_draw_begin(l.get(), w, h);
    return l;
}

struct Pic {
    int w, h, stride;
    std::vector<std::uint32_t> px;
    std::vector<std::uint64_t> work;
    CoopRaster r{};
    Pic(int w_, int h_, int stride_ = 0)
        : w(w_), h(h_), stride(stride_ > 0 ? stride_ : w_), px(static_cast<std::size_t>(stride * h_)),
          work(coop_raster_work_bytes(w_, h_) / 8 + 1) {
        REQUIRE(coop_raster_init(&r, px.data(), w, h, stride, work.data(), work.size() * 8) == 1);
    }
    std::uint32_t at(int x, int y) const { return px[static_cast<std::size_t>(y * stride + x)]; }
    void Draw(const CoopDrawList& l) { coop_raster_draw(&r, &l); }
    std::vector<std::pair<int, int>> Dirty(int max = 64) {
        std::vector<CoopRasterBand> b(static_cast<std::size_t>(max));
        const int n = coop_raster_dirty(&r, b.data(), max);
        std::vector<std::pair<int, int>> out;
        for (int i = 0; i < n; ++i) {
            out.emplace_back(b[static_cast<std::size_t>(i)].y0, b[static_cast<std::size_t>(i)].y1);
        }
        return out;
    }
    // Alpha as characters, one line per row: ' ' none ... '@' opaque.
    std::string Alpha() const {
        static const char kRamp[] = " .:-=+*#%@";
        std::string s = "\n";
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::uint32_t a = at(x, y) >> 24;
                s += a == 0 ? ' ' : kRamp[1 + (a * 9 - 1) / 255];
            }
            s += "\n";
        }
        return s;
    }
    std::uint64_t Hash() const {
        std::uint64_t hsh = 0xcbf29ce484222325ull;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                hsh = (hsh ^ at(x, y)) * 0x100000001b3ull;
            }
        }
        return hsh;
    }
    int Count() const {
        int n = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                n += at(x, y) != 0 ? 1 : 0;
            }
        }
        return n;
    }
};

std::uint32_t A(std::uint32_t p) { return p >> 24; }

constexpr std::uint32_t kRed = 0xFF0000FFu;
constexpr std::uint32_t kWhite = 0xFFFFFFFFu;

// A tiny atlas: 'A' a 2x2 solid block, 'B' a 3x1 strip (255, 128, 0) placed one pixel right and down. Made for 4 px.
struct TinyFont {
    std::uint8_t alpha[8 * 4] = {
        255, 255, 255, 128, 0, 0, 0, 0,  //
        255, 255, 0, 0, 0, 0, 0, 0,      //
        0, 0, 0, 0, 0, 0, 0, 0,          //
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    CoopRasterGlyph glyphs[2] = {
        {'A', 0, 0, 2, 2, 0, 0, 3 * 64, 0},
        {'B', 2, 0, 3, 1, 1, 1, 4 * 64, 0},
    };
    CoopRasterFont font{4, 5 * 64, 2 * 64, 8, 4, alpha, 2, glyphs};
};

}  // namespace

TEST_CASE("raster: init checks its buffers") {
    std::vector<std::uint32_t> px(16 * 8);
    std::vector<std::uint64_t> work(coop_raster_work_bytes(16, 8) / 8 + 1);
    const std::size_t bytes = work.size() * 8;
    CoopRaster r{};
    CHECK(coop_raster_work_bytes(0, 8) == 0);
    CHECK(coop_raster_work_bytes(16, -1) == 0);
    CHECK(coop_raster_work_bytes(COOP_RASTER_MAX_SIDE + 1, 8) == 0);
    CHECK(coop_raster_init(&r, nullptr, 16, 8, 16, work.data(), bytes) == 0);
    CHECK(coop_raster_init(&r, px.data(), 16, 8, 15, work.data(), bytes) == 0);
    CHECK(coop_raster_init(&r, px.data(), 16, 8, 16, work.data(), coop_raster_work_bytes(16, 8) - 1) == 0);
    CHECK(coop_raster_init(&r, px.data(), 16, 8, 16, reinterpret_cast<std::uint8_t*>(work.data()) + 4, bytes - 8) == 0);
    px[5] = 0x12345678u;
    CHECK(coop_raster_init(&r, px.data(), 16, 8, 16, work.data(), bytes) == 1);
    CHECK(px[5] == 0);  // cleared
}

TEST_CASE("raster: a pixel-aligned rect fills exactly its pixels") {
    Pic p(8, 5);
    auto l = NewList(8, 5);
    coop_draw_rect(l.get(), 2, 1, 5, 3, kRed, 0);
    p.Draw(*l);
    CHECK(p.Count() == 6);
    for (int y = 1; y < 3; ++y) {
        for (int x = 2; x < 5; ++x) {
            CHECK(p.at(x, y) == kRed);
        }
    }
    CHECK(p.r.skipped == 0);
}

TEST_CASE("raster: half-covered pixels get half alpha, premultiplied") {
    Pic p(8, 2);
    auto l = NewList(8, 2);
    coop_draw_rect(l.get(), 2.5f, 0, 4, 1, kRed, 0);
    p.Draw(*l);
    CHECK(p.at(2, 0) == 0x80000080u);
    CHECK(p.at(3, 0) == kRed);
    CHECK(p.at(4, 0) == 0);
    CHECK(p.at(2, 1) == 0);
}

TEST_CASE("raster: translucent commands blend over each other in order") {
    Pic p(4, 1);
    auto l = NewList(4, 1);
    coop_draw_rect(l.get(), 0, 0, 2, 1, 0x80FFFFFFu, 0);
    coop_draw_rect(l.get(), 1, 0, 3, 1, 0x80FFFFFFu, 0);
    p.Draw(*l);
    CHECK(p.at(0, 0) == 0x80808080u);
    CHECK(p.at(1, 0) == 0xC0C0C0C0u);
    CHECK(p.at(2, 0) == 0x80808080u);
    CHECK(p.at(3, 0) == 0);
    coop_draw_begin(l.get(), 4, 1);  // an opaque one over a translucent one replaces it
    coop_draw_rect(l.get(), 0, 0, 1, 1, 0x80FFFFFFu, 0);
    coop_draw_rect(l.get(), 0, 0, 1, 1, kRed, 0);
    p.Draw(*l);
    CHECK(p.at(0, 0) == kRed);
}

TEST_CASE("raster: a convex polygon covers the same pixels as the same rect") {
    Pic a(10, 8), b(10, 8);
    auto la = NewList(10, 8), lb = NewList(10, 8);
    coop_draw_rect(la.get(), 1.3f, 2.7f, 6.6f, 5.2f, 0xC08040FFu, 0);
    const float xy[] = {1.3f, 2.7f, 6.6f, 2.7f, 6.6f, 5.2f, 1.3f, 5.2f};
    coop_draw_poly(lb.get(), xy, 4, 0xC08040FFu, 0);
    a.Draw(*la);
    b.Draw(*lb);
    CHECK(a.Count() > 0);
    CHECK(a.px == b.px);
}

TEST_CASE("raster: a triangle's diagonal splits its pixels") {
    Pic p(8, 8);
    auto l = NewList(8, 8);
    coop_draw_triangle(l.get(), 0, 0, 8, 0, 0, 8, kWhite);
    p.Draw(*l);
    CHECK(p.at(0, 0) == kWhite);
    CHECK(p.at(7, 7) == 0);
    CHECK(A(p.at(3, 4)) == 128);  // the edge crosses this pixel corner to corner
    CHECK(A(p.at(4, 3)) == 128);
}

TEST_CASE("raster: circles and rings are symmetric") {
    Pic p(16, 16);
    auto l = NewList(16, 16);
    coop_draw_circle(l.get(), 8, 8, 4, kWhite, 0);
    p.Draw(*l);
    CHECK(p.at(8, 8) == kWhite);
    CHECK(p.at(0, 0) == 0);
    CHECK(p.at(13, 8) == 0);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            CHECK(p.at(x, y) == p.at(15 - x, y));
            CHECK(p.at(x, y) == p.at(x, 15 - y));
        }
    }
    coop_draw_begin(l.get(), 16, 16);
    coop_draw_circle(l.get(), 8, 8, 5, kWhite, 2);  // a ring from radius 4 to 6
    p.Draw(*l);
    CHECK(p.at(8, 8) == 0);
    CHECK(p.at(7, 7) == 0);
    CHECK(A(p.at(13, 8)) > 200);
    CHECK(p.at(15, 8) == 0);
    CHECK(p.at(2, 7) == p.at(13, 7));
}

TEST_CASE("raster: a rounded rect's corners are cut, its sides are not") {
    Pic p(12, 12);
    auto l = NewList(12, 12);
    coop_draw_rect(l.get(), 0, 0, 12, 12, kWhite, 4);
    p.Draw(*l);
    CHECK(p.at(0, 0) == 0);
    CHECK(p.at(11, 11) == 0);
    CHECK(p.at(0, 6) == kWhite);
    CHECK(p.at(6, 0) == kWhite);
    CHECK(p.at(6, 6) == kWhite);
    CHECK(A(p.at(1, 1)) < 255);
    CHECK(p.at(1, 1) == p.at(10, 1));
}

TEST_CASE("raster: a polygon outline is bands along its edges, corners drawn once") {
    Pic p(16, 16);
    auto l = NewList(16, 16);
    const float xy[] = {2, 2, 12, 2, 12, 12, 2, 12};
    coop_draw_poly(l.get(), xy, 4, 0x80FFFFFFu, 1);
    p.Draw(*l);
    CHECK(p.at(7, 7) == 0);
    CHECK(p.at(7, 0) == 0);
    CHECK(A(p.at(7, 1)) == 96);   // a band from y 1.25 to 2.75: 3 of 4 sub-rows in each of rows 1 and 2
    CHECK(A(p.at(7, 2)) == 96);
    CHECK(p.at(7, 3) == 0);
    CHECK(p.at(1, 1) == 0);       // bands end at the corner points, as the Longshot replay's do
    CHECK(A(p.at(2, 2)) == 96);   // the corner: both bands, the larger counts
    CHECK(p.at(7, 11) == p.at(7, 2));
}

TEST_CASE("raster: text from an atlas") {
    TinyFont tf;
    Pic p(16, 12);
    coop_raster_set_font(&p.r, COOP_FONT_UI, &tf.font);
    auto l = NewList(16, 12);
    coop_draw_text(l.get(), 1, 1, 4, kWhite, "A");
    p.Draw(*l);
    CHECK(p.Count() == 4);  // at its own size the atlas is copied 1:1
    CHECK(p.at(1, 1) == kWhite);
    CHECK(p.at(2, 2) == kWhite);
    CHECK(p.at(3, 1) == 0);

    coop_draw_begin(l.get(), 16, 12);
    coop_draw_text(l.get(), 1, 1, 4, kWhite, "A?A\nA");  // '?' is missing: 2 px, nothing drawn
    p.Draw(*l);
    CHECK(p.Count() == 12);
    CHECK(p.at(6, 1) == kWhite);  // 1 + 3 + 2
    CHECK(p.at(5, 1) == 0);
    CHECK(p.at(1, 6) == kWhite);  // next line: 5 px down
    CHECK(p.r.skipped == 0);

    coop_draw_begin(l.get(), 16, 12);
    coop_draw_text(l.get(), 0, 0, 4, kWhite, "B");  // glyph offset (1, 1), coverage 255 128 0
    p.Draw(*l);
    CHECK(p.Count() == 2);
    CHECK(p.at(1, 1) == kWhite);
    CHECK(p.at(2, 1) == 0x80808080u);

    coop_draw_begin(l.get(), 16, 12);
    coop_draw_text(l.get(), 1, 1, 8, kWhite, "A");  // twice the size: 4x4, bilinear edges
    p.Draw(*l);
    CHECK(p.at(2, 2) == kWhite);
    CHECK(p.at(3, 3) == kWhite);
    CHECK(A(p.at(1, 2)) == 191);
    CHECK(A(p.at(4, 3)) == 191);
    CHECK(A(p.at(5, 3)) == 64);  // half a texel of fade past the edge, both sides alike
    CHECK(A(p.at(0, 3)) == 64);
    CHECK(p.at(6, 3) == 0);

    coop_draw_begin(l.get(), 16, 12);
    coop_draw_font(l.get(), COOP_FONT_MEDIUM);  // no atlas for it: skipped
    coop_draw_text(l.get(), 1, 1, 4, kWhite, "A");
    p.Draw(*l);
    CHECK(p.Count() == 0);
    CHECK(p.r.skipped == 1);
}

TEST_CASE("raster: the font's advances measure what it draws") {
    TinyFont tf;
    std::uint32_t cps[2];
    float adv[2];
    CoopFontAdvances fa{};
    coop_raster_font_advances(&tf.font, cps, adv, &fa);
    REQUIRE(fa.n == 2);
    CHECK(cps[0] == 'A');
    CHECK(adv[0] == doctest::Approx(0.75f));
    CHECK(fa.missing == doctest::Approx(0.5f));
    CHECK(coop_text_width(&fa, 4, "A?A") == doctest::Approx(8.0f));  // the pen above: 3 + 2 + 3
}

TEST_CASE("raster: images are sampled and tinted") {
    const std::uint32_t texels[4] = {kRed, 0xFF00FF00u, 0xFFFF0000u, 0};
    const CoopRasterImage img{2, 2, texels};
    Pic p(4, 4);
    coop_raster_set_image(&p.r, COOP_IMAGE_HEART_0, &img);
    auto l = NewList(4, 4);
    coop_draw_image(l.get(), COOP_IMAGE_HEART_0, 0, 0, 2, 2, kWhite);
    p.Draw(*l);
    CHECK(p.at(0, 0) == kRed);
    CHECK(p.at(1, 0) == 0xFF00FF00u);
    CHECK(p.at(0, 1) == 0xFFFF0000u);
    CHECK(p.at(1, 1) == 0);
    coop_draw_begin(l.get(), 4, 4);
    coop_draw_image(l.get(), COOP_IMAGE_HEART_0, 0, 0, 2, 2, 0x80FFFFFFu);
    coop_draw_image(l.get(), COOP_IMAGE_HEART_4, 0, 0, 2, 2, kWhite);  // not set: skipped
    p.Draw(*l);
    CHECK(p.at(0, 0) == 0x80000080u);
    CHECK(p.r.skipped == 1);
}

TEST_CASE("raster: dirty rows are the ones that changed since the last frame") {
    Pic p(16, 16);
    auto l = NewList(16, 16);
    using Bands = std::vector<std::pair<int, int>>;
    coop_draw_rect(l.get(), 0, 2, 4, 5, kRed, 0);
    p.Draw(*l);
    CHECK(p.Dirty() == Bands{{2, 5}});
    p.Draw(*l);
    CHECK(p.Dirty().empty());
    coop_draw_begin(l.get(), 16, 16);
    coop_draw_rect(l.get(), 0, 8, 4, 10, kRed, 0);
    p.Draw(*l);
    CHECK(p.Dirty() == Bands{{2, 5}, {8, 10}});
    coop_draw_begin(l.get(), 16, 16);
    coop_draw_rect(l.get(), 0, 8, 4, 10, kWhite, 0);  // same rows, other colour
    p.Draw(*l);
    CHECK(p.Dirty() == Bands{{8, 10}});
    coop_draw_begin(l.get(), 16, 16);
    p.Draw(*l);
    CHECK(p.Dirty() == Bands{{8, 10}});
    CHECK(p.Count() == 0);  // cleared
    p.Draw(*l);
    CHECK(p.Dirty().empty());
    for (int y = 1; y < 8; y += 2) {
        coop_draw_rect(l.get(), 0, static_cast<float>(y), 1, static_cast<float>(y + 1), kRed, 0);
    }
    p.Draw(*l);
    CHECK(p.Dirty(2) == Bands{{1, 2}, {3, 8}});  // out of room: the last band covers the rest
}

TEST_CASE("raster: clipping, a wider stride and bad commands") {
    Pic p(8, 4, 10);
    for (int y = 0; y < 4; ++y) {
        p.px[static_cast<std::size_t>(y * 10 + 8)] = 0xDEADBEEFu;  // the stride's padding is not ours
    }
    auto l = NewList(8, 4);
    coop_draw_rect(l.get(), -100, -100, 1e9f, 2, kRed, 0);
    coop_draw_circle(l.get(), 4, 4, 1e9f, 0x10FFFFFFu, 0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    coop_draw_rect(l.get(), nan, 0, 4, 4, kRed, 0);
    const float tri[] = {0, 0, 4, 4, 0, 4};
    coop_draw_poly(l.get(), tri, 3, kRed, 0);
    l->cmd[l->n - 1].n = 2;  // not a shape
    coop_draw_rect(l.get(), 0, 0, 1, 1, kRed, 0);
    l->cmd[l->n - 1].kind = 99;
    p.Draw(*l);
    CHECK(p.r.skipped == 3);
    CHECK(A(p.at(0, 0)) == 255);
    CHECK(A(p.at(7, 1)) == 255);
    CHECK(A(p.at(7, 3)) == 16);
    for (int y = 0; y < 4; ++y) {
        CHECK(p.px[static_cast<std::size_t>(y * 10 + 8)] == 0xDEADBEEFu);
    }
}

TEST_CASE("raster: premultiplied to straight") {
    const std::uint32_t in[4] = {0x80000080u, 0, kRed, 0x40102030u};
    std::uint32_t out[4];
    coop_raster_to_straight(in, out, 4);
    CHECK(out[0] == 0x800000FFu);
    CHECK(out[1] == 0);
    CHECK(out[2] == kRed);
    CHECK(out[3] == 0x404080BFu);
}

namespace {

// A scene with every kind of command, for the golden pictures.
void Scene(CoopDrawList* l, float s) {
    coop_draw_rect(l, 1 * s, 1 * s, 30 * s, 11 * s, 0xC0202020u, 3 * s);
    coop_draw_circle(l, 7 * s, 6 * s, 3.5f * s, 0xFF40C0FFu, 0);
    coop_draw_circle(l, 7 * s, 6 * s, 4.5f * s, 0xFFFFFFFFu, 1 * s);
    coop_draw_triangle(l, 13 * s, 2.3f * s, 19.7f * s, 6 * s, 13 * s, 9.8f * s, 0xE0FF8040u);
    const float xy[] = {21 * s, 2 * s, 28.5f * s, 3 * s, 29 * s, 9 * s, 22 * s, 10.4f * s, 20.2f * s, 6 * s};
    coop_draw_poly(l, xy, 5, 0xFF00FFFFu, 1);
    coop_draw_text(l, 22 * s, 4 * s, 4 * s, 0xFFFFFFFFu, "AB\nA");
    coop_draw_image(l, COOP_IMAGE_HEART_0, 24.5f * s, 5.5f * s, 27.5f * s, 8.5f * s, 0xC0FFFFFFu);
}

const char* kGoldenSmall = "TODO";
const char* kGoldenLargeHash = "TODO";

}  // namespace

TEST_CASE("raster: golden pictures") {
    TinyFont tf;
    const std::uint32_t texels[4] = {kRed, 0xFF00FF00u, 0xFFFF0000u, 0x80FFFFFFu};
    const CoopRasterImage img{2, 2, texels};

    Pic small(32, 12);
    coop_raster_set_font(&small.r, COOP_FONT_UI, &tf.font);
    coop_raster_set_image(&small.r, COOP_IMAGE_HEART_0, &img);
    auto l = NewList(32, 12);
    Scene(l.get(), 1);
    small.Draw(*l);
    CHECK(small.r.skipped == 0);
    const std::string got = small.Alpha();
    if (got != kGoldenSmall) {
        FAIL_CHECK("small scene differs; got:" << got);
    }

    Pic large(320, 120);
    coop_raster_set_font(&large.r, COOP_FONT_UI, &tf.font);
    coop_raster_set_image(&large.r, COOP_IMAGE_HEART_0, &img);
    auto ll = NewList(320, 120);
    Scene(ll.get(), 10);
    large.Draw(*ll);
    char hash[24];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(large.Hash()));
    if (std::string(hash) != kGoldenLargeHash) {
        FAIL_CHECK("large scene hash differs; got " << hash);
    }
}
