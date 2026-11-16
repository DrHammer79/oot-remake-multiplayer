// SPDX-License-Identifier: GPL-3.0-or-later
// The ExeFS module's pad hook (switch/adapter/pad_hook.h, v1 step 8 (d)), on the host: what the game gets from a
// GetNpadStates result, the pad co-op keeps, the edges and the UI events.
#include <doctest/doctest.h>

#include "../../switch/adapter/pad_hook.h"

#include <vector>

namespace {

using coop::ui::npad::kA;
using coop::ui::npad::kB;
using coop::ui::npad::kL;
using coop::ui::npad::kMinus;
using coop::ui::npad::kR;
using pad_hook::Hook;
using pad_hook::NpadState;

constinit Hook g_module_hook{5000};  // the module's is constinit (no static constructors there)

NpadState State(uint64_t buttons, int32_t lx = 0, int32_t ly = 0, bool connected = true) {
    NpadState s{};
    s.buttons = buttons;
    s.stick[0] = lx;
    s.stick[1] = ly;
    s.stick[2] = 7;
    s.attributes = connected ? pad_hook::kAttrConnected : 0;
    return s;
}

// One read of player 1 (No.1): what the game gets.
NpadState Read(Hook& h, uint64_t buttons, uint64_t now, int32_t lx = 0, int32_t ly = 0) {
    NpadState s = State(buttons, lx, ly);
    h.OnStates(&s, 1, 0, now);
    return s;
}

std::vector<int> Kinds(Hook& h) {
    CoopUiEvent ev[32];
    std::vector<int> out;
    for (size_t n = h.TakeEvents(ev, 32), i = 0; i < n; ++i) out.push_back(ev[i].kind);
    return out;
}

// Opens the menu the way a player does: L and R held, then Minus.
void Open(Hook& h, uint64_t now) {
    Read(h, kL | kR, now);
    Read(h, kL | kR | kMinus, now + 16);
}

}  // namespace

TEST_CASE("pad hook: npad ids map to players 1-4, Handheld is player 1") {
    CHECK(pad_hook::PlayerOf(0) == 0);
    CHECK(pad_hook::PlayerOf(3) == 3);
    CHECK(pad_hook::PlayerOf(4) == -1);
    CHECK(pad_hook::PlayerOf(0x20) == 0);
    CHECK(pad_hook::PlayerOf(0x10) == -1);
    CHECK(g_module_hook.open() == false);
}

TEST_CASE("pad hook: closed, the game gets its pad unchanged and co-op keeps it with every edge") {
    Hook h;
    const NpadState got = Read(h, kA, 0, 1000, -2000);
    CHECK(got.buttons == kA);
    CHECK(got.stick[0] == 1000);
    CHECK(got.stick[1] == -2000);
    CHECK(got.stick[2] == 7);
    CHECK(h.pad(0).buttons == kA);
    CHECK(h.pad(0).stick[1] == -2000);
    CHECK(h.pad(0).reads == 1);
    Read(h, kB, 16);
    pad_hook::Edge e[8];
    REQUIRE(h.TakeEdges(e, 8) == 3);
    CHECK((e[0].player == 0 && e[0].bit == 0 && e[0].down == 1));  // A down
    CHECK((e[1].bit == 0 && e[1].down == 0));                      // A up
    CHECK((e[2].bit == 1 && e[2].down == 1));                      // B down
    CHECK(h.TakeEdges(e, 8) == 0);
    CHECK(Kinds(h).empty());
}

TEST_CASE("pad hook: a result that is not connected is not the pad") {
    Hook h;
    Read(h, kA, 0);
    NpadState other = State(0, 0, 0, false);  // the game also asks the styles that are not connected
    h.OnStates(&other, 1, 0x20, 16);
    CHECK(h.pad(0).buttons == kA);
    CHECK(h.pad(0).reads == 1);
    NpadState none{};
    h.OnStates(&none, 0, 0, 32);  // no states
    h.OnStates(nullptr, 1, 0, 32);
    CHECK(h.pad(0).reads == 1);
}

TEST_CASE("pad hook: players 2-4 are kept and never held back") {
    Hook h;
    Open(h, 0);
    REQUIRE(h.open());
    NpadState s = State(kA, 30000, 0);
    h.OnStates(&s, 1, 1, 50);
    CHECK(s.buttons == kA);
    CHECK(s.stick[0] == 30000);
    CHECK(h.pad(1).buttons == kA);
    CHECK(h.pad(0).buttons == (kL | kR | kMinus));
}

