// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's own menu on Longshot, opt-in (coop_own_menu.h): the state, the pad, the keys, the mouse and the drawing,
// over CoopPause's pages. Nothing runs unless LICHII_COOP_OWN_MENU=1.
#include "coop_own_menu.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "coop/ui/pad.hpp"
#include "coop_draw.h"
#include "coop_draw_hos.h"
#include "coop_pause.h"
#include "hos_coop_api.h"

namespace CoopOwnMenu {
namespace {

// The Multiplayer and Mods tabs, as Hookshot's menu gets them.
struct PausePages {
    int Tabs() { return CoopPause::Tabs(); }
    bool Input(int tab, int event, int arg, int arg2) { return CoopPause::Input(tab, event, arg, arg2); }
};

std::mutex g_mutex;          // everything below: the window's, the game's and the render thread call in
Menu g_menu;
coop::ui::PadReader g_pad;
float g_width = 0, g_height = 0;   // the picture at the last draw (the mouse's fractions are of it)

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool Typing(int tab) {
    HosCoopMenuPage page{};
    page.size = sizeof page;
    return CoopPause::Page(tab, &page) && page.typing != 0;
}

}  // namespace

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_OWN_MENU");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

bool Open() {
    if (!Enabled()) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    return g_menu.open();
}

bool Key(std::uint32_t vk) {
    if (!Enabled()) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    const bool open = g_menu.open();
    CoopUiEvent e{};
    e.kind = KeyEvent(vk, open, open && Typing(g_menu.tab()));
    if (e.kind == COOP_UI_NONE) {
        return open && !Typing(g_menu.tab());   // open: every other key is the menu's too (the game reads none)
    }
    PausePages pages;
    return g_menu.Handle(e, pages);
}

bool Char(std::uint32_t) {
    if (!Enabled()) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    return g_menu.open() && !Typing(g_menu.tab());   // open: letters are the menu's, unless the page types
}

bool Mouse(float fx, float fy, int event) {
    if (!Enabled()) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (!g_menu.open()) {
        return false;
    }
    CoopUiEvent e{};
    e.x = fx * g_width;
    e.y = fy * g_height;
    switch (event) {
        case HOS_COOP_MOUSE_MOVE: e.kind = COOP_UI_POINTER_MOVE; break;
        case HOS_COOP_MOUSE_LEFT: e.kind = COOP_UI_POINTER_CLICK; break;
        case HOS_COOP_MOUSE_WHEEL_UP: e.kind = COOP_UI_UP; break;
        case HOS_COOP_MOUSE_WHEEL_DOWN: e.kind = COOP_UI_DOWN; break;
        default: return true;
    }
    PausePages pages;
    g_menu.Handle(e, pages);
    return true;
}

std::uint64_t Pad(std::uint64_t buttons, std::int32_t stick[4]) {
    if (!Enabled() || stick == nullptr) {
        return buttons;
    }
    std::lock_guard lock(g_mutex);
    const bool was_open = g_menu.open();
    const coop::ui::PadEvents events = g_pad.Feed(buttons, stick[0], stick[1], NowMs(), was_open);
    PausePages pages;
    for (std::size_t i = 0; i < events.n; ++i) {
        g_menu.Handle(events.e[i], pages);
    }
    if (!was_open && !g_menu.open()) {
        return buttons;
    }
    for (int i = 0; i < 4; ++i) {
        stick[i] = 0;   // the menu has the pad (and the frame it opened or closed on)
    }
    return 0;
}

void Draw(const HosCoopDraw& d) {
    if (!Enabled() || d.width <= 0 || d.height <= 0 || !d.rect || !d.triangle || !d.text || !d.text_width) {
        return;
    }
    std::lock_guard lock(g_mutex);
    g_width = d.width;
    g_height = d.height;
    if (!g_menu.open()) {
        return;
    }
    const int tab = g_menu.tab();
    std::vector<std::string> tabs;
    for (int i = 0; i < CoopPause::Tabs(); ++i) {
        const char* name = CoopPause::TabName(i);
        tabs.emplace_back(name != nullptr ? name : "");
    }
    HosCoopMenuPage page{};
    page.size = sizeof page;
    if (!CoopPause::Page(tab, &page)) {
        return;
    }
    HosCoopMenuPicture picture{};
    picture.size = sizeof picture;
    picture.aspect = d.width / d.height;
    const bool has_picture = CoopPause::Picture(tab, &picture);
    coop::ui::MenuModel model = ModelOf(tabs, tab, page, has_picture ? &picture : nullptr);
    static thread_local std::unique_ptr<CoopDrawList> list;
    if (!list) {
        list = std::make_unique<CoopDrawList>();
    }
    coop_draw_begin(list.get(), d.width, d.height);
    CoopDraw::Measure m;
    m.host = &d;
    coop::ui::MenuLayout layout = coop::ui::DrawMenu(*list, m, model);
    m.Restore();
    CoopDraw::Replay(*list, d, nullptr);
    if (d.use_font) {
        d.use_font(d.user, 0);
    }
    g_menu.Drawn(std::move(model), std::move(layout));
}

}  // namespace CoopOwnMenu
