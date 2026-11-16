// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op UI input (coop/ui/pad.hpp, coop/ui/focus.hpp): the pad as events, the focus stack.
#include <doctest/doctest.h>

#include <vector>

#include "coop/ui/focus.hpp"
#include "coop/ui/pad.hpp"

namespace {

using namespace coop::ui;

std::vector<int> Kinds(const PadEvents& p) {
    std::vector<int> k;
    for (std::size_t i = 0; i < p.n; ++i) {
        k.push_back(p.e[i].kind);
    }
    return k;
}

}  // namespace

TEST_CASE("ui pad: closed, only L and R held then Minus opens") {
    PadReader r;
    CHECK(Kinds(r.Feed(npad::kMinus, 0, 0, 0, false)).empty());   // Minus alone: the game's
    CHECK(Kinds(r.Feed(0, 0, 0, 10, false)).empty());
    CHECK(Kinds(r.Feed(npad::kA | npad::kUp, 0, 32767, 20, false)).empty());   // closed: no navigation
    CHECK(Kinds(r.Feed(npad::kL | npad::kR, 0, 0, 30, false)).empty());
    CHECK(Kinds(r.Feed(npad::kL | npad::kR | npad::kMinus, 0, 0, 40, false)) == std::vector<int>{COOP_UI_OPEN});
    CHECK(Kinds(r.Feed(npad::kL | npad::kR | npad::kMinus, 0, 0, 50, false)).empty());   // held: once
}

TEST_CASE("ui pad: open, buttons on their press, Minus closes") {
    PadReader r;
    r.Feed(0, 0, 0, 0, true);
    CHECK(Kinds(r.Feed(npad::kA, 0, 0, 10, true)) == std::vector<int>{COOP_UI_CONFIRM});
    CHECK(Kinds(r.Feed(npad::kA, 0, 0, 20, true)).empty());   // held: once
    CHECK(Kinds(r.Feed(npad::kB | npad::kL, 0, 0, 30, true)) == std::vector<int>{COOP_UI_BACK, COOP_UI_TAB_PREV});
    CHECK(Kinds(r.Feed(npad::kR, 0, 0, 40, true)) == std::vector<int>{COOP_UI_TAB_NEXT});
    CHECK(Kinds(r.Feed(npad::kMinus | npad::kA, 0, 0, 50, true)) == std::vector<int>{COOP_UI_CLOSE});   // close wins
}

TEST_CASE("ui pad: a held direction repeats, the stick counts past its dead zone") {
    PadReader r;
    r.Feed(0, 0, 0, 0, true);
    CHECK(Kinds(r.Feed(npad::kDown, 0, 0, 1000, true)) == std::vector<int>{COOP_UI_DOWN});
    CHECK(Kinds(r.Feed(npad::kDown, 0, 0, 1399, true)).empty());
    const auto first = r.Feed(npad::kDown, 0, 0, 1400, true);
    REQUIRE(first.n == 1);
    CHECK(first.e[0].kind == COOP_UI_DOWN);
    CHECK(first.e[0].repeat == 1);
    CHECK(Kinds(r.Feed(npad::kDown, 0, 0, 1450, true)).empty());
    CHECK(Kinds(r.Feed(npad::kDown, 0, 0, 1510, true)) == std::vector<int>{COOP_UI_DOWN});
    CHECK(Kinds(r.Feed(0, 0, 0, 1520, true)).empty());   // let go
    CHECK(Kinds(r.Feed(0, 0, 15000, 1530, true)).empty());   // inside the dead zone
    CHECK(Kinds(r.Feed(0, 0, 20000, 1540, true)) == std::vector<int>{COOP_UI_UP});
    CHECK(Kinds(r.Feed(0, -20000, 0, 1550, true)) == std::vector<int>{COOP_UI_LEFT});   // a new direction: at once
    CHECK(Kinds(r.Feed(npad::kStickLRight, 0, 0, 1560, true)) == std::vector<int>{COOP_UI_RIGHT});
    CHECK(PadReader::Direction(npad::kUp | npad::kLeft, 0, 0) == COOP_UI_UP);   // up/down win
}

TEST_CASE("ui focus: the top owner has input, the pad is masked while anyone does") {
    FocusStack f;
    CHECK(f.Top() == Owner::none);
    CHECK_FALSE(f.MasksPad());
    CHECK_FALSE(f.Push(Owner::none));
    CHECK(f.Push(Owner::chat));
    CHECK(f.Push(Owner::menu));
    CHECK(f.Top() == Owner::menu);
    CHECK(f.MasksPad());
    CHECK(f.Push(Owner::chat));   // again: moved to the top, not twice
    CHECK(f.Size() == 2);
    CHECK(f.Top() == Owner::chat);
    f.Pop(Owner::chat);
    CHECK(f.Top() == Owner::menu);
    f.Pop(Owner::cheats);   // not there: nothing
    CHECK(f.Size() == 1);
    f.Push(Owner::cheats);
    f.Push(Owner::chat);
    CHECK(f.Size() == 3);
    f.Pop(Owner::cheats);   // from the middle
    CHECK(f.Has(Owner::menu));
    CHECK_FALSE(f.Has(Owner::cheats));
    CHECK(f.Top() == Owner::chat);
    f.Pop(Owner::chat);
    f.Pop(Owner::menu);
    CHECK_FALSE(f.MasksPad());
}
