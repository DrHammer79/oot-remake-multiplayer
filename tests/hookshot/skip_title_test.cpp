// SPDX-License-Identifier: GPL-3.0-or-later
// Longshot patch 0028's Skip title screen (hookshot/src/hos_skip_title.h, the patch's host/hos_skip_title.h): it chooses
// only the title's Continue, the load list's file and its question's first answer, in that order; any other screen, a
// known one out of order or focused on another button stops it; at most three choices (player report 133).
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <string>

#include "hos_skip_title.h"

namespace {
constexpr uint64_t kTitle = HOS_SKIP_TITLE_SCREEN, kList = HOS_SKIP_LOAD_LIST_SCREEN, kCheck = HOS_SKIP_LOAD_CHECK_SCREEN;
int Next(int step, uint64_t vt, const char* name, std::string* why = nullptr) {
    const char* w = nullptr;
    const int r = hos_skip_title_next(step, vt, name, &w);
    REQUIRE(w != nullptr);
    if (why) *why = w;
    return r;
}
}  // namespace

TEST_CASE("skip title: the three screens in order, each on its one button") {
    CHECK(Next(0, kTitle, "Pa_Button_02") == HOS_SKIP_CHOOSE);       // Continue
    CHECK(Next(1, kList, "Pa_SaveData_00") == HOS_SKIP_CHOOSE);      // the file the list focuses
    CHECK(Next(2, kCheck, "Pa_CheckButton_00") == HOS_SKIP_CHOOSE);  // its first answer
    CHECK(hos_skip_title_step_of(kTitle) == 0);
    CHECK(hos_skip_title_step_of(kList) == 1);
    CHECK(hos_skip_title_step_of(kCheck) == 2);
    CHECK(HOS_SKIP_STEPS == 3);
}

TEST_CASE("skip title: an unknown screen stops it for good, at every step") {
    for (int step = 0; step < HOS_SKIP_STEPS; ++step) {
        std::string why;
        INFO("step " << step);
        CHECK(Next(step, 0x84c0bc18ULL, "Pa_Button_00", &why) == HOS_SKIP_STOP);   // (the pause menu's vtable)
        CHECK(why == "a screen that is not the title, the load list or its question");
        CHECK(Next(step, 0, "", nullptr) == HOS_SKIP_STOP);
    }
}

TEST_CASE("skip title: a known screen out of order stops it, an earlier one still updating is waited out") {
    std::string why;
    CHECK(Next(0, kList, "Pa_SaveData_00", &why) == HOS_SKIP_STOP);
    CHECK(why == "a screen out of order");
    CHECK(Next(0, kCheck, "Pa_CheckButton_00") == HOS_SKIP_STOP);
    CHECK(Next(1, kCheck, "Pa_CheckButton_00") == HOS_SKIP_STOP);
    CHECK(Next(1, kTitle, "Pa_Button_02") == HOS_SKIP_WAIT);   // the title behind the list
    CHECK(Next(2, kTitle, "Pa_Button_02") == HOS_SKIP_WAIT);
    CHECK(Next(2, kList, "Pa_SaveData_00") == HOS_SKIP_WAIT);
}

TEST_CASE("skip title: another button on a known screen stops it (New Game, Options, the second answer, no name)") {
    std::string why;
    CHECK(Next(0, kTitle, "Pa_Button_00", &why) == HOS_SKIP_STOP);   // New Game
    CHECK(why == "the title's focus is not on Continue (Pa_Button_02)");
    CHECK(Next(0, kTitle, "Pa_Button_01") == HOS_SKIP_STOP);          // Options
    CHECK(Next(0, kTitle, "Pa_Button_03") == HOS_SKIP_STOP);          // (co-op's rows)
    CHECK(Next(0, kTitle, "") == HOS_SKIP_STOP);                      // (a name that could not be read)
    CHECK(Next(0, kTitle, nullptr) == HOS_SKIP_STOP);
    CHECK(Next(1, kList, "") == HOS_SKIP_STOP);
    CHECK(Next(2, kCheck, "Pa_CheckButton_01", &why) == HOS_SKIP_STOP);
    CHECK(why == "the question's focus is not on its first answer (Pa_CheckButton_00)");
}

TEST_CASE("skip title: no fourth choice") {
    std::string why;
    for (const uint64_t vt : {kTitle, kList, kCheck, uint64_t{0x1234}}) {
        CHECK(Next(3, vt, "Pa_Button_02", &why) == HOS_SKIP_STOP);
        CHECK(why == "made its three choices");
    }
}

