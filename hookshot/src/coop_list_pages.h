// SPDX-License-Identifier: GPL-3.0-or-later
// One set of paging rules for every list of games a player picks from (player report 264, the owner: "let people go
// through pages for the lobby select and make it clear that you can page through it"): the title's Join a game, the
// - menu's Lobby > Find a game, and Hookshot's menu's (F1) Lobby tab Public games and Multiplayer > Find a game.
// - The pad: ZL the page before, ZR the page after, round at both ends (ZR on the last page: the first). The game's L
//   and R are its tabs' (the - menu's strip), so the pages take the shoulder pair under them, as on every list.
//   Hookshot's menu has no ZL or ZR for its tabs (hos_coop_menu.h): there the page is a row of its own, "Page < 2/3 >",
//   Left and Right turn it, the same round.
// - The line: "Page 2/3", with the game's own button letters for ZL and ZR on either side where the game's font draws
//   them (its StaticMsg/GuideKeyIcon.msbt: ZL is U+E046, ZR U+E047, the letters MenuOverlay_00's T_BtnL_00 / T_BtnR_00
//   and the System page's controller picture use), "" with one page only (nothing to turn, nothing said).
// Logic only: tests/hookshot/list_pages_test.cpp.
#pragma once

#include <algorithm>
#include <string>

#include "coop_i18n.h"
#include "coop_text_fit.h"

namespace CoopListPages {

// The game's button letters (UTF-8 of StaticMsg/GuideKeyIcon.msbt's ZL and ZR).
inline constexpr const char* kZL = "\xEE\x81\x86";   // U+E046
inline constexpr const char* kZR = "\xEE\x81\x87";   // U+E047

// How many pages `count` entries take at `per_page` a page (at least one, also for an empty list).
inline int Pages(int count, int per_page) {
    per_page = std::max(1, per_page);
    return count <= per_page ? 1 : (count + per_page - 1) / per_page;
}

// `page` as a page of `pages` (any number: round, so -1 is the last).
inline int Wrap(int page, int pages) {
    pages = std::max(1, pages);
    return (page % pages + pages) % pages;
}

// ZL (dir < 0) or ZR (dir > 0) from `page`: the page before or after, round at both ends. One page: it stays.
inline int Step(int page, int pages, int dir) {
    if (pages <= 1 || dir == 0) return Wrap(page, pages);
    return Wrap(page + (dir < 0 ? -1 : 1), pages);
}

// "Page 2/3" in the player's language (CoopI18n::Tr), with the game's ZL and ZR letters around it when `glyphs` (a
// text the game's own font draws: the title's buttons and hint, the - menu's list), else "(ZL/ZR)" after it in words
// (Hookshot's own font has no such letters). "" with one page.
inline std::string Line(int page, int pages, bool glyphs = true) {
    if (pages <= 1) return std::string();
    const std::string at =
        CoopI18n::Tr("Page " + std::to_string(Wrap(page, pages) + 1) + "/" + std::to_string(pages));
    return glyphs ? std::string(kZL) + " " + at + " " + kZR : at + " (ZL/ZR)";
}

// A list's heading with its page line: `title`, two spaces, `page_line`, within `n` letters of width and bytes
// (CoopTextFit). The page line is never cut: a long title is cut first ("Public ga...  ZL Page 2/3 ZR"); with no room
// for any of it, the page line alone. An empty page line: the title as Fit cuts it.
inline std::string Heading(const std::string& title, const std::string& page_line, std::size_t n) {
    if (page_line.empty()) return CoopTextFit::Fit(title, n);
    const std::string whole = title + "  " + page_line;
    if (whole.size() <= n && CoopTextFit::Halves(whole) <= 2 * n) return whole;
    const std::size_t line_halves = CoopTextFit::Halves(page_line) + 4, line_bytes = page_line.size() + 2;
    if (line_bytes + 4 > n || line_halves + 8 > 2 * n) return CoopTextFit::Fit(page_line, n);
    // (the room left for the title: its letters and its bytes both, as Fit counts them; Fit's "..." inside it)
    const std::size_t room = std::min(n - line_bytes, (2 * n - line_halves) / 2);
    const std::string cut = CoopTextFit::Fit(title, room);
    return cut.empty() ? CoopTextFit::Fit(page_line, n) : cut + "  " + page_line;
}

}  // namespace CoopListPages
