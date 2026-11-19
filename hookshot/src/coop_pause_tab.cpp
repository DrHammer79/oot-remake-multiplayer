// SPDX-License-Identifier: GPL-3.0-or-later
// coop_pause_tab.h's logic: no game reads or calls, no log (the tests drive it).
#include "coop_pause_tab.h"

#include <cstdio>

namespace CoopPauseTab {

namespace {

// A screen whose updates stopped this long ago is taken as gone (the game updates an open menu once a frame).
constexpr std::uint64_t kStaleMs = 250;
// A window over the page keeps it away this long after its last opening, open or closing update.
constexpr std::uint64_t kBlockMs = 250;

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

// What a screen shows, as one line (the part that is compared).
std::string Shows(const Facts& f) {
    char b[160];
    if (f.kind == Kind::kStrip) {
        std::snprintf(b, sizeof b, "%d tabs, %s menu, lights category %d, state %d", f.tabs,
                      f.mode == kPlusMode ? "the Plus" : "the Minus", f.lit, f.state);
    } else {
        std::snprintf(b, sizeof b, "page %d, asked %d, state %d", f.page, f.asked, f.state);
    }
    return b;
}

}  // namespace

Kind KindOf(std::uint64_t vtable_offset) {
    if (vtable_offset == kPagesVtable) return Kind::kPages;
    if (vtable_offset == kStripVtable) return Kind::kStrip;
    for (const std::uint64_t v : kBlockerVtables) {
        if (vtable_offset == v) return Kind::kBlocker;
    }
    return Kind::kOther;
}

// --- Page ---

bool Page::CanShow(std::uint64_t now_ms) const {
    return pages_.screen != 0 && strip_.screen != 0 && now_ms - pages_ms_ < kStaleMs && now_ms - strip_ms_ < kStaleMs &&
           pages_.state == 2 && pages_.page == kSystemPage && pages_.asked == kSystemPage && strip_.state == 2 &&
           strip_.mode == kPlusMode && strip_.tabs == 5 && now_ms >= blocked_until_ms_;
}

void Page::Hide(const char* why) {
    if (!shown_) {
        return;
    }
    shown_ = false;
    why_ = why;
    // Every button held now stays the module's until let go, so none reaches System as a fresh press (an A held
    // as the page goes would otherwise be the System page's A).
    swallow_ |= pad_last_ & ~kPadPassed;
}

Page::Do Page::OnScreen(const Facts& f, std::uint64_t now_ms) {
    Do d;
    switch (f.kind) {
    case Kind::kBlocker:
        if (f.state >= 1 && f.state <= 3) {
            blocked_until_ms_ = now_ms + kBlockMs;
            Hide("a window opened over it");
        }
        return d;
    case Kind::kPages: {
        if (f.screen != page_screen_) {
            page_screen_ = f.screen, page_set_ = -1, texts_set_ = false;
        }
        pages_ = f, pages_ms_ = now_ms;
        if (shown_ && !CanShow(now_ms)) {
            Hide(f.state != 2 ? "the menu is closing" : "the menu left the System page");
        }
        const int want = shown_ ? 1 : 0;
        if (page_set_ != want) {
            d.set_page = true, d.page_shown = shown_, page_set_ = want;
        }
        if (!texts_set_ && f.state == 2) {
            d.set_texts = true, texts_set_ = true;
        }
        return d;
    }
    case Kind::kStrip: {
        if (f.screen != strip_screen_) {
            strip_screen_ = f.screen, tab_key_ = -1;
        }
        strip_ = f, strip_ms_ = now_ms;
        if (shown_ && !CanShow(now_ms)) {
            Hide(f.mode != kPlusMode ? "the Minus menu" : "the tab strip is closing");
        }
        const bool visible = f.tabs == 5 && f.mode == kPlusMode && f.state == 2;
        const int key = (visible ? 1 : 0) | (shown_ ? 2 : 0) | ((f.lit & 0xff) << 2) | ((f.state & 0xf) << 10) |
                        ((f.mode & 0xf) << 14);
        if (f.tabs == 5 && key != tab_key_) {
            tab_key_ = key;
            d.set_tabs = true, d.tab_shown = visible, d.tab_lit = visible && shown_, d.fill_tab = visible;
        }
        return d;
    }
    case Kind::kOther:
        break;
    }
    return d;
}

std::uint64_t Page::OnPad(std::uint64_t buttons, std::uint64_t now_ms) {
    const std::uint64_t down = buttons & ~pad_last_;
    pad_last_ = buttons;
    swallow_ &= buttons;   // (let go: the game gets it again)
    if (shown_ && !CanShow(now_ms)) {
        Hide("the menu's state changed");
    }
    if (!shown_) {
        if ((down & (kPadR | kPadZR)) != 0 && CanShow(now_ms)) {
            shown_ = true, decide_ = false, why_.clear();
            swallow_ |= buttons & ~kPadPassed;   // (R and anything held with it: none of it reaches System)
            return buttons & kPadPassed & ~swallow_;
        }
        return buttons & ~swallow_;
    }
    if ((down & (kPadL | kPadZL)) != 0) {
        Hide("L");
        return buttons & ~swallow_;
    }
    if ((down & kPadA) != 0) {
        decide_ = true;
    }
    return buttons & kPadPassed & ~swallow_;
}

bool Page::TakeDecide() {
    const bool d = decide_;
    decide_ = false;
    return d;
}

std::string Page::TakeWhy() {
    std::string w;
    w.swap(why_);
    return w;
}

// --- the rows ---

RowAction ActionFor(int row, const Session& s) {
    switch (row) {
    case kRowSession:
        return s.in_one ? RowAction::kOpenMain : RowAction::kHost;
    case kRowJoinCode:
        if (s.hosting) return s.code ? RowAction::kCopyCode : RowAction::kOpenMain;   // (the code still coming: shown there)
        return s.in_one ? RowAction::kOpenMain : RowAction::kOpenJoin;
    case kRowPlayers:
        return RowAction::kOpenLobby;
    case kRowLetIn:
        return s.hosting ? RowAction::kAllow : RowAction::kOpenLobby;
    case kRowLeave:
        return s.in_one ? RowAction::kConfirmLeave : RowAction::kNone;   // (a second A leaves: never one stray press)
    default:
        return RowAction::kNone;
    }
}

namespace {

// At most kRowChars bytes, cut before a UTF-8 sequence's start, with "..." when cut.
std::string Fit(std::string t) {
    if (t.size() <= kRowChars) return t;
    std::size_t n = kRowChars - 3;
    while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xc0) == 0x80) --n;
    return t.substr(0, n) + "...";
}

}  // namespace

