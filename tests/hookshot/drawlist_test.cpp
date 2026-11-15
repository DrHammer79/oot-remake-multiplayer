// SPDX-License-Identifier: GPL-3.0-or-later
// CoopDrawList (coop/ui/drawlist.h) and its Longshot backend (hookshot/src/coop_draw_hos.h): commands, limits,
// text widths from tables, and the replay's fonts and images.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "coop/ui/drawlist.h"
#include "coop_draw.h"
#include "coop_draw_hos.h"

namespace {

std::unique_ptr<CoopDrawList> NewList(float w = 1280, float h = 720) {
    auto l = std::make_unique<CoopDrawList>();
    coop_draw_begin(l.get(), w, h);
    return l;
}

CoopDraw::Look Ids() {
    CoopDraw::Look l;
    l.regular = 7;
    l.medium = 8;
    l.hearts = {20, 21, 22, 23, 24};
    return l;
}

struct Host {
    std::vector<std::string> calls;
    HosCoopDraw d{};
    explicit Host(bool draw2) {
        d.width = 100;
        d.height = 100;
        d.user = this;
        d.rect = [](void* u, float, float, float, float, std::uint32_t, float) { static_cast<Host*>(u)->calls.push_back("rect"); };
        d.circle = [](void* u, float, float, float, std::uint32_t, float) { static_cast<Host*>(u)->calls.push_back("circle"); };
        d.triangle = [](void* u, float, float, float, float, float, float, std::uint32_t) {
            static_cast<Host*>(u)->calls.push_back("tri");
        };
        d.text = [](void* u, float, float, float, std::uint32_t, const char* s) {
            static_cast<Host*>(u)->calls.push_back(std::string("text ") + s);
        };
        d.text_width = [](void* u, float px, const char* s) {
            static_cast<Host*>(u)->calls.push_back(std::string("width ") + s);
            return static_cast<float>(std::string(s).size()) * px;
        };
        if (draw2) {
            d.use_font = [](void* u, int id) { static_cast<Host*>(u)->calls.push_back("font " + std::to_string(id)); };
            d.draw_image = [](void* u, int id, float, float, float, float, std::uint32_t) {
                static_cast<Host*>(u)->calls.push_back("image " + std::to_string(id));
            };
        }
    }
};

}  // namespace

TEST_CASE("draw list: commands keep their arguments in order") {
    auto l = NewList(640, 360);
    CHECK(l->width == 640);
    CHECK(l->height == 360);
    CHECK(coop_draw_rect(l.get(), 1, 2, 3, 4, 0xFF0000FFu, 5) == 1);
    CHECK(coop_draw_circle(l.get(), 6, 7, 8, 0x11223344u, 0) == 1);
    CHECK(coop_draw_triangle(l.get(), 1, 2, 3, 4, 5, 6, 7) == 1);
    CHECK(coop_draw_font(l.get(), COOP_FONT_MEDIUM) == 1);
    CHECK(l->font == COOP_FONT_MEDIUM);
    CHECK(coop_draw_text(l.get(), 9, 10, 24, 0xFFFFFFFFu, "Saria") == 1);
    CHECK(coop_draw_image(l.get(), COOP_IMAGE_HEART_4, 0, 0, 38, 38, 0x80FFFFFFu) == 1);
    REQUIRE(l->n == 6);
    CHECK(l->cmd[0].kind == COOP_DRAW_RECT);
    CHECK(l->cmd[0].v[4] == 5);
    CHECK(l->cmd[0].rgba == 0xFF0000FFu);
    CHECK(l->cmd[1].kind == COOP_DRAW_CIRCLE);
    CHECK(l->cmd[1].v[2] == 8);
    CHECK(l->cmd[2].v[5] == 6);
    CHECK(l->cmd[3].id == COOP_FONT_MEDIUM);
    CHECK(std::string(coop_draw_text_of(l.get(), &l->cmd[4])) == "Saria");
    CHECK(l->cmd[4].text_len == 5);
    CHECK(l->cmd[4].v[2] == 24);
    CHECK(l->cmd[5].id == COOP_IMAGE_HEART_4);
    CHECK(std::string(coop_draw_text_of(l.get(), &l->cmd[0])).empty());   // not a text
    coop_draw_begin(l.get(), 10, 10);
    CHECK(l->n == 0);
    CHECK(l->font == COOP_FONT_UI);
}

