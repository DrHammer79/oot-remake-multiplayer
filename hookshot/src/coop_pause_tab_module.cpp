// SPDX-License-Identifier: GPL-3.0-or-later
// coop_pause_tab.h, the module's part: patch 0030's call for every menu
// screen's update, the game's fields read, the game's own calls made on that
// screen's thread (its panes shown or hidden, its tabs lit, texts from the
// pack's labels), the pad filtered, the log. Only in hookshot_coop.dll (the
// tests drive Page and Probe themselves).
#include "coop_pause_tab.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop_pause.h"
#include "coop_runtime.h"
#include "coop_lobby_game.h"
#include "coop_save.h"

namespace CoopPauseTab {

namespace {

const HosCoopApi* g_api = nullptr;
std::mutex g_mutex;   // everything below: screen updates on several of the game's threads, the pad on its own
Probe g_probe;
Page g_page;
std::atomic<bool> g_shown{false};   // (the mouse asks often, from any thread)
// The Leave row's second A: until when it is waited for (0: not). Any thread.
std::atomic<std::uint64_t> g_confirm_until{0};
// Panes found, per screen object (0: not looked up yet or not there).
std::uint64_t g_pages_screen = 0, g_system_pane = 0, g_coop_pane = 0;
std::uint64_t g_strip_screen = 0, g_tab_pane = 0;
std::uint64_t g_fourth_pane = 0;   // the game's fourth tab (Pa_Category_03), which ours follows in the strip's animation
float g_tab_rest_y = 0;            // our tab's own height in the layout (its place once the strip is open)
bool g_tab_shown = false;          // our tab shown now (the strip opening, open or closing)
bool g_pages_bad = false, g_strip_bad = false;   // a pane that is not a part: the page step stays off (logged once)

template <class T>
T Read(std::uint64_t a) {
    T v{};
    return CoopRuntime::Read(a, &v, sizeof v) ? v : T{};
}

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool CanCall() {
    return g_api != nullptr && g_api->call != nullptr && g_api->size >= offsetof(HosCoopApi, scratch) + sizeof(g_api->scratch) &&
           g_api->scratch != nullptr;
}

bool Call(void* ctx, std::uint64_t offset, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
          std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t* x0 = nullptr) {
    HosCoopCall k{};
    k.function = CoopRuntime::MainBase() + offset;
    k.x[0] = a0, k.x[1] = a1, k.x[2] = a2, k.x[3] = a3, k.x[4] = a4;
    std::uint64_t r = 0;
    const bool ok = g_api->call(ctx, &k, &r);
    if (x0 != nullptr) *x0 = r;
    return ok;
}

void CallAlpha(void* ctx, std::uint64_t pane, float alpha) {
    HosCoopCall k{};
    k.function = CoopRuntime::MainBase() + kPaneAlpha;
    k.x[0] = pane;
    k.s0 = alpha;
    std::uint64_t r = 0;
    g_api->call(ctx, &k, &r);
}

// (layout, "name") -> the pane itself, a part's own pane too. kFindPane (main+0x76e2b0) hands a part's inner root
// instead (its layout's pane 0, the one the game hides a tab through); the pause tab's copies are stored hidden on the
// part's own pane, so that is the one shown. The same two calls kFindPane makes first (read from main, 2026-11-18):
// main+0x43c040(layout + 0x40) -> the layout's pane table, main+0x43e300(table, "name") -> the pane by name.
std::uint64_t FindOwnPane(void* ctx, std::uint64_t layout, const char* name) {
    std::uint64_t table = 0, pane = 0;
    if (!Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table) || table == 0) return 0;
    const std::uint64_t at = g_api->scratch(ctx, name, std::strlen(name) + 1);
    return at != 0 && Call(ctx, kPaneByName, table, at, 0, 0, 0, &pane) ? pane : 0;
}

// A part's own layout, from the parts list of the layout holding it (the list kFindPane walks when the name table has
// no such pane, read from main 2026-11-18: sentinel table + 0x128, first [table + 0x130], next [node + 8], each node a
// layout whose name is at [node + 0x148]). `table` is a layout as kPaneTable gives it: its panes by name are kPaneByName.
std::uint64_t PartLayout(std::uint64_t table, const char* name) {
    if (table == 0) return 0;
    const std::uint64_t end = table + 0x128;
    std::uint64_t node = Read<std::uint64_t>(table + 0x130);
    for (int guard = 0; node != 0 && node != end && guard < 256; ++guard) {
        char got[64] = {};
        const std::uint64_t at = Read<std::uint64_t>(node + 0x148);
        if (at != 0 && CoopRuntime::Read(at, got, sizeof got - 1) && std::strcmp(got, name) == 0) return node;
        node = Read<std::uint64_t>(node + 8);
    }
    return 0;
}

std::uint64_t PaneIn(void* ctx, std::uint64_t table, const char* name) {
    if (table == 0) return 0;
    const std::uint64_t at = g_api->scratch(ctx, name, std::strlen(name) + 1);
    std::uint64_t pane = 0;
    return at != 0 && Call(ctx, kPaneByName, table, at, 0, 0, 0, &pane) ? pane : 0;
}

// A part's pane (kind 6 at +0xa8 bits 22..25, docs/game-ui.md): what the visibility call is made on.
bool IsPart(std::uint64_t pane) {
    return pane != 0 && ((Read<std::uint32_t>(pane + 0xa8) >> 22) & 0xf) == 6;
}

void SetVisible(void* ctx, std::uint64_t pane, bool on) {
    Call(ctx, kSetVisible, pane, on ? 1 : 0);
}

// A text pane's text from a label of the pack's (LayoutMsg/<file>), the way the game sets its own (coop_title.cpp's
// SetRowTexts): [0x00] the message ref, [0x20] &file, [0x28] &label, [0x30] file, [0x50] label, [0x90] the pane's path.
bool SetText(void* ctx, std::uint64_t layout, const std::string& path, const std::string& file, const std::string& label) {
    if (file.size() >= 0x20 || label.size() >= 0x40 || path.size() >= 0x40) return false;
    std::uint8_t b[0xd0] = {};
    const std::uint64_t base = g_api->scratch(ctx, b, sizeof b);
    if (base == 0) return false;
    const std::uint64_t at_file = base + 0x30, at_label = base + 0x50, at_path = base + 0x90;
    std::memcpy(b + 0x20, &at_file, 8);
    std::memcpy(b + 0x28, &at_label, 8);
    std::memcpy(b + 0x30, file.c_str(), file.size() + 1);
    std::memcpy(b + 0x50, label.c_str(), label.size() + 1);
    std::memcpy(b + 0x90, path.c_str(), path.size() + 1);
    if (g_api->scratch(ctx, b, sizeof b) != base) return false;
    return Call(ctx, kMsgRefInit, base) && Call(ctx, kMsgRefSet, base, base + 0x20, base + 0x28, 1) &&
           Call(ctx, kPaneText, layout, at_path, base, 1, 0);
}

// --- Live text: our own words in the game's text panes ---
// A message reference is {text, its length in bytes, the message's attributes} (main+0xa6afe0 fills it; for a missing
// label it points at "LabelNotFound", 13). This game's message files are UTF-8. The pack adds placeholder messages
// (strings/en.txt: Coop_Live_00..07, 64 dots each) to LayoutMsg/PlusMenu_00: the module looks one up once, writes its
// own words into that text (the language archive stays loaded, so the text pane may read it later) and hands the
// game's pane-text call (kPaneText) a reference with the new length.
constexpr int kLiveSlots = 8;
constexpr std::uint32_t kLivePlaceholder = 64;   // the placeholder's bytes (strings/en.txt)
struct LiveSlot {
    std::uint64_t text = 0;     // the placeholder's text in the loaded message file (0: not looked up)
    std::uint32_t extra = 0;    // what the game's length counts beyond the text (its 0 at the end: 1, else 0)
    std::uint64_t attrs = 0;    // the reference's third word, kept
    bool bad = false;           // looked up and not ours (an old pack): no live text
    std::string shown;          // what the pane shows now (per pages screen)
};
std::mutex g_live_mutex;        // the slots (the pages' updates come on several threads)
LiveSlot g_live[kLiveSlots];
std::uint64_t g_live_screen = 0, g_live_ms = 0;

// The slot's placeholder, looked up once (the reference the game makes for it).
bool LiveLookup(void* ctx, int slot) {
    LiveSlot& l = g_live[slot];
    if (l.text != 0) return true;
    if (l.bad) return false;
    char label[16];
    std::snprintf(label, sizeof label, "Coop_Live_%02d", slot);
    const std::string file = "LayoutMsg/PlusMenu_00";
    std::uint8_t b[0x80] = {};
    const std::uint64_t base = g_api->scratch(ctx, b, sizeof b);
    if (base == 0) return false;
    const std::uint64_t at_file = base + 0x30, at_label = base + 0x60;
    std::memcpy(b + 0x20, &at_file, 8);
    std::memcpy(b + 0x28, &at_label, 8);
    std::memcpy(b + 0x30, file.c_str(), file.size() + 1);
    std::memcpy(b + 0x60, label, std::strlen(label) + 1);
    if (g_api->scratch(ctx, b, sizeof b) != base) return false;
    if (!Call(ctx, kMsgRefInit, base) || !Call(ctx, kMsgRefSet, base, base + 0x20, base + 0x28, 1)) return false;
    const std::uint64_t text = Read<std::uint64_t>(base);
    const std::uint32_t length = Read<std::uint32_t>(base + 8);
    char head[8] = {};
    CoopRuntime::Read(text, head, sizeof head);
    if (text == 0 || std::memcmp(head, "........", 8) != 0 || length < kLivePlaceholder ||
        length > kLivePlaceholder + 1) {
        l.bad = true;
        LOG_INFO(Core, "coop: pause tab: live text: {} is not the pack's placeholder (text {:#x}, length {}): the rows "
                       "keep their own words (build the puppet pack again)", label, text, length);
        return false;
    }
    l.text = text;
    l.extra = length - kLivePlaceholder;
    l.attrs = Read<std::uint64_t>(base + 0x10);
    LOG_INFO(Core, "coop: pause tab: live text: {} at {:#x}, the game counts {} byte(s) more than its text", label, text,
             l.extra);
    return true;
}

// `utf8` (at most kLivePlaceholder - 1 bytes) in the pane at `path`, through slot `slot`. True when shown.
bool LiveText(void* ctx, std::uint64_t layout, const std::string& path, int slot, const std::string& utf8) {
    LiveSlot& l = g_live[slot];
    if (l.shown == utf8) return true;
    if (path.size() >= 0x40 || utf8.size() >= kLivePlaceholder || !LiveLookup(ctx, slot)) return false;
    std::string bytes = utf8;
    bytes.push_back('\0');
    if (!CoopRuntime::Write(l.text, bytes.data(), bytes.size())) return false;
    std::uint8_t b[0x60] = {};
    const std::uint64_t base = g_api->scratch(ctx, b, sizeof b);
    if (base == 0) return false;
    const std::uint32_t length = static_cast<std::uint32_t>(utf8.size()) + l.extra;
    std::memcpy(b + 0x00, &l.text, 8);
    std::memcpy(b + 0x08, &length, 4);
    std::memcpy(b + 0x10, &l.attrs, 8);
    std::memcpy(b + 0x20, path.c_str(), path.size() + 1);
    if (g_api->scratch(ctx, b, sizeof b) != base) return false;
    if (!Call(ctx, kPaneText, layout, base + 0x20, base, 1, 0)) return false;
    l.shown = utf8;
    return true;
}

// What the rows say now (co-op's host status and the Lobby's room). No lock of ours held: they take their own.
SessionView ViewNow() {
    HosCoopStatus st{};
    st.size = sizeof st;
    CoopHookshot::HostMenu().Status(&st);
    SessionView v;
    v.s.hosting = st.state == HOS_COOP_HOSTING;
    v.s.in_one = (st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED) || CoopSave::SessionLive();
    v.connecting = st.state == HOS_COOP_STARTING || st.state == HOS_COOP_JOINING || st.state == HOS_COOP_WAIT_HOST;
    std::string room = st.relay_room[0] != 0 ? st.relay_room : st.relay_code[0] != 0 ? st.relay_code : "";
    if (const auto at = room.find('@'); at != std::string::npos) room.resize(at);   // (the name friends type first)
    v.room = room;
    v.s.code = v.s.hosting && (!room.empty() || st.invite[0] != 0);
    v.confirm_leave = NowMs() < g_confirm_until.load();
    const CoopLobby::Seen seen = CoopLobby::Now();
    v.players = seen.connected ? static_cast<int>(seen.players.size()) : 0;
    if (v.s.hosting) v.asking = CoopLobby::Asking();
    return v;
}

// The rows' live texts while our page shows: at most every 250 ms, a pane only when its words changed.
void LiveRows(void* ctx, std::uint64_t screen, const SessionView& v) {
    std::lock_guard lock(g_live_mutex);
    const std::uint64_t now = NowMs();
    if (screen != g_live_screen) {
        g_live_screen = screen;
        for (LiveSlot& l : g_live) l.shown.clear();   // (a new pages screen: its panes show the pack's words again)
    } else if (now - g_live_ms < 250) {
        return;
    }
    g_live_ms = now;
    const std::uint64_t layout = Read<std::uint64_t>(screen + kScreenLayout);
    if (layout == 0) return;
    const std::vector<std::string> rows = RowTexts(v);
    for (int i = 0; i < kRows && i < kLiveSlots; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_Button_%02d/T_Btn_00", i);
        LiveText(ctx, layout, path, i, rows[static_cast<std::size_t>(i)]);
    }
}

// The pages: our page over System's, or System's alone. The rows' texts once per screen object.
void ApplyPages(void* ctx, std::uint64_t screen, const Page::Do& d) {
    const std::uint64_t layout = Read<std::uint64_t>(screen + kScreenLayout);
    if (layout == 0) return;
    if (screen != g_pages_screen) {
        g_pages_screen = screen, g_pages_bad = false;
        g_system_pane = FindOwnPane(ctx, layout, "Pa_System_00");
        g_coop_pane = FindOwnPane(ctx, layout, "Pa_Coop_00");
        const bool ok = IsPart(g_system_pane) && IsPart(g_coop_pane);
        g_pages_bad = !ok;
        LOG_INFO(Core, "coop: pause tab: the pages' panes: Pa_System_00 {:#x}, Pa_Coop_00 {:#x}{}", g_system_pane,
                 g_coop_pane, ok ? "" : " (not both parts: the Multiplayer page stays off; is the pack's layout in?)");
    }
    if (g_pages_bad) return;
    if (d.set_page) {
        SetVisible(ctx, g_coop_pane, d.page_shown);
        SetVisible(ctx, g_system_pane, !d.page_shown);
    }
    if (d.set_texts) {
        int set = 0;
        for (int i = 0; i < kRows; ++i) {
            char path[64], label[64];
            std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_Button_%02d/T_Btn_00", i);
            std::snprintf(label, sizeof label, "Pa_Coop_00-Pa_Button_%02d-T_Btn_00", i);
            set += SetText(ctx, layout, path, "LayoutMsg/PlusMenu_00", label) ? 1 : 0;
        }
        // Inside our copy: its controller picture (System's, not ours) hidden.
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
        const std::uint64_t ours = PartLayout(table, "Pa_Coop_00");
        const std::uint64_t controller = PaneIn(ctx, ours, "Pa_SystemController_00");
        if (IsPart(controller)) SetVisible(ctx, controller, false);
        LOG_INFO(Core, "coop: pause tab: the page's rows: {} of {} texts set; its controller picture {}", set, kRows,
                 IsPart(controller) ? "hidden" : "not found");
    }
}

// The strip: the fifth tab shown in the Plus menu (filled as a Plus tab, our word), lit while our page shows, and
// System's tab lit only when System is the page shown.
void ApplyStrip(void* ctx, const Facts& f, const Page::Do& d) {
    const std::uint64_t layout = Read<std::uint64_t>(f.screen + kScreenLayout);
    const std::uint64_t tabs = Read<std::uint64_t>(f.screen + kStripTabArray);
    if (layout == 0 || tabs == 0) return;
    if (f.screen != g_strip_screen) {
        g_strip_screen = f.screen;
        g_tab_pane = FindOwnPane(ctx, layout, "Pa_Category_04");
        g_fourth_pane = FindOwnPane(ctx, layout, "Pa_Category_03");
        g_tab_rest_y = IsPart(g_tab_pane) ? Read<float>(g_tab_pane + kPaneOwnY) : 0;
        g_tab_shown = false;
        g_strip_bad = !IsPart(g_tab_pane);
        LOG_INFO(Core, "coop: pause tab: the strip's fifth tab: Pa_Category_04 {:#x}{}", g_tab_pane,
                 g_strip_bad ? " (not a part: left alone)" : "");
    }
    if (g_strip_bad) return;
    SetVisible(ctx, g_tab_pane, d.tab_shown);
    g_tab_shown = d.tab_shown;
    if (!d.tab_shown) return;
    const std::uint64_t tab3 = Read<std::uint64_t>(tabs + 8 * 3), tab4 = Read<std::uint64_t>(tabs + 8 * 4);
    if (tab3 == 0 || tab4 == 0) return;
    if (d.fill_tab) {
        Call(ctx, kTabFill, tab4, kSystemPage, 1);   // (a Plus tab's look, as the game fills its four)
        const bool text = SetText(ctx, layout, "Pa_Category_04/T_Text_00", "LayoutMsg/MenuOverlay_00",
                                  "Pa_Category_04-T_Text_00");
        Call(ctx, kTabOpen, tab4, 0);   // (opened and shown, as the game's fill ends for each of its tabs)
        // Its word faded in: the strip's own "text in" animation fades in the game's four tabs' words only (it names
        // their panes, 8 to 11), so ours stays at alpha 0 (seen in a run, 2026-11-18: N_MenuChangeTextIn_00's own
        // alpha, +0x74, 0 in ours, 1 in the game's). Set to 1 with the game's alpha setter.
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
        const std::uint64_t text_in = PaneIn(ctx, PartLayout(table, "Pa_Category_04"), "N_MenuChangeTextIn_00");
        if (text_in != 0) CallAlpha(ctx, text_in, 1.0f);
        LOG_INFO(Core, "coop: pause tab: the fifth tab filled (its word {}, {})", text ? "set" : "not set",
                 text_in != 0 ? "faded in" : "its text pane not found");
    }
    Call(ctx, kTabLight, tab3, f.lit == kSystemPage && !d.tab_lit ? 1 : 0, 0);
    Call(ctx, kTabLight, tab4, d.tab_lit ? 1 : 0, 0);
}

// Our tab through the strip's opening and closing, as the game's four: the strip's animation moves and fades the
// parent of the game's tabs (seen in a run, 2026-11-19: the fourth tab's place on the screen, +0x50, from 1024 down to
// 1000 in 300 ms as the strip opens, and its alpha with its parents', +0x78, from 0.16 to 1), and ours sits under
// RootPane, out of that parent. So at each strip update ours takes the fourth's height (its own place, +0x70: RootPane
// is the screen's origin, then kPaneMoved) and alpha (the alpha setter). Not when the two
// are far apart (not the strip seen: ours stays where the pack put it).
void FollowFourth(void* ctx) {
    if (!g_tab_shown || g_fourth_pane == 0 || g_strip_bad) return;
    const float y = Read<float>(g_fourth_pane + kPaneScreenY), alpha = Read<float>(g_fourth_pane + kPaneScreenAlpha);
    if (!std::isfinite(y) || !std::isfinite(alpha) || std::fabs(y - g_tab_rest_y) > kFollowMost) return;
    if (Read<float>(g_tab_pane + kPaneOwnY) != y) {
        CoopRuntime::Write(g_tab_pane + kPaneOwnY, &y, sizeof y);
        Call(ctx, kPaneMoved, g_tab_pane);
    }
    CallAlpha(ctx, g_tab_pane, std::clamp(alpha, 0.0f, 1.0f));
}

// The System page's focused button (0..4), the row of ours under the game's cursor frame; -1 when none is.
int FocusedRow() {
    const std::uint64_t pages = g_pages_screen;
    const std::uint64_t page = pages != 0 ? Read<std::uint64_t>(pages + kSystemPageObject) : 0;
    if (page == 0) return -1;
    for (int i = 0; i < kRows; ++i) {
        const std::uint64_t button = Read<std::uint64_t>(page + kSystemButtons + 8 * static_cast<std::uint64_t>(i));
        const std::uint8_t state = button != 0 ? Read<std::uint8_t>(button + kButtonState) : 0;
        if (state != 0 && (state & 0xfe) != 4) return i;
    }
    return -1;
}

// What co-op is doing now, as the rows need it.
Session SessionNow() {
    HosCoopStatus st{};
    st.size = sizeof st;
    CoopHookshot::HostMenu().Status(&st);
    Session s;
    s.hosting = st.state == HOS_COOP_HOSTING;
    s.in_one = (st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED) || CoopSave::SessionLive();
    s.code = s.hosting && (st.relay_room[0] != 0 || st.relay_code[0] != 0 || st.invite[0] != 0);
    return s;
}

// A row's action (after the lock: co-op's menu and the chat take their own).
void Act(int row) {
    const RowAction a = ActionFor(row, SessionNow());
    LOG_INFO(Core, "coop: pause tab: row {} chosen: {}", row, ActionWords(a));
    switch (a) {
    case RowAction::kHost: CoopPause::HostNow(); break;
    case RowAction::kConfirmLeave:
        if (NowMs() < g_confirm_until.exchange(0)) {
            LOG_INFO(Core, "coop: pause tab: the second A: co-op stops");
            CoopPause::LeaveNow();
        } else {
            g_confirm_until = NowMs() + kConfirmMs;   // (the row says "A again" until then)
        }
        break;
    case RowAction::kOpenMain: CoopPause::Open(CoopPauseMenu::Page::kMain); break;
    case RowAction::kOpenJoin: CoopPause::Open(CoopPauseMenu::Page::kJoin); break;
    case RowAction::kCopyCode: CoopPause::CopyJoinCode(); break;
    case RowAction::kOpenLobby: CoopPause::OpenLobby(); break;
    case RowAction::kAllow: CoopHook::SendChat("/allow"); break;   // (its answer, or "nobody asks", shows in the chat)
    case RowAction::kNone: break;
    }
}

}  // namespace

