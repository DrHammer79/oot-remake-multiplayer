// SPDX-License-Identifier: GPL-3.0-or-later
// The pause menu's Multiplayer tab (coop_pause_tab.h): when its page shows, what
// the module does to the game, what the pad lets through, and the probe's lines.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_pause_tab.h"

namespace {

using namespace CoopPauseTab;

Facts Strip(std::uint64_t screen, int tabs, int lit, int mode = kPlusMode, int state = 2) {
    Facts f;
    f.kind = Kind::kStrip;
    f.screen = screen;
    f.tabs = tabs, f.lit = lit, f.mode = mode, f.state = state;
    return f;
}

Facts Pages(std::uint64_t screen, int page, int asked, int state = 2) {
    Facts f;
    f.kind = Kind::kPages;
    f.screen = screen;
    f.page = page, f.asked = asked, f.state = state;
    return f;
}

Facts Blocker(int state) {
    Facts f;
    f.kind = Kind::kBlocker;
    f.screen = 0xb000;
    f.state = state;
    return f;
}

bool Has(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines) {
        if (l.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The menu open on System, the strip with the pack's five tabs, at `now`.
void OnSystem(Page& p, std::uint64_t now, int tabs = 5) {
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    p.OnScreen(Strip(0x9000, tabs, kSystemPage), now);
}

}  // namespace

TEST_CASE("pause tab: the screens are known by their class's vtable") {
    CHECK(KindOf(kPagesVtable) == Kind::kPages);
    CHECK(KindOf(kStripVtable) == Kind::kStrip);
    for (const std::uint64_t v : kBlockerVtables) {
        CHECK(KindOf(v) == Kind::kBlocker);
    }
    CHECK(KindOf(0x4bf06e8) == Kind::kOther);   // the title (UITitleScreen)
    CHECK(KindOf(0x4be8300) == Kind::kOther);   // the HUD (UIMapMenuScreen): updates every frame in play
    // Guest addresses, main at 0x80020000 (docs/game-ui.md and the game's names list).
    CHECK(0x80020000 + kPagesVtable == 0x84c0baa8);
    CHECK(0x80020000 + kStripVtable == 0x84c08aa0);
    CHECK(0x80020000 + kMenuManager == 0x84e9cfa8);
    CHECK(0x80020000 + kBlockerVtables[0] == 0x84c0fbf8);   // UISystemWindowScreen
    CHECK(0x80020000 + kBlockerVtables[7] == 0x84c07068);   // UIGameOverScreen
    CHECK(0x80020000 + kSetVisible == 0x8022aea0);
    CHECK(0x80020000 + kTabFill == 0x80a8ab50);
    CHECK(0x80020000 + kTabLight == 0x802ecf60);
    CHECK(0x80020000 + kPaneTable == 0x8045c040);
    CHECK(0x80020000 + kPaneAlpha == 0x802f8ac0);
    CHECK(0x80020000 + kTabOpen == 0x80228350);
    CHECK(0x80020000 + kPaneMoved == 0x8154af50);
    CHECK(0x80020000 + kPaneByName == 0x8045e300);
}

TEST_CASE("pause tab: the fourth tab's place and fades taken one update ahead, not past where they head") {
    CHECK(Ahead(1023.83f, 1020.12f, 1000.0f) == doctest::Approx(1016.41f));   // (the strip's drop, a run's numbers)
    CHECK(Ahead(1000.48f, 1000.34f, 1000.0f) == doctest::Approx(1000.20f));
    CHECK(Ahead(1000.19f, 1000.0f, 1000.0f) == 1000.0f);                      // (at rest: stays)
    CHECK(Ahead(1001.0f, 1000.2f, 1000.0f) == 1000.0f);                       // (not past its rest)
    CHECK(Ahead(0.5f, 0.844f, 1.0f) == 1.0f);                                 // (a fade in: not past 1)
    CHECK(Ahead(0.156f, 0.5f, 1.0f) == doctest::Approx(0.844f));
    CHECK(Ahead(0.8f, 0.5f, 0.0f) == doctest::Approx(0.2f));                  // (a fade out)
    CHECK(Ahead(0.4f, 0.1f, 0.0f) == 0.0f);
    CHECK(Ahead(1.0f, 1.0f, 1.0f) == 1.0f);
}

TEST_CASE("pause tab: the fifth tab comes in with the strip's opening, filled once per opening") {
    Page p;
    auto d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), 1000);   // (closed)
    CHECK_FALSE(d.tab_shown);
    CHECK_FALSE(d.fill_tab);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), 1016);   // (opening: with the game's four)
    CHECK(d.set_tabs);
    CHECK(d.tab_shown);
    CHECK(d.fill_tab);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 2), 1300);   // (open: not filled again)
    CHECK(d.set_tabs);
    CHECK(d.tab_shown);
    CHECK_FALSE(d.fill_tab);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 3), 2000);   // (closing: fading out with the four)
    CHECK(d.tab_shown);
    CHECK_FALSE(d.fill_tab);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), 2300);   // (closed: hidden)
    CHECK_FALSE(d.tab_shown);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), 3000);   // (the next opening: filled again)
    CHECK(d.fill_tab);
}

