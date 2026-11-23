// SPDX-License-Identifier: GPL-3.0-or-later
// Chat as the game's own UI (hookshot/src/coop_native_ui.h), against a pretend game: guest memory as a map of bytes
// and a list of the calls made.
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop_native_ui.h"

namespace ui = CoopNativeUi;

namespace {

constexpr std::uint64_t kMain = 0x80020000;

struct FakeGame final : ui::Game {
    std::map<std::uint64_t, std::uint8_t> mem;
    struct Made {
        std::uint64_t function;
        std::array<std::uint64_t, 5> x;
    };
    std::vector<Made> calls;
    std::map<std::uint64_t, std::uint64_t> returns;  // function -> x0
    std::set<std::uint64_t> missing;                 // functions that are not there

    bool Read(std::uint64_t a, void* out, std::size_t n) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = mem.find(a + i);
            if (it == mem.end()) return false;
            o[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < n; ++i) mem[a + i] = p[i];
        return true;
    }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        if (missing.count(f) != 0) return std::nullopt;
        calls.push_back({f, x});
        const auto it = returns.find(f);
        return it != returns.end() ? it->second : 0;
    }
    std::uint64_t Main() const override { return kMain; }
    template <typename T>
    void Put(std::uint64_t a, T v) {
        Write(a, &v, sizeof v);
    }
    template <typename T>
    T Get(std::uint64_t a) {
        T v{};
        REQUIRE(Read(a, &v, sizeof v));
        return v;
    }
    std::string Text(std::uint64_t a) {
        std::string s;
        for (char c; Read(a + s.size(), &c, 1) && c != 0;) s.push_back(c);
        return s;
    }
    std::vector<Made> CallsTo(std::uint64_t f) const {
        std::vector<Made> out;
        for (const auto& c : calls) {
            if (c.function == f) out.push_back(c);
        }
        return out;
    }
};

constexpr std::uint64_t kBlock = 0x90000000;
constexpr std::uint64_t kHolder = 0x91000000;
constexpr std::uint64_t kWindow = 0x92000000;
constexpr std::uint64_t kBanner = 0x93000000;
constexpr std::uint64_t kLayout = 0x94000000;
constexpr std::uint64_t kVtable = 0x95000000;  // the banner's slots (the window's is its own vtable)
constexpr std::uint64_t kManager = 0x96000000;
constexpr std::uint64_t kScreens = 0x97000000;
constexpr std::uint64_t kPane = 0x98000000;
constexpr std::uint64_t kInner = 0x98100000;
constexpr std::uint64_t kBox = 0x98200000;
constexpr std::uint64_t kOpen = 0x804d6100, kClose = 0x802f6990;

std::uint64_t At(std::uint64_t main_offset) { return kMain + main_offset; }

// A game with the message window and the banner loaded, both closed.
struct World {
    FakeGame g;
    ui::TextRing ring;
    ui::NameTint tint;
    World() {
        ring.SetBlock(kBlock);
        g.Put<std::uint64_t>(At(ui::addr::kInGameHolder), kHolder);
        g.returns[At(ui::addr::kInGameGet)] = kWindow;
        g.Put<std::uint64_t>(kWindow, At(ui::addr::kInGameVtable));
        g.Put<std::uint64_t>(At(ui::addr::kInGameVtable) + 5 * 8, kOpen);
        g.Put<std::uint64_t>(At(ui::addr::kInGameVtable) + 6 * 8, kClose);
        for (const std::uint64_t s : {kWindow, kBanner}) {
            g.Put<std::uint64_t>(s + ui::addr::kScreenLayout, kLayout);
            g.Put<std::int8_t>(s + ui::addr::kScreenRequest, 0);
            g.Put<std::uint8_t>(s + ui::addr::kScreenState, 0);
        }
        g.Put<float>(kWindow + ui::addr::kInGameTime, 0.0f);
        g.Put<std::uint64_t>(kWindow + ui::addr::kInGameText, 0);
        g.Put<std::uint8_t>(kWindow + ui::addr::kInGameShow, 0);
        // The banner, found in the UI manager's list by its vtable.
        g.Put<std::uint64_t>(At(ui::addr::kUiManagerGot), kManager - 8);
        g.Put<std::uint64_t>(kManager - 8, kManager);
        g.Put<std::uint32_t>(kManager + ui::addr::kUiCount, 2);
        g.Put<std::uint64_t>(kManager + ui::addr::kUiScreens, kScreens);
        g.Put<std::uint64_t>(kScreens, kWindow);
        g.Put<std::uint64_t>(kScreens + 8, kBanner);
        g.Put<std::uint64_t>(kBanner, At(ui::addr::kInfoVtable));
        g.Put<std::uint64_t>(At(ui::addr::kInfoVtable) + 5 * 8, kOpen);
        g.Put<std::uint64_t>(At(ui::addr::kInfoVtable) + 6 * 8, kClose);
        // T_Name_00: a text pane (kind 2) and its text box with the game's own colours.
        g.returns[At(ui::addr::kPaneByName)] = kPane;
        g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 2u << 22);
        g.Put<std::uint64_t>(kPane + 8, kInner);
        g.Put<std::uint64_t>(kInner + 8, kBox);
        const float game[8] = {1, 1, 1, 1, 0.9f, 0.9f, 0.9f, 1};
        g.Write(kBox + ui::addr::kBoxColour0, game, sizeof game);
        (void)kVtable;
    }
    ui::Ref RefAt(std::uint64_t a) { return g.Get<ui::Ref>(a); }
};