// The button's pane name, read as the game's own screen request reads it (main+0x3379160 -> f_837a28f0(dst, *(button+0x18),
// *(button+0x20)) -> strlen and memcpy of the first): +0x18 holds the name's text, +0x20 the parent pane. The 0.11.4
// candidate (title-press-a, 11-17) read a length at +0x18 and the text at *(button+0x20): the parent pane's vtable bytes,
// logged as "button p..": the title's check stopped the skip (fail-safe) and nothing was ever chosen.
namespace {
// A small guest memory: words and strings at addresses, bytes outside them unreadable.
std::map<uint64_t, uint8_t> g_mem;
void Put(uint64_t a, const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) g_mem[a + i] = static_cast<const uint8_t*>(p)[i];
}
void Put64(uint64_t a, uint64_t v) { Put(a, &v, 8); }
void PutText(uint64_t a, const char* s) { Put(a, s, std::strlen(s) + 1); }
int Read(uint64_t a, void* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const auto it = g_mem.find(a + i);
        if (it == g_mem.end()) return 0;
        static_cast<uint8_t*>(out)[i] = it->second;
    }
    return 1;
}
constexpr uint64_t kButton = 0x90001000ULL, kText = 0x90002000ULL, kParent = 0x90003000ULL;
constexpr uint64_t kParentVtable = 0x84c4c470ULL;   // (a pane's vtable in main's data: 70 c4 c4 84 00 00 00 00)
void Title(const char* name) {
    g_mem.clear();
    Put64(kButton + 0x18, kText);
    Put64(kButton + 0x20, kParent);
    PutText(kText, name);
    Put64(kParent, kParentVtable);
    Put64(kParent + 0x30, 0);
}
std::string Name(uint64_t button = kButton, size_t cap = 64) {
    char out[64] = "x";
    return hos_skip_title_button_name(button, Read, out, cap);
}
// The candidate's read, kept to show what it gave: n = *(button+0x18), the text at *(button+0x20)
std::string OldName() {
    uint64_t n = 0, at = 0;
    Read(kButton + 0x18, &n, 8);
    Read(kButton + 0x20, &at, 8);
    if (n >= 64) n = 63;
    char out[64] = {};
    for (uint64_t i = 0; i < n && Read(at + i, &out[i], 1); ++i) {}
    return out;
}
}  // namespace

TEST_CASE("skip title: the focused button's name is the text at *(button+0x18), not the parent pane at +0x20") {
    Title("Pa_Button_02");
    CHECK(Name() == "Pa_Button_02");
    CHECK(Next(0, kTitle, Name().c_str()) == HOS_SKIP_CHOOSE);
    // The wrong read: the parent pane's vtable bytes, so the title's check stopped the skip with nothing chosen.
    const std::string old = OldName();
    CHECK(old == std::string{'\x70', '\xc4', '\xc4', '\x84'});
    std::string why;
    CHECK(Next(0, kTitle, old.c_str(), &why) == HOS_SKIP_STOP);
    CHECK(why == "the title's focus is not on Continue (Pa_Button_02)");
    // The other two screens read the same way.
    Title("Pa_CheckButton_00");
    CHECK(Next(2, kCheck, Name().c_str()) == HOS_SKIP_CHOOSE);
    Title("Pa_SaveData_00");
    CHECK(Next(1, kList, Name().c_str()) == HOS_SKIP_CHOOSE);
}

TEST_CASE("skip title: a name that cannot be read is none (the skip stops, nothing is chosen)") {
    Title("Pa_Button_02");
    CHECK(Name(0) == "");                              // no button
    CHECK(Name(kButton + 0x100) == "");                // its words unreadable
    CHECK(Name(kButton, 1) == "");                     // no room
    Put64(kButton + 0x18, 0x40);                       // a null pointer's page
    CHECK(Name() == "");
    Put64(kButton + 0x18, kParent);                    // pointing at a pane, not text: not printable
    CHECK(Name() == "");
    g_mem.clear();
    Put64(kButton + 0x18, kText);
    Put(kText, "Pa_Button_02", 12);                    // no end within what can be read
    CHECK(Name() == "");
    Title("Pa_Button_02_and_a_name_far_too_long_for_a_pane");
    CHECK(Name(kButton, 16) == "");                    // longer than the room: none, not cut
    CHECK(Next(0, kTitle, "") == HOS_SKIP_STOP);
}
