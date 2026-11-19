// SPDX-License-Identifier: GPL-3.0-or-later
// The pause menu's Multiplayer tab (issue 264, piece 4; docs/hookshot.md "The
// pause menu's Multiplayer tab"): a fifth tab after System in the game's own
// pause menu, and its page. The pack adds the layouts, both stored hidden
// (tools/puppet_pack/pause_menu.py): the tab Pa_Category_04 on the tab strip,
// the page Pa_Coop_00 (a copy of the System page) where a shown page sits.
// Patch 0030 hands the module every menu screen's update (hos_coop_game_screen)
// and asks it which page the mouse's tab strip counts (hos_coop_game_pause_page).
//
// The game keeps System as its page while ours shows (agreed on #264): R or ZR
// on System, with the menu open and still and nothing over it, shows our page
// over System's and lights the fifth tab; L or ZL gives System back. While it
// shows, the pad is ours but B, + and - (the game closes the menu, or goes to
// the Minus menu, as from any page), so nothing reaches the System page under
// it. The menu closing or leaving System, a window over it, or Game Over gives
// System back at once. Works with co-op idle (solo).
//
// Logic only here (no game reads or calls, no log): Page and Probe, driven by
// tests/hookshot/pause_tab_test.cpp. coop_pause_tab_module.cpp reads the game,
// makes the game's calls and logs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hos_coop_api.h"