ui::Shown Line(std::string speaker, std::string text, std::uint32_t colour = 0) {
    ui::Shown s;
    s.speaker = std::move(speaker);
    s.text = std::move(text);
    s.seconds = 4.0f;
    s.colour = colour;
    return s;
}

}  // namespace

TEST_CASE("native ui: the game's addresses as checked on the 1.1.1 main (main+ offsets)") {
    CHECK(kMain + ui::addr::kInGameVtable == 0x84c09678);
    CHECK(kMain + ui::addr::kInfoVtable == 0x84c0d7c8);  // what the constructor stores, not slot 2 (0x84c0d7d8)
    CHECK(kMain + ui::addr::kInGameGet == 0x833d2690);
    CHECK(kMain + ui::addr::kInGameHolder == 0x84ea4518);
    CHECK(kMain + ui::addr::kSetPaneText == 0x8045b970);
    CHECK(kMain + ui::addr::kPaneByName == 0x8078e2b0);
    CHECK(kMain + ui::addr::kSetColour0 == 0x8152c740);
    CHECK(kMain + ui::addr::kSetColour1 == 0x8152c810);
    CHECK(kMain + ui::addr::kNameT_Name_00 == 0x83d141c6);
    CHECK(kMain + ui::addr::kNameT_Text_00 == 0x83de2306);
    CHECK(kMain + ui::addr::kUiManagerGot == 0x84c9e098);
    CHECK(ui::addr::kMalloc == 0x14c6c90);
    CHECK(ui::addr::kInGameText == 0x2b0);
    CHECK(ui::addr::kInGameShow == 0x2d0);
    CHECK(ui::addr::kInGameTime == 0x408);
}

TEST_CASE("native ui: other players' text never carries a game tag or a control byte") {
    // A colour tag (0x0E, u16 group 0, u16 type 3, u16 size 2, u16 index 1) as it could come: its bytes all go.
    const std::string tag("hi \x0E\x00\x00\x03\x00\x02\x00\x01\x00there", 3 + 9 + 5);
    CHECK(ui::Clean(tag, 100) == "hi there");
    CHECK(ui::Clean(std::string("a\x0E" "b", 3), 100) == "ab");
    CHECK(ui::Clean("tab\there", 100) == "tab here");
    CHECK(ui::Clean("line\nbreak", 100) == "line break");
    CHECK(ui::Clean("line\nbreak", 100, true) == "line\nbreak");
    CHECK(ui::Clean("del\x7f!", 100) == "del!");
    CHECK(ui::Clean("bad \xC3(", 100) == "bad ?(");
    CHECK(ui::Clean("\xE0\x80\x80", 100) == "???");     // overlong
    CHECK(ui::Clean("\xED\xA0\x80", 100) == "???");     // a surrogate
    CHECK(ui::Clean("caf\xC3\xA9 ok", 4) == "caf");     // cut at a whole character
    CHECK(ui::Clean("\xE4\xBD\xA0\xE5\xA5\xBD", 5) == "\xE4\xBD\xA0");
}

TEST_CASE("native ui: a chat line's speaker and text") {
    CHECK(ui::SplitSpeaker("Saria: hello: there").speaker == "Saria");
    CHECK(ui::SplitSpeaker("Saria: hello: there").text == "hello: there");
    CHECK(ui::SplitSpeaker("Mido joined").speaker.empty());
    CHECK(ui::SplitSpeaker(": odd").speaker.empty());
}