TEST_CASE("pause tab: the menu closed on our page opens on it again, not after L") {
    Page p;
    std::uint64_t now = 1000;
    OnSystem(p, now);
    CHECK(p.OnPad(kPadR, now) == 0);
    CHECK(p.Shown());
    p.OnPad(0, now + 16);
    // The menu closes (B or +): our page goes with it.
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), now + 32);
    CHECK_FALSE(p.Shown());
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 3), now + 32);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), now + 300);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), now + 300);
    CHECK_FALSE(p.Shown());
    // The next opening, on System as the game keeps it: our page from its first opening update.
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now + 5000);
    auto d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 1), now + 5000);
    CHECK(p.Shown());
    CHECK(d.set_page);
    CHECK(d.page_shown);
    // (as a run had it: the pages open while the strip still opens; the page stays, the pad's update too)
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), now + 5100);
    p.OnPad(0, now + 5100);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now + 5100);
    CHECK(p.Shown());
    // L, then the menu closed and opened again: System's.
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), now + 5300);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 2), now + 5300);
    p.OnPad(kPadL, now + 5400);
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 5416);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), now + 5500);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), now + 5800);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now + 9000);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 1), now + 9000);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: a menu that opens on another page forgets ours") {
    Page p;
    OnSystem(p, 1000);
    p.OnPad(kPadR, 1000);
    p.OnPad(0, 1016);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), 1032);
    CHECK_FALSE(p.Shown());
    p.OnScreen(Pages(0xa000, 1, 1, 2), 3000);   // (open on another page)
    p.OnScreen(Strip(0x9000, 5, 1, kPlusMode, 2), 3000);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), 3100);   // (then stepped to System)
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 2), 3100);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: R on System shows the page and lights the fifth tab, L gives System back") {
    Page p;
    std::uint64_t now = 1000;
    // First updates: our page hidden (System shown), the fifth tab shown, System's lit.
    auto d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    CHECK(d.set_page);
    CHECK_FALSE(d.page_shown);
    CHECK(d.set_texts);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
    CHECK(d.set_tabs);
    CHECK(d.tab_shown);
    CHECK_FALSE(d.tab_lit);
    CHECK(d.fill_tab);
    CHECK_FALSE(p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now).any());   // (nothing again)
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now).any());
    // R: taken, the page shows at the next updates.
    CHECK(p.OnPad(kPadR, now) == 0);
    CHECK(p.Shown());
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 16);
    CHECK(d.set_page);
    CHECK(d.page_shown);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 16);
    CHECK(d.set_tabs);
    CHECK(d.tab_lit);
    // R held: still the module's (no fresh R for the game).
    CHECK(p.OnPad(kPadR, now + 32) == 0);
    CHECK(p.OnPad(0, now + 48) == 0);
    // L: back to System; L stays the module's until let go, so the game does not step to Items.
    CHECK(p.OnPad(kPadL, now + 64) == 0);
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeWhy() == "L");
    CHECK(p.OnPad(kPadL, now + 80) == 0);
    CHECK(p.OnPad(0, now + 96) == 0);
    CHECK(p.OnPad(kPadL, now + 112) == kPadL);   // (a new L is the game's: Items)
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 120);
    CHECK(d.set_page);
    CHECK_FALSE(d.page_shown);
    // ZR and ZL do the same.
    p.OnPad(0, now + 130);
    CHECK(p.OnPad(kPadZR, now + 140) == 0);
    CHECK(p.Shown());
    CHECK(p.OnPad(kPadZL, now + 150) == 0);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: under the page the game gets only B, + and -, and up and down for its cursor, A is ours") {
    Page p;
    std::uint64_t now = 1000;
    OnSystem(p, now);
    REQUIRE(p.OnPad(kPadR, now) == 0);
    p.OnPad(0, now + 1);
    const std::uint64_t x = 1ull << 2, y = 1ull << 3, left = 1ull << 12, right = 1ull << 14, stick_right = 1ull << 18;
    CHECK(p.OnPad(kPadA, now + 2) == 0);   // A: ours, not the System page's
    CHECK(p.TakeDecide());
    CHECK_FALSE(p.TakeDecide());
    CHECK(p.OnPad(x | y | left | right | stick_right | kPadZL, now + 3) == 0);   // (ZL: back to System, taken)
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 4);
    REQUIRE(p.OnPad(kPadR, now + 5) == 0);
    p.OnPad(0, now + 6);
    // Up and down reach the game: the System page's focus moves, and the game's cursor frame with it over our rows.
    CHECK(p.OnPad(kPadDown, now + 7) == kPadDown);
    CHECK(p.OnPad(kPadUp | kPadStickUp, now + 8) == (kPadUp | kPadStickUp));
    CHECK(p.OnPad(kPadDown | kPadA, now + 9) == kPadDown);
    CHECK(p.TakeDecide());
    p.OnPad(0, now + 10);
    CHECK(p.OnPad(kPadB, now + 11) == kPadB);
    CHECK(p.OnPad(kPadPlus, now + 12) == kPadPlus);
    CHECK(p.OnPad(kPadMinus, now + 13) == kPadMinus);
    CHECK(p.Shown());   // (the game acts on B, + and -; the screens' updates then give System back)
}

