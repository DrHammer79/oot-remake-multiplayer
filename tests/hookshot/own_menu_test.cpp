// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's own menu (coop/ui/menu_view.hpp, hookshot/src/coop_own_menu.h): the page as a model, the layout into the
// draw list, the keys, and the menu's state over a page source.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "coop/ui/drawlist.h"
#include "coop/ui/menu_view.hpp"
#include "coop_own_menu.h"

namespace {

using coop::ui::MenuModel;
using coop::ui::MenuRow;
using coop::ui::RowKind;

// Half the size a byte, as the overlay's tests measure.
struct HalfMeasure {
    float Width(int, float px, const char* s) const { return static_cast<float>(std::string(s).size()) * px * 0.5f; }
};

std::unique_ptr<CoopDrawList> NewList(float w = 1280, float h = 720) {
    auto l = std::make_unique<CoopDrawList>();
    coop_draw_begin(l.get(), w, h);
    return l;
}

std::vector<std::string> Texts(const CoopDrawList& l) {
    std::vector<std::string> t;
    for (std::uint32_t i = 0; i < l.n; ++i) {
        if (l.cmd[i].kind == COOP_DRAW_TEXT) {
            t.push_back(coop_draw_text_of(&l, &l.cmd[i]));
        }
    }
    return t;
}

bool HasText(const CoopDrawList& l, const std::string& s) {
    for (const auto& t : Texts(l)) {
        if (t == s) return true;
    }
    return false;
}

MenuRow Row(const char* name, RowKind kind, const char* value = "") {
    MenuRow r;
    r.name = name;
    r.kind = kind;
    r.value = value;
    return r;
}

MenuModel Page() {
    MenuModel m;
    m.tabs = {"Multiplayer", "Mods"};
    m.tab = 0;
    m.rows = {Row("Session", RowKind::heading), Row("Host a game", RowKind::action),
              Row("Players", RowKind::choice, "4"), Row("Voice", RowKind::toggle),
              Row("Only the host can change this.", RowKind::note)};
    m.rows[2].left = true;
    m.rows[3].on = true;
    m.rows[3].fixed = true;
    m.selected = 1;
    m.help_title = "Host a game";
    m.help = "Opens a game for others to join over the internet or the local network.";
    m.gold = "Your save is the session's.";
    m.change = "change";
    m.confirm = "host";
    m.back = "close";
    return m;
}

struct FakePages {
    int tabs = 2;
    bool back_taken = true;
    std::vector<std::string> calls;
    int Tabs() { return tabs; }
    bool Input(int tab, int event, int arg, int arg2) {
        calls.push_back(std::to_string(tab) + ":" + std::to_string(event) + ":" + std::to_string(arg) + ":" +
                        std::to_string(arg2));
        return event != HOS_COOP_MENU_BACK || back_taken;
    }
};

CoopUiEvent Ev(std::uint8_t kind, float x = 0, float y = 0) {
    CoopUiEvent e{};
    e.kind = kind;
    e.x = x;
    e.y = y;
    return e;
}

std::string Call(int tab, int event, int arg = 0, int arg2 = 0) {
    return std::to_string(tab) + ":" + std::to_string(event) + ":" + std::to_string(arg) + ":" + std::to_string(arg2);
}

}  // namespace

TEST_CASE("own menu view: tabs, rows by kind, help and prompt into the draw list") {
    auto l = NewList();
    const auto m = Page();
    const auto lay = coop::ui::DrawMenu(*l, HalfMeasure{}, m);
    CHECK(l->dropped == 0);
    for (const char* s : {"Multiplayer", "Mods", "Session", "Host a game", "Players", "4", "Voice", "Off", "On",
                          "Your save is the session's.", "(<>) change    (A) host    (B) close"}) {
        CHECK_MESSAGE(HasText(*l, s), s);
    }
    REQUIRE(lay.rows.size() == 5);   // all fit
    CHECK(lay.rows[1].top > lay.rows[0].top);
    CHECK(lay.tabs.size() == 2);
    CHECK(lay.tabs[0][2] < lay.tabs[1][0]);   // side by side
    CHECK(l->cmd[0].kind == COOP_DRAW_FONT);   // starts and ends on the backend's own font
    CHECK(l->cmd[l->n - 1].kind == COOP_DRAW_FONT);
    // The choice's arrows: left lit, right dim.
    int tris = 0;
    for (std::uint32_t i = 0; i < l->n; ++i) tris += l->cmd[i].kind == COOP_DRAW_TRIANGLE ? 1 : 0;
    CHECK(tris == 2);
}

