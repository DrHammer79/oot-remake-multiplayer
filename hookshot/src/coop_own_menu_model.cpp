// SPDX-License-Identifier: GPL-3.0-or-later
// The own menu's module-free parts (coop_own_menu.h): a Hookshot page as a MenuModel, keys as events.
#include <utility>

#include "coop_i18n.h"
#include "coop_own_menu.h"

namespace CoopOwnMenu {

std::uint32_t ListColour(std::uint32_t c) {
    const std::uint32_t r = c >> 24 & 0xFF, g = c >> 16 & 0xFF, b = c >> 8 & 0xFF, a = c & 0xFF;
    return r | g << 8 | b << 16 | a << 24;
}

coop::ui::MenuModel ModelOf(const std::vector<std::string>& tabs, int tab, const HosCoopMenuPage& page,
                            const HosCoopMenuPicture* picture) {
    const auto str = [](const char* s) { return std::string(s != nullptr ? s : ""); };
    coop::ui::MenuModel m;
    m.tabs = tabs;
    m.tab = tab;
    m.selected = page.selected;
    m.typing = page.typing != 0;
    m.help_title = str(page.help_title);
    m.help = str(page.help);
    m.hint = str(page.hint);
    m.gold = str(page.gold);
    m.change = str(page.change);
    m.confirm = str(page.confirm);
    m.back = str(page.back);
    m.off_word = CoopI18n::Tr("Off");
    m.on_word = CoopI18n::Tr("On");
    if (page.rows != nullptr) {
        for (int i = 0; i < page.rows_n; ++i) {
            const HosCoopMenuRow& r = page.rows[i];
            coop::ui::MenuRow row;
            row.name = str(r.name);
            row.value = str(r.value);
            row.kind = r.kind <= HOS_COOP_MENU_NOTE ? static_cast<coop::ui::RowKind>(r.kind) : coop::ui::RowKind::note;
            row.on = r.on != 0;
            row.left = r.left != 0;
            row.right = r.right != 0;
            row.fixed = r.fixed != 0;
            m.rows.push_back(std::move(row));
        }
    }
    if (picture != nullptr) {
        m.live = picture->live != 0;
        const int n = picture->shapes != nullptr ? std::min<int>(picture->shapes_n, HOS_COOP_MENU_MAX_SHAPES) : 0;
        for (int i = 0; i < n; ++i) {
            const HosCoopMenuShape& s = picture->shapes[i];
            coop::ui::MenuShape shape;
            shape.rgba = ListColour(s.rgba);
            shape.outline = s.outline != 0;
            const int pts = std::min<int>(s.n, HOS_COOP_MENU_SHAPE_POINTS);
            for (int k = 0; k < pts; ++k) {
                shape.points.push_back({s.xy[2 * k], s.xy[2 * k + 1]});
            }
            m.shapes.push_back(std::move(shape));
        }
    }
    return m;
}

std::uint8_t KeyEvent(std::uint32_t vk, bool open, bool typing) {
    if (vk == kOpenKey) {
        return open ? COOP_UI_CLOSE : COOP_UI_OPEN;
    }
    if (!open || typing) {
        return COOP_UI_NONE;
    }
    switch (vk) {
        case 0x26: return COOP_UI_UP;      // the arrows
        case 0x28: return COOP_UI_DOWN;
        case 0x25: return COOP_UI_LEFT;
        case 0x27: return COOP_UI_RIGHT;
        case 0x0D: return COOP_UI_CONFIRM;   // Enter
        case 0x1B:                           // Esc
        case 0x08: return COOP_UI_BACK;      // Backspace
        case 0x51:                           // Q
        case 0x21: return COOP_UI_TAB_PREV;  // Page Up
        case 0x45:                           // E
        case 0x22: return COOP_UI_TAB_NEXT;  // Page Down
        default: return COOP_UI_NONE;
    }
}

}  // namespace CoopOwnMenu