TEST_CASE("pad hook: L+R then Minus opens the menu, the game gets no buttons and centred sticks") {
    Hook h;
    CHECK(Read(h, kL | kR, 0).buttons == (kL | kR));
    const NpadState opening = Read(h, kL | kR | kMinus, 16, 0, 32000);
    CHECK(h.open());
    CHECK(opening.buttons == 0);  // the Minus that opened it never reaches the game
    CHECK(opening.stick[1] == 0);
    CHECK(opening.stick[2] == 0);
    CHECK(Kinds(h) == std::vector<int>{COOP_UI_OPEN});
    const NpadState held = Read(h, kA, 32, 0, 32000);  // A and a held stick: the menu's, not Link's
    CHECK(held.buttons == 0);
    CHECK(held.stick[1] == 0);
    CHECK(h.pad(0).stick[1] == 32000);  // co-op still sees the real stick
    CHECK(Kinds(h) == std::vector<int>{COOP_UI_CONFIRM, COOP_UI_UP});
}

TEST_CASE("pad hook: Minus closes, buttons held then stay from the game until let go") {
    Hook h;
    Open(h, 0);
    Read(h, kB, 100);
    const NpadState closing = Read(h, kB | kMinus, 116);
    CHECK_FALSE(h.open());
    CHECK(closing.buttons == 0);  // nor the Minus that closed it
    CHECK(Read(h, kB | kMinus, 132).buttons == 0);
    CHECK(Read(h, kB, 148).buttons == 0);    // B still held from the menu
    CHECK(Read(h, kB | kA, 164).buttons == kA);  // A is a new press
    Read(h, 0, 180);
    CHECK(Read(h, kB, 196).buttons == kB);   // let go and pressed again: the game's
    CHECK(Kinds(h) == std::vector<int>{COOP_UI_OPEN, COOP_UI_BACK, COOP_UI_CLOSE});
}

TEST_CASE("pad hook: the menu closes by itself after auto_close_ms") {
    Hook h(5000);
    Open(h, 1000);
    CHECK(Read(h, 0, 5000, 0, 32000).stick[1] == 0);
    CHECK(h.open());
    const NpadState after = Read(h, 0, 6016, 0, 32000);
    CHECK_FALSE(h.open());
    CHECK(h.auto_closes() == 1);
    CHECK(after.stick[1] == 0);  // the read it closed on is still the menu's
    CHECK(Read(h, 0, 6032, 0, 32000).stick[1] == 32000);
    CHECK(Kinds(h) == std::vector<int>{COOP_UI_OPEN, COOP_UI_UP, COOP_UI_CLOSE});
    Hook never;
    Open(never, 0);
    Read(never, 0, 1000000);
    CHECK(never.open());
}

TEST_CASE("pad hook: older states in one result get the same mask") {
    Hook h;
    Open(h, 0);
    NpadState s[3] = {State(kA, 0, 32000), State(kB, 0, 32000), State(kL, 0, 32000)};
    h.OnStates(s, 3, 0, 50);
    for (const NpadState& x : s) {
        CHECK(x.buttons == 0);
        CHECK(x.stick[1] == 0);
    }
    h.SetOpen(false, 60);
    NpadState t[2] = {State(kA), State(kA | kB)};
    h.OnStates(t, 2, 0, 70);  // A was held while open: latched; B in the older state passes
    CHECK(t[0].buttons == 0);
    CHECK(t[1].buttons == kB);
}

TEST_CASE("pad hook: the queues keep the oldest and count what they lost") {
    Hook h;
    for (int i = 0; i < 40; ++i) Read(h, (i % 2) ? kA : 0, static_cast<uint64_t>(i) * 16);
    pad_hook::Edge e[64];
    CHECK(h.TakeEdges(e, 64) == 39);
    CHECK(h.dropped_edges() == 0);
    for (int i = 0; i < 80; ++i) Read(h, (i % 2) ? 0 : kA, 1000 + static_cast<uint64_t>(i) * 16);
    CHECK(h.TakeEdges(e, 64) == 64);  // 79 edges (the first read repeats the held A)
    CHECK(h.dropped_edges() == 15);
    CHECK(e[0].down == 0);  // the oldest kept: A up after the first batch ended with A down
}