TEST_CASE("own menu view: the pointer finds rows and value halves, never headings or notes") {
    auto l = NewList();
    const auto m = Page();
    const auto lay = coop::ui::DrawMenu(*l, HalfMeasure{}, m);
    const float mid = (lay.rows[2].top + lay.rows[2].bottom) / 2;
    int half = 9;
    CHECK(coop::ui::RowAt(lay, m, lay.rows_left + 5, mid, &half) == 2);
    CHECK(half == 0);
    CHECK(coop::ui::RowAt(lay, m, lay.value_left + 2, mid, &half) == 2);
    CHECK(half == -1);
    CHECK(coop::ui::RowAt(lay, m, lay.value_right - 2, mid, &half) == 2);
    CHECK(half == 1);
    CHECK(coop::ui::RowAt(lay, m, lay.rows_left + 5, (lay.rows[0].top + lay.rows[0].bottom) / 2, &half) == -1);
    CHECK(coop::ui::RowAt(lay, m, lay.rows_left + 5, (lay.rows[4].top + lay.rows[4].bottom) / 2, &half) == -1);
    CHECK(coop::ui::RowAt(lay, m, lay.rows_right + 50, mid, &half) == -1);   // the help column
    CHECK(coop::ui::TabAt(lay, (lay.tabs[1][0] + lay.tabs[1][2]) / 2, (lay.tabs[1][1] + lay.tabs[1][3]) / 2) == 1);
    CHECK(coop::ui::TabAt(lay, 0, 0) == -1);
}

TEST_CASE("own menu view: a long page scrolls so the selected row shows") {
    auto m = Page();
    m.rows.clear();
    for (int i = 0; i < 60; ++i) {
        m.rows.push_back(Row(("Row " + std::to_string(i)).c_str(), RowKind::action, "x"));
    }
    m.selected = 50;
    auto l = NewList();
    const auto lay = coop::ui::DrawMenu(*l, HalfMeasure{}, m);
    REQUIRE_FALSE(lay.rows.empty());
    CHECK(lay.rows.size() < 60);
    CHECK(lay.rows.front().row > 0);
    bool shown = false;
    for (const auto& b : lay.rows) shown = shown || b.row == 50;
    CHECK(shown);
    CHECK_FALSE(HasText(*l, "Row 0"));
    m.selected = -1;   // none: from the top
    auto top = NewList();
    CHECK(coop::ui::DrawMenu(*top, HalfMeasure{}, m).rows.front().row == 0);
}

