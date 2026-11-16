// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's own menu on Longshot, opt-in (coop_own_menu.h): the state, the pad, the keys, the mouse and the drawing,
// over CoopPause's pages. Nothing runs unless LICHII_COOP_OWN_MENU=1.
#include "coop_own_menu.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "common/logging.h"
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

// The menu shows only through hos_coop_draw2. Not drawn within this long of opening (or of its last draw) it closes,
// so the game never loses its pad to a menu nobody sees; never drawn at all: this Hookshot does not draw it, so it
// opens no more.
constexpr std::uint64_t kUndrawnMs = 1000;

std::mutex g_mutex;          // everything below: the window's, the game's and the render thread call in
Menu g_menu;
coop::ui::PadReader g_pad;
coop::ui::PadGate g_gate;
float g_width = 0, g_height = 0;   // the picture at the last draw (the mouse's fractions are of it)
bool g_cannot_draw = false;        // no hos_coop_draw2 here: the menu does not open
std::uint64_t g_shown_ms = 0;      // when it opened, then its last draw
bool g_ever_drawn = false;         // hos_coop_draw2 has drawn it at least once
std::mutex g_focus_mutex;          // g_focus alone (taken last, never holding it while taking another lock)
coop::ui::FocusStack g_focus;      // all of co-op's UI: the own menu, the chat box, the cheats panel
KeyLatch g_keys;                   // keys held when the menu closed
bool g_latch_keys = false;         // it just closed: latch at the next KeysTaken

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

// An event to the menu (g_mutex held): no OPEN where it cannot be drawn.
bool Do(const CoopUiEvent& e) {
    if (e.kind == COOP_UI_OPEN && (g_cannot_draw || g_menu.open())) {
        return !g_cannot_draw;   // (open already: taken; cannot draw: the key or combination stays the game's)
    }
    PausePages pages;
    const bool was_open = g_menu.open();
    const bool taken = g_menu.Handle(e, pages);
    if (!was_open && g_menu.open()) {
        g_shown_ms = NowMs();
        std::lock_guard focus(g_focus_mutex);
        g_focus.Push(coop::ui::Owner::menu);
    } else if (was_open && !g_menu.open()) {
        {
            std::lock_guard focus(g_focus_mutex);
            g_focus.Pop(coop::ui::Owner::menu);
        }
        g_latch_keys = true;   // the keys held now stay co-op's until let go (KeysTaken)
    }
    return taken;
}

// It cannot be drawn here (g_mutex held): closed if open, and no more opening.
void CannotDraw(const char* why) {
    if (!g_cannot_draw) {
        LOG_WARNING(Core, "coop: co-op's own menu (LICHII_COOP_OWN_MENU=1) is off: {} (docs/ui.md)", why);
    }
    if (g_menu.open()) {
        CoopUiEvent close{};
        close.kind = COOP_UI_CLOSE;
        Do(close);
    }
    g_cannot_draw = true;
}

}  // namespace

void SetFocus(coop::ui::Owner owner, bool on) {
    std::lock_guard lock(g_focus_mutex);
    if (!on) {
        g_focus.Pop(owner);
    } else if (!g_focus.Has(owner)) {   // (already on: stays where it is, the menu over it keeps the top)
        g_focus.Push(owner);
    }
}

coop::ui::Owner FocusTop() {
    std::lock_guard lock(g_focus_mutex);
    return g_focus.Top();
}

bool KeysTaken(bool (*down)(int vk)) {
    if (!Enabled() || down == nullptr) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    const auto held = [down](std::uint32_t vk) { return down(static_cast<int>(vk)); };
    if (g_latch_keys) {
        g_latch_keys = false;
        g_keys.Latch(held);
    }
    return g_keys.Held(held) || g_menu.open();
}

void NoDraw2() {
    if (!Enabled()) {
        return;
    }
    std::lock_guard lock(g_mutex);
    CannotDraw("this Hookshot draws through hos_coop_draw only (an older one: no hos_coop_draw2)");
}

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
    return Do(e);
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
    Do(e);
    return true;
}

std::uint64_t Pad(std::uint64_t buttons, std::int32_t stick[4]) {
    if (!Enabled() || stick == nullptr) {
        return buttons;
    }
    std::lock_guard lock(g_mutex);
    const std::uint64_t now = NowMs();
    if (g_menu.open() && now > g_shown_ms + kUndrawnMs) {
        if (!g_ever_drawn) {
            CannotDraw("it was not drawn (this Hookshot calls neither hos_coop_draw2 nor hos_coop_draw for it)");
        } else {   // drawn before (a long frame, a loading screen): closed, it may open again
            CoopUiEvent close{};
            close.kind = COOP_UI_CLOSE;
            Do(close);
        }
    }
    const bool was_open = g_menu.open();
    const coop::ui::PadEvents events = g_pad.Feed(buttons, stick[0], stick[1], now, was_open);
    for (std::size_t i = 0; i < events.n; ++i) {
        Do(events.e[i]);
    }
    const bool menu_has_pad = was_open || g_menu.open();   // (and the frame it opened or closed on)
    if (menu_has_pad) {
        for (int i = 0; i < 4; ++i) {
            stick[i] = 0;
        }
    }
    // None while it has the pad; after, no button held then until it is let go (no B, Minus or A as a new press).
    return g_gate.Pass(buttons, menu_has_pad);
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
    g_shown_ms = NowMs();   // drawn: the menu is seen
    g_ever_drawn = true;
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