TEST_CASE("native ui: columns: Latin 1, Chinese, Japanese and Korean 2") {
    CHECK(ui::Columns("Link") == 4);
    CHECK(ui::Columns("\xE4\xBD\xA0\xE5\xA5\xBD") == 4);           // 你好
    CHECK(ui::Columns("\xEC\x95\x88\xEB\x85\x95") == 4);           // 안녕
    CHECK(ui::Columns("\xD0\x9F\xD1\x80\xD0\xB8") == 3);           // При (Cyrillic: narrow)
    CHECK(ui::Columns("\xEF\xBC\x81") == 2);                        // a full-width !
}

TEST_CASE("native ui: wrapping fits the game's pane: whole words, pages, nothing lost") {
    const auto one = ui::Wrap("hello there", 44, 2);
    REQUIRE(one.size() == 1);
    CHECK(one[0] == "hello there");
    const auto two = ui::Wrap("aaaa bbbb cccc", 9, 2);
    REQUIRE(two.size() == 1);
    CHECK(two[0] == "aaaa bbbb\ncccc");
    const auto pages = ui::Wrap("aaaa bbbb cccc dddd eeee", 9, 2);
    REQUIRE(pages.size() == 2);
    CHECK(pages[0] == "aaaa bbbb\ncccc dddd");
    CHECK(pages[1] == "eeee");
    // A word longer than a line breaks inside, at a character; every row fits.
    const std::string longword(100, 'x');
    const auto cut = ui::Wrap(longword, 44, 2);
    std::string joined;
    for (const auto& p : cut) {
        std::size_t from = 0;
        while (from <= p.size()) {
            const auto nl = p.find('\n', from);
            const std::string row = p.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
            CHECK(ui::Columns(row) <= 44);
            joined += row;
            if (nl == std::string::npos) break;
            from = nl + 1;
        }
    }
    CHECK(joined == longword);
    // Chinese: two columns a character, broken anywhere.
    std::string zh;
    for (int i = 0; i < 30; ++i) zh += "\xE4\xBD\xA0";
    const auto zp = ui::Wrap(zh, 44, 2);
    REQUIRE(zp.size() == 1);
    const auto nl = zp[0].find('\n');
    REQUIRE(nl != std::string::npos);
    CHECK(ui::Columns(zp[0].substr(0, nl)) == 44);
    CHECK(ui::Columns(zp[0].substr(nl + 1)) == 16);
    CHECK(ui::Wrap("", 44, 2).empty());
    CHECK(ui::Wrap("   ", 44, 2).empty());
}

TEST_CASE("native ui: the feed shows each line once, in pages, game-paced, and drops stale ones") {
    ui::ChatFeed f;
    f.Update({{"Saria: old", false, 100, 0}}, 1000);  // there before: not shown again
    CHECK(f.waiting() == 0);
    f.Update({{"Saria: old", false, 150, 0}, {"Saria: hi", false, 0, 0x3070FF}}, 1050);
    f.Update({{"Saria: hi", false, 30, 0x3070FF}}, 1080);  // the same line a frame later
    REQUIRE(f.waiting() == 1);
    auto a = f.Next(1100);
    REQUIRE(a);
    CHECK(a->speaker == "Saria");
    CHECK(a->text == "hi");
    CHECK(a->colour == 0x3070FF);
    CHECK(f.Busy(1100));
    CHECK_FALSE(f.Next(1200));
    // A long line: pages of two lines, in turn.
    std::string words;
    for (int i = 0; i < 40; ++i) words += "word ";
    f.Update({{"Mido: " + words, false, 0, 0}}, 20000);
    CHECK(f.waiting() == 3);  // 200 columns: 44 x 2 a page
    const auto p1 = f.Next(20000);
    REQUIRE(p1);
    CHECK(p1->speaker == "Mido");
    CHECK(p1->text.find('\n') != std::string::npos);
    CHECK(p1->seconds <= 8.0f);
    // A notice: one line a page, no speaker, no colour.
    f.Update({{"Ruto joined the game", true, 0, 0x123456}}, 30000);
    while (f.waiting() > 1) f.Next(1'000'000 + f.waiting());  // (the rest of Mido's)
    const auto n = f.Next(2'000'000);
    CHECK_FALSE(n);  // stale by then
    f.Update({{"Ruto left", true, 0, 0}}, 3'000'000);
    const auto m = f.Next(3'000'000);
    REQUIRE(m);
    CHECK(m->notice);
    CHECK(m->speaker.empty());
    CHECK(m->text == "Ruto left");
    // Never more than kWaiting wait.
    std::vector<ui::ChatIn> burst;
    for (int i = 0; i < 40; ++i) burst.push_back({"P" + std::to_string(i) + ": x", false, 0, 0});
    f.Update(burst, 4'000'000);
    CHECK(f.waiting() == ui::ChatFeed::kWaiting);
    // A page put back is first again.
    auto first = f.Next(4'000'000);
    REQUIRE(first);
    f.PutBack(*first);
    CHECK_FALSE(f.Busy(4'000'000));
    CHECK(f.Next(4'000'000)->speaker == first->speaker);
}

