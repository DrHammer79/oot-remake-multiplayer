// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's own menu on Longshot (docs/ui.md "Own menu"), OPT-IN: LICHII_COOP_OWN_MENU=1. Off (the default) none of
// this runs and Hookshot's own menu shows the Multiplayer and Mods tabs as always.
//
// On, the same tabs and rows (CoopPause: the pages Hookshot's menu gets through hos_coop_menu.h) are also drawn by
// co-op itself, over the picture through hos_coop_draw2 (coop/ui/menu_view.hpp into the draw list, replayed onto
// Hookshot's table): the screen an ExeFS build will show where no Hookshot menu exists. Open and close it with F9, or
// on the pad hold L and R and press Minus (Minus closes). While it is open the game gets no pad buttons or keys.
//
// The parts here are the module-free ones (tested in tests/hookshot/own_menu_test.cpp): the page as a MenuModel, the
// menu's state over a page source, and the keys as events. coop_own_menu.cpp ties them to CoopPause and the exports.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coop/ui/events.h"
#include "coop/ui/focus.hpp"
#include "coop/ui/menu_view.hpp"
#include "hos_coop_draw.h"
#include "hos_coop_menu.h"
#include "hos_coop_menu_picture.h"

namespace CoopOwnMenu {

// A Hookshot menu page (and its picture, or none) as the own menu's model. Shape colours turn from 0xRRGGBBAA into
// the draw list's 0xAABBGGRR; the picture's 0..1 points stay.
coop::ui::MenuModel ModelOf(const std::vector<std::string>& tabs, int tab, const HosCoopMenuPage& page,
                            const HosCoopMenuPicture* picture);
std::uint32_t ListColour(std::uint32_t rrggbbaa);

// A key (Windows virtual-key codes, as hos_coop_key gets them) as an event: F9 opens and closes; while open the
// arrows move, Enter confirms, Esc or Backspace go back, Q / E (or Page Up / Down) change the tab. COOP_UI_NONE: not
// the menu's (while the page types into a field every key but F9 is the field's: CoopPause::Key takes it).
std::uint8_t KeyEvent(std::uint32_t vk, bool open, bool typing);
inline constexpr std::uint32_t kOpenKey = 0x78;   // F9

// The menu's state. `Pages`: int Tabs(); bool Input(int tab, int event, int arg, int arg2) (hos_coop_menu.h events).
class Menu {
public:
    bool open() const { return open_; }
    int tab() const { return tab_; }
    const coop::ui::FocusStack& focus() const { return focus_; }

    // The last drawn page and where its rows went, for the pointer.
    void Drawn(coop::ui::MenuModel model, coop::ui::MenuLayout layout) {
        model_ = std::move(model);
        layout_ = std::move(layout);
    }

    // True when the event was the menu's.
    template <class Pages>
    bool Handle(const CoopUiEvent& e, Pages& pages) {
        if (e.kind == COOP_UI_OPEN) {
            if (open_ || pages.Tabs() <= 0) {
                return open_;
            }
            open_ = true;
            tab_ = std::clamp(tab_, 0, pages.Tabs() - 1);
            focus_.Push(coop::ui::Owner::menu);
            pages.Input(tab_, HOS_COOP_MENU_SHOW, 0, 0);
            return true;
        }
        if (!open_) {
            return false;
        }
        switch (e.kind) {
            case COOP_UI_CLOSE:
                Close(pages);
                return true;
            case COOP_UI_UP: pages.Input(tab_, HOS_COOP_MENU_UP, 0, 0); return true;
            case COOP_UI_DOWN: pages.Input(tab_, HOS_COOP_MENU_DOWN, 0, 0); return true;
            case COOP_UI_LEFT: pages.Input(tab_, HOS_COOP_MENU_LEFT, 0, 0); return true;
            case COOP_UI_RIGHT: pages.Input(tab_, HOS_COOP_MENU_RIGHT, 0, 0); return true;
            case COOP_UI_CONFIRM: pages.Input(tab_, HOS_COOP_MENU_CONFIRM, 0, 0); return true;
            case COOP_UI_BACK:
                if (!pages.Input(tab_, HOS_COOP_MENU_BACK, 0, 0)) {
                    Close(pages);   // nothing on the tab to go back to: the menu closes (as Hookshot's)
                }
                return true;
            case COOP_UI_TAB_PREV: Turn(pages, -1); return true;
            case COOP_UI_TAB_NEXT: Turn(pages, 1); return true;
            case COOP_UI_POINTER_MOVE: {
                const int row = coop::ui::RowAt(layout_, model_, e.x, e.y, nullptr);
                if (row >= 0) {
                    pages.Input(tab_, HOS_COOP_MENU_SELECT, row, 0);
                }
                return true;
            }
            case COOP_UI_POINTER_CLICK: {
                const int to = coop::ui::TabAt(layout_, e.x, e.y);
                if (to >= 0 && to != tab_ && to < pages.Tabs()) {
                    Go(pages, to);
                    return true;
                }
                int half = 0;
                const int row = coop::ui::RowAt(layout_, model_, e.x, e.y, &half);
                if (row >= 0) {
                    pages.Input(tab_, HOS_COOP_MENU_CLICK, row, half);
                }
                return true;
            }
            default:
                return false;   // text: the page's field takes it (CoopPause::Key / Char)
        }
    }

private:
    template <class Pages>
    void Close(Pages& pages) {
        pages.Input(tab_, HOS_COOP_MENU_HIDE, 0, 0);
        open_ = false;
        focus_.Pop(coop::ui::Owner::menu);
    }
    template <class Pages>
    void Go(Pages& pages, int to) {
        pages.Input(tab_, HOS_COOP_MENU_HIDE, 0, 0);
        tab_ = to;
        pages.Input(tab_, HOS_COOP_MENU_SHOW, 0, 0);
    }
    template <class Pages>
    void Turn(Pages& pages, int by) {
        const int n = pages.Tabs();
        if (n > 1) {
            Go(pages, ((tab_ + by) % n + n) % n);
        }
    }

    bool open_ = false;
    int tab_ = 0;
    coop::ui::FocusStack focus_;
    coop::ui::MenuModel model_;
    coop::ui::MenuLayout layout_;
};

// --- The module's (coop_own_menu.cpp): all do nothing unless LICHII_COOP_OWN_MENU=1. Thread-safe. ---
bool Enabled();
bool Open();
// A key from hos_coop_key: true when the menu took it.
bool Key(std::uint32_t vk);
// A typed character from hos_coop_char: true when the menu took it (open, and the page is not typing).
bool Char(std::uint32_t ch);
// The mouse from hos_coop_mouse (fractions of the window, HOS_COOP_MOUSE_*): true when the menu took it.
bool Mouse(float fx, float fy, int event);
// The pad from hos_coop_pad: the buttons the game should get (none while the menu is open or as it opens; the sticks
// zeroed then too).
std::uint64_t Pad(std::uint64_t buttons, std::int32_t stick[4]);
// Draws the menu when open (hos_coop_draw2, after the overlay).
void Draw(const HosCoopDraw& d);

}  // namespace CoopOwnMenu