TEST_CASE("pause tab: the page does not show off System, mid-step, in the Minus menu, without the pack or under a window") {
    std::uint64_t now = 1000;
    SUBCASE("on Items") {
        Page p;
        p.OnScreen(Pages(0xa000, 4, 4), now);
        p.OnScreen(Strip(0x9000, 5, 4), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);   // (the game's: Items to System)
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("mid-step to System") {
        Page p;
        p.OnScreen(Pages(0xa000, 4, kSystemPage), now);
        p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("the Minus menu") {
        Page p;
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
        p.OnScreen(Strip(0x9000, 5, 2, 0), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("four tabs: the pack's layouts are not there") {
        Page p;
        OnSystem(p, now, 4);
        CHECK(p.OnPad(kPadR, now) == kPadR);
        CHECK_FALSE(p.Shown());
        CHECK_FALSE(p.OnScreen(Strip(0x9000, 4, kSystemPage), now).set_tabs);   // (no fifth tab to touch)
    }
    SUBCASE("the menu still opening") {
        Page p;
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 1), now);
        p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("a window over the System page (Save, Options, a question)") {
        Page p;
        OnSystem(p, now);
        p.OnScreen(Blocker(2), now);
        CHECK(p.OnPad(kPadR, now + 10) == kPadR);
        CHECK_FALSE(p.Shown());
        // Gone (state 0) for a while: R is ours again.
        p.OnScreen(Blocker(0), now + 20);
        p.OnPad(0, now + 299);
        OnSystem(p, now + 300);
        CHECK(p.OnPad(kPadR, now + 300) == 0);
        CHECK(p.Shown());
    }
    SUBCASE("the screens' updates stopped") {
        Page p;
        OnSystem(p, now);
        CHECK(p.OnPad(kPadR, now + 1000) == kPadR);
        CHECK_FALSE(p.Shown());
    }
}

TEST_CASE("pause tab: closing, the Minus menu, a window or Game Over give System back at once") {
    std::uint64_t now = 1000;
    const auto shown = [&](Page& p) {
        OnSystem(p, now);
        REQUIRE(p.OnPad(kPadR, now) == 0);
        p.OnPad(0, now + 1);
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 2);
        REQUIRE(p.Shown());
    };
    SUBCASE("B closes the menu") {
        Page p;
        shown(p);
        CHECK(p.OnPad(kPadB, now + 3) == kPadB);
        const auto d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), now + 20);
        CHECK_FALSE(p.Shown());
        CHECK(d.set_page);
        CHECK_FALSE(d.page_shown);
        CHECK(p.TakeWhy() == "the menu is closing");
    }
    SUBCASE("the Minus menu") {
        Page p;
        shown(p);
        p.OnScreen(Strip(0x9000, 5, 2, 0), now + 20);
        CHECK_FALSE(p.Shown());
        CHECK(p.TakeWhy() == "the Minus menu");
    }
    SUBCASE("Game Over or a window") {
        Page p;
        shown(p);
        p.OnPad(kPadA, now + 3);   // (A held as the page goes)
        p.OnScreen(Blocker(1), now + 20);
        CHECK_FALSE(p.Shown());
        CHECK(p.TakeWhy() == "a window opened over it");
        CHECK(p.OnPad(kPadA, now + 21) == 0);         // still held: not a fresh A for System
        CHECK(p.OnPad(0, now + 22) == 0);
        CHECK(p.OnPad(kPadA, now + 23) == kPadA);     // a new press is the game's
    }
    SUBCASE("a new pages screen object (a load) starts over: our page hidden") {
        Page p;
        shown(p);
        const auto d = p.OnScreen(Pages(0xa100, kSystemPage, kSystemPage, 0), now + 20);
        CHECK_FALSE(p.Shown());
        CHECK(d.set_page);
        CHECK_FALSE(d.page_shown);
    }
}