namespace CoopPauseTab {

// The game's screens, by their class's vtable (offsets in main, build e0f3ccfaf50065ae; docs/game-ui.md).
inline constexpr std::uint64_t kPagesVtable = 0x4bebaa8;   // main+: UIPlusMenuScreen (PlusMenu_00, the four pages)
inline constexpr std::uint64_t kStripVtable = 0x4be8aa0;   // main+: UIMenuOverlayScreen (MenuOverlay_00, the tabs)
// Screens that open over the System page or end the game: while one opens, shows or closes, our page is not shown.
inline constexpr std::uint64_t kBlockerVtables[] = {
    0x4befbf8,   // UISystemWindowScreen (the System page's questions)
    0x4bef108,   // UISystemOptionScreen
    0x4bef840,   // UISystemSaveLoadScreen
    0x4beeba0,   // UISystemLoadListScreen
    0x4bef4a0,   // UISystemOverlayScreen (the System page's child windows)
    0x4be91f8,   // MessageDialogScreen
    0x4be3820,   // UIAmiiboWindowScreen
    0x4be7068,   // UIGameOverScreen
};
// Fields read (docs/game-ui.md, "The pause menu"; seen in a run, 2026-11-18):
inline constexpr std::uint64_t kScreenState = 0x1a6;   // u8, every screen: 0, 1 opening, 2 open, 3 closing
inline constexpr std::uint64_t kScreenLayout = 0x28;   // a screen's layout
inline constexpr std::uint64_t kStripTabs = 0x328;     // u32: its Pa_Category_%02d parts (5 with the pack's tab)
inline constexpr std::uint64_t kStripTabArray = 0x330; // -> its tab objects, one pointer each
inline constexpr std::uint64_t kStripMode = 0x350;     // s32: 1 the Plus menu's tabs, else the Minus menu's
inline constexpr std::uint64_t kStripLit = 0x358;      // s32: the category it lights (0, 3, 4, 5 the Plus pages)
inline constexpr std::uint64_t kMenuManager = 0x4e7cfa8;   // main+: the global holding the menu manager
inline constexpr std::uint64_t kManagerPage = 0x530;   // s32: the Plus menu's current page (a category)
inline constexpr std::uint64_t kManagerAsked = 0x538;  // s32: the page asked for (the step's)
inline constexpr int kSystemPage = 5;                  // the System page's category
inline constexpr int kPlusMode = 1;
inline constexpr int kRows = 5;                        // the page's buttons (Pa_Button_00..04), System's too
// The game's functions (main+), as its own code calls them:
inline constexpr std::uint64_t kFindPane = 0x76e2b0;   // (layout, "name") -> pane, or 0 (coop_map_page.h's)
inline constexpr std::uint64_t kPaneTable = 0x43c040;  // (layout + 0x40) -> its pane table (kFindPane's first call)
inline constexpr std::uint64_t kPaneByName = 0x43e300; // (table, "name") -> the pane, a part's own (kFindPane gives
                                                       //  a part's inner root instead)
inline constexpr std::uint64_t kSetVisible = 0x20aea0; // (pane, visible): pane +0xa8 bit 0 (as f_802edef0 hides a tab)
inline constexpr std::uint64_t kTabFill = 0xa6ab50;    // (tab, category, enabled): its text and Plus/Minus look
inline constexpr std::uint64_t kTabLight = 0x2ccf60;   // (tab, lit, instant): its CurrentIn / CurrentOut
inline constexpr std::uint64_t kPaneAlpha = 0x2d8ac0;  // (pane, s0 alpha 0..1): a pane's own alpha (+0x74), marked dirty
// A pane's own place's height (+0x70, its x at +0x6c), its place on the screen (+0x4c, +0x50) and its alpha with its
// parents' (+0x78), as read in a run (2026-11-19: the strip's fourth tab and our fifth through the strip's opening).
inline constexpr std::uint64_t kPaneOwnY = 0x70, kPaneScreenY = 0x50, kPaneScreenAlpha = 0x78;
inline constexpr float kFollowMost = 200.0f;
// (pane): the pane's place marked changed (bit 0x40 of its first word) and its matrices made again, as the game's own
// place setter (main+0x20a3e0: (pane, x, y, z)) ends for a part after storing x and y at +0x6c; nothing for a pane whose
// +0xa8 lacks bit 20. Read from main, 2026-11-19: a place written without it stays where it was drawn.
inline constexpr std::uint64_t kPaneMoved = 0x152af50;   // the most our tab is moved from its own place to follow the fourth
inline constexpr std::uint64_t kTabOpen = 0x208350;    // (tab, 0): the tab's part opened and shown, as the game's fill
                                                       //  ends for each of its tabs (else no text shows)
inline constexpr std::uint64_t kMsgRefInit = 0xa66f70; // (ref): {0, 0, -1}, 24 bytes (coop_title.cpp's)
inline constexpr std::uint64_t kMsgRefSet = 0xa6afe0;  // (ref, &"LayoutMsg/...", &"label", 1)
inline constexpr std::uint64_t kPaneText = 0xa67d30;   // (layout, "part/T_Text_00", ref, 1, 0)

// Switch pad bits (nn::hid::NpadButton), as hos_coop_pad gets them.
inline constexpr std::uint64_t kPadA = 1ull << 0, kPadB = 1ull << 1, kPadL = 1ull << 6, kPadR = 1ull << 7,
                               kPadZL = 1ull << 8, kPadZR = 1ull << 9, kPadPlus = 1ull << 10, kPadMinus = 1ull << 11,
                               kPadUp = 1ull << 13, kPadDown = 1ull << 15, kPadStickUp = 1ull << 17,
                               kPadStickDown = 1ull << 19;
// What the game still gets while our page shows: closing the menu, the Minus menu, and up and down, which move the
// System page's focus under ours: the game's own cursor frame (drawn where the focused button is, and our rows sit
// where System's buttons do) moves over our rows, with its sound (seen in a run, 2026-11-18). A is ours: the row is the
// System page's focused button. Left and right are not passed (past the page's edge they would step the menu).
inline constexpr std::uint64_t kPadPassed = kPadB | kPadPlus | kPadMinus | kPadUp | kPadDown | kPadStickUp | kPadStickDown;
// The System page's buttons (read from main: its set-up f_80f990e0): the page object at the pages screen + 0x320,
// its buttons at +0x38, +0x40 ... (5); a button's +0x38 byte is its focus state (Longshot's oot_autoload.c: non-zero and
// not 4 or 5 is focused).
inline constexpr std::uint64_t kSystemPageObject = 0x320, kSystemButtons = 0x38, kButtonState = 0x38;

enum class Kind { kOther, kPages, kStrip, kBlocker };
Kind KindOf(std::uint64_t vtable_offset);

// What one update of a screen shows (read by the module from the game).
struct Facts {
    Kind kind = Kind::kOther;
    std::uint64_t screen = 0;
    int state = 0;
    int tabs = 0, mode = 0, lit = 0;   // the strip's
    int page = -1, asked = -1;         // the pages' (the menu manager's)
};

// The page: when it shows, and what the module does to the game on each screen's update.
// One caller at a time (the module's lock): screen updates on the game's threads, the pad on its input thread.
class Page {
public:
    // What to do to the game at this screen's update (on its thread, with its ctx).
    struct Do {
        bool set_page = false;   // the pages: our page shown (and System hidden) or not
        bool page_shown = false;
        bool set_texts = false;  // the pages: our rows' texts (once per screen object)
        bool set_tabs = false;   // the strip: the fifth tab shown or not, lit or not; System's tab lit or not
        bool tab_shown = false, tab_lit = false, fill_tab = false;
        bool any() const { return set_page || set_texts || set_tabs; }
    };
    Do OnScreen(const Facts& f, std::uint64_t now_ms);
    // Player 1's pad before the game gets it: the buttons the game gets.
    std::uint64_t OnPad(std::uint64_t buttons, std::uint64_t now_ms);