std::vector<std::string> RowTexts(const SessionView& v) {
    std::vector<std::string> rows(kRows);
    if (v.connecting) {
        rows[kRowSession] = "Connecting...";
    } else if (v.s.hosting) {
        rows[kRowSession] = "Hosting";
    } else if (v.s.in_one) {
        rows[kRowSession] = "In a session";
    } else {
        rows[kRowSession] = "Host a game";
    }
    if (v.s.hosting) {
        rows[kRowJoinCode] = v.room.empty() ? "Room: coming..." : "Room: " + v.room;
    } else {
        rows[kRowJoinCode] = v.s.in_one ? "Room" : "Join a game";
    }
    rows[kRowPlayers] = v.players <= 0   ? "Players"
                        : v.players == 1 ? "Players: just you"
                                         : "Players: " + std::to_string(v.players);
    if (!v.s.hosting) {
        rows[kRowLetIn] = "Let in";
    } else if (v.asking.empty()) {
        rows[kRowLetIn] = "Let in: nobody asks";
    } else if (v.asking.size() == 1) {
        rows[kRowLetIn] = "Let in " + v.asking.front();
    } else {
        rows[kRowLetIn] = "Let in " + v.asking.front() + " (+" + std::to_string(v.asking.size() - 1) + ")";
    }
    if (v.confirm_leave) {
        rows[kRowLeave] = v.s.hosting ? "A again: stop hosting" : "A again: leave";
    } else {
        rows[kRowLeave] = !v.s.in_one ? "Leave (not in a session)" : v.s.hosting ? "Stop hosting" : "Leave";
    }
    for (std::string& r : rows) r = Fit(std::move(r));
    return rows;
}

const char* ActionWords(RowAction a) {
    switch (a) {
    case RowAction::kOpenMain: return "co-op's menu, its main page";
    case RowAction::kOpenJoin: return "co-op's menu, its Join page";
    case RowAction::kCopyCode: return "the room name or join code to the clipboard, then co-op's main page";
    case RowAction::kOpenLobby: return "co-op's menu, its Lobby tab";
    case RowAction::kAllow: return "the oldest player who asks to join let in (/allow)";
    case RowAction::kHost: return "host now, with the kit's settings";
    case RowAction::kConfirmLeave: return "leave (or stop hosting) after a second A";
    case RowAction::kNone: break;
    }
    return "nothing";
}

// --- Probe ---

std::vector<std::string> Probe::Seen(const Facts& f, std::uint64_t now_ms) {
    std::vector<std::string> out;
    if ((f.kind != Kind::kPages && f.kind != Kind::kStrip) || f.screen == 0) {
        return out;
    }
    Last& last = f.kind == Kind::kPages ? pages_ : strip_;
    const char* what = f.kind == Kind::kPages ? "the pause menu's pages (PlusMenu_00)" : "the tab strip (MenuOverlay_00)";
    if (last.screen != f.screen) {
        last = Last{};
        last.screen = f.screen;
        last.first_ms = now_ms;
        out.push_back("coop: pause tab: " + std::string(what) + ", screen " + Hex(f.screen) +
                      ": its updates reach the module (patch 0030); " + Shows(f));
        last.facts = f;
    } else if (Shows(f) != Shows(last.facts)) {
        out.push_back("coop: pause tab: " + std::string(what) + ": " + Shows(f) + " (was " + Shows(last.facts) + ")");
        last.facts = f;
    }
    ++last.updates;
    if (!last.counted && now_ms - last.first_ms >= 1000) {
        last.counted = true;
        out.push_back("coop: pause tab: " + std::string(what) + ": " + std::to_string(last.updates) +
                      " updates in its first " + std::to_string(now_ms - last.first_ms) + " ms");
    }
    return out;
}

}  // namespace CoopPauseTab