TEST_CASE("pause tab: the probe logs a screen's first update, its first second's count and each change") {
    Probe p;
    auto lines = p.Seen(Strip(0x9000, 5, 5), 1000);
    REQUIRE(lines.size() == 1);
    CHECK(Has(lines, "the tab strip (MenuOverlay_00), screen 0x9000: its updates reach the module (patch 0030)"));
    CHECK(Has(lines, "5 tabs, the Plus menu, lights category 5, state 2"));
    for (int i = 1; i < 60; ++i) {
        CHECK(p.Seen(Strip(0x9000, 5, 5), 1000 + 16 * static_cast<std::uint64_t>(i)).empty());
    }
    lines = p.Seen(Strip(0x9000, 5, 5), 2000);
    REQUIRE(lines.size() == 1);
    CHECK(Has(lines, "the tab strip (MenuOverlay_00): 61 updates in its first 1000 ms"));
    CHECK(p.Seen(Strip(0x9000, 5, 5), 3000).empty());
    lines = p.Seen(Strip(0x9000, 5, 4), 3016);
    REQUIRE(lines.size() == 1);
    CHECK(Has(lines, "lights category 4, state 2 (was 5 tabs, the Plus menu, lights category 5, state 2)"));
    CHECK(Has(p.Seen(Strip(0x9000, 5, 2, 0), 3032), "the Minus menu, lights category 2"));   // (mode 0: Minus, seen in a run)
    lines = p.Seen(Pages(0xa000, 5, 5), 3048);
    CHECK(Has(lines, "the pause menu's pages (PlusMenu_00), screen 0xa000: its updates reach the module"));
    CHECK(Has(p.Seen(Pages(0xa000, 5, 4), 3064), "page 5, asked 4, state 2 (was page 5, asked 5, state 2)"));
    CHECK(Has(p.Seen(Strip(0x9100, 4, 5), 4000), "screen 0x9100: its updates reach the module"));
    Facts other;
    other.screen = 0x9000;
    CHECK(p.Seen(other, 0).empty());
    CHECK(p.Seen(Blocker(2), 0).empty());
}