TEST_CASE("draw list: an outline's copies of the same words are stored once") {
    auto l = NewList();
    for (int i = 0; i < 9; ++i) {
        coop_draw_text(l.get(), static_cast<float>(i), 0, 20, 0, "Darunia");
    }
    coop_draw_text(l.get(), 0, 0, 20, 0, "Daru");   // a prefix is other words
    coop_draw_text(l.get(), 0, 0, 20, 0, nullptr);  // none: ""
    CHECK(l->n == 11);
    CHECK(l->text_used == 8 + 5 + 1);
    CHECK(std::string(coop_draw_text_of(l.get(), &l->cmd[8])) == "Darunia");
    CHECK(std::string(coop_draw_text_of(l.get(), &l->cmd[9])) == "Daru");
    CHECK(std::string(coop_draw_text_of(l.get(), &l->cmd[10])).empty());
}

TEST_CASE("draw list: full, the rest is dropped and counted, nothing written past the end") {
    auto l = NewList();
    for (int i = 0; i < COOP_DRAW_MAX_CMDS + 5; ++i) {
        coop_draw_rect(l.get(), 0, 0, 1, 1, 0, 0);
    }
    CHECK(l->n == COOP_DRAW_MAX_CMDS);
    CHECK(l->dropped == 5);
    CHECK(coop_draw_text(l.get(), 0, 0, 1, 0, "late") == 0);
    CHECK(l->text_used == 0);   // no words kept for a command that did not fit
    CHECK(l->dropped == 6);

    auto t = NewList();
    const std::string big(COOP_DRAW_MAX_TEXT / 2, 'a');
    CHECK(coop_draw_text(t.get(), 0, 0, 1, 0, big.c_str()) == 1);
    CHECK(coop_draw_text(t.get(), 0, 0, 1, 0, (big + "b").c_str()) == 0);   // no room for its words
    CHECK(t->dropped == 1);
    CHECK(t->n == 1);
    CHECK(coop_draw_text(t.get(), 0, 0, 1, 0, big.c_str()) == 1);   // the last words again: no room needed
}

TEST_CASE("draw list: polygons of 3 to 8 points, clips in pairs") {
    auto l = NewList();
    const float xy[18] = {0, 0, 1, 0, 1, 1, 0, 1, 0.5f, 0.5f, 0, 0, 0, 0, 0, 0, 9, 9};
    CHECK(coop_draw_poly(l.get(), xy, 2, 0, 0) == 0);
    CHECK(coop_draw_poly(l.get(), xy, 9, 0, 0) == 0);
    CHECK(coop_draw_poly(l.get(), nullptr, 3, 0, 0) == 0);
    CHECK(l->dropped == 0);   // not shapes, not lost ones
    CHECK(coop_draw_poly(l.get(), xy, 4, 0x12345678u, 1) == 1);
    CHECK(l->cmd[0].n == 4);
    CHECK(l->cmd[0].id == 1);
    CHECK(l->cmd[0].v[7] == 1);
    CHECK(coop_draw_clip_pop(l.get()) == 0);   // nothing pushed
    CHECK(coop_draw_clip_push(l.get(), 0, 0, 10, 10) == 1);
    CHECK(l->clip_depth == 1);
    CHECK(coop_draw_clip_pop(l.get()) == 1);
    CHECK(l->clip_depth == 0);
    CHECK(l->n == 3);
}

TEST_CASE("draw list: text widths from a font's advance table") {
    const std::uint32_t cps[] = {'A', 'i', 0xE9, 0x30A2};   // A, i, e acute, katakana A
    const float adv[] = {0.6f, 0.25f, 0.5f, 1.0f};
    const CoopFontAdvances f{4, cps, adv, 0.75f};
    CHECK(coop_text_width(&f, 10, "") == 0);
    CHECK(coop_text_width(&f, 10, "Ai") == doctest::Approx(8.5f));
    CHECK(coop_text_width(&f, 20, "\xC3\xA9") == doctest::Approx(10.0f));       // one codepoint, two bytes
    CHECK(coop_text_width(&f, 10, "\xE3\x82\xA2" "A") == doctest::Approx(16.0f));
    CHECK(coop_text_width(&f, 10, "z") == doctest::Approx(7.5f));               // not in the table
    CHECK(coop_text_width(&f, 10, "\xC3" "A") == doctest::Approx(13.5f));       // a cut sequence: missing, then A
    CHECK(coop_text_width(&f, 10, "\xFF") == doctest::Approx(7.5f));
    CHECK(coop_text_width(nullptr, 10, "A") == 0);
    CHECK(coop_text_width(&f, 10, nullptr) == 0);
}