TEST_CASE("native ui: the typed line: the end that fits, a caret, (not sent)") {
    const auto t = ui::TypingLine("you", "hello", false);
    CHECK(t.speaker == "you");
    CHECK(t.text == "hello_");
    CHECK(t.seconds == ui::kTypingSeconds);
    CHECK(ui::TypingLine("you", "hi", true, "(pas envoy\xC3\xA9)").text == "hi_ (pas envoy\xC3\xA9)");
    const auto l = ui::TypingLine("you", std::string(300, 'y'), false);
    CHECK(l.text.substr(0, 3) == "...");
    CHECK(l.text.substr(l.text.size() - 2) == "y_");
    const auto nl = l.text.find('\n');
    REQUIRE(nl != std::string::npos);
    CHECK(ui::Columns(l.text.substr(0, nl)) <= ui::kLineColumns);
    CHECK(ui::Columns(l.text.substr(nl + 1)) <= ui::kLineColumns);
    CHECK(l.text.find('\n', nl + 1) == std::string::npos);  // two lines at most
    CHECK(ui::TypingLine("you", std::string("a\x0E" "b", 3), false).text == "ab_");
}

TEST_CASE("native ui: texts live in the game's memory, the Ref as the game reads it") {
    FakeGame g;
    ui::TextRing r;
    CHECK(r.Put(g, "x") == 0);  // no block yet
    r.SetBlock(kBlock);
    const auto a = r.Put(g, "hello\nthere");
    REQUIRE(a == kBlock);
    const auto ref = g.Get<ui::Ref>(a);
    CHECK(ref.text == a + sizeof(ui::Ref));
    CHECK(ref.bytes == 11);
    CHECK(ref.end == -1);
    CHECK(g.Text(ref.text) == "hello\nthere");
    CHECK(r.Put(g, "b") == kBlock + ui::TextRing::kSlotBytes);
    CHECK(r.Owns(ref.text));
    CHECK_FALSE(r.Owns(kBlock + ui::TextRing::kBlockBytes));
    CHECK(r.Scratch() == kBlock + ui::TextRing::kSlotBytes * ui::TextRing::kSlots);
    for (std::size_t i = 2; i < ui::TextRing::kSlots; ++i) r.Put(g, "z");
    CHECK(r.Put(g, "again") == kBlock);  // round again, long after
}

TEST_CASE("native ui: a chat line in the game's message window, as f_833d25c0 shows one") {
    World w;
    REQUIRE(ui::InGameScreen(w.g) == kWindow);
    CHECK(ui::WindowHolds(w.g, kWindow, w.ring) == ui::Holds::kNothing);
    REQUIRE(ui::ShowLine(w.g, w.ring, w.tint, Line("Saria", "hello")));
    // The speaker on T_Name_00 through the game's setter, its text one of ours.
    const auto sets = w.g.CallsTo(At(ui::addr::kSetPaneText));
    REQUIRE(sets.size() == 1);
    CHECK(sets[0].x[0] == kLayout);
    CHECK(sets[0].x[1] == 0x83d141c6);
    CHECK(sets[0].x[3] == 1);
    CHECK(w.g.Text(w.RefAt(sets[0].x[2]).text) == "Saria");
    // The line's Ref where the game keeps its message, the time in its units.
    const auto ref = w.RefAt(kWindow + ui::addr::kInGameText);
    CHECK(w.g.Text(ref.text) == "hello");
    CHECK(ref.bytes == 5);
    CHECK(ref.end == -1);
    CHECK(w.g.Get<float>(kWindow + ui::addr::kInGameTime) == doctest::Approx(4.0f * ui::addr::kTimePerSecond));
    // Closed with no open asked: no show flag (as the game), then opened (slot 5, mode 1).
    CHECK(w.g.Get<std::uint8_t>(kWindow + ui::addr::kInGameShow) == 0);
    const auto opens = w.g.CallsTo(kOpen);
    REQUIRE(opens.size() == 1);
    CHECK(opens[0].x[0] == kWindow);
    CHECK(opens[0].x[1] == 1);
    CHECK(ui::WindowHolds(w.g, kWindow, w.ring) == ui::Holds::kOurs);
    // No colour: the name plate keeps the game's.
    CHECK(w.g.CallsTo(At(ui::addr::kSetColour0)).empty());
}