void Init(const HosCoopApi* api) {
    g_api = api;
}

void OnScreen(void* ctx, std::uint64_t screen) noexcept {
    try {
        if (screen == 0) return;
        const std::uint64_t base = CoopRuntime::MainBase();
        const std::uint64_t vtable = Read<std::uint64_t>(screen);
        if (vtable < base) return;
        Facts f;
        f.kind = KindOf(vtable - base);
        if (f.kind == Kind::kOther) return;   // (most updates: the HUD's and the other screens', once a frame each)
        f.screen = screen;
        f.state = Read<std::uint8_t>(screen + kScreenState);
        if (f.kind == Kind::kStrip) {
            f.tabs = static_cast<int>(Read<std::uint32_t>(screen + kStripTabs));
            f.mode = Read<std::int32_t>(screen + kStripMode);
            f.lit = Read<std::int32_t>(screen + kStripLit);
        } else if (f.kind == Kind::kPages) {
            if (const std::uint64_t manager = Read<std::uint64_t>(base + kMenuManager); manager != 0) {
                f.page = Read<std::int32_t>(manager + kManagerPage);
                f.asked = Read<std::int32_t>(manager + kManagerAsked);
            }
        }
        std::vector<std::string> lines;
        Page::Do d;
        std::string why;
        bool was = false, now_shown = false;
        int focused = -1;
        {
            std::lock_guard lock(g_mutex);
            lines = g_probe.Seen(f, NowMs());
            was = g_page.Shown();
            d = g_page.OnScreen(f, NowMs());
            now_shown = g_page.Shown();
            why = g_page.TakeWhy();
            g_shown = now_shown;
            if (f.kind == Kind::kPages && now_shown) focused = FocusedRow();
            if (d.any() && ctx != nullptr && CanCall()) {
                if (f.kind == Kind::kPages) ApplyPages(ctx, screen, d);
                if (f.kind == Kind::kStrip) ApplyStrip(ctx, f, d);
            }
            if (f.kind == Kind::kStrip && ctx != nullptr && CanCall()) FollowFourth(ctx);
        }
        for (const std::string& line : lines) LOG_INFO(Core, "{}", line);
        if (was && !now_shown) LOG_INFO(Core, "coop: pause tab: System back ({})", why);
        if (f.kind == Kind::kPages && now_shown && f.state == 2 && ctx != nullptr && CanCall()) {
            if (focused != kRowLeave) g_confirm_until = 0;   // (the cursor left the Leave row: no second A waited for)
            LiveRows(ctx, screen, ViewNow());   // (after our lock: the status and the Lobby take their own)
        }
        if (!now_shown) g_confirm_until = 0;
    } catch (...) {
        // (a log line or a look lost is all: the game's update goes on)
    }
}