TEST_CASE("own menu view: the picture's shapes into its square, a live square without backing") {
    auto m = Page();
    coop::ui::MenuShape tri;
    tri.rgba = 0xFF0000FFu;
    tri.points = {{0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    coop::ui::MenuShape bad;
    bad.points = {{0, 0}, {1, 1}};   // two points: not a shape
    m.shapes = {tri, bad};
    auto l = NewList();
    coop::ui::DrawMenu(*l, HalfMeasure{}, m);
    int polys = 0;
    for (std::uint32_t i = 0; i < l->n; ++i) {
        if (l->cmd[i].kind == COOP_DRAW_POLY) {
            ++polys;
            CHECK(l->cmd[i].n == 3);
            CHECK(l->cmd[i].rgba == 0xFF0000FFu);
            CHECK(l->cmd[i].v[0] > l->cmd[i].v[4]);   // the apex right of the bottom-left corner
        }
    }
    CHECK(polys == 1);
    const std::uint32_t with_backing = l->n;
    m.live = true;
    auto live = NewList();
    coop::ui::DrawMenu(*live, HalfMeasure{}, m);
    CHECK(live->n == with_backing - 1);   // the same, less the square's backing
}

TEST_CASE("own menu model: a Hookshot page and picture as the model") {
    HosCoopMenuRow rows[3] = {};
    rows[0].name = "Host";
    rows[0].value = "";
    rows[0].kind = HOS_COOP_MENU_ACTION;
    rows[1].name = "Players";
    rows[1].value = "4";
    rows[1].kind = HOS_COOP_MENU_CHOICE;
    rows[1].left = 1;
    rows[1].fixed = 1;
    rows[2].name = nullptr;
    rows[2].kind = 77;   // unknown: shown as a note
    HosCoopMenuPage page{};
    page.size = sizeof page;
    page.rows_n = 3;
    page.rows = rows;
    page.selected = 1;
    page.typing = 1;
    page.help_title = "Players";
    page.help = nullptr;
    page.confirm = "ok";
    HosCoopMenuShape shape{};
    shape.rgba = 0x11223344u;   // RRGGBBAA
    shape.n = 3;
    shape.outline = 1;
    shape.xy[0] = 0.1f;
    shape.xy[5] = 0.9f;
    HosCoopMenuPicture pic{};
    pic.size = sizeof pic;
    pic.shapes_n = 1;
    pic.shapes = &shape;
    pic.live = 1;
    const auto m = CoopOwnMenu::ModelOf({"Multiplayer", "Mods"}, 1, page, &pic);
    CHECK(m.tab == 1);
    REQUIRE(m.rows.size() == 3);
    CHECK(m.rows[1].kind == RowKind::choice);
    CHECK(m.rows[1].value == "4");
    CHECK(m.rows[1].left);
    CHECK_FALSE(m.rows[1].right);
    CHECK(m.rows[1].fixed);
    CHECK(m.rows[2].kind == RowKind::note);
    CHECK(m.rows[2].name.empty());
    CHECK(m.selected == 1);
    CHECK(m.typing);
    CHECK(m.help.empty());
    CHECK(m.confirm == "ok");
    CHECK(m.live);
    REQUIRE(m.shapes.size() == 1);
    CHECK(m.shapes[0].rgba == 0x44332211u);   // the draw list's AABBGGRR
    CHECK(m.shapes[0].outline);
    REQUIRE(m.shapes[0].points.size() == 3);
    CHECK(m.shapes[0].points[0][0] == doctest::Approx(0.1f));
    CHECK(m.shapes[0].points[2][1] == doctest::Approx(0.9f));
    const auto none = CoopOwnMenu::ModelOf({}, 0, HosCoopMenuPage{}, nullptr);
    CHECK(none.rows.empty());
    CHECK(none.shapes.empty());
}

TEST_CASE("own menu keys: F9 opens and closes, the rest only while open and not typing") {
    using CoopOwnMenu::KeyEvent;
    CHECK(KeyEvent(CoopOwnMenu::kOpenKey, false, false) == COOP_UI_OPEN);
    CHECK(KeyEvent(CoopOwnMenu::kOpenKey, true, true) == COOP_UI_CLOSE);
    CHECK(KeyEvent(0x28, false, false) == COOP_UI_NONE);   // closed: the game's
    CHECK(KeyEvent(0x28, true, false) == COOP_UI_DOWN);
    CHECK(KeyEvent(0x28, true, true) == COOP_UI_NONE);     // typing: the field's
    CHECK(KeyEvent(0x0D, true, false) == COOP_UI_CONFIRM);
    CHECK(KeyEvent(0x1B, true, false) == COOP_UI_BACK);
    CHECK(KeyEvent(0x51, true, false) == COOP_UI_TAB_PREV);
    CHECK(KeyEvent(0x22, true, false) == COOP_UI_TAB_NEXT);
    CHECK(KeyEvent('T', true, false) == COOP_UI_NONE);
}

TEST_CASE("own menu state: open shows the tab, tabs turn round, Back with nothing left closes") {
    CoopOwnMenu::Menu menu;
    FakePages p;
    CHECK_FALSE(menu.Handle(Ev(COOP_UI_DOWN), p));   // closed: not the menu's
    CHECK(p.calls.empty());
    CHECK(menu.Handle(Ev(COOP_UI_OPEN), p));
    CHECK(menu.open());
    CHECK(menu.focus().Top() == coop::ui::Owner::menu);
    CHECK(menu.Handle(Ev(COOP_UI_DOWN), p));
    CHECK(menu.Handle(Ev(COOP_UI_CONFIRM), p));
    CHECK(menu.Handle(Ev(COOP_UI_TAB_PREV), p));   // from the first tab round to the last
    CHECK(menu.tab() == 1);
    CHECK(menu.Handle(Ev(COOP_UI_TAB_NEXT), p));
    CHECK(menu.tab() == 0);
    CHECK_FALSE(menu.Handle(Ev(COOP_UI_TEXT_CHAR), p));   // text: the page's field
    CHECK(menu.Handle(Ev(COOP_UI_BACK), p));   // taken by the page: still open
    CHECK(menu.open());
    p.back_taken = false;
    CHECK(menu.Handle(Ev(COOP_UI_BACK), p));
    CHECK_FALSE(menu.open());
    CHECK(menu.focus().Top() == coop::ui::Owner::none);
    CHECK(p.calls == std::vector<std::string>{
                         Call(0, HOS_COOP_MENU_SHOW), Call(0, HOS_COOP_MENU_DOWN), Call(0, HOS_COOP_MENU_CONFIRM),
                         Call(0, HOS_COOP_MENU_HIDE), Call(1, HOS_COOP_MENU_SHOW), Call(1, HOS_COOP_MENU_HIDE),
                         Call(0, HOS_COOP_MENU_SHOW), Call(0, HOS_COOP_MENU_BACK), Call(0, HOS_COOP_MENU_BACK),
                         Call(0, HOS_COOP_MENU_HIDE)});
    FakePages none;
    none.tabs = 0;
    CHECK_FALSE(menu.Handle(Ev(COOP_UI_OPEN), none));   // no tabs: nothing to open
    CHECK_FALSE(menu.open());
}

TEST_CASE("own menu state: the pointer selects and clicks rows where they were drawn, and picks tabs") {
    CoopOwnMenu::Menu menu;
    FakePages p;
    menu.Handle(Ev(COOP_UI_OPEN), p);
    const auto m = Page();
    auto l = NewList();
    auto lay = coop::ui::DrawMenu(*l, HalfMeasure{}, m);
    const float mid = (lay.rows[2].top + lay.rows[2].bottom) / 2;
    const float right = lay.value_right - 2;
    const float tab_x = (lay.tabs[1][0] + lay.tabs[1][2]) / 2, tab_y = (lay.tabs[1][1] + lay.tabs[1][3]) / 2;
    const float head = (lay.rows[0].top + lay.rows[0].bottom) / 2;
    menu.Drawn(m, std::move(lay));
    p.calls.clear();
    CHECK(menu.Handle(Ev(COOP_UI_POINTER_MOVE, right, mid), p));
    CHECK(menu.Handle(Ev(COOP_UI_POINTER_CLICK, right, mid), p));
    CHECK(menu.Handle(Ev(COOP_UI_POINTER_CLICK, right, head), p));   // a heading: nothing
    CHECK(menu.Handle(Ev(COOP_UI_POINTER_CLICK, tab_x, tab_y), p));
    CHECK(menu.tab() == 1);
    CHECK(p.calls == std::vector<std::string>{Call(0, HOS_COOP_MENU_SELECT, 2), Call(0, HOS_COOP_MENU_CLICK, 2, 1),
                                              Call(0, HOS_COOP_MENU_HIDE), Call(1, HOS_COOP_MENU_SHOW)});
    CHECK(menu.Handle(Ev(COOP_UI_CLOSE), p));
    CHECK_FALSE(menu.open());
}