TEST_CASE("draw list replay: logical fonts and hearts become the game look's ids") {
    auto l = NewList();
    coop_draw_font(l.get(), COOP_FONT_REGULAR);
    coop_draw_text(l.get(), 0, 0, 10, 0, "a");
    coop_draw_font(l.get(), COOP_FONT_MEDIUM);
    coop_draw_image(l.get(), COOP_IMAGE_HEART_0 + 2, 0, 0, 1, 1, 0);
    coop_draw_font(l.get(), COOP_FONT_UI);
    const float xy[6] = {0, 0, 1, 0, 1, 1};
    coop_draw_poly(l.get(), xy, 3, 0, 0);   // no call for it in Hookshot's table
    coop_draw_clip_push(l.get(), 0, 0, 1, 1);
    coop_draw_rect(l.get(), 0, 0, 1, 1, 0, 0);
    coop_draw_clip_pop(l.get());
    const auto look = Ids();
    Host h(true);
    CoopDraw::Replay(*l, h.d, &look);
    CHECK(h.calls == std::vector<std::string>{"font 7", "text a", "font 8", "image 22", "font 0", "rect"});
    Host plain(true);
    CoopDraw::Replay(*l, plain.d, nullptr);   // no game look: Hookshot's own font everywhere
    CHECK(plain.calls == std::vector<std::string>{"font 0", "text a", "font 0", "image 0", "font 0", "rect"});
    Host old(false);   // an older Hookshot's table: no fonts or pictures to call
    CoopDraw::Replay(*l, old.d, &look);
    CHECK(old.calls == std::vector<std::string>{"text a", "rect"});
    CHECK(CoopDraw::HostImage(&look, 9) == 0);
    CHECK(CoopDraw::HostFont(&look, 99) == 0);
}

TEST_CASE("draw list measure: Hookshot's widths in the chosen font, its own font back after") {
    const auto look = Ids();
    Host h(true);
    CoopDraw::Measure m;
    m.host = &h.d;
    m.look = &look;
    CHECK(m.Width(-1, 2, "ab") == doctest::Approx(4.0f));   // no font chosen yet: Hookshot's current one
    CHECK(m.Width(COOP_FONT_UI, 2, "a") == doctest::Approx(2.0f));   // already Hookshot's own: no switch
    CHECK(m.Width(COOP_FONT_MEDIUM, 2, "a") == doctest::Approx(2.0f));
    CHECK(m.Width(COOP_FONT_MEDIUM, 2, "b") == doctest::Approx(2.0f));   // still medium: no second switch
    m.Restore();
    m.Restore();   // once is enough
    CHECK(h.calls == std::vector<std::string>{"width ab", "width a", "font 8", "width a", "width b", "font 0"});

    Host old(false);   // no use_font: widths only
    CoopDraw::Measure o;
    o.host = &old.d;
    o.look = &look;
    o.Width(COOP_FONT_REGULAR, 1, "x");
    o.Restore();
    CHECK(old.calls == std::vector<std::string>{"width x"});

    const std::uint32_t cps[] = {'x'};
    const float adv[] = {0.5f};
    const CoopFontAdvances f{1, cps, adv, 1.0f};
    CoopDraw::Measure t;   // no host: the tables (an ExeFS build)
    t.tables[COOP_FONT_MEDIUM] = &f;
    CHECK(t.Width(COOP_FONT_MEDIUM, 10, "xx") == doctest::Approx(10.0f));
    CHECK(t.Width(COOP_FONT_UI, 10, "xx") == 0);    // no table for it
    CHECK(t.Width(-1, 10, "xx") == 0);               // none chosen: the UI font's
}

TEST_CASE("draw list build: the overlay's commands fit the list, an outline's words stored once") {
    CoopDraw::Scene s;
    for (int i = 0; i < 60; ++i) {   // a long chat with the box open: every kept line shows
        s.chat.push_back({"Player" + std::to_string(i) + ": " + std::string(90, 'x'), false, 1.0f});
    }
    s.box = CoopDraw::ChatBoxView{"typing", false};
    Host h(true);
    CoopDraw::Measure m;
    m.host = &h.d;
    auto l = NewList(1920, 1080);
    CoopDraw::Look look = Ids();
    look.regular_metrics = {};
    CoopDraw::Build(*l, m, s, &look);
    CHECK(l->dropped == 0);
    CHECK(l->n > 60 * 12);           // name and words, each the game's rim, shadow and fill
    CHECK(l->text_used < 60 * 120);  // each line's words once, not six times
}
