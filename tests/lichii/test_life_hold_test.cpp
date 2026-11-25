// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "coop_test_life_hold.h"

using namespace CoopTestLifeHold;

TEST_CASE("test life hold: off unless asked, 1 is his own max, 4 to 80 also his max") {
    CHECK_FALSE(Parse("").has_value());
    CHECK_FALSE(Parse("0").has_value());
    CHECK_FALSE(Parse("yes").has_value());
    CHECK_FALSE(Parse("3").has_value());
    CHECK_FALSE(Parse("81").has_value());
    CHECK(*Parse("1") == 0);
    CHECK(*Parse("80") == 80);
}

TEST_CASE("test life hold: a hit is undone in Link's component, a down Link is left") {
    // The lane's save: 3 hearts (12). A Lizalfos hit took 5.
    auto w = Step(0, 7, 12);
    CHECK(*w.life == 12);
    CHECK_FALSE(w.max.has_value());
    w = Step(80, 12, 12);  // 20 hearts asked: max and life both
    CHECK(*w.max == 80);
    CHECK(*w.life == 80);
    w = Step(80, 80, 80);
    CHECK_FALSE(w.life.has_value());
    CHECK_FALSE(w.max.has_value());
    w = Step(80, 0, 80);  // down: the game's own way
    CHECK_FALSE(w.life.has_value());
    CHECK_FALSE(Step(0, -1, 12).life.has_value());  // unread
}
