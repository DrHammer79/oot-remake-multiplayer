// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "coop_char_ages.h"

using CoopCharAges::Input;
using CoopCharAges::Picks;
using CoopCharAges::Stored;

namespace {

// Installed NPC models: Saria 2, Zelda (adult era) 8, Sheik 9 (number -> file, as the puppet pack lists them).
const std::map<std::uint16_t, std::uint16_t> kFiles{{2, 2}, {8, 8}, {9, 9}};

Input At(bool child, std::int32_t pick) {
    Input in;
    in.childhood = child;
    in.pick = pick;
    return in;
}

} // namespace

TEST_CASE("char ages: off, no mod, no age, the title: nothing") {
    Picks p;
    CHECK_FALSE(p.Frame(At(true, 2), kFiles).write);
    Stored s;
    s.on = true;
    Picks on(s);
    Input no_mod;
    no_mod.childhood = true;
    CHECK_FALSE(on.Frame(no_mod, kFiles).write);
    Input no_age;
    no_age.pick = 2;
    CHECK_FALSE(on.Frame(no_age, kFiles).write);
    Input title = At(false, 2);
    title.title = true;
    CHECK_FALSE(on.Frame(title, kFiles).write);
    CHECK_FALSE(on.stored().adult);  // nothing learnt on the title
}

TEST_CASE("char ages: turning it on keeps what is shown, then each age has its own character") {
    Picks p;
    p.SetOn(true);
    auto st = p.Frame(At(true, 2), kFiles);  // a child playing Saria
    CHECK_FALSE(st.write);
    CHECK(st.stored_changed);
    CHECK(p.stored().child == 2);
    // He grows up: the mod's value still says Saria; the adult has no pick yet, so Saria it is for him too.
    st = p.Frame(At(false, 2), kFiles);
    CHECK_FALSE(st.write);
    CHECK(p.stored().adult == 2);
    // On his page as an adult he picks Zelda: stored for the adult only, not written (his page writes it).
    Input page = At(false, 2);
    page.page_open = true;
    CHECK_FALSE(p.Frame(page, kFiles).write);
    page.pick = 8;
    st = p.Frame(page, kFiles);
    CHECK_FALSE(st.write);
    CHECK(st.stored_changed);
    CHECK(p.stored().adult == 8);
    CHECK(p.stored().child == 2);
    CHECK_FALSE(p.Frame(At(false, 8), kFiles).write);  // the page closed on Zelda: nothing to do
    // Back in time: the child's Saria is written into the mod's value.
    st = p.Frame(At(true, 8), kFiles);
    REQUIRE(st.write);
    CHECK(*st.write == 2);
    CHECK_FALSE(st.stored_changed);
    // Taken: nothing more. Grown up again: Zelda.
    CHECK_FALSE(p.Frame(At(true, 2), kFiles).write);
    st = p.Frame(At(false, 2), kFiles);
    REQUIRE(st.write);
    CHECK(*st.write == 8);
}

TEST_CASE("char ages: a value changed by a save load is written over, never stored, re-written at most every 30") {
    Stored s;
    s.on = true;
    s.child = 2;
    s.adult = 9;
    Picks p(s);
    auto st = p.Frame(At(false, 0), kFiles);  // a save of Link loaded as an adult
    REQUIRE(st.write);
    CHECK(*st.write == 9);
    CHECK(p.stored().adult == 9);
    // Not taken yet: no second write for 30 frames, then again.
    for (int i = 0; i < Picks::kRewriteFrames; ++i) {
        CHECK_FALSE(p.Frame(At(false, 0), kFiles).write);
    }
    CHECK(p.Frame(At(false, 0), kFiles).write == 9);
}

TEST_CASE("char ages: a stored pick not installed shows Link and is kept, the settings rows set a pick") {
    Stored s;
    s.on = true;
    s.child = 77;  // a character whose model is not downloaded
    s.adult = 9;
    Picks p(s);
    const auto st = p.Frame(At(true, 9), kFiles);
    REQUIRE(st.write);
    CHECK(*st.write == 0);
    CHECK(p.stored().child == 77);
    p.SetPick(true, 2);
    CHECK(p.Frame(At(true, 0), kFiles).write == 2);
    // A swap still running: nothing written until it ends.
    Picks q(s);
    Input busy = At(true, 9);
    busy.swap_busy = true;
    CHECK_FALSE(q.Frame(busy, kFiles).write);
}

TEST_CASE("char ages: the settings line round-trips, garbage is refused") {
    Stored s;
    s.on = true;
    s.child = 2;
    CHECK(CoopCharAges::Format(s) == "1 2 -1\n");
    CHECK(CoopCharAges::Parse("1 2 -1\n") == s);
    CHECK(CoopCharAges::Parse("0 -1 9") == Stored{false, std::nullopt, 9});
    CHECK_FALSE(CoopCharAges::Parse(""));
    CHECK_FALSE(CoopCharAges::Parse("2 1 1"));
    CHECK_FALSE(CoopCharAges::Parse("1 x 3"));
    CHECK_FALSE(CoopCharAges::Parse("1 -5 3"));
}