TEST_CASE("native ui: the window up already: the show flag, and a refresh does not open it again") {
    World w;
    w.g.Put<std::uint8_t>(kWindow + ui::addr::kScreenState, ui::addr::kStateOpen);
    w.g.Put<std::int8_t>(kWindow + ui::addr::kScreenRequest, 1);
    REQUIRE(ui::ShowLine(w.g, w.ring, w.tint, Line("you", "typ_"), true));
    CHECK(w.g.Get<std::uint8_t>(kWindow + ui::addr::kInGameShow) == 1);
    CHECK(w.g.CallsTo(kOpen).empty());
    REQUIRE(ui::ShowLine(w.g, w.ring, w.tint, Line("you", "next"), false));
    CHECK(w.g.CallsTo(kOpen).size() == 1);  // not a refresh: opened as the game does
}

TEST_CASE("native ui: a message of the game's own is never covered, ending touches only ours") {
    World w;
    w.g.Put<std::uint64_t>(kWindow + ui::addr::kInGameText, 0x88000000);  // the game's text, not in our block
    w.g.Put<float>(kWindow + ui::addr::kInGameTime, 90.0f);
    CHECK(ui::WindowHolds(w.g, kWindow, w.ring) == ui::Holds::kGames);
    CHECK_FALSE(ui::ShowLine(w.g, w.ring, w.tint, Line("Saria", "hi")));
    CHECK_FALSE(ui::EndLine(w.g, w.ring));
    CHECK(w.g.Get<float>(kWindow + ui::addr::kInGameTime) == 90.0f);
    CHECK(w.g.calls.size() == 2);  // only the getter, once each
    // Its time out: free again.
    w.g.Put<float>(kWindow + ui::addr::kInGameTime, 0.0f);
    REQUIRE(ui::ShowLine(w.g, w.ring, w.tint, Line("Saria", "hi")));
    REQUIRE(ui::EndLine(w.g, w.ring));
    CHECK(w.g.Get<float>(kWindow + ui::addr::kInGameTime) == 0.0f);
}

TEST_CASE("native ui: no window, or not this build's: nothing changed") {
    World w;
    w.g.returns[At(ui::addr::kInGameGet)] = 0;
    CHECK(ui::InGameScreen(w.g) == 0);
    CHECK_FALSE(ui::ShowLine(w.g, w.ring, w.tint, Line("a", "b")));
    w.g.returns[At(ui::addr::kInGameGet)] = kWindow;
    w.g.Put<std::uint64_t>(kWindow, 0x84000000);  // another vtable
    CHECK(ui::InGameScreen(w.g) == 0);
    w.g.Put<std::uint64_t>(At(ui::addr::kInGameHolder), 0);
    CHECK(ui::InGameScreen(w.g) == 0);
}

TEST_CASE("native ui: the speaker's name plate in the player's colour, the game's own back after") {
    World w;
    REQUIRE(ui::ShowLine(w.g, w.ring, w.tint, Line("Saria", "hi", 0x30D040)));
    const auto c0 = w.g.CallsTo(At(ui::addr::kSetColour0));
    const auto c1 = w.g.CallsTo(At(ui::addr::kSetColour1));
    REQUIRE(c0.size() == 1);
    REQUIRE(c1.size() == 1);
    CHECK(c0[0].x[0] == kBox);
    CHECK(c1[0].x[0] == kBox);
    CHECK(c0[0].x[1] == w.ring.Scratch());
    float top[4] = {}, bottom[4] = {};
    REQUIRE(w.g.Read(c0[0].x[1], top, sizeof top));
    REQUIRE(w.g.Read(c1[0].x[1], bottom, sizeof bottom));
    CHECK(top[0] == doctest::Approx(0x30 / 255.0f));
    CHECK(top[1] == doctest::Approx(0xD0 / 255.0f));
    CHECK(top[2] == doctest::Approx(0x40 / 255.0f));
    CHECK(top[3] == doctest::Approx(1.0f));  // the game's alpha
    CHECK(bottom[1] == doctest::Approx(0xD0 / 255.0f));
    CHECK(w.tint.tinted());
    // The same colour again: no call.
    REQUIRE(ui::ShowLine(w.g, w.ring, w.tint, Line("Saria", "again", 0x30D040)));
    CHECK(w.g.CallsTo(At(ui::addr::kSetColour0)).size() == 1);
    REQUIRE(w.tint.Restore(w.g, w.ring));
    const auto back = w.g.CallsTo(At(ui::addr::kSetColour1));
    REQUIRE(back.size() == 2);
    REQUIRE(w.g.Read(back[1].x[1], bottom, sizeof bottom));
    CHECK(bottom[0] == doctest::Approx(0.9f));  // the game's own second colour
    CHECK_FALSE(w.tint.tinted());
}

