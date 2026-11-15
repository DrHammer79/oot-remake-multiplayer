// SPDX-License-Identifier: GPL-3.0-or-later
// CoopMapRefresh (dungeon-sync-b, #63/#76): the game's own HUD refresh after another player's map arrives.
#include <doctest/doctest.h>

#include <map>

#include "coop_map_refresh.h"

namespace {
namespace a = CoopMapRefresh::addr;

class FakeGuest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* b = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            b[i] = it == bytes.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* b = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) bytes[address + i] = b[i];
        return true;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

constexpr std::uint64_t kMain = 0x7100000000;
constexpr std::uint64_t kScreen = 0x5000;

}  // namespace

TEST_CASE("map refresh: one call of the game's refresh on the HUD screen, a moment after a map arrives") {
    FakeGuest g;
    CoopMapRefresh::Refresher r;
    CHECK_FALSE(r.Step(g, false, 100));  // nothing wanted
    r.Want(100);
    r.SetScreen(kScreen, kMain);
    CHECK_FALSE(r.Step(g, false, 100 + CoopMapRefresh::kDelayMs - 1));  // the game's setter first
    g.bytes[kScreen + a::kFloors] = 0x4;
    auto c = r.Step(g, false, 100 + CoopMapRefresh::kDelayMs);
    REQUIRE(c);
    CHECK(c->function == kMain + a::kRefresh);
    CHECK(c->x[0] == kScreen);
    // The game's refresh read the flags in: the log has the copy before and after; nothing of ours was written.
    g.bytes[kScreen + a::kFloors] = 0x1f;
    g.bytes[kScreen + a::kHeld] = 1;
    const auto before = g.bytes;
    CHECK_FALSE(r.Step(g, true, 0));
    CHECK(g.bytes == before);
    const auto notes = r.TakeNotes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("floors 0x04, map 0") != std::string::npos);
    CHECK(notes[0].find("floors 0x1f, map 1") != std::string::npos);
    CHECK_FALSE(r.Step(g, false, 1000));  // once
}

TEST_CASE("map refresh: no HUD screen (no patch 0018, a title screen): nothing called, kept until one runs") {
    FakeGuest g;
    CoopMapRefresh::Refresher r;
    r.Want(100);
    CHECK_FALSE(r.Step(g, false, 1000));
    r.SetScreen(kScreen, kMain);
    CHECK(r.Step(g, false, 1100));
}
