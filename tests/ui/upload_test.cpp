// SPDX-License-Identifier: GPL-3.0-or-later
// coop/ui/upload.h: the list hash (frame skip) and the double-buffered upload of changed picture rows.
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "coop/ui/drawlist.h"
#include "coop/ui/upload.h"

namespace {

using Bands = std::vector<std::pair<int, int>>;

struct Up {
    std::vector<std::uint8_t> rows;
    CoopUpload u{};
    Up(int height, int buffers, int gap) : rows(static_cast<std::size_t>(height)) {
        REQUIRE(coop_upload_init(&u, height, buffers, gap, rows.data()) == 1);
    }
    Bands Take(int buffer, int max_rows, int max_bands = 16, int* complete = nullptr) {
        std::vector<CoopRasterBand> b(static_cast<std::size_t>(max_bands > 0 ? max_bands : 1));
        int done = 0;
        const int n = coop_upload_take(&u, buffer, max_rows, b.data(), max_bands, &done);
        if (complete != nullptr) {
            *complete = done;
        }
        Bands out;
        for (int i = 0; i < n; ++i) {
            out.emplace_back(b[static_cast<std::size_t>(i)].y0, b[static_cast<std::size_t>(i)].y1);
        }
        return out;
    }
    void Mark(Bands bands) {
        std::vector<CoopRasterBand> b;
        for (auto [y0, y1] : bands) {
            b.push_back({static_cast<std::uint16_t>(y0), static_cast<std::uint16_t>(y1)});
        }
        coop_upload_mark(&u, b.data(), static_cast<int>(b.size()));
    }
};

}  // namespace

TEST_CASE("upload: the list hash changes with what is drawn, not with the list's history") {
    auto a = std::make_unique<CoopDrawList>();
    auto b = std::make_unique<CoopDrawList>();
    coop_draw_begin(a.get(), 1280, 720);
    coop_draw_begin(b.get(), 1280, 720);
    coop_draw_rect(a.get(), 1, 2, 3, 4, 0xFF0000FFu, 0);
    coop_draw_text(a.get(), 5, 6, 18, 0xFFFFFFFFu, "hello");
    coop_draw_rect(b.get(), 9, 9, 9, 9, 0xFFFFFFFFu, 0);  // b held something else before
    coop_draw_begin(b.get(), 1280, 720);
    coop_draw_rect(b.get(), 1, 2, 3, 4, 0xFF0000FFu, 0);
    coop_draw_text(b.get(), 5, 6, 18, 0xFFFFFFFFu, "hello");
    CHECK(coop_draw_list_hash(a.get()) == coop_draw_list_hash(b.get()));
    coop_draw_begin(b.get(), 1280, 720);
    coop_draw_rect(b.get(), 1, 2, 3, 4, 0xFF0000FEu, 0);  // one colour bit
    coop_draw_text(b.get(), 5, 6, 18, 0xFFFFFFFFu, "hello");
    CHECK(coop_draw_list_hash(a.get()) != coop_draw_list_hash(b.get()));
    coop_draw_begin(b.get(), 1280, 720);
    coop_draw_rect(b.get(), 1, 2, 3, 4, 0xFF0000FFu, 0);
    coop_draw_text(b.get(), 5, 6, 18, 0xFFFFFFFFu, "hellp");  // one letter
    CHECK(coop_draw_list_hash(a.get()) != coop_draw_list_hash(b.get()));
    coop_draw_begin(b.get(), 1920, 1080);  // empty lists of other sizes
    coop_draw_begin(a.get(), 1280, 720);
    CHECK(coop_draw_list_hash(a.get()) != coop_draw_list_hash(b.get()));
}

TEST_CASE("upload: init checks its arguments and marks every row for every texture") {
    std::vector<std::uint8_t> rows(8);
    CoopUpload u{};
    CHECK(coop_upload_init(&u, 0, 2, 0, rows.data()) == 0);
    CHECK(coop_upload_init(&u, 8, 0, 0, rows.data()) == 0);
    CHECK(coop_upload_init(&u, 8, COOP_UPLOAD_MAX_BUFFERS + 1, 0, rows.data()) == 0);
    CHECK(coop_upload_init(&u, 8, 2, -1, rows.data()) == 0);
    CHECK(coop_upload_init(&u, 8, 2, 0, nullptr) == 0);
    REQUIRE(coop_upload_init(&u, 8, 2, 0, rows.data()) == 1);
    CHECK(coop_upload_pending(&u, 0) == 8);
    CHECK(coop_upload_pending(&u, 1) == 8);
}

TEST_CASE("upload: the first frame sends the whole picture to each texture once") {
    Up up(10, 2, 0);
    int complete = 0;
    CHECK(up.Take(0, 100, 16, &complete) == Bands{{0, 10}});
    CHECK(complete == 1);
    CHECK(up.Take(0, 100).empty());
    CHECK(coop_upload_pending(&up.u, 1) == 10);  // the other texture still needs it
    CHECK(up.Take(1, 100, 16, &complete) == Bands{{0, 10}});
    CHECK(complete == 1);
}

TEST_CASE("upload: a changed row is pending for every texture until each had it") {
    Up up(20, 2, 0);
    up.Take(0, 100);
    up.Take(1, 100);
    up.Mark({{3, 5}, {9, 10}});
    int complete = 0;
    CHECK(up.Take(1, 100, 16, &complete) == Bands{{3, 5}, {9, 10}});
    CHECK(complete == 1);
    up.Mark({{12, 13}});  // a later frame: both textures need row 12; texture 0 still needs 3-4 and 9 too
    CHECK(up.Take(0, 100) == Bands{{3, 5}, {9, 10}, {12, 13}});
    CHECK(up.Take(1, 100) == Bands{{12, 13}});
}

TEST_CASE("upload: close bands are merged, gap rows included") {
    Up up(20, 1, 3);
    up.Take(0, 100);
    up.Mark({{2, 3}, {5, 6}, {10, 11}});  // 2 rows between the first two, 4 before the third
    CHECK(up.Take(0, 100) == Bands{{2, 6}, {10, 11}});
}

TEST_CASE("upload: a row budget leaves the rest for the next frame, and the texture is not complete") {
    Up up(10, 1, 0);
    int complete = 1;
    CHECK(up.Take(0, 4, 16, &complete) == Bands{{0, 4}});
    CHECK(complete == 0);
    CHECK(up.Take(0, 4, 16, &complete) == Bands{{4, 8}});
    CHECK(complete == 0);
    CHECK(up.Take(0, 4, 16, &complete) == Bands{{8, 10}});
    CHECK(complete == 1);
    up.Mark({{1, 2}, {3, 4}, {6, 7}});
    CHECK(up.Take(0, 2, 16, &complete) == Bands{{1, 2}, {3, 4}});  // the budget counts rows over all bands
    CHECK(complete == 0);
    CHECK(up.Take(0, 2, 16, &complete) == Bands{{6, 7}});
    CHECK(complete == 1);
}

TEST_CASE("upload: a band limit leaves the rest pending") {
    Up up(10, 1, 0);
    up.Take(0, 100);
    up.Mark({{1, 2}, {3, 4}, {5, 6}});
    int complete = 1;
    CHECK(up.Take(0, 100, 2, &complete) == Bands{{1, 2}, {3, 4}});
    CHECK(complete == 0);
    CHECK(up.Take(0, 100, 2, &complete) == Bands{{5, 6}});
    CHECK(complete == 1);
}
