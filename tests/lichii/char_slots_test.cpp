// lichii/hook/coop_char_slots.h: a Character Select pick whose model is not installed is never kept.
#include <doctest/doctest.h>

#include <map>

#include "../../lichii/hook/coop_char_slots.h"

using CoopCharSlots::Installed;
using CoopCharSlots::Others;
using CoopCharSlots::Own;

namespace {
// Stored number -> file, as the pack maps installed models only (C005 and C010 here).
const std::map<std::uint16_t, std::uint16_t> kFiles{{12, 5}, {30, 10}};
const std::map<std::uint16_t, std::uint16_t> kNone{};
}  // namespace

TEST_CASE("char slots: Link and installed characters are kept") {
    CHECK(Installed(0, kNone));
    CHECK(Installed(-1, kNone));
    CHECK(Installed(12, kFiles));
    CHECK_FALSE(Installed(13, kFiles));
    CHECK_FALSE(Installed(12, kNone));
    CHECK_FALSE(Installed(0x10000 + 12, kFiles));
}

TEST_CASE("char slots: own pick, no mod (no flag): nothing done") {
    Own own;
    const auto s = own.Check(std::nullopt, kFiles);
    CHECK_FALSE(s.set);
    CHECK_FALSE(s.say);
}

TEST_CASE("char slots: own dead pick goes back to the last installed one at once, said once") {
    Own own;
    CHECK_FALSE(own.Check(12, kFiles).set);
    auto s = own.Check(40, kFiles);
    REQUIRE(s.set);
    CHECK(*s.set == 12);
    CHECK(s.say);
    s = own.Check(40, kFiles);   // (the page stores it again before it reads ours)
    REQUIRE(s.set);
    CHECK(*s.set == 12);
    CHECK_FALSE(s.say);
    CHECK_FALSE(own.Check(12, kFiles).set);
    CHECK(own.Check(40, kFiles).say);   // picked again later: said again
}

TEST_CASE("char slots: without the NPC download every character goes back to Link (also a save stored with one)") {
    Own own;
    const auto s = own.Check(30, kNone);   // the first read after a save load
    REQUIRE(s.set);
    CHECK(*s.set == 0);
    CHECK(s.say);
    CHECK_FALSE(own.Check(0, kNone).set);
}

TEST_CASE("char slots: another player's missing pick is said once per number, again only for another one") {
    Others others;
    CHECK_FALSE(others.Tell(2, 0, kNone));
    CHECK(others.Tell(2, 30, kNone));
    CHECK_FALSE(others.Tell(2, 30, kNone));   // every frame after: nothing
    CHECK(others.Tell(3, 30, kNone));         // another player
    CHECK(others.Tell(2, 31, kNone));         // they picked another missing one
    CHECK_FALSE(others.Tell(2, 12, kFiles));  // installed here
    CHECK(others.Tell(2, 31, kNone));
    CHECK_FALSE(others.Tell(17, 31, kNone));  // (no such number)
    CHECK(CoopCharSlots::MissingLine(false, "Saria").rfind("Saria plays", 0) == 0);
    CHECK(CoopCharSlots::MissingLine(true).find("press N") != std::string::npos);
}
