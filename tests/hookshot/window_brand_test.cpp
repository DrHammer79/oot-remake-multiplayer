// SPDX-License-Identifier: GPL-3.0-or-later
// The game window's name and picture (hookshot/src/coop_window_brand.h): the parts that need no window.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "coop_window_brand.h"

TEST_CASE("window brand: the title is Squadala!") {
    CHECK(std::string(CoopWindowBrand::kTitle) == "Squadala!");
    CHECK_FALSE(CoopWindowBrand::TitleNeedsSetting("Squadala!"));
    CHECK(CoopWindowBrand::TitleNeedsSetting("Longshot"));
    CHECK(CoopWindowBrand::TitleNeedsSetting("The Legend of Zelda: Ocarina of Time (Longshot)"));
    CHECK(CoopWindowBrand::TitleNeedsSetting(""));
}

TEST_CASE("window brand: only the game's window class is renamed") {
    CHECK(CoopWindowBrand::IsGameWindowClass("Longshot"));
    CHECK(CoopWindowBrand::IsGameWindowClass("Hookshot"));
    CHECK_FALSE(CoopWindowBrand::IsGameWindowClass("Qt5QWindowIcon"));
    CHECK_FALSE(CoopWindowBrand::IsGameWindowClass(""));
}

TEST_CASE("window brand: icon pixels are bottom-up premultiplied BGRA") {
    // 1 wide, 2 high: top pixel red opaque, bottom pixel half-transparent green
    const std::uint8_t rgba[8] = {255, 0, 0, 255, 0, 200, 0, 128};
    const auto out = CoopWindowBrand::RgbaToIconBgra(rgba, 1, 2);
    REQUIRE(out.size() == 8);
    CHECK(out[0] == 0);  // bottom row first: green * 128 / 255 = 100
    CHECK(out[1] == 100);
    CHECK(out[2] == 0);
    CHECK(out[3] == 128);
    CHECK(out[4] == 0);  // then the red pixel: B G R A
    CHECK(out[5] == 0);
    CHECK(out[6] == 255);
    CHECK(out[7] == 255);
    CHECK(CoopWindowBrand::RgbaToIconBgra(nullptr, 1, 1).empty());
    CHECK(CoopWindowBrand::RgbaToIconBgra(rgba, 0, 2).empty());
}