    bool Shown() const { return shown_; }
    // A pressed on our page since the last call (the row is the System page's focused button).
    bool TakeDecide();
    // Why it last went back to System, for the log ("" when none since the last call).
    std::string TakeWhy();

private:
    bool CanShow(std::uint64_t now_ms) const;
    void Hide(const char* why);

    Facts pages_, strip_;
    std::uint64_t pages_ms_ = 0, strip_ms_ = 0, blocked_until_ms_ = 0;
    bool shown_ = false;
    std::uint64_t pad_last_ = 0;
    std::uint64_t swallow_ = 0;   // buttons the module took, kept from the game until let go
    bool decide_ = false;
    std::string why_;
    // What is set in the game, per screen object (-1: not yet).
    std::uint64_t page_screen_ = 0, strip_screen_ = 0;
    int page_set_ = -1;
    bool texts_set_ = false;
    int tab_key_ = -1;
    bool tab_filled_ = false;   // the fifth tab filled in this opening of the strip
};

// The rows (Pa_Button_00..04): Session, Join code, Players, Let in, Leave. Each A leads to co-op's own parts, until
// the rows have native pages of their own: co-op's menu (Hookshot's menu, its Multiplayer tab, over the game's pause
// menu) at a page, its Lobby tab, the clipboard, or the Lobby's /allow.
enum Row { kRowSession = 0, kRowJoinCode = 1, kRowPlayers = 2, kRowLetIn = 3, kRowLeave = 4 };
enum class RowAction {
    kNone,
    kOpenMain,    // co-op's menu, its main page: what co-op does now, Host and Join, Stop and Leave
    kOpenJoin,    // co-op's menu, its Join page (a room name or a code)
    kCopyCode,    // the room name (or join code) to the clipboard, then the main page, which shows it
    kOpenLobby,   // co-op's menu, its Lobby tab: the players, the host's controls
    kAllow,       // the Lobby's /allow: the oldest player who asks to join is let in (its line says who, or nobody)
    kHost,        // host at once, with the kit's settings (its room name, relay, port, approval)
    kConfirmLeave,   // Leave or Stop hosting: the row asks for a second A first (kConfirmMs), then co-op stops
};
inline constexpr std::uint64_t kConfirmMs = 3000;
// What co-op is doing, as the rows need it.
struct Session {
    bool hosting = false;   // this game hosts (its server listens)
    bool in_one = false;    // in a session, hosted or joined (or starting one)
    bool code = false;      // hosting, and a room name or a join code is there to give
};
RowAction ActionFor(int row, const Session& s);

// What the rows say, in the game's own text panes (the module writes them through the pack's placeholder messages,
// Coop_Live_NN, in LayoutMsg/PlusMenu_00): the session's state on the rows themselves.
struct SessionView {
    Session s;
    bool connecting = false;     // starting a server, or joining (not in yet)
    std::string room;            // hosting: the room name (else a join code) friends type, "" none yet
    int players = 0;             // in the session, us included (0: not in one)
    std::vector<std::string> asking;   // hosting: who asks to join, oldest first
    bool confirm_leave = false;  // the Leave row waits for its second A
};
inline constexpr std::size_t kRowChars = 40;   // the most a row's text holds (the System page's button width)
std::vector<std::string> RowTexts(const SessionView& v);
const char* ActionWords(RowAction a);   // for the log

// The log lines a run needs (the first step's probe, kept): a screen object's first update, its updates over its
// first second, and every change of what it shows. Pages and strip only.
class Probe {
public:
    std::vector<std::string> Seen(const Facts& f, std::uint64_t now_ms);

private:
    struct Last {
        std::uint64_t screen = 0;
        Facts facts;
        std::uint64_t first_ms = 0;
        unsigned updates = 0;
        bool counted = false;
    };
    Last pages_, strip_;
};

// The module's part (coop_pause_tab_module.cpp).
void Init(const HosCoopApi* api);
// hos_coop_game_screen: every menu screen's update, on the game's threads; ctx may call() (patch 0030).
void OnScreen(void* ctx, std::uint64_t screen) noexcept;
// hos_coop_pad, after the title's: the buttons the game gets (the sticks zeroed while our page shows).
std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) noexcept;
// hos_coop_game_pause_page: 4 while our page shows, else -1.
int MousePage() noexcept;

}  // namespace CoopPauseTab