TEST_CASE("pause tab: each row leads to co-op's own part, by what co-op is doing") {
    Session idle;
    Session joined;
    joined.in_one = true;
    Session hosting_no_code;
    hosting_no_code.hosting = hosting_no_code.in_one = true;
    Session hosting = hosting_no_code;
    hosting.code = true;

    for (const Session& s : {idle, joined, hosting_no_code, hosting}) {
        CHECK(ActionFor(kRowSession, s) == (s.in_one ? RowAction::kOpenMain : RowAction::kHost));
        CHECK(ActionFor(kRowPlayers, s) == RowAction::kOpenLobby);
        CHECK(ActionFor(kRowLeave, s) == (s.in_one ? RowAction::kConfirmLeave : RowAction::kNone));
        CHECK(ActionFor(-1, s) == RowAction::kNone);              // (no row under the cursor)
        CHECK(ActionFor(kRows, s) == RowAction::kNone);
    }
    // Join code: the host copies its room name or code (shown on the main page); a host whose code is still coming
    // sees the main page; in another's session the main page; alone the Join page.
    CHECK(ActionFor(kRowJoinCode, hosting) == RowAction::kCopyCode);
    CHECK(ActionFor(kRowJoinCode, hosting_no_code) == RowAction::kOpenMain);
    CHECK(ActionFor(kRowJoinCode, joined) == RowAction::kOpenMain);
    CHECK(ActionFor(kRowJoinCode, idle) == RowAction::kOpenJoin);
    // Let in: the host's /allow; anyone else the Lobby tab (who is in, who asks).
    CHECK(ActionFor(kRowLetIn, hosting) == RowAction::kAllow);
    CHECK(ActionFor(kRowLetIn, hosting_no_code) == RowAction::kAllow);
    CHECK(ActionFor(kRowLetIn, joined) == RowAction::kOpenLobby);
    CHECK(ActionFor(kRowLetIn, idle) == RowAction::kOpenLobby);
    // Each has words for the log.
    for (RowAction a : {RowAction::kNone, RowAction::kOpenMain, RowAction::kOpenJoin, RowAction::kCopyCode,
                        RowAction::kOpenLobby, RowAction::kAllow, RowAction::kHost, RowAction::kConfirmLeave}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }
}

TEST_CASE("pause tab: the rows say what co-op is doing, in at most a row's width") {
    SessionView idle;
    CHECK(RowTexts(idle) ==
          std::vector<std::string>{"Host a game", "Join a game", "Players", "Let in", "Leave (not in a session)"});

    SessionView host;
    host.s.hosting = host.s.in_one = true;
    host.players = 1;
    CHECK(RowTexts(host) ==
          std::vector<std::string>{"Hosting", "Room: coming...", "Players: just you", "Let in: nobody asks", "Stop hosting"});
    host.room = "forest-gang";
    host.players = 3;
    host.asking = {"Saria"};
    CHECK(RowTexts(host)[kRowJoinCode] == "Room: forest-gang");
    CHECK(RowTexts(host)[kRowPlayers] == "Players: 3");
    CHECK(RowTexts(host)[kRowLetIn] == "Let in Saria");
    host.asking = {"Saria", "Mido", "Darunia"};
    CHECK(RowTexts(host)[kRowLetIn] == "Let in Saria (+2)");

    SessionView guest;
    guest.s.in_one = true;
    guest.players = 2;
    CHECK(RowTexts(guest) == std::vector<std::string>{"In a session", "Room", "Players: 2", "Let in", "Leave"});
    guest.confirm_leave = true;
    CHECK(RowTexts(guest)[kRowLeave] == "A again: leave");
    host.confirm_leave = true;
    CHECK(RowTexts(host)[kRowLeave] == "A again: stop hosting");
    host.confirm_leave = false;
    SessionView joining;
    joining.connecting = true;
    joining.s.in_one = true;
    CHECK(RowTexts(joining)[kRowSession] == "Connecting...");

    // A long room name is cut at a UTF-8 character's start, with "...".
    host.room = std::string(30, 'a') + "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
    const std::string cut = RowTexts(host)[kRowJoinCode];
    CHECK(cut.size() <= kRowChars);
    CHECK(cut.substr(cut.size() - 3) == "...");
    CHECK((static_cast<unsigned char>(cut[cut.size() - 4]) & 0xc0) != 0xc0);   // (no lead byte left alone)
    for (const SessionView& v : {idle, host, guest, joining}) {
        for (const std::string& r : RowTexts(v)) CHECK(r.size() <= kRowChars);
    }
}
