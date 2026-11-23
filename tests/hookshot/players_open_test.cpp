// SPDX-License-Identifier: GPL-3.0-or-later
// F6 opens the game's - menu on co-op's Multiplayer tab, Players view (hookshot/src/coop_players_open.h).
#include <doctest/doctest.h>

#include "coop_players_open.h"

using CoopPlayersOpen::Run;
using CoopPlayersOpen::Seen;
using CoopPlayersOpen::Step;

namespace {
Seen Shut() { return Seen{}; }
Seen Open(int tab, bool tabs_ok = true) {
    Seen s;
    s.strip_open = true;
    s.our_tab = tab;
    s.tabs_ok = tabs_ok;
    return s;
}
}  // namespace

TEST_CASE("F6 players: Minus opens the - menu, R taps to Multiplayer, then the Players view") {
    Run r;
    CHECK(r.Next(Shut(), 0) == Step::kWait);   // (not started: nothing)
    r.Start(1000, false);
    CHECK(r.On());
    CHECK(r.Next(Shut(), 1000) == Step::kPressMinus);
    CHECK(r.Next(Shut(), 1050) == Step::kPressMinus);   // held for kPressMs
    CHECK(r.Next(Shut(), 1000 + CoopPlayersOpen::kPressMs) == Step::kWait);   // let go
    CHECK(r.Next(Open(-1), 1300) == Step::kWait);   // the strip opened on a game tab: settles first
    CHECK(r.Next(Open(-1), 1300 + CoopPlayersOpen::kTapGapMs) == Step::kTapR);
    CHECK(r.Next(Open(-1), 1300 + CoopPlayersOpen::kTapGapMs + 10) == Step::kWait);   // one read only, then the gap
    CHECK(r.Next(Open(-1), 1300 + 2 * CoopPlayersOpen::kTapGapMs) == Step::kTapR);
    CHECK(r.Next(Open(0), 1300 + 2 * CoopPlayersOpen::kTapGapMs + 50) == Step::kShowPlayers);
    CHECK_FALSE(r.On());
    CHECK(r.taps() == 2);
    CHECK(r.presses() == 1);
}

TEST_CASE("F6 players: from Lobby or Mods it taps L; already on Multiplayer it shows at once") {
    Run r;
    r.Start(0, true);   // the - menu is up already: no Minus (it would shut it)
    CHECK(r.Next(Open(2), 0) == Step::kTapL);
    CHECK(r.Next(Open(1), CoopPlayersOpen::kTapGapMs) == Step::kTapL);
    CHECK(r.Next(Open(0), 2 * CoopPlayersOpen::kTapGapMs) == Step::kShowPlayers);
    Run at;
    at.Start(0, true);
    CHECK(at.Next(Open(0), 0) == Step::kShowPlayers);
    CHECK(at.presses() == 0);
}

TEST_CASE("F6 players: gives up, never presses into another screen") {
    // Another menu in front (the + menu: not the - strip): no Minus, given up.
    Run plus;
    plus.Start(0, true);
    CHECK(plus.Next(Shut(), 0) == Step::kGiveUp);
    // The game never opens the - menu (a cutscene, a load): two presses, then given up.
    Run never;
    never.Start(0, false);
    CHECK(never.Next(Shut(), 0) == Step::kPressMinus);
    CHECK(never.Next(Shut(), 500) == Step::kWait);
    CHECK(never.Next(Shut(), CoopPlayersOpen::kOpenWaitMs + 1) == Step::kPressMinus);
    CHECK(never.Next(Shut(), CoopPlayersOpen::kOpenWaitMs + 200) == Step::kWait);
    CHECK(never.Next(Shut(), 2 * CoopPlayersOpen::kOpenWaitMs + 2) == Step::kGiveUp);
    CHECK(never.presses() == CoopPlayersOpen::kMostPresses);
    // The player shuts the menu on the way: not opened again.
    Run shut;
    shut.Start(0, false);
    CHECK(shut.Next(Shut(), 0) == Step::kPressMinus);
    CHECK(shut.Next(Open(-1), 300) == Step::kWait);
    CHECK(shut.Next(Shut(), 900) == Step::kGiveUp);
    // Typing, the chat box, a load: at once.
    Run typing;
    typing.Start(0, false);
    Seen t = Shut();
    t.blocked = true;
    CHECK(typing.Next(t, 0) == Step::kGiveUp);
    // A strip that never reaches our tab (taps stop) and the whole run's limit.
    Run far;
    far.Start(0, true);
    std::uint64_t now = 0;
    int taps = 0;
    for (; taps < 20; ++taps, now += CoopPlayersOpen::kTapGapMs) {
        const Step s = far.Next(Open(-1), now);
        if (s != Step::kTapR) {
            CHECK(s == Step::kGiveUp);
            break;
        }
    }
    CHECK(taps == CoopPlayersOpen::kMostTaps);
    Run slow;
    slow.Start(0, true);
    CHECK(slow.Next(Open(-1), CoopPlayersOpen::kMostMs + 1) == Step::kGiveUp);
}

TEST_CASE("F6 players: a pack without our - tabs stops once the strip settled (F6's drawn list then)") {
    Run r;
    r.Start(0, false);
    CHECK(r.Next(Shut(), 0) == Step::kPressMinus);
    CHECK(r.Next(Open(-1, false), 200) == Step::kWait);   // (just opened: its tabs are still being looked up)
    CHECK(r.Next(Open(-1, false), 200 + CoopPlayersOpen::kTapGapMs) == Step::kNoTabs);
    CHECK_FALSE(r.On());
}