std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) noexcept {
    try {
        std::uint64_t in = buttons;
        if (stick != nullptr) {   // (the left stick as up and down for our rows; nn::hid: y up)
            if (stick[1] > 20000) in |= kPadStickUp;
            if (stick[1] < -20000) in |= kPadStickDown;
        }
        std::uint64_t out = 0;
        bool was = false, shown = false;
        bool decide = false;
        int row = -1;
        std::string why;
        {
            std::lock_guard lock(g_mutex);
            was = g_page.Shown();
            out = g_page.OnPad(in, NowMs());
            shown = g_page.Shown();
            decide = g_page.TakeDecide();
            row = decide ? FocusedRow() : -1;   // (under the lock: the pages screen is the screens' updates')
            why = g_page.TakeWhy();
            g_shown = shown;
        }
        if (shown && stick != nullptr) {
            stick[0] = 0, stick[2] = 0, stick[3] = 0;   // (only the left stick's up and down: the game's cursor over our rows)
        }
        if (!was && shown) LOG_INFO(Core, "coop: pause tab: our page shown (R on System)");
        if (was && !shown) LOG_INFO(Core, "coop: pause tab: System back ({})", why);
        if (decide) Act(row);
        // (the stick's up and down bits we added are not the game's: only what it sent, filtered)
        return out & (buttons | ~(kPadStickUp | kPadStickDown));
    } catch (...) {
        return buttons;
    }
}

int MousePage() noexcept {
    return g_shown.load() ? 4 : -1;
}

}  // namespace CoopPauseTab