TEST_CASE("native ui: the colour only on a text pane whose box holds colours") {
    World w;
    w.g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 1u << 22);  // a picture pane
    CHECK_FALSE(w.tint.Set(w.g, w.ring, kWindow, 0xFF0000));
    w.g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 2u << 22);
    w.g.Put<float>(kBox + ui::addr::kBoxColour0, 300.0f);  // not a colour: not the box we think
    CHECK_FALSE(w.tint.Set(w.g, w.ring, kWindow, 0xFF0000));
    CHECK(w.g.CallsTo(At(ui::addr::kSetColour0)).empty());
    // And the line still shows, in the game's colour.
    CHECK(ui::ShowLine(w.g, w.ring, w.tint, Line("Saria", "hi", 0xFF0000)));
}

TEST_CASE("native ui: a notice in the game's banner") {
    World w;
    REQUIRE(ui::ScreenByVtable(w.g, ui::addr::kInfoVtable) == kBanner);
    ui::Shown n;
    n.notice = true;
    n.text = "Ruto joined";
    REQUIRE(ui::ShowNotice(w.g, w.ring, n));
    const auto sets = w.g.CallsTo(At(ui::addr::kSetPaneText));
    REQUIRE(sets.size() == 1);
    CHECK(sets[0].x[1] == 0x83de2306);  // T_Text_00
    CHECK(w.g.Text(w.RefAt(sets[0].x[2]).text) == "Ruto joined");
    const auto opens = w.g.CallsTo(kOpen);
    REQUIRE(opens.size() == 1);
    CHECK(opens[0].x[0] == kBanner);
    CHECK(opens[0].x[1] == ui::addr::kInfoOpenMode);
    w.g.Put<std::uint32_t>(kManager + ui::addr::kUiCount, 1);  // the banner not loaded
    CHECK_FALSE(ui::ShowNotice(w.g, w.ring, n));
}

TEST_CASE("native ui: a held banner line stays, changes its text only when it changes, closes when let go") {
    World w;
    ui::Banner b;
    CHECK_FALSE(ui::HeldBanner());
    ui::HoldBanner("You are down  0:32");
    REQUIRE(ui::HeldBanner());
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner()));
    CHECK(b.up());
    CHECK(w.g.CallsTo(kOpen).size() == 1);
    CHECK(w.g.CallsTo(At(ui::addr::kSetPaneText)).size() == 1);
    w.g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 1);  // the open taken
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner()));
    CHECK(w.g.CallsTo(At(ui::addr::kSetPaneText)).size() == 1);  // the same text: nothing set
    ui::HoldBanner("You are down  0:31");
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner()));
    const auto sets = w.g.CallsTo(At(ui::addr::kSetPaneText));
    REQUIRE(sets.size() == 2);
    CHECK(w.g.Text(w.RefAt(sets[1].x[2]).text) == "You are down  0:31");
    CHECK(w.g.CallsTo(kOpen).size() == 1);  // up already: not opened again
    // The banner closed by itself (state 0, nothing asked): opened again.
    w.g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 0);
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner()));
    CHECK(w.g.CallsTo(kOpen).size() == 2);
    ui::EndBanner();
    CHECK_FALSE(ui::HeldBanner());
    CHECK_FALSE(b.Step(w.g, w.ring, ui::HeldBanner()));
    const auto closes = w.g.CallsTo(kClose);
    REQUIRE(closes.size() == 1);
    CHECK(closes[0].x[0] == kBanner);
    CHECK(closes[0].x[1] == static_cast<std::uint64_t>(-1));
    CHECK_FALSE(b.up());
    CHECK_FALSE(b.Step(w.g, w.ring, std::nullopt));
    CHECK(w.g.CallsTo(kClose).size() == 1);  // once
}
